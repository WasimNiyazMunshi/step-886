#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/function.h>
#include <deal.II/base/timer.h>
#include <deal.II/lac/generic_linear_algebra.h>
#define FORCE_USE_OF_TRILINOS
namespace LA
{
#if defined(DEAL_II_WITH_PETSC) && !defined(DEAL_II_PETSC_WITH_COMPLEX) && \
                !(defined(DEAL_II_WITH_TRILINOS) && defined(FORCE_USE_OF_TRILINOS))
using namespace dealii::LinearAlgebraPETSc;
#  define USE_PETSC_LA
#elif defined(DEAL_II_WITH_TRILINOS)
using namespace dealii::LinearAlgebraTrilinos;
#else
#  error DEAL_II_WITH_PETSC or DEAL_II_WITH_TRILINOS required
#endif
} // namespace LA

#include <deal.II/lac/vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/precondition.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/affine_constraints.h>
#include <deal.II/lac/dynamic_sparsity_pattern.h>
#include <deal.II/grid/grid_in.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_tools.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/fe_system.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/grid/grid_refinement.h>
#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/error_estimator.h>
#include <deal.II/base/utilities.h>
#include <deal.II/base/conditional_ostream.h>
#include <deal.II/base/index_set.h>
#include <deal.II/lac/sparsity_tools.h>
#include <deal.II/distributed/tria.h>
#include <deal.II/distributed/grid_refinement.h>
#include <deal.II/distributed/solution_transfer.h>
//This header gives us the functionality to store data at quadrature points
#include <deal.II/base/quadrature_point_data.h>
#include <deal.II/base/tensor_function.h>
#include <fstream>
#include <iostream>
#include <random>

namespace Step886

{
  using namespace dealii;

  class PhaseField
  {
      // Function declarations
    public:
      PhaseField ();
      void
      run ();

    private:

      // 1. Mesh and boundary conditions setup at the beginning
      void
      setup_mesh_and_bcs (); // Called at the start to create a mesh
      void
      setup_dofs_and_initial_fields (); // Sets up DoFs, initial solution vectors and history field
      // 2. Elastic subproblem setup and solution
      void
      setup_constraints_elastic (const unsigned int load_step);
      void
      setup_system_elastic (const unsigned int load_step);
      void
      assemble_system_elastic ();
      void
      solve_linear_system_elastic ();
      double
      damage_gauss_pt (LA::MPI::Vector &locally_relevant_solution_damage,
          const DoFHandler<2>::active_cell_iterator &cell,
          const unsigned int q_point,
          const FEValues<2> &fe_values_elastic); // Called during system assembly for the elastic subproblem
      void
      solve_elastic_subproblem (const unsigned int load_step); // Calls the above functions

      // 3. Damage subproblem setup and solution
      void
      setup_boundary_values_damage ();
      void
      setup_system_damage ();
      void
      assemble_system_damage ();
      void
      solve_linear_system_damage ();
      double
      H_plus (const SymmetricTensor<2, 2> &strain, const auto &x_q); // Called during system assembly for the damage subproblem
      void
      solve_damage_subproblem (); // Calls the above functions

      // 4. Convergence check after each iteration
      bool
      check_convergence ();

      // 5. Post-processing: output results, refine grid, and calculate displacement
      void
      output_results (const unsigned int load_step) const; // Called if convergence is achieved
      void
      load_disp_calculation (const unsigned int load_step); // Called if convergence is achieved
      void
      update_history_field (); // Called if convergence is achieved
      void
      refine_grid (const unsigned int load_step); // Called if convergence is not achieved

      MPI_Comm mpi_communicator;
      ConditionalOStream pcout;
      TimerOutput computing_timer;

      parallel::distributed::Triangulation<2> triangulation;

      // Objects for elasticity
      const FESystem<2> fe_elastic;
      DoFHandler<2> dof_handler_elastic;
      IndexSet locally_owned_dofs_elastic;
      IndexSet locally_relevant_dofs_elastic;
      AffineConstraints<double> constraints_elastic;
      LA::MPI::SparseMatrix system_matrix_elastic;
      LA::MPI::Vector locally_relevant_solution_elastic;
      LA::MPI::Vector completely_distributed_solution_elastic_old;
      LA::MPI::Vector completely_distributed_solution_elastic;
      LA::MPI::Vector system_rhs_elastic;
      const QGauss<2> quadrature_formula_elastic;

      // Objects for damage
      const FE_Q<2> fe_damage;
      DoFHandler<2> dof_handler_damage;
      IndexSet locally_owned_dofs_damage;
      IndexSet locally_relevant_dofs_damage;
      AffineConstraints<double> constraints_damage;
      LA::MPI::SparseMatrix system_matrix_damage;
      LA::MPI::Vector locally_relevant_solution_damage;
      LA::MPI::Vector completely_distributed_solution_damage_old;
      LA::MPI::Vector completely_distributed_solution_damage;
      LA::MPI::Vector system_rhs_damage;
      const QGauss<2> quadrature_formula_damage;
      const QGauss<1> face_quadrature_formula_damage;

      class MyQData : public TransferableQuadraturePointData
      {
        public:
          MyQData () = default;
          virtual
          ~MyQData () = default;

          unsigned int
          number_of_values () const override
          {
            return 2; // Indicate that there are two values to handle
          }

          virtual void
          pack_values (std::vector<double> &scalars) const override
          {
            Assert (scalars.size () == 2, ExcInternalError ()); // Ensure the vector has exactly two elements
            scalars[0] = value_H; // Pack the first value
            scalars[1] = value_H_new; // Pack the second value
          }

          virtual void
          unpack_values (const std::vector<double> &scalars) override
          {
            Assert (scalars.size () == 2, ExcInternalError ()); // Ensure the vector has exactly two elements
            value_H = scalars[0]; // Unpack the first value
            value_H_new = scalars[1]; // Unpack the second value
          }

          double value_H; // First value
          double value_H_new; // Second value
      };

      CellDataStorage<typename Triangulation<2>::cell_iterator, MyQData> quadrature_point_history_field;

      const double x_min = 0;
      const double y_min = 0;
      const double x_max = 10;
      const double y_max = 10;

      const double uy = 10*1e-5; // increment in loading
      const double l = 3.5e-2; // length scale parameter
      const unsigned int num_load_steps = 300;//2500; // number of load steps
      const double tol = 1e-2; // tolerance for error in solution
      const double GC = 2e-4; //energy release rate
      const double E = 30;
      const double nu = 0.3;
     // const double interface_width = 0.01/2;
      const double crack_length = 2.5;
      const double beta = 2;

  };

