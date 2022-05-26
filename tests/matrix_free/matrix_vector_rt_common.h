// ---------------------------------------------------------------------
//
// Copyright (C) 2022 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE.md at
// the top level directory of deal.II.
//
// ---------------------------------------------------------------------


// This test template evaluates a simple operator on FE_PolyTensor
// elements using FEEvaluation and compares the result with the output
// of FEValues (which is considered to be the reference) on cartesian
// meshes without hanging nodes. (It will be extended to also handle general
// meshes and hanging nodes in the future.) The test do not include
// multithreading because FEValues is not thread-safe.
// See matrix_vector_rt_01.cc for an example that uses this template.

#include <deal.II/base/logstream.h>
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/tensor.h>
#include <deal.II/base/utilities.h>

#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe_raviart_thomas.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/mapping_q.h>
#include <deal.II/fe/mapping_q1_eulerian.h>

#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/manifold_lib.h>
#include <deal.II/grid/tria.h>

#include <deal.II/lac/affine_constraints.h>
#include <deal.II/lac/dynamic_sparsity_pattern.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/vector.h>

#include <deal.II/matrix_free/fe_evaluation.h>
#include <deal.II/matrix_free/matrix_free.h>
#include <deal.II/matrix_free/matrix_free.templates.h>

#include <deal.II/numerics/vector_tools.h>

#include <fstream>
#include <iostream>

#include "../tests.h"


// forward declare this function. will be implemented in .cc files
template <int dim, int fe_degree>
void
test();

enum TestType : unsigned char
{
  values           = 0,
  values_gradients = 1,
  gradients        = 2,
  divergence       = 3
};

std::string
enum_to_string(TestType const enum_type)
{
  std::string string_type;
  switch (enum_type)
    {
      case TestType::values:
        string_type = "Values ";
        break;
      case TestType::gradients:
        string_type = "Gradients ";
        break;
      case TestType::values_gradients:
        string_type = "Values and Gradients ";
        break;
      case TestType::divergence:
        string_type = "Divergence ";
        break;
      default:
        AssertThrow(false, ExcNotImplemented());
        break;
    }
  return string_type;
}

template <int dim,
          int fe_degree,
          int n_q_points_1d = fe_degree + 1,
          typename Number   = double>
class MatrixFreeTest
{
public:
  MatrixFreeTest(const MatrixFree<dim, Number> &data_in,
                 const TestType                 test_type)
    : data(data_in)
    , test_type(test_type)
  {
    evaluation_flag =
      (test_type == TestType::values) ?
        EvaluationFlags::values :
        ((test_type == TestType::gradients) ?
           EvaluationFlags::gradients :
           ((test_type == TestType::values_gradients) ?
              EvaluationFlags::values | EvaluationFlags::gradients :
              EvaluationFlags::gradients));
  };

  virtual ~MatrixFreeTest(){};

  void
  operator()(const MatrixFree<dim, Number> &              data,
             Vector<Number> &                             dst,
             const Vector<Number> &                       src,
             const std::pair<unsigned int, unsigned int> &cell_range) const
  {
    FEEvaluation<dim, fe_degree, n_q_points_1d, dim, Number> fe_eval(data);

    for (unsigned int cell = cell_range.first; cell < cell_range.second; ++cell)
      {
        fe_eval.reinit(cell);
        fe_eval.gather_evaluate(src, evaluation_flag);

        for (unsigned int q = 0; q < fe_eval.n_q_points; ++q)
          {
            if (test_type < TestType::gradients)
              fe_eval.submit_value(Number(10) * fe_eval.get_value(q), q);
            if (test_type == TestType::gradients ||
                test_type == TestType::values_gradients)
              fe_eval.submit_gradient(fe_eval.get_gradient(q), q);
            else if (test_type == TestType::divergence)
              fe_eval.submit_divergence(fe_eval.get_divergence(q), q);
          }

        fe_eval.integrate_scatter(evaluation_flag, dst);
      }
  };


  void
  test_functions(Vector<Number> &dst, const Vector<Number> &src) const
  {
    data.cell_loop(&MatrixFreeTest::operator(), this, dst, src);
  };

protected:
  const MatrixFree<dim, Number> &  data;
  EvaluationFlags::EvaluationFlags evaluation_flag;
  const TestType                   test_type;
};


