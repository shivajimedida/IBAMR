// Filename: IBEulerianForceFunction.cpp
// Created on 28 Sep 2004 by Boyce Griffith
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

#include <ostream>
#include <string>

#include "SAMRAI/pdat/CellData.h"
#include "SAMRAI/math/HierarchyDataOpsReal.h"
#include "ibamr/IBHierarchyIntegrator.h"
#include "SAMRAI/hier/IntVector.h"
#include "SAMRAI/hier/Patch.h"
#include "SAMRAI/math/PatchCellDataBasicOps.h"
#include "SAMRAI/hier/PatchData.h"
#include "SAMRAI/hier/PatchHierarchy.h"
#include "SAMRAI/hier/PatchLevel.h"
#include "SAMRAI/math/PatchSideDataBasicOps.h"
#include "SAMRAI/pdat/SideData.h"
#include "SAMRAI/hier/Variable.h"
#include "ibamr/namespaces.h" // IWYU pragma: keep
#include "ibtk/CartGridFunction.h"

#include "SAMRAI/tbox/Utilities.h"

/////////////////////////////// NAMESPACE ////////////////////////////////////

namespace IBAMR
{
/////////////////////////////// STATIC ///////////////////////////////////////

////////////////////////////// PUBLIC ///////////////////////////////////////

IBHierarchyIntegrator::IBEulerianForceFunction::IBEulerianForceFunction(const IBHierarchyIntegrator* const ib_solver)
    : CartGridFunction(ib_solver->getName() + "::IBEulerianForceFunction"), d_ib_solver(ib_solver)
{
    // intentionally blank
    return;
}

IBHierarchyIntegrator::IBEulerianForceFunction::~IBEulerianForceFunction()
{
    // intentionally blank
    return;
}

bool IBHierarchyIntegrator::IBEulerianForceFunction::isTimeDependent() const
{
    return true;
}

void IBHierarchyIntegrator::IBEulerianForceFunction::setDataOnPatchHierarchy(
    const int data_idx,
    const boost::shared_ptr<Variable>& var,
    const boost::shared_ptr<PatchHierarchy>& hierarchy,
    const double data_time,
    const bool initial_time,
    const int coarsest_ln_in,
    const int finest_ln_in)
{
    if (initial_time)
    {
        d_ib_solver->d_hier_velocity_data_ops->setToScalar(data_idx, 0.0);
        return;
    }
    if (d_ib_solver->d_body_force_fcn)
    {
        d_ib_solver->d_body_force_fcn->setDataOnPatchHierarchy(data_idx, var, hierarchy, data_time, initial_time,
                                                               coarsest_ln_in, finest_ln_in);
    }
    else
    {
        d_ib_solver->d_hier_velocity_data_ops->setToScalar(data_idx, 0.0);
    }
    const int coarsest_ln = (coarsest_ln_in == -1 ? 0 : coarsest_ln_in);
    const int finest_ln = (finest_ln_in == -1 ? hierarchy->getFinestLevelNumber() : finest_ln_in);
    for (int level_num = coarsest_ln; level_num <= finest_ln; ++level_num)
    {
        setDataOnPatchLevel(data_idx, var, hierarchy->getPatchLevel(level_num), data_time, initial_time);
    }
    return;
}

void IBHierarchyIntegrator::IBEulerianForceFunction::setDataOnPatch(const int data_idx,
                                                                    const boost::shared_ptr<Variable>& /*var*/,
                                                                    const boost::shared_ptr<Patch>& patch,
                                                                    const double /*data_time*/,
                                                                    const bool initial_time,
                                                                    const boost::shared_ptr<PatchLevel>& /*level*/)
{
    auto f_data = patch->getPatchData(data_idx);
    auto f_cc_data = boost::dynamic_pointer_cast<CellData<double>>(f_data);
    auto f_sc_data = boost::dynamic_pointer_cast<SideData<double>>(f_data);
    TBOX_ASSERT(f_cc_data || f_sc_data);
    if (initial_time)
    {
        if (f_cc_data) f_cc_data->fillAll(0.0);
        if (f_sc_data) f_sc_data->fillAll(0.0);
        return;
    }
    auto f_ib_data = patch->getPatchData(d_ib_solver->d_f_idx);
    auto f_ib_cc_data = boost::dynamic_pointer_cast<CellData<double>>(f_ib_data);
    auto f_ib_sc_data = boost::dynamic_pointer_cast<SideData<double>>(f_ib_data);
    TBOX_ASSERT(f_ib_cc_data || f_ib_sc_data);
    TBOX_ASSERT((f_ib_cc_data && f_cc_data) || (f_ib_sc_data && f_sc_data));
    if (f_cc_data)
    {
        PatchCellDataBasicOps<double> patch_ops;
        patch_ops.add(f_cc_data, f_cc_data, f_ib_cc_data, patch->getBox());
    }
    if (f_sc_data)
    {
        PatchSideDataBasicOps<double> patch_ops;
        patch_ops.add(f_sc_data, f_sc_data, f_ib_sc_data, patch->getBox());
    }
    return;
}

/////////////////////////////// PROTECTED ////////////////////////////////////

/////////////////////////////// PRIVATE //////////////////////////////////////

/////////////////////////////// NAMESPACE ////////////////////////////////////

} // namespace IBAMR

//////////////////////////////////////////////////////////////////////////////