  double
  lambda (const double E,
      const double nu, const double beta, const Point<2> &p )
  {
    if (p[1] > 0.8*p[0]+1) // Point lies below the interface
      return (E * nu) / ((1 + nu) * (1 - 2 * nu));
  else
    return ((E/beta) * nu) / ((1 + nu) * (1 - 2 * nu));
  }

  double
  mu (const double E,
      const double nu, const double beta, const Point<2> &p)
  {
    if (p[1] > 0.8*p[0]+1) // Point lies below the interface
          return E / (2 * (1 + nu));
    else
    return (E/beta) / (2 * (1 + nu));
  }

  double
    gc (const double GC,
        const double beta,
        //const double interface_width,
        const Point<2> &p)
    {
      //if ((std::fabs(0.8*p[0]-p[1]+1)/std::sqrt(0.8*0.8 + 1))-interface_width<=1e-4) // check the distance of the point from interface line
    if (p[1] > 0.8*p[0]+1) // Point lies below the interface
      return GC;
    else
      return GC/beta;
    }


  double
  Conductivity_damage (const Point<2> &p) //const
  {
    return p[0] - p[0] + 1;
  }

  void
  right_hand_side_elastic (const std::vector<Point<2>> &points,
      std::vector<Tensor<1, 2>> &values)
  {
    AssertDimension (values.size (), points.size ());

    for (unsigned int point_n = 0; point_n < points.size (); ++point_n)
      {
        values[point_n][0] = 0; //x component of body force
        values[point_n][1] = 0; //y component of body force
        //values[point_n][2] = 0; //z component of body force
      }

  }
  void
  Traction_elastic (const std::vector<Point<2>> &points,
      std::vector<Tensor<1, 2>> &values)

  {
    AssertDimension (values.size (), points.size ());
    for (unsigned int point_n = 0; point_n < points.size (); ++point_n)
      {
        values[point_n][0] = 0; //x component of traction
        values[point_n][1] = 0; //y component of traction
        //values[point_n][2] = 0; //y component of traction
      }

  }

  PhaseField::PhaseField ()
      :
          mpi_communicator (MPI_COMM_WORLD),
          pcout (std::cout,
              (Utilities::MPI::this_mpi_process (mpi_communicator) == 0)),
          computing_timer (mpi_communicator, pcout, TimerOutput::never,
              TimerOutput::wall_times),
          triangulation (mpi_communicator),
          fe_elastic (FE_Q <2> (1), 2),
          dof_handler_elastic (triangulation),
          quadrature_formula_elastic (fe_elastic.degree + 1),
          fe_damage (1),
          dof_handler_damage (triangulation),
          quadrature_formula_damage (fe_elastic.degree + 1),
          face_quadrature_formula_damage(fe_damage.degree + 1)
  {
  }

  double
  PhaseField::H_plus (const SymmetricTensor<2, 2> &strain, const auto &x_q)
  {
    double Mac_tr_strain, Mac_first_principal_strain,
        Mac_second_principal_strain,
        tr_sqr_Mac_Principal_strain;

    const double tr_strain = trace (strain);
    if (tr_strain > 0)
      {
        Mac_tr_strain = tr_strain;
      }
    else
      {
        Mac_tr_strain = 0;
      }
    const std::array<double, 2> Principal_strains = eigenvalues (strain);
    if (Principal_strains[0] > 0)
      {
        Mac_first_principal_strain = Principal_strains[0];
      }
    else
      {
        Mac_first_principal_strain = 0;
      }
    if (Principal_strains[1] > 0)
      {
        Mac_second_principal_strain = Principal_strains[1];
      }
    else
      {
        Mac_second_principal_strain = 0;
      }

    tr_sqr_Mac_Principal_strain = pow (Mac_first_principal_strain, 2)
        + pow (Mac_second_principal_strain, 2);

    double H_plus_val;
    H_plus_val = 0.5 * lambda (E, nu, beta, x_q) * pow (Mac_tr_strain, 2)
        + mu (E, nu, beta, x_q) * tr_sqr_Mac_Principal_strain;
    return H_plus_val;
  }

  double
  PhaseField::damage_gauss_pt (LA::MPI::Vector &locally_relevant_solution_damage,
      const DoFHandler<2>::active_cell_iterator &cell,
      const unsigned int q_point,
      const FEValues<2> &fe_values_elastic)
  {
    int node = 0;
    double d = 0;

    for (const auto vertex : cell->vertex_indices ())
      {
        int a = (int) ((cell->vertex_dof_index (vertex, 0)) / 2);
        d = d + locally_relevant_solution_damage[a]
            * fe_values_elastic.shape_value (2 * node, q_point);
        node = node + 1;
      }

    return d;

  }

  void
  PhaseField::setup_mesh_and_bcs ()

  {
    /*const unsigned int nx = 64;
    const unsigned int ny = 10;
    const std::vector<unsigned int> repetitions = {nx,ny};

    const Point<2> p1(x_min,y_min), p2(x_max,y_max);

    GridGenerator::subdivided_hyper_rectangle (triangulation, repetitions, p1,
        p2);*/ // create coarse mesh

    GridIn<2> grid_in;
    grid_in.attach_triangulation(triangulation);

    std::ifstream f("IncInt2DGeoPG4.msh");
    //std::ifstream f("BIM_inc_validation_new2");
    grid_in.read_msh(f);

    // The boundary ids need to be setup right after the mesh is generated (before any refinement) and ids need to be setup using all cells and not just the locally owned cells

    for (const auto &cell : triangulation.active_cell_iterators ())
      {
        for (const auto &face : cell->face_iterators ())
          {
            if (face->at_boundary ())
              {
                const auto center = face->center ();
                if (std::fabs (center (1) - (y_min)) < 1e-12)
                  face->set_boundary_id (1);

                else if (std::fabs (center (1) - y_max) < 1e-12)
                  face->set_boundary_id (2);

                else if (std::fabs (center (0) - (x_min)) < 1e-12)
                  face->set_boundary_id (3242);

                else if (std::fabs (center (0) - x_max) < 1e-12)
                  face->set_boundary_id (4222);
              }
          }
      }
    //triangulation.coarsen_global();
    triangulation.refine_global(1);


    pcout << "No. of levels in triangulation: "
    << triangulation.n_global_levels () << std::endl;

    // Refining the mesh for pre-crack

    /*for (unsigned int cycle = 0; cycle < 2; ++cycle)
     {
     for (const auto &cell : triangulation.active_cell_iterators()) // active_cell_iterator iterates over cells which have not been refined.
     //In other words, it does not include cells that have been refined.

     {
     if (cell->is_locally_owned()) // is_locally_owned  can only be called for active_cell
     {
     double min_x_cell = std::numeric_limits<double>::infinity();
     double min_y_cell = std::numeric_limits<double>::infinity();
     double max_y_cell = -std::numeric_limits<double>::infinity();
     for (const auto vertex_number : cell->vertex_indices())
     {
     const auto vert = cell->vertex(vertex_number);
     min_x_cell = std::min(min_x_cell, vert[0]);
     min_y_cell = std::min(min_y_cell, vert[1]);
     max_y_cell = std::max(max_y_cell, vert[1]);
     }

     if (((std::fabs(min_y_cell - y_mid) <= l) && (min_x_cell <= x_mid))
     || ((std::fabs(max_y_cell - y_mid) <= l) && (min_x_cell <= x_mid)))
     cell->set_refine_flag();
     }

     }

     triangulation.prepare_coarsening_and_refinement();
     triangulation.execute_coarsening_and_refinement();
     pcout << "No. of levels in triangulation: "  << triangulation.n_global_levels() << std::endl;

     }*/


  }