template <int dim, int fe_degree, typename Number>
void
do_test(const DoFHandler<dim> &          dof,
        const AffineConstraints<double> &constraints,
        const TestType                   test_type)
{
  deallog << "Testing " << enum_to_string(test_type) << std::endl;

  //   constraints.distribute(solution);
  MatrixFree<dim, Number> mf_data;
  {
    const QGaussLobatto<1>                           quad(fe_degree + 2);
    typename MatrixFree<dim, Number>::AdditionalData data;
    data.tasks_parallel_scheme = MatrixFree<dim, Number>::AdditionalData::none;
    data.mapping_update_flags  = update_gradients | update_hessians;
    mf_data.reinit(dof, constraints, quad, data);
  }

  // create vector with random entries
  Vector<Number> solution, initial_condition;

  mf_data.initialize_dof_vector(solution);
  mf_data.initialize_dof_vector(initial_condition);

  for (unsigned int i = 0; i < dof.n_dofs(); ++i)
    {
      if (constraints.is_constrained(i))
        continue;
      initial_condition[i] = random_value<Number>();
    }

  // MatrixFree solution
  MatrixFreeTest<dim, fe_degree, fe_degree + 2, Number> mf(mf_data, test_type);
  mf.test_functions(solution, initial_condition);


  SparsityPattern        sp;
  SparseMatrix<double>   system_matrix;
  DynamicSparsityPattern dsp(dof.n_dofs(), dof.n_dofs());
  DoFTools::make_sparsity_pattern(dof, dsp);
  sp.copy_from(dsp);
  system_matrix.reinit(sp);

  FEValues<dim> fe_val(dof.get_fe(),
                       Quadrature<dim>(mf_data.get_quadrature(0)),
                       update_values | update_gradients | update_JxW_values);


  const unsigned int dofs_per_cell = fe_val.get_fe().dofs_per_cell;
  std::vector<types::global_dof_index> local_dof_indices(dofs_per_cell);
  FullMatrix<double> local_matrix(dofs_per_cell, dofs_per_cell);


  const FEValuesExtractors::Vector velocities(0);

  // Assemble matrix
  for (const auto &cell : dof.active_cell_iterators())
    {
      fe_val.reinit(cell);
      local_matrix = 0;

      for (const auto q : fe_val.quadrature_point_indices())
        for (unsigned int i = 0; i < dofs_per_cell; ++i)
          {
            const Tensor<1, dim> phi_i = fe_val[velocities].value(i, q) * 10.;
            const Tensor<2, dim> grad_phi_i = fe_val[velocities].gradient(i, q);
            const Number div_phi_i = fe_val[velocities].divergence(i, q);

            for (unsigned int j = 0; j < dofs_per_cell; ++j)
              {
                const Tensor<1, dim> phi_j = fe_val[velocities].value(j, q);
                const Tensor<2, dim> grad_phi_j =
                  fe_val[velocities].gradient(j, q);
                const Number div_phi_j = fe_val[velocities].divergence(j, q);

                if (test_type < TestType::gradients)
                  local_matrix(i, j) += phi_j * phi_i * fe_val.JxW(q);
                if (test_type == TestType::gradients ||
                    test_type == TestType::values_gradients)
                  local_matrix(i, j) +=
                    scalar_product(grad_phi_i, grad_phi_j) * fe_val.JxW(q);
                else if (test_type == TestType::divergence)
                  local_matrix(i, j) += div_phi_i * div_phi_j * fe_val.JxW(q);
              }
          }
      cell->get_dof_indices(local_dof_indices);
      for (unsigned int i = 0; i < dofs_per_cell; ++i)
        for (unsigned int j = 0; j < dofs_per_cell; ++j)
          system_matrix.add(local_dof_indices[i],
                            local_dof_indices[j],
                            local_matrix(i, j));
    }

  Vector<Number> ref(solution.size());

  // Compute reference
  system_matrix.vmult(ref, initial_condition);

  ref -= solution;

  const double diff_norm = ref.linfty_norm() / solution.linfty_norm();
  deallog << "Norm of difference: " << diff_norm << std::endl << std::endl;
}


int
main()
{
  initlog();

  {
    deallog.push("2d");
    test<2, 2>();
    test<2, 3>();
    deallog.pop();
    deallog.push("3d");
    test<3, 2>();
    test<3, 3>();
    deallog.pop();
  }
}