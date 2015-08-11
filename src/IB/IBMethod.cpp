// Filename: IBMethod.cpp
// Created on 21 Sep 2011 by Boyce Griffith
//
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

/////////////////////////////// INCLUDES /////////////////////////////////////

#include <stddef.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "SAMRAI/hier/PatchHierarchy.h"
#include "SAMRAI/hier/PatchLevel.h"
#include "SAMRAI/hier/Box.h"
#include "SAMRAI/hier/BoxContainer.h"
#include "SAMRAI/geom/CartesianGridGeometry.h"
#include "SAMRAI/geom/CartesianPatchGeometry.h"
#include "SAMRAI/pdat/CellData.h"
#include "SAMRAI/pdat/CellIndex.h"
#include "SAMRAI/mesh/GriddingAlgorithm.h"
#include "SAMRAI/math/HierarchyDataOpsReal.h"
#include "SAMRAI/hier/Index.h"
#include "SAMRAI/hier/IntVector.h"
#include "SAMRAI/mesh/ChopAndPackLoadBalancer.h"
#include "SAMRAI/hier/Patch.h"
#include "SAMRAI/math/PatchCellDataOpsReal.h"
#include "SAMRAI/hier/PatchHierarchy.h"
#include "SAMRAI/hier/PatchLevel.h"
#include "SAMRAI/xfer/RefineSchedule.h"
#include "SAMRAI/hier/Variable.h"
#include "SAMRAI/hier/VariableContext.h"
#include "SAMRAI/hier/VariableDatabase.h"
#include "boost/array.hpp"
#include "boost/multi_array.hpp"
#include "ibamr/IBAnchorPointSpec.h"
#include "ibamr/IBHierarchyIntegrator.h"
#include "ibamr/IBInstrumentPanel.h"
#include "ibamr/IBInstrumentationSpec.h"
#include "ibamr/IBLagrangianForceStrategy.h"
#include "ibamr/IBLagrangianSourceStrategy.h"
#include "ibamr/IBMethod.h"
#include "ibamr/IBMethodPostProcessStrategy.h"
#include "ibamr/namespaces.h" // IWYU pragma: keep
#include "ibtk/HierarchyMathOps.h"
#include "ibtk/IBTK_CHKERRQ.h"
#include "ibtk/IndexUtilities.h"
#include "ibtk/LData.h"
#include "ibtk/LDataManager.h"
#include "ibtk/LEInteractor.h"
#include "ibtk/LInitStrategy.h"
#include "ibtk/LMesh.h"
#include "ibtk/LNode.h"
#include "ibtk/LSiloDataWriter.h"
#include "ibtk/ibtk_utilities.h"
#include "petscmat.h"
#include "petscsys.h"
#include "petscvec.h"
#include "SAMRAI/tbox/Array.h"
#include "SAMRAI/tbox/Database.h"
#include "SAMRAI/tbox/MathUtilities.h"
#include "SAMRAI/tbox/PIO.h"

#include "SAMRAI/tbox/RestartManager.h"
#include "SAMRAI/tbox/SAMRAI_MPI.h"
#include "SAMRAI/tbox/Utilities.h"

namespace IBTK
{
class RobinPhysBdryPatchStrategy;
} // namespace IBTK

namespace SAMRAI
{
namespace xfer
{

class CoarsenSchedule;
} // namespace xfer
} // namespace SAMRAI

/////////////////////////////// NAMESPACE ////////////////////////////////////

namespace IBAMR
{
/////////////////////////////// STATIC ///////////////////////////////////////

namespace
{
inline double cos_kernel(const double x, const double eps)
{
    if (std::abs(x) > eps)
    {
        return 0.0;
    }
    else
    {
        return 0.5 * (1.0 + cos(M_PI * x / eps)) / eps;
    }
}

// Version of IBMethod restart file data.
static const int IB_METHOD_VERSION = 1;
}

/////////////////////////////// PUBLIC ///////////////////////////////////////

IBMethod::IBMethod(const std::string& object_name,
                   const boost::shared_ptr<Database>& input_db,
                   bool register_for_restart)
    : d_ghosts(DIM)
{
    // Set the object name and register it with the restart manager.
    d_object_name = object_name;
    d_registered_for_restart = false;
    if (register_for_restart)
    {
        RestartManager::getManager()->registerRestartItem(d_object_name, this);
        d_registered_for_restart = true;
    }

    // Ensure all pointers to helper objects are NULL.
    d_l_initializer = NULL;
    d_ib_force_fcn = NULL;
    d_ib_force_fcn_needs_init = true;
    d_ib_source_fcn = NULL;
    d_ib_source_fcn_needs_init = true;
    d_normalize_source_strength = false;
    d_post_processor = NULL;
    d_silo_writer = NULL;

    // Set some default values.
    d_interp_kernel_fcn = "IB_4";
    d_spread_kernel_fcn = "IB_4";
    d_ghosts = IntVector(DIM, std::max(LEInteractor::getMinimumGhostWidth(d_interp_kernel_fcn),
                                       LEInteractor::getMinimumGhostWidth(d_spread_kernel_fcn)));
    d_do_log = false;

    // Initialize object with data read from the input and restart databases.
    bool from_restart = RestartManager::getManager()->isFromRestart();
    if (from_restart) getFromRestart();
    if (input_db) getFromInput(input_db, from_restart);

    // Check the choices for the kernel function.
    if (d_interp_kernel_fcn != d_spread_kernel_fcn)
    {
        pout << "WARNING: different kernel functions are being used for velocity "
                "interpolation and "
                "force spreading.\n"
             << "         recommended usage is to employ the same kernel functions for both "
                "interpolation and spreading.\n";
    }

    // Get the Lagrangian Data Manager.
    d_l_data_manager = LDataManager::getManager(d_object_name + "::LDataManager", d_interp_kernel_fcn,
                                                d_spread_kernel_fcn, d_ghosts, d_registered_for_restart);
    d_ghosts = d_l_data_manager->getGhostCellWidth();

    // Create the instrument panel object.
    d_instrument_panel = boost::make_shared<IBInstrumentPanel>(
        d_object_name + "::IBInstrumentPanel",
        (input_db->isDatabase("IBInstrumentPanel") ? input_db->getDatabase("IBInstrumentPanel") : NULL));

    // Reset the current time step interval.
    d_current_time = std::numeric_limits<double>::quiet_NaN();
    d_new_time = std::numeric_limits<double>::quiet_NaN();
    d_half_time = std::numeric_limits<double>::quiet_NaN();

    // Indicate all Lagrangian data needs ghost values to be refilled, and that
    // all intermediate data needs to be initialized.
    d_X_current_needs_ghost_fill = true;
    d_X_new_needs_ghost_fill = true;
    d_X_half_needs_ghost_fill = true;
    d_X_LE_new_needs_ghost_fill = true;
    d_X_LE_half_needs_ghost_fill = true;
    d_F_current_needs_ghost_fill = true;
    d_F_new_needs_ghost_fill = true;
    d_F_half_needs_ghost_fill = true;
    d_X_half_needs_reinit = true;
    d_X_LE_half_needs_reinit = true;
    d_U_half_needs_reinit = true;

    // Indicate that the Jacobian matrix has not been allocated.
    d_force_jac = NULL;
    return;
}

IBMethod::~IBMethod()
{
    if (d_registered_for_restart)
    {
        RestartManager::getManager()->unregisterRestartItem(d_object_name);
        d_registered_for_restart = false;
    }
    if (d_force_jac)
    {
        PetscErrorCode ierr;
        ierr = MatDestroy(&d_force_jac);
        IBTK_CHKERRQ(ierr);
    }
    return;
}

void IBMethod::registerIBLagrangianForceFunction(const boost::shared_ptr<IBLagrangianForceStrategy>& ib_force_fcn)
{
    TBOX_ASSERT(ib_force_fcn);
    d_ib_force_fcn = ib_force_fcn;
    return;
}

void IBMethod::registerIBLagrangianSourceFunction(const boost::shared_ptr<IBLagrangianSourceStrategy>& ib_source_fcn)
{
    TBOX_ASSERT(ib_source_fcn);
    d_ib_source_fcn = ib_source_fcn;
    return;
}

void IBMethod::registerLInitStrategy(const boost::shared_ptr<LInitStrategy>& l_initializer)
{
    TBOX_ASSERT(l_initializer);
    d_l_initializer = l_initializer;
    d_l_data_manager->registerLInitStrategy(d_l_initializer);
    return;
}

void IBMethod::freeLInitStrategy()
{
    d_l_initializer.reset();
    d_l_data_manager->freeLInitStrategy();
    return;
}

void IBMethod::registerIBMethodPostProcessor(const boost::shared_ptr<IBMethodPostProcessStrategy>& post_processor)
{
    d_post_processor = post_processor;
    return;
}

LDataManager* IBMethod::getLDataManager() const
{
    return d_l_data_manager;
}

boost::shared_ptr<IBInstrumentPanel> IBMethod::getIBInstrumentPanel() const
{
    return d_instrument_panel;
}

void IBMethod::registerLSiloDataWriter(const boost::shared_ptr<LSiloDataWriter>& silo_writer)
{
    TBOX_ASSERT(silo_writer);
    d_silo_writer = silo_writer;
    d_l_data_manager->registerLSiloDataWriter(d_silo_writer);
    return;
}

const IntVector& IBMethod::getMinimumGhostCellWidth() const
{
    return d_ghosts;
}

void IBMethod::setupTagBuffer(std::vector<int>& tag_buffer, const boost::shared_ptr<PatchHierarchy>& hierarchy) const
{
    const int finest_hier_ln = hierarchy->getMaxNumberOfLevels() - 1;
    const auto tsize = tag_buffer.size();
    tag_buffer.resize(finest_hier_ln);
    for (auto i = tsize; i < finest_hier_ln; ++i) tag_buffer[i] = 0;
    const int gcw = d_ghosts.max();
    for (int tag_ln = 0; tag_ln < finest_hier_ln; ++tag_ln)
    {
        const int data_ln = tag_ln + 1;
        const int can_be_refined = data_ln < finest_hier_ln;
        if (!d_l_initializer->getLevelHasLagrangianData(data_ln, can_be_refined)) continue;
        tag_buffer[tag_ln] = std::max(tag_buffer[tag_ln], gcw);
    }
    for (int ln = finest_hier_ln - 2; ln >= 0; --ln)
    {
        tag_buffer[ln] =
            std::max(tag_buffer[ln], tag_buffer[ln + 1] / hierarchy->getRatioToCoarserLevel(ln + 1).max() + 1);
    }
    return;
}

void IBMethod::preprocessIntegrateData(double current_time, double new_time, int /*num_cycles*/)
{
    d_current_time = current_time;
    d_new_time = new_time;
    d_half_time = current_time + 0.5 * (new_time - current_time);

    int ierr;
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    const double start_time = d_ib_solver->getStartTime();
    const bool initial_time = MathUtilities<double>::equalEps(current_time, start_time);

    if (d_ib_force_fcn)
    {
        if (d_ib_force_fcn_needs_init)
        {
            resetLagrangianForceFunction(current_time, initial_time);
            d_ib_force_fcn_needs_init = false;
        }
        d_ib_force_fcn->setTimeInterval(current_time, new_time);
    }
    if (d_ib_source_fcn)
    {
        if (d_ib_source_fcn_needs_init)
        {
            resetLagrangianSourceFunction(current_time, initial_time);
            d_ib_source_fcn_needs_init = false;
        }
        d_ib_source_fcn->setTimeInterval(current_time, new_time);
    }

    // Look-up or allocate Lagangian data.
    d_X_current_data.resize(finest_ln + 1);
    d_X_new_data.resize(finest_ln + 1);
    d_X_half_data.resize(finest_ln + 1);
    d_X_jac_data.resize(finest_ln + 1);
    d_U_current_data.resize(finest_ln + 1);
    d_U_new_data.resize(finest_ln + 1);
    d_U_half_data.resize(finest_ln + 1);
    d_U_jac_data.resize(finest_ln + 1);
    d_F_current_data.resize(finest_ln + 1);
    d_F_new_data.resize(finest_ln + 1);
    d_F_half_data.resize(finest_ln + 1);
    d_F_jac_data.resize(finest_ln + 1);
    if (d_use_fixed_coupling_ops)
    {
        d_X_LE_new_data.resize(finest_ln + 1);
        d_X_LE_half_data.resize(finest_ln + 1);
    }
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        d_X_current_data[ln] = d_l_data_manager->getLData(LDataManager::POSN_DATA_NAME, ln);
        d_X_new_data[ln] = d_l_data_manager->createLData("X_new", ln, NDIM);
        d_U_current_data[ln] = d_l_data_manager->getLData(LDataManager::VEL_DATA_NAME, ln);
        d_U_new_data[ln] = d_l_data_manager->createLData("U_new", ln, NDIM);
        d_F_current_data[ln] = d_l_data_manager->getLData("F", ln);
        if (d_use_fixed_coupling_ops)
        {
            d_X_LE_new_data[ln] = d_l_data_manager->createLData("X_LE_new", ln, NDIM);
        }

        // Initialize X^{n+1} to equal X^{n}, and initialize U^{n+1} to equal
        // U^{n}.
        ierr = VecCopy(d_X_current_data[ln]->getVec(), d_X_new_data[ln]->getVec());
        IBTK_CHKERRQ(ierr);
        ierr = VecCopy(d_U_current_data[ln]->getVec(), d_U_new_data[ln]->getVec());
        IBTK_CHKERRQ(ierr);

        if (d_use_fixed_coupling_ops)
        {
            // Initialize X_LE^{n+1} to equal X^{n}.
            ierr = VecCopy(d_X_current_data[ln]->getVec(), d_X_LE_new_data[ln]->getVec());
            IBTK_CHKERRQ(ierr);
        }
    }