  void
    PhaseField::setup_dofs_and_initial_fields ()

  {
    dof_handler_damage.distribute_dofs (fe_damage);
        dof_handler_elastic.distribute_dofs (fe_elastic);

        pcout << "   Number of locally owned cells on the process:       "
        << triangulation.n_locally_owned_active_cells () << std::endl;

        pcout << "Number of global cells:" << triangulation.n_global_active_cells ()
        << std::endl;

        pcout << "  Total Number of globally active cells:       "
        << triangulation.n_global_active_cells () << std::endl
        << "   Number of degrees of freedom for elasticity: "
        << dof_handler_elastic.n_dofs () << std::endl
        << " Number of degrees of freedom for damage: "
        << dof_handler_damage.n_dofs () << std::endl;

        //Initialising damage vectors
        locally_owned_dofs_damage = dof_handler_damage.locally_owned_dofs ();
        locally_relevant_dofs_damage = DoFTools::extract_locally_relevant_dofs (
            dof_handler_damage);

        completely_distributed_solution_damage_old.reinit (
            locally_owned_dofs_damage, mpi_communicator);
        locally_relevant_solution_damage.reinit (locally_owned_dofs_damage,
            locally_relevant_dofs_damage, mpi_communicator);

        locally_owned_dofs_elastic = dof_handler_elastic.locally_owned_dofs ();
        locally_relevant_dofs_elastic = DoFTools::extract_locally_relevant_dofs (
            dof_handler_elastic);

        completely_distributed_solution_elastic_old.reinit (
            locally_owned_dofs_elastic, mpi_communicator);
        locally_relevant_solution_elastic.reinit (locally_owned_dofs_elastic,
                    locally_relevant_dofs_elastic, mpi_communicator);

        for (const auto &cell : triangulation.active_cell_iterators ())
          {
            if (cell->is_locally_owned ())
              {
                quadrature_point_history_field.initialize (cell, 4);
              }
          }

        FEValues <2> fe_values_damage (fe_damage, quadrature_formula_damage,
            update_values | update_gradients | update_JxW_values
            | update_quadrature_points);

        for (const auto &cell : triangulation.active_cell_iterators ())
          if (cell->is_locally_owned ())
            {
              const std::vector<std::shared_ptr<MyQData>> lqph =
                  quadrature_point_history_field.get_data (cell);
              for (const unsigned int q_index : fe_values_damage.quadrature_point_indices ())
                {
                  lqph[q_index]->value_H = 0.0;
                  lqph[q_index]->value_H_new = 0.0;
                }
            }
  }

  void
  PhaseField::setup_constraints_elastic (const unsigned int load_step)
  {
    constraints_elastic.clear ();
    constraints_elastic.reinit (locally_relevant_dofs_elastic);

    DoFTools::make_hanging_node_constraints (dof_handler_elastic,
        constraints_elastic);

   /* for (const auto &cell : dof_handler_elastic.active_cell_iterators ())
          if (cell->is_locally_owned ())
            {
              for (const auto &face : cell->face_iterators ())
                {
                  //if (face->at_boundary ())
                    {
                      //const auto center = face->center ();
                      //if (std::fabs (center (0) - x_min) < 1e-12) //face lies at x=x_min
                        {

                          for (const auto vertex_number : cell->vertex_indices ())
                            {
                              const auto vert = cell->vertex (vertex_number);

                              //if (std::fabs (vert (1) - y_min) < 1e-12 && std::fabs (vert (0)- x_min)< 1e-12)
                                {
                                  const unsigned int y_dof =
                                      cell->vertex_dof_index (vertex_number, 1);
                                  constraints_elastic.add_line (y_dof);
                                  constraints_elastic.set_inhomogeneity (y_dof, 0);
                                }
                            }
                        }
                    }
                }
            }*/
    const FEValuesExtractors::Scalar u_x(0);
    const FEValuesExtractors::Scalar u_y_top(1);
    const FEValuesExtractors::Scalar u_y_bottom(1);

    const ComponentMask u_x_mask = fe_elastic.component_mask(u_x);
    const ComponentMask u_y_mask_bottom = fe_elastic.component_mask(u_y_bottom);
    const ComponentMask u_y_mask_top = fe_elastic.component_mask(u_y_top);

    const double u_y_values_top = uy*load_step;
    const double u_fix = 0.000;

    VectorTools::interpolate_boundary_values(dof_handler_elastic,1,
        Functions::ConstantFunction<2>(u_fix,2),
        constraints_elastic,u_x_mask);

    VectorTools::interpolate_boundary_values(dof_handler_elastic,1,
            Functions::ConstantFunction<2>(u_fix,2),
            constraints_elastic,u_y_mask_bottom);

    /*VectorTools::interpolate_boundary_values(dof_handler_elastic,2,
            Functions::ConstantFunction<2>(u_fix,2),
            constraints_elastic,u_x_mask);*/

    VectorTools::interpolate_boundary_values(dof_handler_elastic,2,
        Functions::ConstantFunction<2>(u_y_values_top,2),
        constraints_elastic,u_y_mask_top);

    constraints_elastic.close ();
  }

