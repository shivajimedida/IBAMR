// Filename: IBKirchhoffRodForceGen.h
// Created on 22 Jun 2010 by Boyce Griffith
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

#ifndef included_IBKirchhoffRodForceGen
#define included_IBKirchhoffRodForceGen

/////////////////////////////// INCLUDES /////////////////////////////////////

#include <stddef.h>
#include <unistd.h>
#include <vector>

#include "ibamr/IBRodForceSpec.h"
#include "petscmat.h"
#include "SAMRAI/tbox/Database.h"

namespace IBTK
{
class LData;
class LDataManager;
} // namespace IBTK
namespace SAMRAI
{
namespace hier
{

class PatchHierarchy;
} // namespace hier
} // namespace SAMRAI

namespace boost
{
template <class T, size_t N>
class array;
} // namespace boost

/////////////////////////////// CLASS DEFINITION /////////////////////////////

namespace IBAMR
{
/*!
 * \brief Class IBKirchhoffRodForceGen computes the forces and torques generated
 * by a collection of linear elements based on Kirchhoff rod theory.
 *
 * \note Class IBKirchhoffRodForceGen DOES NOT correct for periodic
 * displacements of IB points.
 */
class IBKirchhoffRodForceGen
{
public:
    /*!
     * \brief Default constructor.
     */
    IBKirchhoffRodForceGen(const boost::shared_ptr<SAMRAI::tbox::Database>& input_db = NULL);

    /*!
     * \brief Destructor.
     */
    ~IBKirchhoffRodForceGen();

    /*!
     * \brief Setup the data needed to compute the beam forces on the specified
     * level of the patch hierarchy.
     */
    void initializeLevelData(const boost::shared_ptr<SAMRAI::hier::PatchHierarchy>& hierarchy,
                             int level_number,
                             double init_data_time,
                             bool initial_time,
                             IBTK::LDataManager* l_data_manager);

    /*!
     * \brief Compute the curvilinear force and torque generated by the given
     * configuration of the curvilinear mesh.
     *
     * \note Nodal forces and moments computed by this method are \em added to
     * the force and moment vectors.
     */
    void computeLagrangianForceAndTorque(const boost::shared_ptr<IBTK::LData>& F_data,
                                         const boost::shared_ptr<IBTK::LData>& N_data,
                                         const boost::shared_ptr<IBTK::LData>& X_data,
                                         const boost::shared_ptr<IBTK::LData>& D_data,
                                         const boost::shared_ptr<SAMRAI::hier::PatchHierarchy>& hierarchy,
                                         int level_number,
                                         double data_time,
                                         IBTK::LDataManager* l_data_manager);

private:
    /*!
     * \brief Copy constructor.
     *
     * \note This constructor is not implemented and should not be used.
     *
     * \param from The value to copy to this object.
     */
    IBKirchhoffRodForceGen(const IBKirchhoffRodForceGen& from);

    /*!
     * \brief Assignment operator.
     *
     * \note This operator is not implemented and should not be used.
     *
     * \param that The value to assign to this object.
     *
     * \return A reference to this object.
     */
    IBKirchhoffRodForceGen& operator=(const IBKirchhoffRodForceGen& that);

    /*!
     * \brief Read input values, indicated above, from given database.
     *
     * The database pointer may be null.
     */
    void getFromInput(const boost::shared_ptr<SAMRAI::tbox::Database>& db);

    /*!
     * \name Data maintained separately for each level of the patch hierarchy.
     */
    //\{
    std::vector<Mat> d_D_next_mats, d_X_next_mats;
    std::vector<std::vector<int>> d_petsc_curr_node_idxs, d_petsc_next_node_idxs;
    std::vector<std::vector<boost::array<double, IBRodForceSpec::NUM_MATERIAL_PARAMS>>> d_material_params;
    std::vector<bool> d_is_initialized;
    //\}
};
} // namespace IBAMR

//////////////////////////////////////////////////////////////////////////////

#endif //#ifndef included_IBKirchhoffRodForceGen