    // Keep track of Lagrangian data objects that need to have ghost values
    // filled, or that need to be reinitialized.
    d_X_new_needs_ghost_fill = true;
    d_X_LE_new_needs_ghost_fill = true;
    d_X_half_needs_reinit = true;
    d_X_LE_half_needs_reinit = true;
    d_U_half_needs_reinit = true;
    return;
}

void IBMethod::postprocessIntegrateData(double current_time, double new_time, int /*num_cycles*/)
{
    int ierr;
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    const double dt = new_time - current_time;
    const int integrator_step = d_ib_solver->getIntegratorStep();

    // Update the instrumentation data.
    updateIBInstrumentationData(integrator_step + 1, new_time);
    if (d_instrument_panel->isInstrumented())
    {
        const std::vector<std::string>& instrument_name = d_instrument_panel->getInstrumentNames();
        const std::vector<double>& flow_data = d_instrument_panel->getFlowValues();
        for (unsigned int m = 0; m < flow_data.size(); ++m)
        {
            // NOTE: Flow volume is calculated in default units.
            d_total_flow_volume[m] += flow_data[m] * dt;
            if (d_do_log)
                plog << "flow volume through " << instrument_name[m] << ":\t " << d_total_flow_volume[m] << "\n";
        }
    }

    // Reset time-dependent Lagrangian data.
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        ierr = VecSwap(d_X_current_data[ln]->getVec(), d_X_new_data[ln]->getVec());
        IBTK_CHKERRQ(ierr);
        ierr = VecSwap(d_U_current_data[ln]->getVec(), d_U_new_data[ln]->getVec());
        IBTK_CHKERRQ(ierr);
        if (d_F_new_data[ln])
        {
            ierr = VecSwap(d_F_current_data[ln]->getVec(), d_F_new_data[ln]->getVec());
            IBTK_CHKERRQ(ierr);
        }
        else if (d_F_half_data[ln])
        {
            ierr = VecSwap(d_F_current_data[ln]->getVec(), d_F_half_data[ln]->getVec());
            IBTK_CHKERRQ(ierr);
        }
    }
    d_X_current_needs_ghost_fill = true;
    d_F_current_needs_ghost_fill = true;

    // Deallocate Lagrangian scratch data.
    d_X_current_data.clear();
    d_X_new_data.clear();
    d_X_half_data.clear();
    d_X_jac_data.clear();
    d_X_LE_new_data.clear();
    d_X_LE_half_data.clear();
    d_U_current_data.clear();
    d_U_new_data.clear();
    d_U_half_data.clear();
    d_U_jac_data.clear();
    d_F_current_data.clear();
    d_F_new_data.clear();
    d_F_half_data.clear();
    d_F_jac_data.clear();

    // Reset the current time step interval.
    d_current_time = std::numeric_limits<double>::quiet_NaN();
    d_new_time = std::numeric_limits<double>::quiet_NaN();
    d_half_time = std::numeric_limits<double>::quiet_NaN();
    return;
}

void IBMethod::createSolverVecs(Vec& X_vec, Vec& F_vec)
{
    PetscErrorCode ierr;
    const int level_num = d_hierarchy->getFinestLevelNumber();
    ierr = VecDuplicate(d_X_new_data[level_num]->getVec(), &X_vec);
    IBTK_CHKERRQ(ierr);
    ierr = VecDuplicate(d_X_new_data[level_num]->getVec(), &F_vec);
    IBTK_CHKERRQ(ierr);
    return;
}

void IBMethod::setupSolverVecs(Vec& X_vec, Vec& F_vec)
{
    PetscErrorCode ierr;
    const int level_num = d_hierarchy->getFinestLevelNumber();
    ierr = VecCopy(d_X_new_data[level_num]->getVec(), X_vec);
    IBTK_CHKERRQ(ierr);
    ierr = VecZeroEntries(F_vec);
    IBTK_CHKERRQ(ierr);
    return;
}

void IBMethod::setUpdatedPosition(Vec& X_new_vec)
{
    PetscErrorCode ierr;
    const int level_num = d_hierarchy->getFinestLevelNumber();
    ierr = VecCopy(X_new_vec, d_X_new_data[level_num]->getVec());
    IBTK_CHKERRQ(ierr);
    d_X_new_needs_ghost_fill = true;
    d_X_half_needs_reinit = true;
    return;
}