  void
  PhaseField::setup_system_elastic (const unsigned int load_step)
  {
    TimerOutput::Scope ts (computing_timer, "setup_system_elastic");

    locally_owned_dofs_elastic = dof_handler_elastic.locally_owned_dofs ();
    locally_relevant_dofs_elastic = DoFTools::extract_locally_relevant_dofs (
        dof_handler_elastic);

    locally_relevant_solution_elastic.reinit (locally_owned_dofs_elastic,
        locally_relevant_dofs_elastic, mpi_communicator);

    system_rhs_elastic.reinit (locally_owned_dofs_elastic, mpi_communicator);

    completely_distributed_solution_elastic.reinit (locally_owned_dofs_elastic,
        mpi_communicator);

    setup_constraints_elastic (load_step);

    DynamicSparsityPattern dsp (locally_relevant_dofs_elastic);

    DoFTools::make_sparsity_pattern (dof_handler_elastic, dsp,
        constraints_elastic, false);

    SparsityTools::distribute_sparsity_pattern (dsp,
        dof_handler_elastic.locally_owned_dofs (), mpi_communicator,
        locally_relevant_dofs_elastic);

    system_matrix_elastic.reinit (locally_owned_dofs_elastic,
        locally_owned_dofs_elastic, dsp, mpi_communicator);
  }

  void
  PhaseField::assemble_system_elastic ()

  {
    TimerOutput::Scope ts (computing_timer, "assembly_elastic");

    FEValues <2> fe_values_elastic (fe_elastic, quadrature_formula_elastic,
        update_values | update_gradients | update_quadrature_points
        | update_JxW_values);

    const unsigned int dofs_per_cell = fe_elastic.n_dofs_per_cell ();
    const unsigned int n_q_points = quadrature_formula_elastic.size ();

    FullMatrix<double> cell_matrix_elastic (dofs_per_cell, dofs_per_cell);
    Vector<double> cell_rhs_elastic (dofs_per_cell);

    std::vector < types::global_dof_index > local_dof_indices (dofs_per_cell);

    std::vector<Tensor<1, 2>> rhs_values_elastic (n_q_points);

    for (const auto &cell : dof_handler_elastic.active_cell_iterators ())
      if (cell->is_locally_owned ())

        {
          cell_matrix_elastic = 0;
          cell_rhs_elastic = 0;
          fe_values_elastic.reinit (cell);

          right_hand_side_elastic (fe_values_elastic.get_quadrature_points (),
              rhs_values_elastic);
          for (const unsigned int q_point : fe_values_elastic.quadrature_point_indices ())
            {
              const auto &x_q = fe_values_elastic.quadrature_point (q_point);

              double d = damage_gauss_pt (locally_relevant_solution_damage,
                  cell, q_point, fe_values_elastic);

              for (const unsigned int i : fe_values_elastic.dof_indices ())

                {
                  const unsigned int component_i =
                      fe_elastic.system_to_component_index (i).first;

                  for (const unsigned int j : fe_values_elastic.dof_indices ())
                    {
                      const unsigned int component_j =
                          fe_elastic.system_to_component_index (j).first;

                        {
                          cell_matrix_elastic (i, j) +=
                              (pow ((1 - d), 2)) * ( //
                              (fe_values_elastic.shape_grad (i, q_point)[component_i] * //
                              fe_values_elastic.shape_grad (j, q_point)[component_j]
                               * //
                               lambda (E, nu, beta, x_q)) //
                              +//
                              (fe_values_elastic.shape_grad (i, q_point)[component_j] * //
                              fe_values_elastic.shape_grad (j, q_point)[component_i]
                               * //
                               mu (E, nu, beta, x_q)) //
                              +//
                              ((component_i == component_j) ? //
                              (fe_values_elastic.shape_grad (i, q_point) * //
                              fe_values_elastic.shape_grad (j, q_point)
                               * //
                               mu (E, nu, beta, x_q)) : //
                              0) //
                              )
                              * //
                              fe_values_elastic.JxW (q_point); //
                        }
                    }
                }
            }

          for (const unsigned int i : fe_values_elastic.dof_indices ())
            {
              const unsigned int component_i =
                  fe_elastic.system_to_component_index (i).first;

              for (const unsigned int q_point : fe_values_elastic.quadrature_point_indices ())
                cell_rhs_elastic (i) +=
                    fe_values_elastic.shape_value (i, q_point) * rhs_values_elastic[q_point][component_i]
                    * fe_values_elastic.JxW (q_point);
            }

          // traction contribution to rhs
          /* for (const auto &face : cell->face_iterators())
           {
           if (face->at_boundary() &&
           face->boundary_id() == 10)

           {
           fe_face_values_elastic.reinit(cell, face);
           for (const auto i : fe_face_values_elastic.dof_indices())

           {const unsigned int component_i =
           fe_elastic.system_to_component_index(i).first;
           for (const auto face_q_point :
           fe_face_values_elastic.quadrature_point_indices())
           {

           cell_rhs_elastic(i) +=
           fe_face_values_elastic.shape_value( i, face_q_point)
           *(traction_values_elastic[face_q_point][component_i])
           *fe_face_values_elastic.JxW(face_q_point);



           }
           }
           }
           }*/

          cell->get_dof_indices (local_dof_indices);

          constraints_elastic.distribute_local_to_global (cell_matrix_elastic,
              cell_rhs_elastic, local_dof_indices, system_matrix_elastic,
              system_rhs_elastic);
        }

    system_matrix_elastic.compress (VectorOperation::add);
    system_rhs_elastic.compress (VectorOperation::add);
  }

  void
  PhaseField::solve_linear_system_elastic ()
  {
    TimerOutput::Scope ts (computing_timer, "solve_linear_system_elastic");

    SolverControl solver_control (20000,
        1e-12/** system_rhs_elastic.l2_norm()*/);

    SolverCG < TrilinosWrappers::MPI::Vector > solver (solver_control);

    LA::MPI::PreconditionAMG::AdditionalData data;
#ifdef USE_PETSC_LA
        data.symmetric_operator = true;
#else
    // Trilinos defaults are good
#endif
    LA::MPI::PreconditionAMG preconditioner;
    preconditioner.initialize (system_matrix_elastic, data);

    solver.solve (system_matrix_elastic,
        completely_distributed_solution_elastic, system_rhs_elastic,
        preconditioner);

    pcout << "   Solved in " << solver_control.last_step () << " iterations."
    << std::endl;

    constraints_elastic.distribute (completely_distributed_solution_elastic);

    locally_relevant_solution_elastic = completely_distributed_solution_elastic;
  }

