// Copyright (c) 2002-2014, Boyce Griffith
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of The University of North Carolina nor the names of
//      its contributors may be used to endorse or promote products derived from
//      this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// Config files
#include <IBAMR_config.h>
#include <IBTK_config.h>
#include <SAMRAI_config.h>

// Headers for basic PETSc functions
#include <petscsys.h>

// Headers for basic SAMRAI objects
#include <BergerRigoutsos.h>
#include <CartesianGridGeometry.h>
#include <ChopAndPackLoadBalancer.h>
#include <StandardTagAndInitialize.h>

// Headers for basic libMesh objects
#include <libmesh/boundary_info.h>
#include <libmesh/boundary_mesh.h>
#include <libmesh/equation_systems.h>
#include <libmesh/exodusII_io.h>
#include <libmesh/mesh.h>
#include <libmesh/mesh_function.h>
#include <libmesh/mesh_generation.h>

// Headers for application-specific algorithm/data structure objects
#include <boost/multi_array.hpp>
#include <ibamr/IBExplicitHierarchyIntegrator.h>
#include <ibamr/IBFEMethod.h>
#include <ibamr/INSCollocatedHierarchyIntegrator.h>
#include <ibamr/INSStaggeredHierarchyIntegrator.h>
#include <ibamr/app_namespaces.h>
#include <ibtk/AppInitializer.h>
#include <ibtk/LEInteractor.h>
#include <ibtk/libmesh_utilities.h>
#include <ibtk/muParserCartGridFunction.h>
#include <ibtk/muParserRobinBcCoefs.h>

inline double kernel(double x)
{
    x += 4.;
    const double x2 = x * x;
    const double x3 = x * x2;
    const double x4 = x * x3;
    const double x5 = x * x4;
    const double x6 = x * x5;
    const double x7 = x * x6;
    if (x <= 0.)
        return 0.;
    else if (x <= 1.)
        return .1984126984126984e-3 * x7;
    else if (x <= 2.)
        return .1111111111111111e-1 * x6 - .1388888888888889e-2 * x7 - .3333333333333333e-1 * x5 +
               .5555555555555556e-1 * x4 - .5555555555555556e-1 * x3 + .3333333333333333e-1 * x2 -
               .1111111111111111e-1 * x + .1587301587301587e-2;
    else if (x <= 3.)
        return .4333333333333333 * x5 - .6666666666666667e-1 * x6 + .4166666666666667e-2 * x7 - 1.500000000000000 * x4 +
               3.055555555555556 * x3 - 3.700000000000000 * x2 + 2.477777777777778 * x - .7095238095238095;
    else if (x <= 4.)
        return 9. * x4 - 1.666666666666667 * x5 + .1666666666666667 * x6 - .6944444444444444e-2 * x7 -
               28.44444444444444 * x3 + 53. * x2 - 54.22222222222222 * x + 23.59047619047619;
    else if (x <= 5.)
        return 96. * x3 - 22.11111111111111 * x4 + 3. * x5 - .2222222222222222 * x6 + .6944444444444444e-2 * x7 -
               245.6666666666667 * x2 + 344. * x - 203.9650793650794;
    else if (x <= 6.)
        return 483.5000000000000 * x2 - 147.0555555555556 * x3 + 26.50000000000000 * x4 - 2.833333333333333 * x5 +
               .1666666666666667 * x6 - .4166666666666667e-2 * x7 - 871.2777777777778 * x + 664.0904761904762;
    else if (x <= 7.)
        return 943.1222222222222 * x - 423.7000000000000 * x2 + 104.9444444444444 * x3 - 15.50000000000000 * x4 +
               1.366666666666667 * x5 - .6666666666666667e-1 * x6 + .1388888888888889e-2 * x7 - 891.1095238095238;
    else if (x <= 8.)
        return 416.1015873015873 - 364.0888888888889 * x + 136.5333333333333 * x2 - 28.44444444444444 * x3 +
               3.555555555555556 * x4 - .2666666666666667 * x5 + .1111111111111111e-1 * x6 - .1984126984126984e-3 * x7;
    else
        return 0.;
}