void IBMethod::setLinearizedPosition(Vec& X_vec)
{
    PetscErrorCode ierr;
    const int level_num = d_hierarchy->getFinestLevelNumber();
    std::vector<boost::shared_ptr<LData>>* X_jac_data;
    bool* X_jac_needs_ghost_fill;
    getLinearizedPositionData(&X_jac_data, &X_jac_needs_ghost_fill);
    ierr = VecCopy(X_vec, (*X_jac_data)[level_num]->getVec());
    IBTK_CHKERRQ(ierr);
    *X_jac_needs_ghost_fill = true;
    if (!d_force_jac)
    {
        int n_local, n_global;
        ierr = VecGetLocalSize(X_vec, &n_local);
        IBTK_CHKERRQ(ierr);
        ierr = VecGetSize(X_vec, &n_global);
        IBTK_CHKERRQ(ierr);
        ierr = MatCreateMFFD(PETSC_COMM_WORLD, n_local, n_local, n_global, n_global, &d_force_jac);
        IBTK_CHKERRQ(ierr);
        ierr = MatMFFDSetFunction(d_force_jac, computeForce_SAMRAI, this);
        IBTK_CHKERRQ(ierr);
        ierr = MatSetOptionsPrefix(d_force_jac, "ib_");
        IBTK_CHKERRQ(ierr);
        ierr = MatSetFromOptions(d_force_jac);
        IBTK_CHKERRQ(ierr);
    }
    ierr = MatMFFDSetBase(d_force_jac, (*X_jac_data)[level_num]->getVec(), NULL);
    IBTK_CHKERRQ(ierr);
    ierr = MatAssemblyBegin(d_force_jac, MAT_FINAL_ASSEMBLY);
    IBTK_CHKERRQ(ierr);
    ierr = MatAssemblyEnd(d_force_jac, MAT_FINAL_ASSEMBLY);
    IBTK_CHKERRQ(ierr);
    return;
}

void IBMethod::computeResidual(Vec& R_vec)
{
    PetscErrorCode ierr;
    const int level_num = d_hierarchy->getFinestLevelNumber();
    const double dt = d_new_time - d_current_time;
    ierr = VecWAXPY(R_vec, -dt, d_U_half_data[level_num]->getVec(), d_X_new_data[level_num]->getVec());
    IBTK_CHKERRQ(ierr);
    ierr = VecAXPY(R_vec, -1.0, d_X_current_data[level_num]->getVec());
    IBTK_CHKERRQ(ierr);
    return;
}

void IBMethod::computeLinearizedResidual(Vec& X_vec, Vec& R_vec)
{
    PetscErrorCode ierr;
    const int level_num = d_hierarchy->getFinestLevelNumber();
    const double dt = d_new_time - d_current_time;
    ierr = VecWAXPY(R_vec, -dt, d_U_jac_data[level_num]->getVec(), X_vec);
    IBTK_CHKERRQ(ierr);
    return;
}

void IBMethod::updateFixedLEOperators()
{
    if (!d_use_fixed_coupling_ops) return;
    int ierr;
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        ierr = VecCopy(d_X_new_data[ln]->getVec(), d_X_LE_new_data[ln]->getVec());
        IBTK_CHKERRQ(ierr);
    }
    d_X_LE_new_needs_ghost_fill = true;
    d_X_LE_half_needs_reinit = true;
    return;
}

void IBMethod::interpolateVelocity(const int u_data_idx,
                                   const std::vector<boost::shared_ptr<CoarsenSchedule>>& u_synch_scheds,
                                   const std::vector<boost::shared_ptr<RefineSchedule>>& u_ghost_fill_scheds,
                                   const double data_time)
{
    std::vector<boost::shared_ptr<LData>> *U_data, *X_LE_data;
    bool* X_LE_needs_ghost_fill;
    getVelocityData(&U_data, data_time);
    getLECouplingPositionData(&X_LE_data, &X_LE_needs_ghost_fill, data_time);
    d_l_data_manager->interp(u_data_idx, *U_data, *X_LE_data, u_synch_scheds, u_ghost_fill_scheds, data_time);
    resetAnchorPointValues(*U_data,
                           /*coarsest_ln*/ 0,
                           /*finest_ln*/ d_hierarchy->getFinestLevelNumber());
    d_U_half_needs_reinit = !MathUtilities<double>::equalEps(data_time, d_half_time);
    return;
}

void IBMethod::interpolateLinearizedVelocity(const int u_data_idx,
                                             const std::vector<boost::shared_ptr<CoarsenSchedule>>& u_synch_scheds,
                                             const std::vector<boost::shared_ptr<RefineSchedule>>& u_ghost_fill_scheds,
                                             const double data_time)
{
    std::vector<boost::shared_ptr<LData>> *U_jac_data, *X_LE_data;
    bool* X_LE_needs_ghost_fill;
    getLinearizedVelocityData(&U_jac_data);
    getLECouplingPositionData(&X_LE_data, &X_LE_needs_ghost_fill, data_time);
    d_l_data_manager->interp(u_data_idx, *U_jac_data, *X_LE_data, u_synch_scheds, u_ghost_fill_scheds, data_time);
    resetAnchorPointValues(*U_jac_data,
                           /*coarsest_ln*/ 0,
                           /*finest_ln*/ d_hierarchy->getFinestLevelNumber());
    return;
}

void IBMethod::eulerStep(const double current_time, const double new_time)
{
    int ierr;
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    const double dt = new_time - current_time;
    std::vector<boost::shared_ptr<LData>>* U_data;
    getVelocityData(&U_data, current_time);
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        ierr = VecWAXPY(d_X_new_data[ln]->getVec(), dt, (*U_data)[ln]->getVec(), d_X_current_data[ln]->getVec());
        IBTK_CHKERRQ(ierr);
    }
    d_X_new_needs_ghost_fill = true;
    d_X_half_needs_reinit = true;
    return;
}

void IBMethod::midpointStep(const double current_time, const double new_time)
{
    int ierr;
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    const double dt = new_time - current_time;
    std::vector<boost::shared_ptr<LData>>* U_data;
    getVelocityData(&U_data, current_time + 0.5 * dt);
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        ierr = VecWAXPY(d_X_new_data[ln]->getVec(), dt, (*U_data)[ln]->getVec(), d_X_current_data[ln]->getVec());
        IBTK_CHKERRQ(ierr);
    }
    d_X_new_needs_ghost_fill = true;
    d_X_half_needs_reinit = true;
    return;
}

void IBMethod::trapezoidalStep(const double current_time, const double new_time)
{
    int ierr;
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    const double dt = new_time - current_time;
    std::vector<boost::shared_ptr<LData>> *U_current_data, *U_new_data;
    getVelocityData(&U_current_data, current_time);
    getVelocityData(&U_new_data, new_time);
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        ierr = VecWAXPY(d_X_new_data[ln]->getVec(), 0.5 * dt, (*U_current_data)[ln]->getVec(),
                        d_X_current_data[ln]->getVec());
        IBTK_CHKERRQ(ierr);
        ierr = VecAXPY(d_X_new_data[ln]->getVec(), 0.5 * dt, (*U_new_data)[ln]->getVec());
        IBTK_CHKERRQ(ierr);
    }
    d_X_new_needs_ghost_fill = true;
    d_X_half_needs_reinit = true;
    return;
}

bool IBMethod::hasFluidSources() const
{
    return d_ib_source_fcn != NULL;
}

void IBMethod::computeLagrangianForce(const double data_time)
{
    int ierr;
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    std::vector<boost::shared_ptr<LData>> *F_data, *X_data, *U_data;
    bool *F_needs_ghost_fill, *X_needs_ghost_fill;
    getForceData(&F_data, &F_needs_ghost_fill, data_time);
    getPositionData(&X_data, &X_needs_ghost_fill, data_time);
    getVelocityData(&U_data, data_time);
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        ierr = VecSet((*F_data)[ln]->getVec(), 0.0);
        IBTK_CHKERRQ(ierr);
        if (d_ib_force_fcn)
        {
            d_ib_force_fcn->computeLagrangianForce((*F_data)[ln], (*X_data)[ln], (*U_data)[ln], d_hierarchy, ln,
                                                   data_time, d_l_data_manager);
        }
    }
    *F_needs_ghost_fill = true;
    return;
}

void IBMethod::computeLinearizedLagrangianForce(Vec& X_vec, const double /*data_time*/)
{
    PetscErrorCode ierr;
    const int level_num = d_hierarchy->getFinestLevelNumber();
    std::vector<boost::shared_ptr<LData>>* F_jac_data;
    bool* F_jac_needs_ghost_fill;
    getLinearizedForceData(&F_jac_data, &F_jac_needs_ghost_fill);
    Vec F_vec = (*F_jac_data)[level_num]->getVec();
    ierr = MatMult(d_force_jac, X_vec, F_vec);
    IBTK_CHKERRQ(ierr);
    ierr = VecScale(F_vec, 0.5);
    IBTK_CHKERRQ(ierr);
    return;
}

void IBMethod::spreadForce(const int f_data_idx,
                           RobinPhysBdryPatchStrategy* f_phys_bdry_op,
                           const std::vector<boost::shared_ptr<RefineSchedule>>& f_prolongation_scheds,
                           const double data_time)
{
    std::vector<boost::shared_ptr<LData>> *F_data, *X_LE_data;
    bool *F_needs_ghost_fill, *X_LE_needs_ghost_fill;
    getForceData(&F_data, &F_needs_ghost_fill, data_time);
    getLECouplingPositionData(&X_LE_data, &X_LE_needs_ghost_fill, data_time);
    resetAnchorPointValues(*F_data,
                           /*coarsest_ln*/ 0,
                           /*finest_ln*/ d_hierarchy->getFinestLevelNumber());
    d_l_data_manager->spread(f_data_idx, *F_data, *X_LE_data, f_phys_bdry_op, f_prolongation_scheds, data_time,
                             *F_needs_ghost_fill, *X_LE_needs_ghost_fill);
    *F_needs_ghost_fill = false;
    *X_LE_needs_ghost_fill = false;
    return;
}