  void
  PhaseField::setup_boundary_values_damage ()
  {
    TimerOutput::Scope ts (computing_timer, "setup_bv_damage");

    constraints_damage.clear ();
    constraints_damage.reinit (locally_relevant_dofs_damage);
    DoFTools::make_hanging_node_constraints (dof_handler_damage,
        constraints_damage);

    // Create initial crack, if any
    for (const auto &cell : dof_handler_damage.active_cell_iterators())
      if (cell->is_locally_owned())

        {
          //for (const auto &face : cell->face_iterators())

          for (const auto vertex_number : cell->vertex_indices())
            {
              const auto vert = cell->vertex(vertex_number);
              const Point<2>& node = vert;
                if (
                    (std::fabs(node(1)-0.5*(y_max+y_min))<1e-4)
                    &&(node(0) - crack_length <1e-4)
                   )
                {
                  const unsigned int dof = cell->vertex_dof_index(vertex_number, 0);
                  constraints_damage.add_line(dof);
                  constraints_damage.set_inhomogeneity(dof,1);
                }
            }
        }
    constraints_damage.close ();
  }

  void
  PhaseField::setup_system_damage ()
  {
    TimerOutput::Scope ts (computing_timer, "setup_system_damage");

    locally_owned_dofs_damage = dof_handler_damage.locally_owned_dofs ();
    locally_relevant_dofs_damage = DoFTools::extract_locally_relevant_dofs (
        dof_handler_damage);

    locally_relevant_solution_damage.reinit (locally_owned_dofs_damage,
        locally_relevant_dofs_damage, mpi_communicator);

    system_rhs_damage.reinit (locally_owned_dofs_damage, mpi_communicator);

    completely_distributed_solution_damage.reinit (locally_owned_dofs_damage,
        mpi_communicator);

    DynamicSparsityPattern dsp (locally_relevant_dofs_damage);

    DoFTools::make_sparsity_pattern (dof_handler_damage, dsp,
        constraints_damage, false);
    SparsityTools::distribute_sparsity_pattern (dsp,
        dof_handler_damage.locally_owned_dofs (), mpi_communicator,
        locally_relevant_dofs_damage);

    system_matrix_damage.reinit (locally_owned_dofs_damage,
        locally_owned_dofs_damage, dsp, mpi_communicator);
  }

  void
  PhaseField::assemble_system_damage ()
  {

    TimerOutput::Scope ts (computing_timer, "assembly_damage");

    FEValues <2> fe_values_damage (fe_damage, quadrature_formula_damage,
        update_values | update_gradients | update_JxW_values
        | update_quadrature_points);
    FEFaceValues <2> fe_face_values_damage (fe_damage, face_quadrature_formula_damage,
                        update_values | update_quadrature_points |
                        update_normal_vectors |  update_JxW_values);
    FEValues <2> fe_values_elastic (fe_elastic, quadrature_formula_damage,
        update_values | update_gradients | update_JxW_values
        | update_quadrature_points);

    const unsigned int dofs_per_cell = fe_damage.n_dofs_per_cell ();
    const unsigned int n_q_points = quadrature_formula_damage.size ();

    FullMatrix<double> cell_matrix_damage (dofs_per_cell, dofs_per_cell);
    Vector<double> cell_rhs_damage (dofs_per_cell);

    std::vector < types::global_dof_index > local_dof_indices (dofs_per_cell);

    // Storing strain tensor for all Gauss points of a cell in vector strain_values.
    std::vector<SymmetricTensor<2, 2>> strain_values (n_q_points);

    for (const auto &cell : dof_handler_damage.active_cell_iterators ())
      if (cell->is_locally_owned ())
        {
          std::vector < std::shared_ptr < MyQData >> qpdH =
              quadrature_point_history_field.get_data (cell);

          const DoFHandler<2>::active_cell_iterator elastic_cell =
              Triangulation <2> ::active_cell_iterator (cell)->as_dof_handler_iterator (
                  dof_handler_elastic);

          fe_values_damage.reinit (cell);
          fe_values_elastic.reinit (elastic_cell);

          const FEValuesExtractors::Vector displacements (
              /* first vector component = */0);
          fe_values_elastic[displacements].get_function_symmetric_gradients (
              locally_relevant_solution_elastic, strain_values);

          cell_matrix_damage = 0;
          cell_rhs_damage = 0;

          for (const unsigned int q_index : fe_values_damage.quadrature_point_indices ())
            {
              const auto &x_q = fe_values_damage.quadrature_point (q_index);
              double H_call = H_plus (strain_values[q_index], x_q);

              const double H = std::max(H_call, qpdH[q_index]->value_H);
              qpdH[q_index]->value_H_new = H;

              double g_c;
              g_c = gc (GC, beta, x_q);

              for (const unsigned int i : fe_values_damage.dof_indices ())
                {
                  for (const unsigned int j : fe_values_damage.dof_indices ())
                    {

                      /*cell_matrix_damage (i, j) +=
                      // contribution to stiffness from -laplace u term
                          Conductivity_damage (x_q) * fe_values_damage.shape_grad (
                              i, q_index)
                          * // kappa*grad phi_i(x_q)
                          fe_values_damage.shape_grad (j, q_index)
                          * // grad phi_j(x_q)
                          fe_values_damage.JxW (q_index) // dx
                          +
                          // Contribution to stiffness from u term

                          ((1 + (2 * l * H) / g_c) * (1 / pow (l, 2))
                           * fe_values_damage.shape_value (i, q_index) * // phi_i(x_q)
                           fe_values_damage.shape_value (j, q_index) * // phi_j(x_q)
                           fe_values_damage.JxW (q_index)); // dx*/
                      cell_matrix_damage (i, j) +=
                          // contribution to stiffness from -laplace u term
                          g_c*l* fe_values_damage.shape_grad (
                              i, q_index)
                              * // kappa*grad phi_i(x_q)
                              fe_values_damage.shape_grad (j, q_index)
                              * // grad phi_j(x_q)
                              fe_values_damage.JxW (q_index) // dx
                              +
                              // Contribution to stiffness from u term

                              ( (g_c/l +2*H)
                                  * fe_values_damage.shape_value (i, q_index) * // phi_i(x_q)
                                  fe_values_damage.shape_value (j, q_index) * // phi_j(x_q)
                                  fe_values_damage.JxW (q_index)); // dx
                    }
                  //const auto &x_q = fe_values_damage.quadrature_point(q_index);
                 /* cell_rhs_damage (i) += (fe_values_damage.shape_value (i,
                                              q_index)
                                          * // phi_i(x_q)
                                          (2 / (l * g_c))
                                          * H * fe_values_damage.JxW (q_index)); // dx*/

                  cell_rhs_damage (i) += fe_values_damage.shape_value (i,  q_index)// phi_i(x_q)
                                  *2* H * fe_values_damage.JxW (q_index); // dx
                }
            }


           const double sum_Gc = GC + GC/beta;
                    const double weakening_factor = -0.867;
                    for (const auto &face : cell->face_iterators())
                      {
                        const auto center = face->center ();
                        if ((std::fabs(0.8*center(0)-center(1)+1)/std::sqrt(0.8*0.8 + 1))<=1e-4) // check the distance of the point from interface line

                          {
                            fe_face_values_damage.reinit(cell, face);
                            for (const auto face_q_point : fe_face_values_damage.quadrature_point_indices())
                              for (const auto i : fe_face_values_damage.dof_indices())
                                for (const auto j : fe_face_values_damage.dof_indices())
                                  {
                                    cell_matrix_damage(i,j) +=

                                        0.5*fe_face_values_damage.shape_value( i, face_q_point)
                                        *fe_face_values_damage.shape_value( j, face_q_point)
                                        *(sum_Gc*weakening_factor)
                                        *fe_face_values_damage.JxW(face_q_point);

                                  }

                          }
                      }

          cell->get_dof_indices (local_dof_indices);
          constraints_damage.distribute_local_to_global (cell_matrix_damage,
              cell_rhs_damage, local_dof_indices, system_matrix_damage,
              system_rhs_damage);
        }

    system_matrix_damage.compress (VectorOperation::add);
    system_rhs_damage.compress (VectorOperation::add);
  }