// Elasticity model data.
namespace ModelData
{
// Tether (penalty) force function.
static double kappa_s = 1.0e6;
static double eta_s = 0.0;
MeshFunction* U_fcn;
void tether_force_function(VectorValue<double>& F,
                           const TensorValue<double>& /*FF*/,
                           const libMesh::Point& X,
                           const libMesh::Point& s,
                           Elem* const /*elem*/,
                           const vector<NumericVector<double>*>& /*system_data*/,
                           double /*time*/,
                           void* /*ctx*/)
{
    DenseVector<double> U(NDIM);
    (*U_fcn)(s, 0.0, U);
    for (unsigned int d = 0; d < NDIM; ++d)
    {
        F(d) = kappa_s * (s(d) - X(d)) - eta_s * U(d);
    }
    return;
}
}
using namespace ModelData;

// Function prototypes
static ofstream drag_stream, lift_stream, U_L1_norm_stream, U_L2_norm_stream, U_max_norm_stream;
void postprocess_data(const boost::shared_ptr<PatchHierarchy >& patch_hierarchy,
                      const boost::shared_ptr<INSHierarchyIntegrator>& navier_stokes_integrator,
                      Mesh& mesh,
                      EquationSystems* equation_systems,
                      const int iteration_num,
                      const double loop_time,
                      const string& data_dump_dirname);

/*******************************************************************************
 * For each run, the input filename and restart information (if needed) must   *
 * be given on the command line.  For non-restarted case, command line is:     *
 *                                                                             *
 *    executable <input file name>                                             *
 *                                                                             *
 * For restarted run, command line is:                                         *
 *                                                                             *
 *    executable <input file name> <restart directory> <restart number>        *
 *                                                                             *
 *******************************************************************************/