void IBMethod::spreadLinearizedForce(const int f_data_idx,
                                     RobinPhysBdryPatchStrategy* f_phys_bdry_op,
                                     const std::vector<boost::shared_ptr<RefineSchedule>>& f_prolongation_scheds,
                                     const double data_time)
{
    std::vector<boost::shared_ptr<LData>> *F_jac_data, *X_LE_data;
    bool *F_jac_needs_ghost_fill, *X_LE_needs_ghost_fill;
    getLinearizedForceData(&F_jac_data, &F_jac_needs_ghost_fill);
    getLECouplingPositionData(&X_LE_data, &X_LE_needs_ghost_fill, data_time);
    resetAnchorPointValues(*F_jac_data,
                           /*coarsest_ln*/ 0,
                           /*finest_ln*/ d_hierarchy->getFinestLevelNumber());
    d_l_data_manager->spread(f_data_idx, *F_jac_data, *X_LE_data, f_phys_bdry_op, f_prolongation_scheds, data_time,
                             *F_jac_needs_ghost_fill, *X_LE_needs_ghost_fill);
    *F_jac_needs_ghost_fill = false;
    *X_LE_needs_ghost_fill = false;
    return;
}

void IBMethod::computeLagrangianFluidSource(const double data_time)
{
    if (!d_ib_source_fcn) return;
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (d_n_src[ln] == 0) continue;
        std::fill(d_Q_src[ln].begin(), d_Q_src[ln].end(), 0.0);
        d_ib_source_fcn->computeSourceStrengths(d_Q_src[ln], d_hierarchy, ln, data_time, d_l_data_manager);
    }
    return;
}

void IBMethod::spreadFluidSource(const int q_data_idx,
                                 const std::vector<boost::shared_ptr<RefineSchedule>>& /*q_prolongation_scheds*/,
                                 const double data_time)
{
    if (!d_ib_source_fcn) return;

    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();

    // Get the present source locations.
    std::vector<boost::shared_ptr<LData>>* X_data;
    bool* X_needs_ghost_fill;
    getLECouplingPositionData(&X_data, &X_needs_ghost_fill, data_time);
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (d_n_src[ln] == 0) continue;
        d_ib_source_fcn->getSourceLocations(d_X_src[ln], d_r_src[ln], (*X_data)[ln], d_hierarchy, ln, data_time,
                                            d_l_data_manager);
    }

    // Spread the sources/sinks onto the Cartesian grid.
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (d_n_src[ln] == 0) continue;
        TBOX_ASSERT(ln == d_hierarchy->getFinestLevelNumber());
        auto level = d_hierarchy->getPatchLevel(ln);
        for (auto p = level->begin(); p != level->end(); ++p)
        {
            auto patch = *p;
            const Box& patch_box = patch->getBox();
            const Index& patch_lower = patch_box.lower();
            const Index& patch_upper = patch_box.upper();
            const auto pgeom = BOOST_CAST<CartesianPatchGeometry>(patch->getPatchGeometry());
            const double* const xLower = pgeom->getXLower();
            const double* const xUpper = pgeom->getXUpper();
            const double* const dx = pgeom->getDx();
            const auto q_data = BOOST_CAST<CellData<double>>(patch->getPatchData(q_data_idx));
            for (int n = 0; n < d_n_src[ln]; ++n)
            {
                // The source radius must be an integer multiple of the grid
                // spacing.
                boost::array<double, NDIM> r;
                for (unsigned int d = 0; d < NDIM; ++d)
                {
                    r[d] = std::max(std::floor(d_r_src[ln][n] / dx[d] + 0.5), 2.0) * dx[d];
                }

                // Determine the approximate source stencil box.
                const Index i_center =
                    IndexUtilities::getCellIndex(d_X_src[ln][n], xLower, xUpper, dx, patch_lower, patch_upper);
                Box stencil_box(i_center, i_center, BlockId::invalidId());
                for (unsigned int d = 0; d < NDIM; ++d)
                {
                    stencil_box.grow(d, static_cast<int>(r[d] / dx[d]) + 1);
                }

                // Spread the source strength onto the Cartesian grid.
                const Box it_box = patch_box * stencil_box;
                for (auto b = CellGeometry::begin(it_box), e = CellGeometry::end(it_box); b != e; ++b)
                {
                    const CellIndex& i = *b;
                    double wgt = 1.0;
                    for (unsigned int d = 0; d < NDIM; ++d)
                    {
                        const double X_center = xLower[d] + dx[d] * (static_cast<double>(i(d) - patch_lower(d)) + 0.5);
                        wgt *= cos_kernel(X_center - d_X_src[ln][n][d], r[d]);
                    }
                    (*q_data)(i) += d_Q_src[ln][n] * wgt;
                }
            }
        }
    }

    // Compute the net inflow into the computational domain.
    const int wgt_idx = getHierarchyMathOps()->getCellWeightPatchDescriptorIndex();
    PatchCellDataOpsReal<double> patch_cc_data_ops;
    double Q_sum = 0.0;
    double Q_max = 0.0;
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        Q_sum = std::accumulate(d_Q_src[ln].begin(), d_Q_src[ln].end(), Q_sum);
        for (unsigned int k = 0; k < d_Q_src[ln].size(); ++k)
        {
            Q_max = std::max(Q_max, std::abs(d_Q_src[ln][k]));
        }
    }
    auto pressure_hier_data_ops = BOOST_CAST<HierarchyCellDataOpsReal<double>>(getPressureHierarchyDataOps());
    const double q_total = pressure_hier_data_ops->integral(q_data_idx, wgt_idx);
    if (std::abs(q_total - Q_sum) > 1.0e-12 && std::abs(q_total - Q_sum) / std::max(Q_max, 1.0) > 1.0e-12)
    {
#if (NDIM == 2)
        TBOX_ERROR(d_object_name << "::spreadFluidSource():\n"
                                 << "  Lagrangian and Eulerian source/sink strengths are inconsistent:\n"
                                 << "    Sum_{i,j} q_{i,j} h^2     = " << q_total << "\n"
                                 << "    Sum_{l=1,...,n_src} Q_{l} = " << Q_sum << "\n");
#endif
#if (NDIM == 3)
        TBOX_ERROR(d_object_name << "::spreadFluidSource():\n"
                                 << "  Lagrangian and Eulerian source/sink strengths are inconsistent:\n"
                                 << "    Sum_{i,j,k} q_{i,j,k} h^3 = " << q_total << "\n"
                                 << "    Sum_{l=1,...,n_src} Q_{l} = " << Q_sum << "\n");
#endif
    }

    // Balance the net inflow/outflow with outflow/inflow along the upper/lower
    // boundaries of the computational domain (if needed).
    if (d_normalize_source_strength)
    {
        auto grid_geom = BOOST_CAST<CartesianGridGeometry>(d_hierarchy->getGridGeometry());
        TBOX_ASSERT(grid_geom->getPhysicalDomain().size() == 1);
        const Box& domain_box = grid_geom->getPhysicalDomain().front();
        const double* const dx_coarsest = grid_geom->getDx();
        Box interior_box = domain_box;
        for (unsigned int d = 0; d < NDIM - 1; ++d)
        {
            interior_box.grow(d, -1);
        }
        BoxContainer bdry_boxes;
        bdry_boxes.removeIntersections(domain_box, interior_box);
        double vol = static_cast<double>(bdry_boxes.getTotalSizeOfBoxes());
        for (unsigned int d = 0; d < NDIM; ++d)
        {
            vol *= dx_coarsest[d];
        }
        const double q_norm = -q_total / vol;
        for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
        {
            auto level = d_hierarchy->getPatchLevel(ln);
            BoxContainer level_bdry_boxes(bdry_boxes);
            level_bdry_boxes.refine(level->getRatioToLevelZero());
            for (auto p = level->begin(); p != level->end(); ++p)
            {
                auto patch = *p;
                const Box& patch_box = patch->getBox();
                const auto q_data = BOOST_CAST<CellData<double>>(patch->getPatchData(q_data_idx));
                for (auto blist = level_bdry_boxes.begin(), e = level_bdry_boxes.end(); blist != e; ++blist)
                {
                    const Box it_box = *blist * patch_box;
                    for (auto b = CellGeometry::begin(it_box), e = CellGeometry::end(it_box); b != e; ++b)
                    {
                        (*q_data)(*b) += q_norm;
                    }
                }
            }
        }
        auto pressure_hier_data_ops = BOOST_CAST<HierarchyCellDataOpsReal<double>>(getPressureHierarchyDataOps());
        const double integral_q = pressure_hier_data_ops->integral(q_data_idx, wgt_idx);
        if (std::abs(integral_q) > 1.0e-10 * std::max(1.0, getPressureHierarchyDataOps()->maxNorm(q_data_idx, wgt_idx)))
        {
            TBOX_ERROR(d_object_name << "::spreadFluidSource():\n"
                                     << "  ``external' source/sink does not correctly offset net "
                                        "inflow/outflow into domain.\n"
                                     << "  integral{q} = " << integral_q << " != 0.\n");
        }
    }
    return;
}