  void
  PhaseField::solve_linear_system_damage ()
  {
    TimerOutput::Scope ts (computing_timer, "solve_linear_system_damage");

    SolverControl solver_control (20000,
        1e-12/** system_rhs_damage.l2_norm()*/);

    SolverCG < TrilinosWrappers::MPI::Vector > solver (solver_control);

    LA::MPI::PreconditionAMG::AdditionalData data;
#ifdef USE_PETSC_LA
        data.symmetric_operator = true;
#else
    // Trilinos defaults are good
#endif
    LA::MPI::PreconditionAMG preconditioner;
    preconditioner.initialize (system_matrix_damage, data);

    solver.solve (system_matrix_damage, completely_distributed_solution_damage,
        system_rhs_damage, preconditioner);

    pcout << "   Solved in " << solver_control.last_step () << " iterations."
    << std::endl;

    constraints_damage.distribute (completely_distributed_solution_damage);
    locally_relevant_solution_damage = completely_distributed_solution_damage;
  }

  void
  PhaseField::solve_elastic_subproblem (const unsigned int load_step)
  {
    setup_system_elastic (load_step);
    assemble_system_elastic ();
    solve_linear_system_elastic ();
  }

  void
  PhaseField::output_results (const unsigned int load_step) const

  {
    std::vector < std::string > displacement_names (2, "displacement");
    std::vector < DataComponentInterpretation::DataComponentInterpretation > displacement_component_interpretation (
        2, DataComponentInterpretation::component_is_part_of_vector);

    DataOut <2> data_out_phasefield;
    data_out_phasefield.add_data_vector (dof_handler_elastic,
        locally_relevant_solution_elastic, displacement_names,
        displacement_component_interpretation);
    data_out_phasefield.add_data_vector (dof_handler_damage,
        locally_relevant_solution_damage, "damage");

    Vector<double> subdomain (triangulation.n_active_cells ());
    for (unsigned int i = 0; i < subdomain.size (); ++i)
      subdomain (i) = triangulation.locally_owned_subdomain ();
    data_out_phasefield.add_data_vector (subdomain, "subdomain");
    data_out_phasefield.build_patches ();
    data_out_phasefield.write_vtu_with_pvtu_record ("./", "solution", load_step,
        mpi_communicator, 2, 0);
  }

  void
  PhaseField::load_disp_calculation (const unsigned int load_step)

  {
    // TODO: You mark these vectors as 'static' because you want them to carry state from one call to this function to the next. That is exactly what member variables are there for -- so make these vectors member variables.
    static Vector<double> load_values_y (num_load_steps + 1);
    static Vector<double> displacement_values (num_load_steps + 1);

    Tensor < 1, 2 > y_max_force; //force vector on the y_max face

    const QGauss < 1 > face_quadrature (fe_elastic.degree);
    FEFaceValues <2> fe_face_values (fe_elastic, face_quadrature,
        update_gradients | update_quadrature_points| update_normal_vectors);
    std::vector<SymmetricTensor<2, 2>> strain_values (face_quadrature.size ());
    const FEValuesExtractors::Vector displacements (0);

    FEValues <2> fe_values_elastic (fe_elastic, quadrature_formula_elastic,
        update_values | update_gradients | update_quadrature_points
        | update_JxW_values );
    for (const auto &cell : dof_handler_elastic.active_cell_iterators ())
      if (cell->is_locally_owned ())
        for (unsigned int f : cell->face_indices ())
          if (cell->face (f)->at_boundary () && (cell->face (f)->boundary_id ()
              == 2))
            {
              fe_face_values.reinit (cell, f);
              fe_values_elastic.reinit (cell);
              fe_face_values[displacements].get_function_symmetric_gradients (
                  locally_relevant_solution_elastic, strain_values);
              for (unsigned int q = 0; q < fe_face_values.n_quadrature_points;
                  ++q)

                {
                  const auto &x_q = fe_face_values.quadrature_point (q);
                  const Tensor<2, 2> strain = strain_values[q]; //strain tensor at a gauss point
                  double tr_strain = strain[0][0] + strain[1][1];
                  double d = damage_gauss_pt (locally_relevant_solution_damage,
                      cell, q, fe_values_elastic);

                  Tensor < 2, 2 > stress;
                  stress[0][0] = pow ((1 - d), 2)
                      * (lambda (E, nu, beta, x_q) * tr_strain + 2 * mu (E, nu, beta, x_q)
                                                      * strain[0][0]);
                  stress[0][1] = pow ((1 - d), 2)
                      * (2 * mu (E, nu, beta, x_q) * strain[0][1]);

                  stress[1][1] = pow ((1 - d), 2)
                      * (lambda (E, nu, beta, x_q) * tr_strain + 2 * mu (E, nu, beta, x_q)
                                                      * strain[1][1]);

                  const Tensor<1, 2> force_density = stress
                      * fe_face_values.normal_vector (q);
                  y_max_force += force_density * fe_face_values.JxW (q);
                }
            }


    double y_max_force_y;
    y_max_force_y = y_max_force[1];
    y_max_force_y = Utilities::MPI::sum (y_max_force_y, mpi_communicator);


    if (Utilities::MPI::this_mpi_process (mpi_communicator) == 0)
      {
        // Code to be executed only on process 0
        load_values_y[load_step] = y_max_force_y;
          {
            const double disp = uy * load_step;
            displacement_values[load_step] = disp;
          }
        //load displacement plot
        std::ofstream m_file;
        m_file.open ("load_displacement.m");
        m_file << "% Matlab code generated by dealii to plot load displacement curves"
        << std::endl;
        m_file << "clc" << std::endl;
        m_file << "clear" << std::endl;
        m_file << "close all" << std::endl;
        m_file << "load_y=[" << load_values_y << "]" << std::endl;
        m_file << "displacement=[" << displacement_values << "]" << std::endl;
        m_file << "plot( displacement,load_y,'linewidth',2)" << std::endl;
        m_file << "xlabel(\"Displacement (mm)\",'Interpreter', 'latex')"
        << std::endl;
        m_file << "ylabel(\"Reaction force (kN)\",'Interpreter', 'latex')"
        << std::endl;
        m_file << "set(gca,'fontname', 'Courier','FontSize',15,'FontWeight','bold','linewidth',1); grid on"
        << std::endl;
        m_file << "set(gcf,'Position', [250 250 700 500])" << std::endl;
        m_file << "xlim([0,0.025])" << std::endl;
        m_file << "box on" << std::endl;
        m_file.close ();
      }
  }