int main(int argc, char* argv[])
{
    // Initialize libMesh, PETSc, MPI, and SAMRAI.
    LibMeshInit init(argc, argv);
    SAMRAI_MPI::setCommunicator(PETSC_COMM_WORLD);
    SAMRAI_MPI::setCallAbortInSerialInsteadOfExit();
    SAMRAIManager::startup();

    { // cleanup dynamically allocated objects prior to shutdown

        // Parse command line options, set some standard options from the input
        // file, initialize the restart database (if this is a restarted run),
        // and enable file logging.
        auto app_initializer = boost::make_shared<AppInitializer>(argc, argv, "IB.log");
        auto input_db = app_initializer->getInputDatabase();

        // Setup user-defined kernel function.
        LEInteractor::s_kernel_fcn = &kernel;
        LEInteractor::s_kernel_fcn_stencil_size = 8;

        // Get various standard options set in the input file.
        const bool dump_viz_data = app_initializer->dumpVizData();
        const int viz_dump_interval = app_initializer->getVizDumpInterval();
        const bool uses_visit = dump_viz_data && app_initializer->getVisItDataWriter();
        const bool uses_exodus = dump_viz_data && !app_initializer->getExodusIIFilename().empty();
        const string exodus_filename = app_initializer->getExodusIIFilename();

        const bool dump_restart_data = app_initializer->dumpRestartData();
        const int restart_dump_interval = app_initializer->getRestartDumpInterval();
        const string restart_dump_dirname = app_initializer->getRestartDumpDirectory();

        const bool dump_postproc_data = app_initializer->dumpPostProcessingData();
        const int postproc_data_dump_interval = app_initializer->getPostProcessingDataDumpInterval();
        const string postproc_data_dump_dirname = app_initializer->getPostProcessingDataDumpDirectory();
        if (dump_postproc_data && (postproc_data_dump_interval > 0) && !postproc_data_dump_dirname.empty())
        {
            Utilities::recursiveMkdir(postproc_data_dump_dirname);
        }

        const bool dump_timer_data = app_initializer->dumpTimerData();
        const int timer_dump_interval = app_initializer->getTimerDumpInterval();

        // Create a simple FE mesh.
        Mesh solid_mesh(NDIM);
        const double dx = input_db->getDouble("DX");
        const double ds = input_db->getDouble("MFAC") * dx;
        string elem_type = input_db->getString("ELEM_TYPE");
        const double R = 0.5;
        if (NDIM == 2 && (elem_type == "TRI3" || elem_type == "TRI6"))
        {
            const int num_circum_nodes = ceil(2.0 * M_PI * R / ds);
            for (int k = 0; k < num_circum_nodes; ++k)
            {
                const double theta = 2.0 * M_PI * static_cast<double>(k) / static_cast<double>(num_circum_nodes);
                solid_mesh.add_point(libMesh::Point(R * cos(theta), R * sin(theta)));
            }
            TriangleInterface triangle(solid_mesh);
            triangle.triangulation_type() = TriangleInterface::GENERATE_CONVEX_HULL;
            triangle.elem_type() = Utility::string_to_enum<ElemType>(elem_type);
            triangle.desired_area() = 1.5 * sqrt(3.0) / 4.0 * ds * ds;
            triangle.insert_extra_points() = true;
            triangle.smooth_after_generating() = true;
            triangle.triangulate();
        }
        else
        {
            // NOTE: number of segments along boundary is 4*2^r.
            const double num_circum_segments = 2.0 * M_PI * R / ds;
            const int r = log2(0.25 * num_circum_segments);
            MeshTools::Generation::build_sphere(solid_mesh, R, r, Utility::string_to_enum<ElemType>(elem_type));
        }

        // Ensure nodes on the surface are on the analytic boundary.
        MeshBase::element_iterator el_end = solid_mesh.elements_end();
        for (auto el = solid_mesh.elements_begin(); el != el_end; ++el)
        {
            Elem* const elem = *el;
            for (unsigned int side = 0; side < elem->n_sides(); ++side)
            {
                const bool at_mesh_bdry = !elem->neighbor(side);
                if (!at_mesh_bdry) continue;
                for (unsigned int k = 0; k < elem->n_nodes(); ++k)
                {
                    if (!elem->is_node_on_side(k, side)) continue;
                    Node& n = *elem->get_node(k);
                    n = R * n.unit();
                }
            }
        }
        solid_mesh.prepare_for_use();

        BoundaryMesh boundary_mesh(solid_mesh.comm(), solid_mesh.mesh_dimension() - 1);
        solid_mesh.boundary_info->sync(boundary_mesh);
        boundary_mesh.prepare_for_use();

        bool use_boundary_mesh = input_db->getBoolWithDefault("USE_BOUNDARY_MESH", false);
        Mesh& mesh = use_boundary_mesh ? boundary_mesh : solid_mesh;

        kappa_s = input_db->getDouble("KAPPA_S");
        eta_s = input_db->getDouble("ETA_S");

        // Create major algorithm and data objects that comprise the
        // application.  These objects are configured from the input database
        // and, if this is a restarted run, from the restart database.
        const boost::shared_ptr<INSHierarchyIntegrator>& navier_stokes_integrator;
        const string solver_type = app_initializer->getComponentDatabase("Main")->getString("solver_type");
        if (solver_type == "STAGGERED")
        {
            navier_stokes_integrator = boost::make_shared<INSStaggeredHierarchyIntegrator>(
                "INSStaggeredHierarchyIntegrator",
                app_initializer->getComponentDatabase("INSStaggeredHierarchyIntegrator"));
        }
        else if (solver_type == "COLLOCATED")
        {
            navier_stokes_integrator = boost::make_shared<INSCollocatedHierarchyIntegrator>(
                "INSCollocatedHierarchyIntegrator",
                app_initializer->getComponentDatabase("INSCollocatedHierarchyIntegrator"));
        }
        else
        {
            TBOX_ERROR("Unsupported solver type: " << solver_type << "\n"
                                                   << "Valid options are: COLLOCATED, STAGGERED");
        }
        auto ib_method_ops = boost::make_shared<IBFEMethod>("IBFEMethod",
                           app_initializer->getComponentDatabase("IBFEMethod"),
                           &mesh,
                           app_initializer->getComponentDatabase("GriddingAlgorithm")->getInteger("max_levels"));
        auto time_integrator = boost::make_shared<IBExplicitHierarchyIntegrator>("IBHierarchyIntegrator",
                                              app_initializer->getComponentDatabase("IBHierarchyIntegrator"),
                                              ib_method_ops,
                                              navier_stokes_integrator);
        auto grid_geometry = boost::make_shared<CartesianGridGeometry>(
            "CartesianGeometry", app_initializer->getComponentDatabase("CartesianGeometry"));
        auto patch_hierarchy = boost::make_shared<PatchHierarchy>("PatchHierarchy", grid_geometry);
        auto error_detector = boost::make_shared<StandardTagAndInitialize>("StandardTagAndInitialize",
                                               time_integrator,
                                               app_initializer->getComponentDatabase("StandardTagAndInitialize"));
        auto box_generator = boost::make_shared<BergerRigoutsos>();
        auto load_balancer = boost::make_shared<ChopAndPackLoadBalancer>("ChopAndPackLoadBalancer", app_initializer->getComponentDatabase("ChopAndPackLoadBalancer"));
        auto gridding_algorithm = boost::make_shared<GriddingAlgorithm>("GriddingAlgorithm",
                                        app_initializer->getComponentDatabase("GriddingAlgorithm"),
                                        error_detector,
                                        box_generator,
                                        load_balancer);

        // Configure the IBFE solver.
        ib_method_ops->registerLagBodyForceFunction(tether_force_function);
        EquationSystems* equation_systems = ib_method_ops->getFEDataManager()->getEquationSystems();

        // Create Eulerian initial condition specification objects.
        if (input_db->keyExists("VelocityInitialConditions"))
        {
            auto u_init = boost::make_shared<muParserCartGridFunction>(
                "u_init", app_initializer->getComponentDatabase("VelocityInitialConditions"), grid_geometry);
            navier_stokes_integrator->registerVelocityInitialConditions(u_init);
        }

        if (input_db->keyExists("PressureInitialConditions"))
        {
            auto p_init = boost::make_shared<muParserCartGridFunction>(
                "p_init", app_initializer->getComponentDatabase("PressureInitialConditions"), grid_geometry);
            navier_stokes_integrator->registerPressureInitialConditions(p_init);
        }

        // Create Eulerian boundary condition specification objects (when necessary).
        const IntVector& periodic_shift = grid_geometry->getPeriodicShift();
        vector<boost::shared_ptr<RobinBcCoefStrategy>> u_bc_coefs(NDIM);
        if (periodic_shift.min() > 0)
        {
            for (unsigned int d = 0; d < NDIM; ++d)
            {
                u_bc_coefs[d] = NULL;
            }
        }
        else
        {
            for (unsigned int d = 0; d < NDIM; ++d)
            {
                ostringstream bc_coefs_name_stream;
                bc_coefs_name_stream << "u_bc_coefs_" << d;
                const string bc_coefs_name = bc_coefs_name_stream.str();

                ostringstream bc_coefs_db_name_stream;
                bc_coefs_db_name_stream << "VelocityBcCoefs_" << d;
                const string bc_coefs_db_name = bc_coefs_db_name_stream.str();

                u_bc_coefs[d] = boost::make_shared<muParserRobinBcCoefs>(
                    bc_coefs_name, app_initializer->getComponentDatabase(bc_coefs_db_name), grid_geometry);
            }
            navier_stokes_integrator->registerPhysicalBoundaryConditions(u_bc_coefs);
        }

        // Create Eulerian body force function specification objects.
        if (input_db->keyExists("ForcingFunction"))
        {
            auto f_fcn = boost::make_shared<muParserCartGridFunction>(
                "f_fcn", app_initializer->getComponentDatabase("ForcingFunction"), grid_geometry);
            time_integrator->registerBodyForceFunction(f_fcn);
        }

        // Set up visualization plot file writers.
        auto visit_data_writer  = app_initializer->getVisItDataWriter();
        if (uses_visit)
        {
            time_integrator->registerVisItDataWriter(visit_data_writer);
        }
        AutoPtr<ExodusII_IO> exodus_io(uses_exodus ? new ExodusII_IO(mesh) : NULL);

        // Initialize hierarchy configuration and data on all patches.
        ib_method_ops->initializeFEData();
        time_integrator->initializePatchHierarchy(patch_hierarchy, gridding_algorithm);

        // Deallocate initialization objects.
        app_initializer.reset();

        // Print the input database contents to the log file.
        plog << "Input database:\n";
        input_db->printClassData(plog);

        // Write out initial visualization data.
        int iteration_num = time_integrator->getIntegratorStep();
        double loop_time = time_integrator->getIntegratorTime();
        if (dump_viz_data)
        {
            pout << "\n\nWriting visualization files...\n\n";
            if (uses_visit)
            {
                time_integrator->setupPlotData();
                visit_data_writer->writePlotData(patch_hierarchy, iteration_num, loop_time);
            }
            if (uses_exodus)
            {
                exodus_io->write_timestep(
                    exodus_filename, *equation_systems, iteration_num / viz_dump_interval + 1, loop_time);
            }
        }

        // Open streams to save lift and drag coefficients and the norms of the
        // velocity.
        if (SAMRAI_MPI::getRank() == 0)
        {
            drag_stream.open("C_D.curve", ios_base::out | ios_base::trunc);
            lift_stream.open("C_L.curve", ios_base::out | ios_base::trunc);
            U_L1_norm_stream.open("U_L1.curve", ios_base::out | ios_base::trunc);
            U_L2_norm_stream.open("U_L2.curve", ios_base::out | ios_base::trunc);
            U_max_norm_stream.open("U_max.curve", ios_base::out | ios_base::trunc);

            drag_stream.precision(10);
            lift_stream.precision(10);
            U_L1_norm_stream.precision(10);
            U_L2_norm_stream.precision(10);
            U_max_norm_stream.precision(10);
        }

        // Main time step loop.
        double loop_time_end = time_integrator->getEndTime();
        double dt = 0.0;
        while (!MathUtilities<double>::equalEps(loop_time, loop_time_end) && time_integrator->stepsRemaining())
        {
            iteration_num = time_integrator->getIntegratorStep();
            loop_time = time_integrator->getIntegratorTime();

            pout << "\n";
            pout << "+++++++++++++++++++++++++++++++++++++++++++++++++++\n";
            pout << "At beginning of timestep # " << iteration_num << "\n";
            pout << "Simulation time is " << loop_time << "\n";

            System& U_system = equation_systems->get_system<System>(IBFEMethod::VELOCITY_SYSTEM_NAME);
            AutoPtr<NumericVector<double> > U_vec = U_system.current_local_solution->clone();
            *U_vec = *U_system.solution;
            U_vec->close();
            DofMap& U_dof_map = U_system.get_dof_map();
            vector<unsigned int> vars(NDIM);
            for (unsigned int d = 0; d < NDIM; ++d)
            {
                vars[d] = d;
            }
            U_fcn = boost::make_shared<MeshFunction>(*equation_systems, *U_vec, U_dof_map, vars);
            U_fcn->init();

            dt = time_integrator->getMaximumTimeStepSize();
            time_integrator->advanceHierarchy(dt);
            loop_time += dt;

            delete U_fcn;

            pout << "\n";
            pout << "At end       of timestep # " << iteration_num << "\n";
            pout << "Simulation time is " << loop_time << "\n";
            pout << "+++++++++++++++++++++++++++++++++++++++++++++++++++\n";
            pout << "\n";

            // At specified intervals, write visualization and restart files,
            // print out timer data, and store hierarchy data for post
            // processing.
            iteration_num += 1;
            const bool last_step = !time_integrator->stepsRemaining();
            if (dump_viz_data && (iteration_num % viz_dump_interval == 0 || last_step))
            {
                pout << "\nWriting visualization files...\n\n";
                if (uses_visit)
                {
                    time_integrator->setupPlotData();
                    visit_data_writer->writePlotData(patch_hierarchy, iteration_num, loop_time);
                }
                if (uses_exodus)
                {
                    exodus_io->write_timestep(
                        exodus_filename, *equation_systems, iteration_num / viz_dump_interval + 1, loop_time);
                }
            }
            if (dump_restart_data && (iteration_num % restart_dump_interval == 0 || last_step))
            {
                pout << "\nWriting restart files...\n\n";
                RestartManager::getManager()->writeRestartFile(restart_dump_dirname, iteration_num);
            }
            if (dump_timer_data && (iteration_num % timer_dump_interval == 0 || last_step))
            {
                pout << "\nWriting timer data...\n\n";
                TimerManager::getManager()->print(plog);
            }
            if (dump_postproc_data && (iteration_num % postproc_data_dump_interval == 0 || last_step))
            {
                postprocess_data(patch_hierarchy,
                                 navier_stokes_integrator,
                                 mesh,
                                 equation_systems,
                                 iteration_num,
                                 loop_time,
                                 postproc_data_dump_dirname);
            }
        }

        // Close the logging streams.
        if (SAMRAI_MPI::getRank() == 0)
        {
            drag_stream.close();
            lift_stream.close();
            U_L1_norm_stream.close();
            U_L2_norm_stream.close();
            U_max_norm_stream.close();
        }

        // Cleanup Eulerian boundary condition specification objects (when
        // necessary).
        for (unsigned int d = 0; d < NDIM; ++d) delete u_bc_coefs[d];

    }

    SAMRAIManager::shutdown();
    return 0;
}