void IBMethod::interpolatePressure(int p_data_idx,
                                   const std::vector<boost::shared_ptr<CoarsenSchedule>>& /*p_synch_scheds*/,
                                   const std::vector<boost::shared_ptr<RefineSchedule>>& /*p_ghost_fill_scheds*/,
                                   const double data_time)
{
    if (!d_ib_source_fcn) return;

    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();

    // Get the present source locations.
    std::vector<boost::shared_ptr<LData>>* X_data;
    bool* X_needs_ghost_fill;
    getLECouplingPositionData(&X_data, &X_needs_ghost_fill, data_time);

    // Compute the normalization pressure (if needed).
    double p_norm = 0.0;
    if (d_normalize_source_strength)
    {
        const int wgt_idx = getHierarchyMathOps()->getCellWeightPatchDescriptorIndex();
        auto grid_geom = BOOST_CAST<CartesianGridGeometry>(d_hierarchy->getGridGeometry());
        TBOX_ASSERT(grid_geom->getPhysicalDomain().size() == 1);
        const Box& domain_box = grid_geom->getPhysicalDomain().front();
        Box interior_box = domain_box;
        for (unsigned int d = 0; d < NDIM - 1; ++d)
        {
            interior_box.grow(d, -1);
        }
        BoxContainer bdry_boxes;
        bdry_boxes.removeIntersections(domain_box, interior_box);
        double vol = 0.0;
        for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
        {
            auto level = d_hierarchy->getPatchLevel(ln);
            BoxContainer level_bdry_boxes(bdry_boxes);
            level_bdry_boxes.refine(level->getRatioToLevelZero());
            for (auto p = level->begin(); p != level->end(); ++p)
            {
                auto patch = *p;
                const Box& patch_box = patch->getBox();
                const auto p_data = BOOST_CAST<CellData<double>>(patch->getPatchData(p_data_idx));
                const auto wgt_data = BOOST_CAST<CellData<double>>(patch->getPatchData(wgt_idx));
                for (auto blist = level_bdry_boxes.begin(), e = level_bdry_boxes.end(); blist != e; ++blist)
                {
                    const Box it_box = *blist * patch_box;
                    for (auto b = CellGeometry::begin(it_box), e = CellGeometry::end(it_box); b != e; ++b)
                    {
                        const CellIndex& i = *b;
                        p_norm += (*p_data)(i) * (*wgt_data)(i);
                        vol += (*wgt_data)(i);
                    }
                }
            }
        }
        tbox::SAMRAI_MPI comm(MPI_COMM_WORLD);
        comm.AllReduce(&p_norm, 1, MPI_SUM);
        comm.AllReduce(&vol, 1, MPI_SUM);
        p_norm /= vol;
    }

    // Reset the values of P_src.
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        std::fill(d_P_src[ln].begin(), d_P_src[ln].end(), 0.0);
    }

    // Compute the mean pressure at the sources/sinks associated with each level
    // of the Cartesian grid.
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (d_n_src[ln] == 0) continue;
        auto level = d_hierarchy->getPatchLevel(ln);
        for (auto p = level->begin(); p != level->end(); ++p)
        {
            auto patch = *p;
            const Box& patch_box = patch->getBox();
            const Index& patch_lower = patch_box.lower();
            const Index& patch_upper = patch_box.upper();
            const auto pgeom = BOOST_CAST<CartesianPatchGeometry>(patch->getPatchGeometry());
            const double* const xLower = pgeom->getXLower();
            const double* const xUpper = pgeom->getXUpper();
            const double* const dx = pgeom->getDx();
            const auto p_data = BOOST_CAST<CellData<double>>(patch->getPatchData(p_data_idx));
            for (int n = 0; n < d_n_src[ln]; ++n)
            {
                // The source radius must be an integer multiple of the grid
                // spacing.
                boost::array<double, NDIM> r;
                for (unsigned int d = 0; d < NDIM; ++d)
                {
                    r[d] = std::max(std::floor(d_r_src[ln][n] / dx[d] + 0.5), 2.0) * dx[d];
                }

                // Determine the approximate source stencil box.
                const Index i_center =
                    IndexUtilities::getCellIndex(d_X_src[ln][n], xLower, xUpper, dx, patch_lower, patch_upper);
                Box stencil_box(i_center, i_center, BlockId::invalidId());
                for (unsigned int d = 0; d < NDIM; ++d)
                {
                    stencil_box.grow(d, static_cast<int>(r[d] / dx[d]) + 1);
                }

                // Interpolate the pressure from the Cartesian grid.
                const Box it_box = patch_box * stencil_box;
                for (auto b = CellGeometry::begin(it_box), e = CellGeometry::end(it_box); b != e; ++b)
                {
                    const CellIndex& i = *b;
                    double wgt = 1.0;
                    for (unsigned int d = 0; d < NDIM; ++d)
                    {
                        const double X_center = xLower[d] + dx[d] * (static_cast<double>(i(d) - patch_lower(d)) + 0.5);
                        wgt *= cos_kernel(X_center - d_X_src[ln][n][d], r[d]) * dx[d];
                    }
                    d_P_src[ln][n] += (*p_data)(i)*wgt;
                }
            }
        }
        tbox::SAMRAI_MPI comm(MPI_COMM_WORLD);
        comm.AllReduce(&d_P_src[ln][0], static_cast<int>(d_P_src[ln].size()), MPI_SUM);
        std::transform(d_P_src[ln].begin(), d_P_src[ln].end(), d_P_src[ln].begin(),
                       std::bind2nd(std::plus<double>(), -p_norm));

        // Update the pressures stored by the Lagrangian source strategy.
        d_ib_source_fcn->setSourcePressures(d_P_src[ln], d_hierarchy, ln, data_time, d_l_data_manager);
    }
    return;
}

void IBMethod::postprocessData()
{
    if (!d_post_processor) return;

    auto var_db = VariableDatabase::getDatabase();
    const int u_current_idx =
        var_db->mapVariableAndContextToIndex(d_ib_solver->getVelocityVariable(), d_ib_solver->getCurrentContext());
    const int p_current_idx =
        var_db->mapVariableAndContextToIndex(d_ib_solver->getPressureVariable(), d_ib_solver->getCurrentContext());
    const int f_current_idx =
        var_db->mapVariableAndContextToIndex(d_ib_solver->getBodyForceVariable(), d_ib_solver->getCurrentContext());

    const double current_time = d_ib_solver->getIntegratorTime();
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    auto grid_geom = BOOST_CAST<CartesianGridGeometry>(d_hierarchy->getGridGeometry());

    // Initialize data on each level of the patch hierarchy.
    std::vector<boost::shared_ptr<LData>> X_data(finest_ln + 1);
    std::vector<boost::shared_ptr<LData>> F_data(finest_ln + 1);
    std::vector<boost::shared_ptr<LData>> U_data(finest_ln + 1);
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        X_data[ln] = d_l_data_manager->getLData(LDataManager::POSN_DATA_NAME, ln);
        U_data[ln] = d_l_data_manager->getLData(LDataManager::VEL_DATA_NAME, ln);
        F_data[ln] = d_l_data_manager->getLData("F", ln);
    }

    // Perform the user-defined post-processing.
    d_post_processor->postprocessData(u_current_idx, p_current_idx, f_current_idx, F_data, X_data, U_data, d_hierarchy,
                                      coarsest_ln, finest_ln, current_time, this);
    return;
}

void IBMethod::initializePatchHierarchy(const boost::shared_ptr<PatchHierarchy>& hierarchy,
                                        const boost::shared_ptr<GriddingAlgorithm>& gridding_alg,
                                        int u_data_idx,
                                        const std::vector<boost::shared_ptr<CoarsenSchedule>>& u_synch_scheds,
                                        const std::vector<boost::shared_ptr<RefineSchedule>>& u_ghost_fill_scheds,
                                        int integrator_step,
                                        double init_data_time,
                                        bool initial_time)
{
    // Cache pointers to the patch hierarchy and gridding algorithm.
    d_hierarchy = hierarchy;
    d_gridding_alg = gridding_alg;

    // Lookup the range of hierarchy levels.
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();

    // Initialize various Lagrangian data objects.
    if (initial_time)
    {
        // Initialize the interpolated velocity field.
        std::vector<boost::shared_ptr<LData>> X_data(finest_ln + 1);
        std::vector<boost::shared_ptr<LData>> U_data(finest_ln + 1);
        for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
        {
            if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
            X_data[ln] = d_l_data_manager->getLData(LDataManager::POSN_DATA_NAME, ln);
            U_data[ln] = d_l_data_manager->getLData(LDataManager::VEL_DATA_NAME, ln);
        }
        d_l_data_manager->interp(u_data_idx, U_data, X_data, u_synch_scheds, u_ghost_fill_scheds, init_data_time);
        resetAnchorPointValues(U_data, coarsest_ln, finest_ln);

        // Initialize source/sink data.
        for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
        {
            if (d_ib_source_fcn)
            {
                d_ib_source_fcn->initializeLevelData(d_hierarchy, ln, init_data_time, initial_time, d_l_data_manager);
                d_n_src[ln] = d_ib_source_fcn->getNumSources(d_hierarchy, ln, init_data_time, d_l_data_manager);
                d_X_src[ln].resize(d_n_src[ln], Point::Constant(std::numeric_limits<double>::quiet_NaN()));
                d_r_src[ln].resize(d_n_src[ln], std::numeric_limits<double>::quiet_NaN());
                d_P_src[ln].resize(d_n_src[ln], std::numeric_limits<double>::quiet_NaN());
                d_Q_src[ln].resize(d_n_src[ln], std::numeric_limits<double>::quiet_NaN());
                d_ib_source_fcn->getSourceLocations(d_X_src[ln], d_r_src[ln], X_data[ln], d_hierarchy, ln,
                                                    init_data_time, d_l_data_manager);
            }
        }
    }

    // Initialize the instrumentation data.
    d_instrument_panel->initializeHierarchyIndependentData(d_hierarchy, d_l_data_manager);
    if (d_instrument_panel->isInstrumented())
    {
        d_instrument_panel->initializeHierarchyDependentData(d_hierarchy, d_l_data_manager, integrator_step,
                                                             init_data_time);
        if (d_total_flow_volume.empty())
        {
            d_total_flow_volume.resize(d_instrument_panel->getFlowValues().size(), 0.0);
        }
    }

    // Indicate that the force and source strategies need to be re-initialized.
    d_ib_force_fcn_needs_init = true;
    d_ib_source_fcn_needs_init = true;

    // Deallocate any previously allocated Jacobian data structures.
    if (d_force_jac)
    {
        PetscErrorCode ierr;
        ierr = MatDestroy(&d_force_jac);
        IBTK_CHKERRQ(ierr);
    }
    return;
}