  void
  PhaseField::solve_damage_subproblem ()
  {
    setup_boundary_values_damage ();
    setup_system_damage ();
    assemble_system_damage ();
    solve_linear_system_damage ();
  }

 void
  PhaseField::refine_grid (const unsigned int load_step)
  {
    FEValues <2> fe_values_damage (fe_damage, quadrature_formula_damage,
        update_values | update_gradients | update_JxW_values
        | update_quadrature_points);

    parallel::distributed::ContinuousQuadratureDataTransfer<2, MyQData> data_transfer (
        fe_damage, quadrature_formula_damage, quadrature_formula_damage);

    //The output is a vector of values for all active cells. While it may make sense to compute the value of a solution degree of freedom very accurately,
    //it is usually not necessary to compute the error indicator corresponding to the solution on a cell particularly accurately.
    //We therefore typically use a vector of floats instead of a vector of doubles to represent error indicators.
    Vector<float> estimated_error_per_cell (
        triangulation.n_locally_owned_active_cells ());

    KellyErrorEstimator <2> ::estimate (dof_handler_damage,
        QGauss < 1 > (fe_damage.degree + 1),
          { }, locally_relevant_solution_damage, estimated_error_per_cell);

    // Initialize SolutionTransfer object
    parallel::distributed::SolutionTransfer < 2, LA::MPI::Vector > soltransDamage (dof_handler_damage);

    // Initialize SolutionTransfer object
    parallel::distributed::SolutionTransfer < 2, LA::MPI::Vector > soltransElastic (dof_handler_elastic);

    parallel::distributed::GridRefinement::refine_and_coarsen_fixed_fraction (
        triangulation, estimated_error_per_cell, 0.5, // top 1% cells marked for refinement
        0.0); // bottom 0 % cells marked for coarsening

    //if (triangulation.n_global_levels () >= 5)
          {
            //for (const auto &cell : triangulation.active_cell_iterators_on_level(4))
          for (const auto &cell : triangulation.active_cell_iterators())
          if (cell->is_locally_owned ())
                {
                  double max_length = cell->diameter();
                  const auto center = cell->center ();

                  if (max_length <= l/3)
                  cell->clear_refine_flag ();
                }
          }

    // prepare the triangulation,
    triangulation.prepare_coarsening_and_refinement ();

    // prepare CellDataStorage object for refinement
    data_transfer.prepare_for_coarsening_and_refinement (triangulation,
        quadrature_point_history_field);

    // prepare the SolutionTransfer object for coarsening and refinement
    // and give the solution vector that we intend to interpolate later,
    soltransDamage.prepare_for_coarsening_and_refinement (
        locally_relevant_solution_damage);
    soltransElastic.prepare_for_coarsening_and_refinement (
        locally_relevant_solution_elastic);

    triangulation.execute_coarsening_and_refinement ();

    // redistribute dofs,
    dof_handler_damage.distribute_dofs (fe_damage);
    dof_handler_elastic.distribute_dofs (fe_elastic);

    // Recreate locally_owned_dofs and locally_relevant_dofs index sets
    locally_owned_dofs_damage = dof_handler_damage.locally_owned_dofs ();
    locally_relevant_dofs_damage = DoFTools::extract_locally_relevant_dofs (
        dof_handler_damage);

    completely_distributed_solution_damage_old.reinit (
        locally_owned_dofs_damage, mpi_communicator);
    soltransDamage.interpolate (completely_distributed_solution_damage_old);

    // Apply constraints on the interpolated solution to make sure it conforms with the new mesh
    setup_boundary_values_damage ();

    constraints_damage.distribute (completely_distributed_solution_damage_old);

    // Copy completely_distributed_solution_damage_old to locally_relevant_solution_damage
    locally_relevant_solution_damage.reinit (locally_owned_dofs_damage,
        locally_relevant_dofs_damage, mpi_communicator);
    locally_relevant_solution_damage =
        completely_distributed_solution_damage_old;

    // Interpolating elastic solution similarly
    locally_owned_dofs_elastic = dof_handler_elastic.locally_owned_dofs ();
    locally_relevant_dofs_elastic = DoFTools::extract_locally_relevant_dofs (
        dof_handler_elastic);

    completely_distributed_solution_elastic_old.reinit (
        locally_owned_dofs_elastic, mpi_communicator);
    soltransElastic.interpolate (completely_distributed_solution_elastic_old);

    // Apply constraints on the interpolated solution to make sure it conforms with the new mesh
    setup_constraints_elastic (load_step);
    constraints_elastic.distribute (
        completely_distributed_solution_elastic_old);

    // Copy completely_distributed_solution_damage_old to locally_relevant_solution_damage
    locally_relevant_solution_elastic.reinit (locally_owned_dofs_elastic,
        locally_relevant_dofs_elastic, mpi_communicator);
    locally_relevant_solution_elastic =
        completely_distributed_solution_elastic_old;

    for (const auto &cell : triangulation.active_cell_iterators ())
      {
        if (cell->is_locally_owned ())
          quadrature_point_history_field.initialize (cell, 4);
      }
    data_transfer.interpolate ();

  }