void postprocess_data(const boost::shared_ptr<PatchHierarchy >& /*patch_hierarchy*/,
                      const boost::shared_ptr<INSHierarchyIntegrator>& /*navier_stokes_integrator*/,
                      Mesh& mesh,
                      EquationSystems* equation_systems,
                      const int /*iteration_num*/,
                      const double loop_time,
                      const string& /*data_dump_dirname*/)
{
    const unsigned int dim = mesh.mesh_dimension();
    {
        double F_integral[NDIM];
        for (unsigned int d = 0; d < NDIM; ++d) F_integral[d] = 0.0;
        System& F_system = equation_systems->get_system<System>(IBFEMethod::FORCE_SYSTEM_NAME);
        NumericVector<double>* F_vec = F_system.solution.get();
        NumericVector<double>* F_ghost_vec = F_system.current_local_solution.get();
        F_vec->localize(*F_ghost_vec);
        DofMap& F_dof_map = F_system.get_dof_map();
        std::vector<std::vector<unsigned int> > F_dof_indices(NDIM);
        AutoPtr<FEBase> fe(FEBase::build(dim, F_dof_map.variable_type(0)));
        AutoPtr<QBase> qrule = QBase::build(QGAUSS, dim, FIFTH);
        fe->attach_quadrature_rule(qrule.get());
        const std::vector<std::vector<double> >& phi = fe->get_phi();
        const std::vector<double>& JxW = fe->get_JxW();
        boost::multi_array<double, 2> F_node;
        const MeshBase::const_element_iterator el_begin = mesh.active_local_elements_begin();
        const MeshBase::const_element_iterator el_end = mesh.active_local_elements_end();
        for (auto t !=  = el_begin; el_it != el_end; ++el_it)
        {
            Elem* const elem = *el_it;
            fe->reinit(elem);
            for (unsigned int d = 0; d < NDIM; ++d)
            {
                F_dof_map.dof_indices(elem, F_dof_indices[d], d);
            }
            const int n_qp = qrule->n_points();
            const int n_basis = F_dof_indices[0].size();
            get_values_for_interpolation(F_node, *F_ghost_vec, F_dof_indices);
            for (int qp = 0; qp < n_qp; ++qp)
            {
                for (int k = 0; k < n_basis; ++k)
                {
                    for (int d = 0; d < NDIM; ++d)
                    {
                        F_integral[d] += F_node[k][d] * phi[k][qp] * JxW[qp];
                    }
                }
            }
        }
        SAMRAI_MPI::sumReduction(F_integral, NDIM);
        static const double rho = 1.0;
        static const double U_max = 1.0;
        static const double D = 1.0;
        if (SAMRAI_MPI::getRank() == 0)
        {
            drag_stream << loop_time << " " << -F_integral[0] / (0.5 * rho * U_max * U_max * D) << endl;
            lift_stream << loop_time << " " << -F_integral[1] / (0.5 * rho * U_max * U_max * D) << endl;
        }
    }

    {
        double U_L1_norm = 0.0, U_L2_norm = 0.0, U_max_norm = 0.0;
        System& U_system = equation_systems->get_system<System>(IBFEMethod::VELOCITY_SYSTEM_NAME);
        NumericVector<double>* U_vec = U_system.solution.get();
        NumericVector<double>* U_ghost_vec = U_system.current_local_solution.get();
        U_vec->localize(*U_ghost_vec);
        DofMap& U_dof_map = U_system.get_dof_map();
        std::vector<std::vector<unsigned int> > U_dof_indices(NDIM);
        AutoPtr<FEBase> fe(FEBase::build(dim, U_dof_map.variable_type(0)));
        AutoPtr<QBase> qrule = QBase::build(QGAUSS, dim, FIFTH);
        fe->attach_quadrature_rule(qrule.get());
        const std::vector<std::vector<double> >& phi = fe->get_phi();
        const std::vector<double>& JxW = fe->get_JxW();
        VectorValue<double> U_qp;
        boost::multi_array<double, 2> U_node;
        const MeshBase::const_element_iterator el_begin = mesh.active_local_elements_begin();
        const MeshBase::const_element_iterator el_end = mesh.active_local_elements_end();
        for (auto el_it = el_begin; el_it != el_end; ++el_it)
        {
            Elem* const elem = *el_it;
            fe->reinit(elem);
            for (unsigned int d = 0; d < NDIM; ++d)
            {
                U_dof_map.dof_indices(elem, U_dof_indices[d], d);
            }
            const int n_qp = qrule->n_points();
            get_values_for_interpolation(U_node, *U_ghost_vec, U_dof_indices);
            for (int qp = 0; qp < n_qp; ++qp)
            {
                interpolate(U_qp, qp, U_node, phi);
                for (unsigned int d = 0; d < NDIM; ++d)
                {
                    U_L1_norm += std::abs(U_qp(d)) * JxW[qp];
                    U_L2_norm += U_qp(d) * U_qp(d) * JxW[qp];
                    U_max_norm = std::max(U_max_norm, std::abs(U_qp(d)));
                }
            }
        }
        SAMRAI_MPI::sumReduction(&U_L1_norm, 1);
        SAMRAI_MPI::sumReduction(&U_L2_norm, 1);
        SAMRAI_MPI::maxReduction(&U_max_norm, 1);
        U_L2_norm = sqrt(U_L2_norm);
        if (SAMRAI_MPI::getRank() == 0)
        {
            U_L1_norm_stream << loop_time << " " << U_L1_norm << endl;
            U_L2_norm_stream << loop_time << " " << U_L2_norm << endl;
            U_max_norm_stream << loop_time << " " << U_max_norm << endl;
        }
    }
    return;
}