void IBMethod::registerLoadBalancer(const boost::shared_ptr<ChopAndPackLoadBalancer>& load_balancer,
                                    int workload_data_idx)
{
    TBOX_ASSERT(load_balancer);
    d_load_balancer = load_balancer;
    d_workload_idx = workload_data_idx;
    d_l_data_manager->registerLoadBalancer(load_balancer, workload_data_idx);
    return;
}

void IBMethod::updateWorkloadEstimates(const boost::shared_ptr<PatchHierarchy>& /*hierarchy*/,
                                       int /*workload_data_idx*/)
{
    d_l_data_manager->updateWorkloadEstimates();
    return;
}

void IBMethod::beginDataRedistribution(const boost::shared_ptr<PatchHierarchy>& /*hierarchy*/,
                                       const boost::shared_ptr<GriddingAlgorithm>& /*gridding_alg*/)
{
    d_l_data_manager->beginDataRedistribution();
    return;
}

void IBMethod::endDataRedistribution(const boost::shared_ptr<PatchHierarchy>& hierarchy,
                                     const boost::shared_ptr<GriddingAlgorithm>& /*gridding_alg*/)
{
    d_l_data_manager->endDataRedistribution();

    // Look up the re-distributed Lagrangian position data.
    std::vector<boost::shared_ptr<LData>> X_data(hierarchy->getFinestLevelNumber() + 1);
    for (int ln = 0; ln <= hierarchy->getFinestLevelNumber(); ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        X_data[ln] = d_l_data_manager->getLData(LDataManager::POSN_DATA_NAME, ln);
    }

    // Compute the set of local anchor points.
    static const double eps = 2.0 * sqrt(std::numeric_limits<double>::epsilon());
    auto grid_geom = BOOST_CAST<CartesianGridGeometry>(hierarchy->getGridGeometry());
    const double* const grid_x_lower = grid_geom->getXLower();
    const double* const grid_x_upper = grid_geom->getXUpper();
    for (int ln = 0; ln <= hierarchy->getFinestLevelNumber(); ++ln)
    {
        auto level = d_hierarchy->getPatchLevel(ln);
        const IntVector& periodic_shift = grid_geom->getPeriodicShift(level->getRatioToLevelZero());
        d_anchor_point_local_idxs[ln].clear();
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;

        const auto mesh = d_l_data_manager->getLMesh(ln);
        const auto& local_nodes = mesh->getLocalNodes();
        for (auto cit = local_nodes.begin(); cit != local_nodes.end(); ++cit)
        {
            const auto node_idx = *cit;
            const IBAnchorPointSpec* const anchor_point_spec = node_idx->getNodeDataItem<IBAnchorPointSpec>();
            if (anchor_point_spec)
            {
                d_anchor_point_local_idxs[ln].insert(node_idx->getLocalPETScIndex());
            }
        }

        const boost::multi_array_ref<double, 2>& X_array = *(X_data[ln]->getLocalFormVecVector());
        for (int i = 0; i < static_cast<int>(X_data[ln]->getLocalNodeCount()); ++i)
        {
            for (int d = 0; d < NDIM; ++d)
            {
                if ((periodic_shift[d] == 0) &&
                    (X_array[i][d] <= grid_x_lower[d] + eps || X_array[i][d] >= grid_x_upper[d] - eps))
                {
                    d_anchor_point_local_idxs[ln].insert(i);
                    break;
                }
            }
        }
        X_data[ln]->restoreArrays();
    }

    // Indicate that the force and source strategies need to be re-initialized.
    d_ib_force_fcn_needs_init = true;
    d_ib_source_fcn_needs_init = true;
    return;
}

void IBMethod::initializeLevelData(const boost::shared_ptr<PatchHierarchy>& hierarchy,
                                   int level_number,
                                   double init_data_time,
                                   bool can_be_refined,
                                   bool initial_time,
                                   const boost::shared_ptr<PatchLevel>& old_level,
                                   bool allocate_data)
{
    const int finest_hier_level = hierarchy->getFinestLevelNumber();
    d_l_data_manager->setPatchHierarchy(hierarchy);
    d_l_data_manager->setPatchLevels(0, finest_hier_level);
    d_l_data_manager->initializeLevelData(hierarchy, level_number, init_data_time, can_be_refined, initial_time,
                                          old_level, allocate_data);
    if (initial_time && d_l_data_manager->levelContainsLagrangianData(level_number))
    {
        auto F_data = d_l_data_manager->createLData("F", level_number, NDIM, /*manage_data*/ true);
    }
    if (d_load_balancer && d_l_data_manager->levelContainsLagrangianData(level_number))
    {
        d_load_balancer->setWorkloadPatchDataIndex(d_workload_idx, level_number);
        d_l_data_manager->updateWorkloadEstimates(level_number, level_number);
    }
    return;
}

void IBMethod::resetHierarchyConfiguration(const boost::shared_ptr<PatchHierarchy>& hierarchy,
                                           int coarsest_level,
                                           int finest_level)
{
    const int finest_hier_level = hierarchy->getFinestLevelNumber();
    d_l_data_manager->setPatchHierarchy(hierarchy);
    d_l_data_manager->setPatchLevels(0, finest_hier_level);
    d_l_data_manager->resetHierarchyConfiguration(hierarchy, coarsest_level, finest_level);

    // If we have added or removed a level, resize the anchor point vectors.
    d_anchor_point_local_idxs.clear();
    d_anchor_point_local_idxs.resize(finest_hier_level + 1);

    // If we have added or removed a level, resize the source/sink data vectors.
    d_X_src.resize(finest_hier_level + 1);
    d_r_src.resize(finest_hier_level + 1);
    d_P_src.resize(finest_hier_level + 1);
    d_Q_src.resize(finest_hier_level + 1);
    d_n_src.resize(finest_hier_level + 1, 0);
    return;
}

void IBMethod::applyGradientDetector(const boost::shared_ptr<PatchHierarchy>& hierarchy,
                                     int level_number,
                                     double error_data_time,
                                     int tag_index,
                                     bool initial_time,
                                     bool uses_richardson_extrapolation_too)
{
    TBOX_ASSERT(hierarchy);
    TBOX_ASSERT((level_number >= 0) && (level_number <= hierarchy->getFinestLevelNumber()));
    TBOX_ASSERT(hierarchy->getPatchLevel(level_number));
    auto level = hierarchy->getPatchLevel(level_number);

    // Tag cells that contain Lagrangian nodes.
    d_l_data_manager->applyGradientDetector(hierarchy, level_number, error_data_time, tag_index, initial_time,
                                            uses_richardson_extrapolation_too);

    // Tag cells where the Cartesian source/sink strength is nonzero.
    if (d_ib_source_fcn && !initial_time && hierarchy->finerLevelExists(level_number))
    {
        auto grid_geom = BOOST_CAST<CartesianGridGeometry>(hierarchy->getGridGeometry());
        TBOX_ASSERT(grid_geom->getPhysicalDomain().size() == 1);
        const Box& domain_box = grid_geom->getPhysicalDomain().front();
        const Index& lower = domain_box.lower();
        const Index& upper = domain_box.upper();
        const double* const xLower = grid_geom->getXLower();
        const double* const xUpper = grid_geom->getXUpper();
        const double* const dx = grid_geom->getDx();

        const int finer_level_number = level_number + 1;
        auto finer_level = hierarchy->getPatchLevel(finer_level_number);
        for (int n = 0; n < d_n_src[finer_level_number]; ++n)
        {
            boost::array<double, NDIM> dx_finer;
            const IntVector& ratio_to_level_zero = finer_level->getRatioToLevelZero();
            for (unsigned int d = 0; d < NDIM; ++d)
            {
                dx_finer[d] = dx[d] / static_cast<double>(ratio_to_level_zero(d));
            }

            // The source radius must be an integer multiple of the grid
            // spacing.
            boost::array<double, NDIM> r;
            for (unsigned int d = 0; d < NDIM; ++d)
            {
                r[d] = std::max(std::floor(d_r_src[finer_level_number][n] / dx_finer[d] + 0.5), 2.0) * dx_finer[d];
            }

            // Determine the approximate source stencil box.
            const Index i_center = IndexUtilities::getCellIndex(d_X_src[finer_level_number][n], xLower, xUpper,
                                                                dx_finer.data(), lower, upper);
            Box stencil_box(i_center, i_center, BlockId::invalidId());
            for (unsigned int d = 0; d < NDIM; ++d)
            {
                stencil_box.grow(d, static_cast<int>(r[d] / dx_finer[d]) + 1);
            }
            const Box coarsened_stencil_box = Box::coarsen(stencil_box, finer_level->getRatioToCoarserLevel());
            for (auto p = level->begin(); p != level->end(); ++p)
            {
                auto patch = *p;
                auto tags_data = BOOST_CAST<CellData<int>>(patch->getPatchData(tag_index));
                tags_data->fillAll(1, coarsened_stencil_box);
            }
        }
    }
    return;
}