  bool
  PhaseField::check_convergence ()
  {
    LA::MPI::Vector solution_damage_difference (locally_owned_dofs_damage,
        mpi_communicator);
    LA::MPI::Vector solution_elastic_difference (locally_owned_dofs_elastic,
        mpi_communicator);
    LA::MPI::Vector solution_damage_difference_ghost (locally_owned_dofs_damage,
        locally_relevant_dofs_damage, mpi_communicator);
    LA::MPI::Vector solution_elastic_difference_ghost (
        locally_owned_dofs_elastic, locally_relevant_dofs_elastic,
        mpi_communicator);

    solution_damage_difference = locally_relevant_solution_damage;

    solution_damage_difference -= completely_distributed_solution_damage_old;

    solution_elastic_difference = locally_relevant_solution_elastic;

    solution_elastic_difference -= completely_distributed_solution_elastic_old;

    solution_damage_difference_ghost = solution_damage_difference;

    solution_elastic_difference_ghost = solution_elastic_difference;

    double error_elastic_solution_numerator, error_elastic_solution_denominator,
        error_damage_solution_numerator, error_damage_solution_denominator;

    error_damage_solution_numerator = solution_damage_difference.l2_norm ();
    error_elastic_solution_numerator = solution_elastic_difference.l2_norm ();
    error_damage_solution_denominator =
        completely_distributed_solution_damage.l2_norm ();
    error_elastic_solution_denominator =
        completely_distributed_solution_elastic.l2_norm ();

    double error_elastic_solution, error_damage_solution;
    error_damage_solution = error_damage_solution_numerator
        / error_damage_solution_denominator;
    error_elastic_solution = error_elastic_solution_numerator
        / error_elastic_solution_denominator;

    completely_distributed_solution_elastic_old =
        locally_relevant_solution_elastic;
    completely_distributed_solution_damage_old =
        locally_relevant_solution_damage;

    if ((error_elastic_solution < tol) && (error_damage_solution < tol))
      return true;
    else
      return false;
  }

  void
  PhaseField::update_history_field ()
  {
    FEValues <2> fe_values_damage (fe_damage, quadrature_formula_damage,
        update_values | update_gradients | update_JxW_values
        | update_quadrature_points);

    for (const auto &cell : dof_handler_damage.active_cell_iterators ())
      if (cell->is_locally_owned ())
        {
          const std::vector<std::shared_ptr<MyQData>> lqph =
              quadrature_point_history_field.get_data (cell);
          for (unsigned int q_index = 0;
              q_index < quadrature_formula_damage.size (); ++q_index)
            lqph[q_index]->value_H = lqph[q_index]->value_H_new;
        }
  }

  void
  PhaseField::run ()
  {
    Timer timer;
    timer.start ();
    pcout << "Running with "
#ifdef USE_PETSC_LA
          << "PETSc"
#else
          << "Trilinos"
#endif
          << " on "
          << Utilities::MPI::n_mpi_processes (mpi_communicator) << " MPI rank(s)..."
          << std::endl;

    setup_mesh_and_bcs ();
    unsigned int prerefinement_cycle = 0;
    const unsigned int num_prerefinement_cycles = 20;
    bool prerefinement;
    prerefinement=true;
    start:
    prerefinement_cycle +=1;
    setup_dofs_and_initial_fields();

    // Create the pre-crack in the domain
    LA::MPI::Vector initial_soln_damage (locally_owned_dofs_damage,
        mpi_communicator);
    for (const auto &cell : dof_handler_damage.active_cell_iterators ())
      if (cell->is_locally_owned ())
        for (const auto vertex_number : cell->vertex_indices ())
          {
            const auto vert = cell->vertex(vertex_number);
            const types::global_dof_index vertex_dof_index =
                cell->vertex_dof_index (vertex_number, 0);

              if (
                  (std::fabs(vert(1)-0.5*(y_max+y_min))<1e-4)
                  &&(vert(0) - crack_length <1e-4)
                  )
              initial_soln_damage[vertex_dof_index] = 1;
            else
              initial_soln_damage[vertex_dof_index] = 0;
          }

    initial_soln_damage.compress (VectorOperation::insert);
    locally_relevant_solution_damage = initial_soln_damage;
    output_results(0);
    // Loop over load steps
    for (unsigned int load_step = 1; load_step <= num_load_steps; load_step++)
      {
        pcout << " \n \n load increment number : " << load_step << std::endl;

        // Loop over staggered iterations
        unsigned int iteration = 0;
        bool stoppingCriterion = false;
        while (stoppingCriterion == false)
          {
            iteration = iteration + 1;
            pcout << " \n iteration number:" << iteration << std::endl;
            solve_elastic_subproblem (load_step);
            solve_damage_subproblem ();

            locally_relevant_solution_damage.update_ghost_values ();
            locally_relevant_solution_elastic.update_ghost_values ();

            if (iteration == 1)
              {
                completely_distributed_solution_damage_old =
                    locally_relevant_solution_damage;
                completely_distributed_solution_elastic_old =
                    locally_relevant_solution_elastic;
              }
            else
              stoppingCriterion = check_convergence ();

            if (stoppingCriterion == false)
              {
                refine_grid (load_step);
                if (prerefinement==true)
                  {
                    if ( prerefinement_cycle > num_prerefinement_cycles)
                        prerefinement=false;
                    goto start;
                  }
              }
            else
              break;
          }

        // Once converged, do some clean-up operations
        if ((load_step == 1) || (load_step >= 1 && load_step <= num_load_steps
                                 && std::fabs (load_step % 50) < 1e-6))
          {
            TimerOutput::Scope ts (computing_timer, "output");
            output_results (load_step);
          }

        load_disp_calculation (load_step);

        computing_timer.print_summary ();
        computing_timer.reset ();
        pcout << std::endl;

        update_history_field ();
      }

    timer.stop ();
    pcout << "Total run time: " << timer.wall_time () << " seconds."
    << std::endl;
  }
}

int
main (int argc,
      char *argv[])
{
  try
    {
      using namespace dealii;
      using namespace Step886;

      Utilities::MPI::MPI_InitFinalize mpi_initialization (argc, argv, 1);

      PhaseField phasefield;
      phasefield.run ();
    }
  catch (std::exception &exc)
    {
      std::cerr << std::endl << std::endl
      << "----------------------------------------------------" << std::endl;
      std::cerr << "Exception on processing: " << std::endl << exc.what ()
      << std::endl << "Aborting!" << std::endl
      << "----------------------------------------------------" << std::endl;

      return 1;
    }
  catch (...)
    {
      std::cerr << std::endl << std::endl
      << "----------------------------------------------------" << std::endl;
      std::cerr << "Unknown exception!" << std::endl << "Aborting!" << std::endl
      << "----------------------------------------------------" << std::endl;
      return 1;
    }

  return 0;

}

   