void IBMethod::putToRestart(const boost::shared_ptr<Database>& db) const
{
    db->putInteger("IB_METHOD_VERSION", IB_METHOD_VERSION);
    db->putString("d_interp_kernel_fcn", d_interp_kernel_fcn);
    db->putString("d_spread_kernel_fcn", d_spread_kernel_fcn);
    db->putIntegerArray("d_ghosts", &d_ghosts[0], NDIM);
    const std::vector<std::string>& instrument_names = IBInstrumentationSpec::getInstrumentNames();
    db->putStringVector("instrument_names", instrument_names);
    db->putDoubleVector("d_total_flow_volume", d_total_flow_volume);
    db->putIntegerVector("d_n_src", d_n_src);
    for (unsigned int ln = 0; ln < d_n_src.size(); ++ln)
    {
        for (int n = 0; n < d_n_src[ln]; ++n)
        {
            std::ostringstream id_stream;
            id_stream << ln << "_" << n;
            const std::string id_string = id_stream.str();
            db->putDoubleArray("d_X_src_" + id_string, &d_X_src[ln][n][0], NDIM);
            db->putDouble("d_r_src_" + id_string, d_r_src[ln][n]);
            db->putDouble("d_P_src_" + id_string, d_P_src[ln][n]);
            db->putDouble("d_Q_src_" + id_string, d_Q_src[ln][n]);
        }
    }
    db->putBool("d_normalize_source_strength", d_normalize_source_strength);
    return;
}

/////////////////////////////// PROTECTED ////////////////////////////////////

void IBMethod::getPositionData(std::vector<boost::shared_ptr<LData>>** X_data,
                               bool** X_needs_ghost_fill,
                               double data_time)
{
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    if (MathUtilities<double>::equalEps(data_time, d_current_time))
    {
        *X_data = &d_X_current_data;
        *X_needs_ghost_fill = &d_X_current_needs_ghost_fill;
    }
    else if (MathUtilities<double>::equalEps(data_time, d_half_time))
    {
        for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
        {
            if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
            if (!d_X_half_data[ln])
            {
                d_X_half_data[ln] = d_l_data_manager->createLData("X_half", ln, NDIM);
                d_X_half_needs_reinit = true;
            }
        }
        if (d_X_half_needs_reinit)
        {
            reinitMidpointData(d_X_current_data, d_X_new_data, d_X_half_data);
            d_X_half_needs_reinit = false;
            d_X_half_needs_ghost_fill = true;
        }
        *X_data = &d_X_half_data;
        *X_needs_ghost_fill = &d_X_half_needs_ghost_fill;
    }
    else if (MathUtilities<double>::equalEps(data_time, d_new_time))
    {
        *X_data = &d_X_new_data;
        *X_needs_ghost_fill = &d_X_new_needs_ghost_fill;
    }
    return;
}

void IBMethod::getLinearizedPositionData(std::vector<boost::shared_ptr<LData>>** X_jac_data,
                                         bool** X_jac_needs_ghost_fill)
{
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        if (!d_X_jac_data[ln])
        {
            d_X_jac_data[ln] = d_l_data_manager->createLData("X_jac", ln, NDIM);
            d_X_jac_needs_ghost_fill = true;
        }
    }
    *X_jac_data = &d_X_jac_data;
    *X_jac_needs_ghost_fill = &d_X_jac_needs_ghost_fill;
    return;
}

void IBMethod::getLECouplingPositionData(std::vector<boost::shared_ptr<LData>>** X_LE_data,
                                         bool** X_LE_needs_ghost_fill,
                                         double data_time)
{
    if (!d_use_fixed_coupling_ops)
    {
        getPositionData(X_LE_data, X_LE_needs_ghost_fill, data_time);
        return;
    }
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    if (MathUtilities<double>::equalEps(data_time, d_current_time))
    {
        *X_LE_data = &d_X_current_data;
        *X_LE_needs_ghost_fill = &d_X_current_needs_ghost_fill;
    }
    else if (MathUtilities<double>::equalEps(data_time, d_half_time))
    {
        for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
        {
            if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
            if (!d_X_LE_half_data[ln])
            {
                d_X_LE_half_data[ln] = d_l_data_manager->createLData("X_LE_half", ln, NDIM);
                d_X_LE_half_needs_reinit = true;
            }
        }
        if (d_X_LE_half_needs_reinit)
        {
            reinitMidpointData(d_X_current_data, d_X_LE_new_data, d_X_LE_half_data);
            d_X_LE_half_needs_reinit = false;
            d_X_LE_half_needs_ghost_fill = true;
        }
        *X_LE_data = &d_X_LE_half_data;
        *X_LE_needs_ghost_fill = &d_X_LE_half_needs_ghost_fill;
    }
    else if (MathUtilities<double>::equalEps(data_time, d_new_time))
    {
        *X_LE_data = &d_X_LE_new_data;
        *X_LE_needs_ghost_fill = &d_X_LE_new_needs_ghost_fill;
    }
    return;
}

void IBMethod::getVelocityData(std::vector<boost::shared_ptr<LData>>** U_data, double data_time)
{
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    if (MathUtilities<double>::equalEps(data_time, d_current_time))
    {
        *U_data = &d_U_current_data;
    }
    else if (MathUtilities<double>::equalEps(data_time, d_half_time))
    {
        for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
        {
            if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
            if (!d_U_half_data[ln])
            {
                d_U_half_data[ln] = d_l_data_manager->createLData("U_half", ln, NDIM);
                d_U_half_needs_reinit = true;
            }
        }
        if (d_U_half_needs_reinit)
        {
            reinitMidpointData(d_U_current_data, d_U_new_data, d_U_half_data);
            d_U_half_needs_reinit = false;
        }
        *U_data = &d_U_half_data;
    }
    else if (MathUtilities<double>::equalEps(data_time, d_new_time))
    {
        *U_data = &d_U_new_data;
    }
    return;
}

void IBMethod::getLinearizedVelocityData(std::vector<boost::shared_ptr<LData>>** U_jac_data)
{
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        if (!d_U_jac_data[ln])
        {
            d_U_jac_data[ln] = d_l_data_manager->createLData("U_jac", ln, NDIM);
        }
    }
    *U_jac_data = &d_U_jac_data;
    return;
}

void IBMethod::getForceData(std::vector<boost::shared_ptr<LData>>** F_data, bool** F_needs_ghost_fill, double data_time)
{
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    if (MathUtilities<double>::equalEps(data_time, d_current_time))
    {
        *F_data = &d_F_current_data;
        *F_needs_ghost_fill = &d_F_current_needs_ghost_fill;
    }
    else if (MathUtilities<double>::equalEps(data_time, d_half_time))
    {
        for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
        {
            if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
            if (!d_F_half_data[ln]) d_F_half_data[ln] = d_l_data_manager->createLData("F_half", ln, NDIM);
        }
        *F_data = &d_F_half_data;
        *F_needs_ghost_fill = &d_F_half_needs_ghost_fill;
    }
    else if (MathUtilities<double>::equalEps(data_time, d_new_time))
    {
        for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
        {
            if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
            if (!d_F_new_data[ln]) d_F_new_data[ln] = d_l_data_manager->createLData("F_new", ln, NDIM);
        }
        *F_data = &d_F_new_data;
        *F_needs_ghost_fill = &d_F_new_needs_ghost_fill;
    }
    return;
}

void IBMethod::getLinearizedForceData(std::vector<boost::shared_ptr<LData>>** F_jac_data, bool** F_jac_needs_ghost_fill)
{
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        if (!d_F_jac_data[ln])
        {
            d_F_jac_data[ln] = d_l_data_manager->createLData("F_jac", ln, NDIM);
            d_F_jac_needs_ghost_fill = true;
        }
    }
    *F_jac_data = &d_F_jac_data;
    *F_jac_needs_ghost_fill = &d_F_jac_needs_ghost_fill;
    return;
}

void IBMethod::reinitMidpointData(const std::vector<boost::shared_ptr<LData>>& current_data,
                                  const std::vector<boost::shared_ptr<LData>>& new_data,
                                  const std::vector<boost::shared_ptr<LData>>& half_data)
{
    int ierr;
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        ierr = VecAXPBYPCZ(half_data[ln]->getVec(), 0.5, 0.5, 0.0, current_data[ln]->getVec(), new_data[ln]->getVec());
        IBTK_CHKERRQ(ierr);
    }
    return;
}

void IBMethod::resetAnchorPointValues(std::vector<boost::shared_ptr<LData>> U_data,
                                      const int coarsest_ln,
                                      const int finest_ln)
{
    PetscErrorCode ierr;
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        const int depth = U_data[ln]->getDepth();
        TBOX_ASSERT(depth == NDIM);
        Vec U_vec = U_data[ln]->getVec();
        double* U_arr;
        ierr = VecGetArray(U_vec, &U_arr);
        IBTK_CHKERRQ(ierr);
        for (auto cit = d_anchor_point_local_idxs[ln].begin(); cit != d_anchor_point_local_idxs[ln].end(); ++cit)
        {
            const int& i = *cit;
            for (int d = 0; d < depth; ++d)
            {
                U_arr[depth * i + d] = 0.0;
            }
        }
        ierr = VecRestoreArray(U_vec, &U_arr);
        IBTK_CHKERRQ(ierr);
    }
    return;
}

/////////////////////////////// PRIVATE //////////////////////////////////////

void IBMethod::resetLagrangianForceFunction(const double init_data_time, const bool initial_time)
{
    if (!d_ib_force_fcn) return;
    for (int ln = 0; ln <= d_hierarchy->getFinestLevelNumber(); ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        d_ib_force_fcn->initializeLevelData(d_hierarchy, ln, init_data_time, initial_time, d_l_data_manager);
    }
    return;
}

void IBMethod::resetLagrangianSourceFunction(const double init_data_time, const bool initial_time)
{
    if (!d_ib_source_fcn) return;
    for (int ln = 0; ln <= d_hierarchy->getFinestLevelNumber(); ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
        d_ib_source_fcn->initializeLevelData(d_hierarchy, ln, init_data_time, initial_time, d_l_data_manager);
    }
    return;
}

void IBMethod::updateIBInstrumentationData(const int timestep_num, const double data_time)
{
    if (!d_instrument_panel->isInstrumented()) return;

    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();

    // Compute the positions of the flow meter nets.
    d_instrument_panel->initializeHierarchyDependentData(d_hierarchy, d_l_data_manager, timestep_num, data_time);

    // Compute the flow rates and pressures.
    auto var_db = VariableDatabase::getDatabase();
    const int u_scratch_idx =
        var_db->mapVariableAndContextToIndex(d_ib_solver->getVelocityVariable(), d_ib_solver->getScratchContext());
    const int p_scratch_idx =
        var_db->mapVariableAndContextToIndex(d_ib_solver->getPressureVariable(), d_ib_solver->getScratchContext());

    std::vector<bool> deallocate_u_scratch_data(finest_ln + 1, false);
    std::vector<bool> deallocate_p_scratch_data(finest_ln + 1, false);
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        auto level = d_hierarchy->getPatchLevel(ln);
        if (!level->checkAllocated(u_scratch_idx))
        {
            deallocate_u_scratch_data[ln] = true;
            level->allocatePatchData(u_scratch_idx, data_time);
        }
        if (!level->checkAllocated(p_scratch_idx))
        {
            deallocate_p_scratch_data[ln] = true;
            level->allocatePatchData(p_scratch_idx, data_time);
        }
        getGhostfillRefineSchedules(d_ib_solver->getName() + "::INSTRUMENTATION_DATA_FILL")[ln]->fillData(data_time);
    }

    d_instrument_panel->readInstrumentData(u_scratch_idx, p_scratch_idx, d_hierarchy, d_l_data_manager, timestep_num,
                                           data_time);

    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        auto level = d_hierarchy->getPatchLevel(ln);
        if (deallocate_u_scratch_data[ln]) level->deallocatePatchData(u_scratch_idx);
        if (deallocate_p_scratch_data[ln]) level->deallocatePatchData(p_scratch_idx);
    }
    return;
}

void IBMethod::getFromInput(const boost::shared_ptr<Database>& db, bool is_from_restart)
{
    if (!is_from_restart)
    {
        if (db->isString("interp_kernel_fcn") && db->isString("spread_kernel_fcn"))
        {
            d_interp_kernel_fcn = db->getString("interp_kernel_fcn");
            d_spread_kernel_fcn = db->getString("spread_kernel_fcn");
        }
        if (db->isString("interp_delta_fcn") && db->isString("spread_delta_fcn"))
        {
            d_interp_kernel_fcn = db->getString("interp_delta_fcn");
            d_spread_kernel_fcn = db->getString("spread_delta_fcn");
        }
        else if (db->keyExists("delta_fcn"))
        {
            d_interp_kernel_fcn = db->getString("delta_fcn");
            d_spread_kernel_fcn = db->getString("delta_fcn");
        }
        else if (db->keyExists("kernel_fcn"))
        {
            d_interp_kernel_fcn = db->getString("kernel_fcn");
            d_spread_kernel_fcn = db->getString("kernel_fcn");
        }
        else if (db->keyExists("IB_delta_fcn"))
        {
            d_interp_kernel_fcn = db->getString("IB_delta_fcn");
            d_spread_kernel_fcn = db->getString("IB_delta_fcn");
        }
        else if (db->keyExists("IB_kernel_fcn"))
        {
            d_interp_kernel_fcn = db->getString("IB_kernel_fcn");
            d_spread_kernel_fcn = db->getString("IB_kernel_fcn");
        }

        if (db->isInteger("min_ghost_cell_width"))
        {
            d_ghosts = IntVector(DIM, db->getInteger("min_ghost_cell_width"));
        }
        else if (db->isDouble("min_ghost_cell_width"))
        {
            d_ghosts = IntVector(DIM, static_cast<int>(std::ceil(db->getDouble("min_ghost_cell_width"))));
        }

        if (db->isBool("normalize_source_strength"))
            d_normalize_source_strength = db->getBool("normalize_source_strength");
    }
    if (db->keyExists("do_log"))
        d_do_log = db->getBool("do_log");
    else if (db->keyExists("enable_logging"))
        d_do_log = db->getBool("enable_logging");
    return;
}

void IBMethod::getFromRestart()
{
    auto restart_db = RestartManager::getManager()->getRootDatabase();
    boost::shared_ptr<Database> db;
    if (restart_db->isDatabase(d_object_name))
    {
        db = restart_db->getDatabase(d_object_name);
    }
    else
    {
        TBOX_ERROR(d_object_name << ":  Restart database corresponding to " << d_object_name
                                 << " not found in restart file." << std::endl);
    }
    int ver = db->getInteger("IB_METHOD_VERSION");
    if (ver != IB_METHOD_VERSION)
    {
        TBOX_ERROR(d_object_name << ":  Restart file version different than class version." << std::endl);
    }
    if (db->keyExists("d_interp_kernel_fcn"))
        d_interp_kernel_fcn = db->getString("d_interp_kernel_fcn");
    else if (db->keyExists("d_interp_delta_fcn"))
        d_interp_kernel_fcn = db->getString("d_interp_delta_fcn");
    if (db->keyExists("d_spread_kernel_fcn"))
        d_spread_kernel_fcn = db->getString("d_spread_kernel_fcn");
    else if (db->keyExists("d_spread_delta_fcn"))
        d_spread_kernel_fcn = db->getString("d_spread_delta_fcn");
    db->getIntegerArray("d_ghosts", &d_ghosts[0], NDIM);
    std::vector<std::string> instrument_names = db->getStringVector("instrument_names");
    IBInstrumentationSpec::setInstrumentNames(instrument_names);
    d_total_flow_volume = db->getDoubleVector("d_total_flow_volume");
    d_n_src = db->getIntegerVector("d_n_src");
    d_X_src.resize(d_n_src.size());
    d_r_src.resize(d_n_src.size());
    d_P_src.resize(d_n_src.size());
    d_Q_src.resize(d_n_src.size());
    d_n_src.resize(d_n_src.size(), 0);
    for (unsigned int ln = 0; ln < d_n_src.size(); ++ln)
    {
        d_X_src[ln].resize(d_n_src[ln], Point::Constant(std::numeric_limits<double>::quiet_NaN()));
        d_r_src[ln].resize(d_n_src[ln], std::numeric_limits<double>::quiet_NaN());
        d_P_src[ln].resize(d_n_src[ln], std::numeric_limits<double>::quiet_NaN());
        d_Q_src[ln].resize(d_n_src[ln], std::numeric_limits<double>::quiet_NaN());
        for (int n = 0; n < d_n_src[ln]; ++n)
        {
            std::ostringstream id_stream;
            id_stream << ln << "_" << n;
            const std::string id_string = id_stream.str();
            db->getDoubleArray("d_X_src_" + id_string, &d_X_src[ln][n][0], NDIM);
            d_r_src[ln][n] = db->getDouble("d_r_src_" + id_string);
            d_P_src[ln][n] = db->getDouble("d_P_src_" + id_string);
            d_Q_src[ln][n] = db->getDouble("d_Q_src_" + id_string);
        }
    }
    d_normalize_source_strength = db->getBool("d_normalize_source_strength");
    return;
}

PetscErrorCode IBMethod::computeForce_SAMRAI(void* ctx, Vec X, Vec F)
{
    PetscErrorCode ierr;
    IBMethod* ib_method_ops = static_cast<IBMethod*>(ctx);
    ierr = ib_method_ops->computeForce(X, F);
    IBTK_CHKERRQ(ierr);
    return ierr;
}

PetscErrorCode IBMethod::computeForce(Vec X, Vec F)
{
    PetscErrorCode ierr;
    const int level_num = d_hierarchy->getFinestLevelNumber();
    ierr = VecSwap(X, d_X_half_data[level_num]->getVec());
    IBTK_CHKERRQ(ierr);
    ierr = VecSwap(F, d_F_half_data[level_num]->getVec());
    IBTK_CHKERRQ(ierr);
    computeLagrangianForce(d_half_time);
    ierr = VecSwap(X, d_X_half_data[level_num]->getVec());
    IBTK_CHKERRQ(ierr);
    ierr = VecSwap(F, d_F_half_data[level_num]->getVec());
    IBTK_CHKERRQ(ierr);
    return ierr;
}

/////////////////////////////// NAMESPACE ////////////////////////////////////

} // namespace IBAMR

//////////////////////////////////////////////////////////////////////////////
