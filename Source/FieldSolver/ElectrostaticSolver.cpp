/* Copyright 2019 Remi Lehe
 *
 * This file is part of WarpX.
 *
 * License: BSD-3-Clause-LBNL
 */
#include "WarpX.H"

#include "FieldSolver/ElectrostaticSolver.H"
#include "Parallelization/GuardCellManager.H"
#include "Particles/MultiParticleContainer.H"
#include "Particles/WarpXParticleContainer.H"
#include "Python/WarpX_py.H"
#include "Utils/WarpXAlgorithmSelection.H"
#include "Utils/WarpXConst.H"
#include "Utils/TextMsg.H"
#include "Utils/WarpXUtil.H"
#include "Utils/WarpXProfilerWrapper.H"
#include "Parallelization/WarpXCommUtil.H"

#include <AMReX_Array.H>
#include <AMReX_Array4.H>
#include <AMReX_BLassert.H>
#include <AMReX_Box.H>
#include <AMReX_BoxArray.H>
#include <AMReX_Config.H>
#include <AMReX_DistributionMapping.H>
#include <AMReX_FArrayBox.H>
#include <AMReX_FabArray.H>
#include <AMReX_Geometry.H>
#include <AMReX_GpuControl.H>
#include <AMReX_GpuLaunch.H>
#include <AMReX_GpuQualifiers.H>
#include <AMReX_IndexType.H>
#include <AMReX_IntVect.H>
#include <AMReX_LO_BCTYPES.H>
#include <AMReX_MFIter.H>
#include <AMReX_MLMG.H>
#include <AMReX_MultiFab.H>
#include <AMReX_REAL.H>
#include <AMReX_SPACE.H>
#include <AMReX_Vector.H>
#include <AMReX_MFInterp_C.H>

#include <array>
#include <memory>
#include <string>

using namespace amrex;

void
WarpX::ComputeSpaceChargeField (bool const reset_fields)
{
    WARPX_PROFILE("WarpX::ComputeSpaceChargeField");
    if (reset_fields) {
        // Reset all E and B fields to 0, before calculating space-charge fields
        WARPX_PROFILE("WarpX::ComputeSpaceChargeField::reset_fields");
        for (int lev = 0; lev <= max_level; lev++) {
            for (int comp=0; comp<3; comp++) {
                Efield_fp[lev][comp]->setVal(0);
                Bfield_fp[lev][comp]->setVal(0);
            }
        }
    }

    if (do_electrostatic == ElectrostaticSolverAlgo::LabFrame) {
        AddSpaceChargeFieldLabFrame();
    }
    else {
        // Loop over the species and add their space-charge contribution to E and B.
        // Note that the fields calculated here does not include the E field
        // due to simulation boundary potentials
        for (int ispecies=0; ispecies<mypc->nSpecies(); ispecies++){
            WarpXParticleContainer& species = mypc->GetParticleContainer(ispecies);
            if (species.initialize_self_fields ||
                (do_electrostatic == ElectrostaticSolverAlgo::Relativistic)) {
                AddSpaceChargeField(species);
            }
        }

        // Add the field due to the boundary potentials
        if (do_electrostatic == ElectrostaticSolverAlgo::Relativistic){
            AddBoundaryField();
        }
    }
    // Transfer fields from 'fp' array to 'aux' array.
    // This is needed when using momentum conservation
    // since they are different arrays in that case.
    UpdateAuxilaryData();
    FillBoundaryAux(guard_cells.ng_UpdateAux);

}

/* Compute the potential `phi` by solving the Poisson equation with the
   simulation specific boundary conditions and boundary values, then add the
   E field due to that `phi` to `Efield_fp`.
*/
void
WarpX::AddBoundaryField ()
{
    WARPX_PROFILE("WarpX::AddBoundaryField");

    // Store the boundary conditions for the field solver if they haven't been
    // stored yet
    if (!field_boundary_handler.bcs_set) field_boundary_handler.definePhiBCs();

    // Allocate fields for charge and potential
    const int num_levels = max_level + 1;
    Vector<std::unique_ptr<MultiFab> > rho(num_levels);
    Vector<std::unique_ptr<MultiFab> > phi(num_levels);
    // Use number of guard cells used for local deposition of rho
    const amrex::IntVect ng = guard_cells.ng_depos_rho;
    for (int lev = 0; lev <= max_level; lev++) {
        BoxArray nba = boxArray(lev);
        nba.surroundingNodes();
        rho[lev] = std::make_unique<MultiFab>(nba, DistributionMap(lev), 1, ng);
        rho[lev]->setVal(0.);
        phi[lev] = std::make_unique<MultiFab>(nba, DistributionMap(lev), 1, 1);
        phi[lev]->setVal(0.);
    }

    // Set the boundary potentials appropriately
    setPhiBC(phi);

    // beta is zero for boundaries
    std::array<Real, 3> beta = {0._rt};

    // Compute the potential phi, by solving the Poisson equation
    computePhi( rho, phi, beta, self_fields_required_precision,
                self_fields_absolute_tolerance, self_fields_max_iters,
                self_fields_verbosity );

    // Compute the corresponding electric and magnetic field, from the potential phi.
    computeE( Efield_fp, phi, beta );
    computeB( Bfield_fp, phi, beta );
}

void
WarpX::AddSpaceChargeField (WarpXParticleContainer& pc)
{
    WARPX_PROFILE("WarpX::AddSpaceChargeField");

    // Store the boundary conditions for the field solver if they haven't been
    // stored yet
    if (!field_boundary_handler.bcs_set) field_boundary_handler.definePhiBCs();

#ifdef WARPX_DIM_RZ
    WARPX_ALWAYS_ASSERT_WITH_MESSAGE(n_rz_azimuthal_modes == 1,
                                     "Error: RZ electrostatic only implemented for a single mode");
#endif

    // Allocate fields for charge and potential
    const int num_levels = max_level + 1;
    Vector<std::unique_ptr<MultiFab> > rho(num_levels);
    Vector<std::unique_ptr<MultiFab> > phi(num_levels);
    // Use number of guard cells used for local deposition of rho
    const amrex::IntVect ng = guard_cells.ng_depos_rho;
    for (int lev = 0; lev <= max_level; lev++) {
        BoxArray nba = boxArray(lev);
        nba.surroundingNodes();
        rho[lev] = std::make_unique<MultiFab>(nba, DistributionMap(lev), 1, ng);
        phi[lev] = std::make_unique<MultiFab>(nba, DistributionMap(lev), 1, 1);
        phi[lev]->setVal(0.);
    }

    // Deposit particle charge density (source of Poisson solver)
    bool const local = false;
    bool const reset = true;
    bool const do_rz_volume_scaling = true;
    pc.DepositCharge(rho, local, reset, do_rz_volume_scaling);

    // Get the particle beta vector
    bool const local_average = false; // Average across all MPI ranks
    std::array<Real, 3> beta = pc.meanParticleVelocity(local_average);
    for (Real& beta_comp : beta) beta_comp /= PhysConst::c; // Normalize

    // Compute the potential phi, by solving the Poisson equation
    computePhi( rho, phi, beta, pc.self_fields_required_precision,
                pc.self_fields_absolute_tolerance, pc.self_fields_max_iters,
                pc.self_fields_verbosity );

    // Compute the corresponding electric and magnetic field, from the potential phi
    computeE( Efield_fp, phi, beta );
    computeB( Bfield_fp, phi, beta );

}

void
WarpX::AddSpaceChargeFieldLabFrame ()
{
    WARPX_PROFILE("WarpX::AddSpaceChargeFieldLabFrame");

    // Store the boundary conditions for the field solver if they haven't been
    // stored yet
    if (!field_boundary_handler.bcs_set) field_boundary_handler.definePhiBCs();

#ifdef WARPX_DIM_RZ
    WARPX_ALWAYS_ASSERT_WITH_MESSAGE(n_rz_azimuthal_modes == 1,
                                     "Error: RZ electrostatic only implemented for a single mode");
#endif

    // reset rho_fp before depositing charge density for this step
    for (int lev = 0; lev <= max_level; lev++) {
        rho_fp[lev]->setVal(0.);
    }

    // Deposit particle charge density (source of Poisson solver)
    bool const local = true;
    bool const interpolate_across_levels = false;
    bool const reset = false;
    bool const do_rz_volume_scaling = false;
    for (int ispecies=0; ispecies<mypc->nSpecies(); ispecies++){
        WarpXParticleContainer& species = mypc->GetParticleContainer(ispecies);
        species.DepositCharge(
            rho_fp, local, reset, do_rz_volume_scaling, interpolate_across_levels
        );
    }
#ifdef WARPX_DIM_RZ
    for (int lev = 0; lev <= max_level; lev++) {
        ApplyInverseVolumeScalingToChargeDensity(rho_fp[lev].get(), lev);
    }
#endif
    SyncRho(); // Apply filter, perform MPI exchange, interpolate across levels

    // beta is zero in lab frame
    // Todo: use simpler finite difference form with beta=0
    std::array<Real, 3> beta = {0._rt};

    // set the boundary potentials appropriately
    setPhiBC(phi_fp);

    // Compute the potential phi, by solving the Poisson equation
    if ( IsPythonCallBackInstalled("poissonsolver") ) ExecutePythonCallback("poissonsolver");
    else computePhi( rho_fp, phi_fp, beta, self_fields_required_precision,
                     self_fields_absolute_tolerance, self_fields_max_iters,
                     self_fields_verbosity );

    // Compute the electric field. Note that if an EB is used the electric
    // field will be calculated in the computePhi call.
#ifndef AMREX_USE_EB
    computeE( Efield_fp, phi_fp, beta );
#else
    if ( IsPythonCallBackInstalled("poissonsolver") ) computeE( Efield_fp, phi_fp, beta );
#endif

    // Compute the magnetic field
    computeB( Bfield_fp, phi_fp, beta );
}

/* Compute the potential `phi` by solving the Poisson equation with `rho` as
   a source, assuming that the source moves at a constant speed \f$\vec{\beta}\f$.
   This uses the amrex solver.

   More specifically, this solves the equation
   \f[
       \vec{\nabla}^2 r \phi - (\vec{\beta}\cdot\vec{\nabla})^2 r \phi = -\frac{r \rho}{\epsilon_0}
   \f]

   \param[in] rho The charge density a given species
   \param[out] phi The potential to be computed by this function
   \param[in] beta Represents the velocity of the source of `phi`
   \param[in] required_precision The relative convergence threshold for the MLMG solver
   \param[in] absolute_tolerance The absolute convergence threshold for the MLMG solver
   \param[in] max_iters The maximum number of iterations allowed for the MLMG solver
   \param[in] verbosity The verbosity setting for the MLMG solver
*/
void
WarpX::computePhi (const amrex::Vector<std::unique_ptr<amrex::MultiFab> >& rho,
                   amrex::Vector<std::unique_ptr<amrex::MultiFab> >& phi,
                   std::array<Real, 3> const beta,
                   Real const required_precision,
                   Real absolute_tolerance,
                   int const max_iters,
                   int const verbosity) const
{
#ifdef WARPX_DIM_RZ
    // Create a new geometry with the z coordinate scaled by gamma
    amrex::Real const gamma = std::sqrt(1._rt/(1._rt - beta[2]*beta[2]));

    amrex::Vector<amrex::Geometry> geom_scaled(max_level + 1);
    for (int lev = 0; lev <= max_level; ++lev) {
        const amrex::Geometry & geom_lev = Geom(lev);
        const amrex::Real* current_lo = geom_lev.ProbLo();
        const amrex::Real* current_hi = geom_lev.ProbHi();
        amrex::Real scaled_lo[AMREX_SPACEDIM];
        amrex::Real scaled_hi[AMREX_SPACEDIM];
        scaled_lo[0] = current_lo[0];
        scaled_hi[0] = current_hi[0];
        scaled_lo[1] = current_lo[1]*gamma;
        scaled_hi[1] = current_hi[1]*gamma;
        amrex::RealBox rb = RealBox(scaled_lo, scaled_hi);
        geom_scaled[lev].define(geom_lev.Domain(), &rb);
    }
#endif

    // scale rho appropriately; also determine if rho is zero everywhere
    amrex::Real max_norm_b = 0.0;
    for (int lev=0; lev < rho.size(); lev++){
        rho[lev]->mult(-1._rt/PhysConst::ep0);
        max_norm_b = amrex::max(max_norm_b, rho[lev]->norm0());
    }
    amrex::ParallelDescriptor::ReduceRealMax(max_norm_b);

    bool always_use_bnorm = (max_norm_b > 0);
    if (!always_use_bnorm) {
        if (absolute_tolerance == 0.0) absolute_tolerance = amrex::Real(1e-6);
        WarpX::GetInstance().RecordWarning(
            "ElectrostaticSolver", "Max norm of rho is 0", WarnPriority::low
        );
    }

    LPInfo info;

    for (int lev=0; lev<=finest_level; lev++) {

#ifndef AMREX_USE_EB
#ifdef WARPX_DIM_RZ
        Real const dx = geom_scaled[lev].CellSize(0);
        Real const dz_scaled = geom_scaled[lev].CellSize(1);
        int max_semicoarsening_level = 0;
        int semicoarsening_direction = -1;
        if (dz_scaled > dx) {
            semicoarsening_direction = 1;
            max_semicoarsening_level = static_cast<int>(std::log2(dz_scaled/dx));
        } else if (dz_scaled < dx) {
            semicoarsening_direction = 0;
            max_semicoarsening_level = static_cast<int>(std::log2(dx/dz_scaled));
        }
        if (max_semicoarsening_level > 0) {
            info.setSemicoarsening(true);
            info.setMaxSemicoarseningLevel(max_semicoarsening_level);
            info.setSemicoarseningDirection(semicoarsening_direction);
        }
        // Define the linear operator (Poisson operator)
        MLEBNodeFDLaplacian linop( {geom_scaled[lev]}, {boxArray(lev)}, {DistributionMap(lev)}, info );
        linop.setSigma({0._rt, 1._rt});
#else
        // Set the value of beta
        amrex::Array<amrex::Real,AMREX_SPACEDIM> beta_solver =
#if defined(WARPX_DIM_1D_Z)
            {{ beta[2] }};  // beta_x and beta_z
#elif defined(WARPX_DIM_XZ) || defined(WARPX_DIM_RZ)
            {{ beta[0], beta[2] }};  // beta_x and beta_z
#else
            {{ beta[0], beta[1], beta[2] }};
#endif
        Array<Real,AMREX_SPACEDIM> dx_scaled
            {AMREX_D_DECL(Geom(lev).CellSize(0)/std::sqrt(1._rt-beta_solver[0]*beta_solver[0]),
                          Geom(lev).CellSize(1)/std::sqrt(1._rt-beta_solver[1]*beta_solver[1]),
                          Geom(lev).CellSize(2)/std::sqrt(1._rt-beta_solver[2]*beta_solver[2]))};
        int max_semicoarsening_level = 0;
        int semicoarsening_direction = -1;
        int min_dir = std::distance(dx_scaled.begin(),
                                    std::min_element(dx_scaled.begin(),dx_scaled.end()));
        int max_dir = std::distance(dx_scaled.begin(),
                                    std::max_element(dx_scaled.begin(),dx_scaled.end()));
        if (dx_scaled[max_dir] > dx_scaled[min_dir]) {
            semicoarsening_direction = max_dir;
            max_semicoarsening_level = static_cast<int>
                (std::log2(dx_scaled[max_dir]/dx_scaled[min_dir]));
        }
        if (max_semicoarsening_level > 0) {
            info.setSemicoarsening(true);
            info.setMaxSemicoarseningLevel(max_semicoarsening_level);
            info.setSemicoarseningDirection(semicoarsening_direction);
        }
        MLNodeTensorLaplacian linop( {Geom(lev)}, {boxArray(lev)}, {DistributionMap(lev)}, info );
        linop.setBeta( beta_solver );
#endif
#else
        // With embedded boundary: extract EB info
        MLEBNodeFDLaplacian linop( {Geom(lev)}, {boxArray(lev)}, {DistributionMap(lev)}, info, {&WarpX::fieldEBFactory(lev)});

#ifdef WARPX_DIM_RZ
            linop.setSigma({0._rt, 1._rt});
#else
            // Note: this assumes that the beam is propagating along
            // one of the axes of the grid, i.e. that only *one* of the Cartesian
            // components of `beta` is non-negligible.
            linop.setSigma({AMREX_D_DECL(
                1._rt-beta[0]*beta[0], 1._rt-beta[1]*beta[1], 1._rt-beta[2]*beta[2])});
#endif

        // if the EB potential only depends on time, the potential can be passed
        // as a float instead of a callable
        if (field_boundary_handler.phi_EB_only_t) {
            linop.setEBDirichlet(field_boundary_handler.potential_eb_t(gett_new(0)));
        }
        else linop.setEBDirichlet(field_boundary_handler.getPhiEB(gett_new(0)));

#endif

        // Solve the Poisson equation
        linop.setDomainBC( field_boundary_handler.lobc, field_boundary_handler.hibc );
#ifdef WARPX_DIM_RZ
        linop.setRZ(true);
#endif
        MLMG mlmg(linop);
        mlmg.setVerbose(verbosity);
        mlmg.setMaxIter(max_iters);
        mlmg.setAlwaysUseBNorm(always_use_bnorm);

        // Solve Poisson equation at lev
        mlmg.solve( {phi[lev].get()}, {rho[lev].get()},
                    required_precision, absolute_tolerance );

        // Interpolation from phi[lev] to phi[lev+1]
        // (This provides both the boundary conditions and initial guess for phi[lev+1])
        if (lev < finest_level) {

            // Allocate phi_cp for lev+1
            BoxArray ba = phi[lev+1]->boxArray();
            const IntVect& refratio = refRatio(lev);
            ba.coarsen(refratio);
            const int ncomp = linop.getNComp();
            MultiFab phi_cp(ba, phi[lev+1]->DistributionMap(), ncomp, 1);

            // Copy from phi[lev] to phi_cp (in parallel)
            const amrex::IntVect& ng = IntVect::TheUnitVector();
            const amrex::Periodicity& crse_period = Geom(lev).periodicity();
            WarpXCommUtil::ParallelCopy(phi_cp, *phi[lev], 0, 0, 1, ng, ng, crse_period);

            // Local interpolation from phi_cp to phi[lev+1]
#ifdef AMREX_USE_OMP
#pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
            for (MFIter mfi(*phi[lev+1],TilingIfNotGPU()); mfi.isValid(); ++mfi)
            {
                Array4<Real> const& phi_fp_arr = phi[lev+1]->array(mfi);
                Array4<Real> const& phi_cp_arr = phi_cp.array(mfi);

                Box const& b = mfi.tilebox(phi[lev+1]->ixType().toIntVect());
                amrex::ParallelFor( b,
                [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
                {
                    mf_nodebilin_interp(i, j, k, 0, phi_fp_arr, 0, phi_cp_arr, 0, refratio);
                });
            }

        }

#ifdef AMREX_USE_EB
        // use amrex to directly calculate the electric field since with EB's the
        // simple finite difference scheme in WarpX::computeE sometimes fails
        if (do_electrostatic == ElectrostaticSolverAlgo::LabFrame)
        {
#if defined(WARPX_DIM_1D_Z)
            mlmg.getGradSolution(
                {amrex::Array<amrex::MultiFab*,1>{
                    get_pointer_Efield_fp(lev, 2)
                    }}
            );
#elif defined(WARPX_DIM_XZ) || defined(WARPX_DIM_RZ)
            mlmg.getGradSolution(
                {amrex::Array<amrex::MultiFab*,2>{
                    get_pointer_Efield_fp(lev, 0),get_pointer_Efield_fp(lev, 2)
                    }}
            );
#elif defined(WARPX_DIM_3D)
            mlmg.getGradSolution(
                {amrex::Array<amrex::MultiFab*,3>{
                    get_pointer_Efield_fp(lev, 0),get_pointer_Efield_fp(lev, 1),
                    get_pointer_Efield_fp(lev, 2)
                    }}
            );
            get_pointer_Efield_fp(lev, 1)->mult(-1._rt);
#endif
            get_pointer_Efield_fp(lev, 0)->mult(-1._rt);
            get_pointer_Efield_fp(lev, 2)->mult(-1._rt);
        }
#endif

    }

}


/* \brief Set Dirichlet boundary conditions for the electrostatic solver.

    The given potential's values are fixed on the boundaries of the given
    dimension according to the desired values from the simulation input file,
    boundary.potential_lo and boundary.potential_hi.

   \param[inout] phi The electrostatic potential
   \param[in] idim The dimension for which the Dirichlet boundary condition is set
*/
void
WarpX::setPhiBC( amrex::Vector<std::unique_ptr<amrex::MultiFab>>& phi ) const
{
    // check if any dimension has non-periodic boundary conditions
    if (!field_boundary_handler.has_non_periodic) return;

    // get the boundary potentials at the current time
    amrex::Array<amrex::Real,AMREX_SPACEDIM> phi_bc_values_lo;
    amrex::Array<amrex::Real,AMREX_SPACEDIM> phi_bc_values_hi;
    phi_bc_values_lo[WARPX_ZINDEX] = field_boundary_handler.potential_zlo(gett_new(0));
    phi_bc_values_hi[WARPX_ZINDEX] = field_boundary_handler.potential_zhi(gett_new(0));
#ifndef WARPX_DIM_1D_Z
    phi_bc_values_lo[0] = field_boundary_handler.potential_xlo(gett_new(0));
    phi_bc_values_hi[0] = field_boundary_handler.potential_xhi(gett_new(0));
#endif
#if defined(WARPX_DIM_3D)
    phi_bc_values_lo[1] = field_boundary_handler.potential_ylo(gett_new(0));
    phi_bc_values_hi[1] = field_boundary_handler.potential_yhi(gett_new(0));
#endif

    auto dirichlet_flag = field_boundary_handler.dirichlet_flag;

    // loop over all mesh refinement levels and set the boundary values
    for (int lev=0; lev <= max_level; lev++) {

        amrex::Box domain = Geom(lev).Domain();
        domain.surroundingNodes();

#ifdef AMREX_USE_OMP
#pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
#endif
        for ( MFIter mfi(*phi[lev], TilingIfNotGPU()); mfi.isValid(); ++mfi ) {
            // Extract the potential
            auto phi_arr = phi[lev]->array(mfi);
            // Extract tileboxes for which to loop
            const Box& tb  = mfi.tilebox( phi[lev]->ixType().toIntVect() );

            // loop over dimensions
            for (int idim=0; idim<AMREX_SPACEDIM; idim++){
                // check if neither boundaries in this dimension should be set
                if (!(dirichlet_flag[2*idim] || dirichlet_flag[2*idim+1])) continue;

                // a check can be added below to test if the boundary values
                // are already correct, in which case the ParallelFor over the
                // cells can be skipped

                if (!domain.strictly_contains(tb)) {
                    amrex::ParallelFor( tb,
                        [=] AMREX_GPU_DEVICE (int i, int j, int k) {

                            IntVect iv(AMREX_D_DECL(i,j,k));

                            if (dirichlet_flag[2*idim] && iv[idim] == domain.smallEnd(idim)){
                                phi_arr(i,j,k) = phi_bc_values_lo[idim];
                            }
                            if (dirichlet_flag[2*idim+1] && iv[idim] == domain.bigEnd(idim)) {
                                phi_arr(i,j,k) = phi_bc_values_hi[idim];
                            }

                        } // loop ijk
                    );
                }
            } // idim
    }} // lev & MFIter
}

/* \brief Compute the electric field that corresponds to `phi`, and
          add it to the set of MultiFab `E`.

   The electric field is calculated by assuming that the source that
   produces the `phi` potential is moving with a constant speed \f$\vec{\beta}\f$:
   \f[
    \vec{E} = -\vec{\nabla}\phi + (\vec{\beta}\cdot\vec{\beta})\phi \vec{\beta}
   \f]
   (where the second term represent the term \f$\partial_t \vec{A}\f$, in
    the case of a moving source)

   \param[inout] E Electric field on the grid
   \param[in] phi The potential from which to compute the electric field
   \param[in] beta Represents the velocity of the source of `phi`
*/
void
WarpX::computeE (amrex::Vector<std::array<std::unique_ptr<amrex::MultiFab>, 3> >& E,
                 const amrex::Vector<std::unique_ptr<amrex::MultiFab> >& phi,
                 std::array<amrex::Real, 3> const beta ) const
{
    for (int lev = 0; lev <= max_level; lev++) {

        const Real* dx = Geom(lev).CellSize();

#ifdef AMREX_USE_OMP
#    pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
        for ( MFIter mfi(*phi[lev], TilingIfNotGPU()); mfi.isValid(); ++mfi )
        {
#if defined(WARPX_DIM_3D)
            const Real inv_dx = 1._rt/dx[0];
            const Real inv_dy = 1._rt/dx[1];
            const Real inv_dz = 1._rt/dx[2];
#elif defined(WARPX_DIM_XZ) || defined(WARPX_DIM_RZ)
            const Real inv_dx = 1._rt/dx[0];
            const Real inv_dz = 1._rt/dx[1];
#else
            const Real inv_dz = 1._rt/dx[0];
#endif
#if (AMREX_SPACEDIM >= 2)
            const Box& tbx  = mfi.tilebox( E[lev][0]->ixType().toIntVect() );
#endif
#if defined(WARPX_DIM_3D)
            const Box& tby  = mfi.tilebox( E[lev][1]->ixType().toIntVect() );
#endif
            const Box& tbz  = mfi.tilebox( E[lev][2]->ixType().toIntVect() );

            const auto& phi_arr = phi[lev]->array(mfi);
#if (AMREX_SPACEDIM >= 2)
            const auto& Ex_arr = (*E[lev][0])[mfi].array();
#endif
#if defined(WARPX_DIM_3D)
            const auto& Ey_arr = (*E[lev][1])[mfi].array();
#endif
            const auto& Ez_arr = (*E[lev][2])[mfi].array();

            Real beta_x = beta[0];
            Real beta_y = beta[1];
            Real beta_z = beta[2];

            // Calculate the electric field
            // Use discretized derivative that matches the staggering of the grid.
#if defined(WARPX_DIM_3D)
            amrex::ParallelFor( tbx, tby, tbz,
                [=] AMREX_GPU_DEVICE (int i, int j, int k) {
                    Ex_arr(i,j,k) +=
                        +(beta_x*beta_x-1)*inv_dx*( phi_arr(i+1,j,k)-phi_arr(i,j,k) )
                        +beta_x*beta_y*0.25_rt*inv_dy*(phi_arr(i  ,j+1,k)-phi_arr(i  ,j-1,k)
                                                  + phi_arr(i+1,j+1,k)-phi_arr(i+1,j-1,k))
                        +beta_x*beta_z*0.25_rt*inv_dz*(phi_arr(i  ,j,k+1)-phi_arr(i  ,j,k-1)
                                                  + phi_arr(i+1,j,k+1)-phi_arr(i+1,j,k-1));
                },
                [=] AMREX_GPU_DEVICE (int i, int j, int k) {
                    Ey_arr(i,j,k) +=
                        +beta_y*beta_x*0.25_rt*inv_dx*(phi_arr(i+1,j  ,k)-phi_arr(i-1,j  ,k)
                                                  + phi_arr(i+1,j+1,k)-phi_arr(i-1,j+1,k))
                        +(beta_y*beta_y-1)*inv_dy*( phi_arr(i,j+1,k)-phi_arr(i,j,k) )
                        +beta_y*beta_z*0.25_rt*inv_dz*(phi_arr(i,j  ,k+1)-phi_arr(i,j  ,k-1)
                                                  + phi_arr(i,j+1,k+1)-phi_arr(i,j+1,k-1));
                },
                [=] AMREX_GPU_DEVICE (int i, int j, int k) {
                    Ez_arr(i,j,k) +=
                        +beta_z*beta_x*0.25_rt*inv_dx*(phi_arr(i+1,j,k  )-phi_arr(i-1,j,k  )
                                                  + phi_arr(i+1,j,k+1)-phi_arr(i-1,j,k+1))
                        +beta_z*beta_y*0.25_rt*inv_dy*(phi_arr(i,j+1,k  )-phi_arr(i,j-1,k  )
                                                  + phi_arr(i,j+1,k+1)-phi_arr(i,j-1,k+1))
                        +(beta_z*beta_z-1)*inv_dz*( phi_arr(i,j,k+1)-phi_arr(i,j,k) );
                }
            );
#elif defined(WARPX_DIM_XZ) || defined(WARPX_DIM_RZ)
            amrex::ParallelFor( tbx, tbz,
                [=] AMREX_GPU_DEVICE (int i, int j, int k) {
                    Ex_arr(i,j,k) +=
                        +(beta_x*beta_x-1)*inv_dx*( phi_arr(i+1,j,k)-phi_arr(i,j,k) )
                        +beta_x*beta_z*0.25_rt*inv_dz*(phi_arr(i  ,j+1,k)-phi_arr(i  ,j-1,k)
                                                  + phi_arr(i+1,j+1,k)-phi_arr(i+1,j-1,k));
                },
                [=] AMREX_GPU_DEVICE (int i, int j, int k) {
                    Ez_arr(i,j,k) +=
                        +beta_z*beta_x*0.25_rt*inv_dx*(phi_arr(i+1,j  ,k)-phi_arr(i-1,j  ,k)
                                                  + phi_arr(i+1,j+1,k)-phi_arr(i-1,j+1,k))
                        +(beta_z*beta_z-1)*inv_dz*( phi_arr(i,j+1,k)-phi_arr(i,j,k) );
                }
            );
            ignore_unused(beta_y);
#else
            amrex::ParallelFor( tbz,
                [=] AMREX_GPU_DEVICE (int i, int j, int k) {
                    Ez_arr(i,j,k) +=
                        +(beta_z*beta_z-1)*inv_dz*( phi_arr(i+1,j,k)-phi_arr(i,j,k) );
                }
            );
            ignore_unused(beta_x,beta_y);
#endif
        }
    }
}


/* \brief Compute the magnetic field that corresponds to `phi`, and
          add it to the set of MultiFab `B`.

   The magnetic field is calculated by assuming that the source that
   produces the `phi` potential is moving with a constant speed \f$\vec{\beta}\f$:
   \f[
    \vec{B} = -\frac{1}{c}\vec{\beta}\times\vec{\nabla}\phi
   \f]
   (this represents the term \f$\vec{\nabla} \times \vec{A}\f$, in the case of a moving source)

   \param[inout] E Electric field on the grid
   \param[in] phi The potential from which to compute the electric field
   \param[in] beta Represents the velocity of the source of `phi`
*/
void
WarpX::computeB (amrex::Vector<std::array<std::unique_ptr<amrex::MultiFab>, 3> >& B,
                 const amrex::Vector<std::unique_ptr<amrex::MultiFab> >& phi,
                 std::array<amrex::Real, 3> const beta ) const
{
    // return early if beta is 0 since there will be no B-field
    if ((beta[0] == 0._rt) && (beta[1] == 0._rt) && (beta[2] == 0._rt)) return;

    for (int lev = 0; lev <= max_level; lev++) {

        const Real* dx = Geom(lev).CellSize();

#ifdef AMREX_USE_OMP
#    pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
        for ( MFIter mfi(*phi[lev], TilingIfNotGPU()); mfi.isValid(); ++mfi )
        {
#if defined(WARPX_DIM_3D)
            const Real inv_dx = 1._rt/dx[0];
            const Real inv_dy = 1._rt/dx[1];
            const Real inv_dz = 1._rt/dx[2];
#elif defined(WARPX_DIM_XZ) || defined(WARPX_DIM_RZ)
            const Real inv_dx = 1._rt/dx[0];
            const Real inv_dz = 1._rt/dx[1];
#else
            const Real inv_dz = 1._rt/dx[0];
#endif
            const Box& tbx  = mfi.tilebox( B[lev][0]->ixType().toIntVect() );
            const Box& tby  = mfi.tilebox( B[lev][1]->ixType().toIntVect() );
            const Box& tbz  = mfi.tilebox( B[lev][2]->ixType().toIntVect() );

            const auto& phi_arr = phi[lev]->array(mfi);
            const auto& Bx_arr = (*B[lev][0])[mfi].array();
            const auto& By_arr = (*B[lev][1])[mfi].array();
            const auto& Bz_arr = (*B[lev][2])[mfi].array();

            Real beta_x = beta[0];
            Real beta_y = beta[1];
            Real beta_z = beta[2];

            constexpr Real inv_c = 1._rt/PhysConst::c;

            // Calculate the magnetic field
            // Use discretized derivative that matches the staggering of the grid.
#if defined(WARPX_DIM_3D)
            amrex::ParallelFor( tbx, tby, tbz,
                [=] AMREX_GPU_DEVICE (int i, int j, int k) {
                    Bx_arr(i,j,k) += inv_c * (
                        -beta_y*inv_dz*0.5_rt*(phi_arr(i,j  ,k+1)-phi_arr(i,j  ,k)
                                          + phi_arr(i,j+1,k+1)-phi_arr(i,j+1,k))
                        +beta_z*inv_dy*0.5_rt*(phi_arr(i,j+1,k  )-phi_arr(i,j,k  )
                                          + phi_arr(i,j+1,k+1)-phi_arr(i,j,k+1)));
                },
                [=] AMREX_GPU_DEVICE (int i, int j, int k) {
                    By_arr(i,j,k) += inv_c * (
                        -beta_z*inv_dx*0.5_rt*(phi_arr(i+1,j,k  )-phi_arr(i,j,k  )
                                          + phi_arr(i+1,j,k+1)-phi_arr(i,j,k+1))
                        +beta_x*inv_dz*0.5_rt*(phi_arr(i  ,j,k+1)-phi_arr(i  ,j,k)
                                          + phi_arr(i+1,j,k+1)-phi_arr(i+1,j,k)));
                },
                [=] AMREX_GPU_DEVICE (int i, int j, int k) {
                    Bz_arr(i,j,k) += inv_c * (
                        -beta_x*inv_dy*0.5_rt*(phi_arr(i  ,j+1,k)-phi_arr(i  ,j,k)
                                          + phi_arr(i+1,j+1,k)-phi_arr(i+1,j,k))
                        +beta_y*inv_dx*0.5_rt*(phi_arr(i+1,j  ,k)-phi_arr(i,j  ,k)
                                          + phi_arr(i+1,j+1,k)-phi_arr(i,j+1,k)));
                }
            );
#elif defined(WARPX_DIM_XZ) || defined(WARPX_DIM_RZ)
            amrex::ParallelFor( tbx, tby, tbz,
                [=] AMREX_GPU_DEVICE (int i, int j, int k) {
                    Bx_arr(i,j,k) += inv_c * (
                        -beta_y*inv_dz*( phi_arr(i,j+1,k)-phi_arr(i,j,k) ));
                },
                [=] AMREX_GPU_DEVICE (int i, int j, int k) {
                    By_arr(i,j,k) += inv_c * (
                        -beta_z*inv_dx*0.5_rt*(phi_arr(i+1,j  ,k)-phi_arr(i,j  ,k)
                                          + phi_arr(i+1,j+1,k)-phi_arr(i,j+1,k))
                        +beta_x*inv_dz*0.5_rt*(phi_arr(i  ,j+1,k)-phi_arr(i  ,j,k)
                                          + phi_arr(i+1,j+1,k)-phi_arr(i+1,j,k)));
                },
                [=] AMREX_GPU_DEVICE (int i, int j, int k) {
                    Bz_arr(i,j,k) += inv_c * (
                        +beta_y*inv_dx*( phi_arr(i+1,j,k)-phi_arr(i,j,k) ));
                }
            );
#else
            amrex::ParallelFor( tbx, tby,
                [=] AMREX_GPU_DEVICE (int i, int j, int k) {
                    Bx_arr(i,j,k) += inv_c * (
                        -beta_y*inv_dz*( phi_arr(i+1,j,k)-phi_arr(i,j,k) ));
                },
                [=] AMREX_GPU_DEVICE (int i, int j, int k) {
                    By_arr(i,j,k) += inv_c * (
                        +beta_x*inv_dz*(phi_arr(i+1,j,k)-phi_arr(i,j,k)));
                }
            );
            ignore_unused(beta_z,tbz,Bz_arr);
#endif
        }
    }
}

void ElectrostaticSolver::BoundaryHandler::definePhiBCs ( )
{
    int dim_start = 0;
#ifdef WARPX_DIM_RZ
    WarpX& warpx = WarpX::GetInstance();
    auto geom = warpx.Geom(0);
    if (geom.ProbLo(0) == 0){
        lobc[0] = LinOpBCType::Neumann;
        dirichlet_flag[0] = false;
        dim_start = 1;

        // handle the r_max boundary explicity
        if (WarpX::field_boundary_hi[0] == FieldBoundaryType::PEC) {
            hibc[0] = LinOpBCType::Dirichlet;
            dirichlet_flag[1] = true;
        }
        else if (WarpX::field_boundary_hi[0] == FieldBoundaryType::None) {
            hibc[0] = LinOpBCType::Neumann;
            dirichlet_flag[1] = false;
        }
    }
#endif
    for (int idim=dim_start; idim<AMREX_SPACEDIM; idim++){
        if ( WarpX::field_boundary_lo[idim] == FieldBoundaryType::Periodic
             && WarpX::field_boundary_hi[idim] == FieldBoundaryType::Periodic ) {
            lobc[idim] = LinOpBCType::Periodic;
            hibc[idim] = LinOpBCType::Periodic;
            dirichlet_flag[idim*2] = false;
            dirichlet_flag[idim*2+1] = false;
        }
        else {
            has_non_periodic = true;
            if ( WarpX::field_boundary_lo[idim] == FieldBoundaryType::PEC ) {
                lobc[idim] = LinOpBCType::Dirichlet;
                dirichlet_flag[idim*2] = true;
            }
            else if ( WarpX::field_boundary_lo[idim] == FieldBoundaryType::None ) {
                lobc[idim] = LinOpBCType::Neumann;
                dirichlet_flag[idim*2] = false;
            }
            else {
                WARPX_ALWAYS_ASSERT_WITH_MESSAGE(false,
                    "Field boundary conditions have to be either periodic, PEC or none "
                    "when using the electrostatic solver"
                );
            }

            if ( WarpX::field_boundary_hi[idim] == FieldBoundaryType::PEC ) {
                hibc[idim] = LinOpBCType::Dirichlet;
                dirichlet_flag[idim*2+1] = true;
            }
            else if ( WarpX::field_boundary_hi[idim] == FieldBoundaryType::None ) {
                hibc[idim] = LinOpBCType::Neumann;
                dirichlet_flag[idim*2+1] = false;
            }
            else {
                WARPX_ALWAYS_ASSERT_WITH_MESSAGE(false,
                    "Field boundary conditions have to be either periodic, PEC or none "
                    "when using the electrostatic solver"
                );
            }
        }
    }
    bcs_set = true;
}

void ElectrostaticSolver::BoundaryHandler::buildParsers ()
{
    potential_xlo_parser = makeParser(potential_xlo_str, {"t"});
    potential_xhi_parser = makeParser(potential_xhi_str, {"t"});
    potential_ylo_parser = makeParser(potential_ylo_str, {"t"});
    potential_yhi_parser = makeParser(potential_yhi_str, {"t"});
    potential_zlo_parser = makeParser(potential_zlo_str, {"t"});
    potential_zhi_parser = makeParser(potential_zhi_str, {"t"});
    potential_eb_parser = makeParser(potential_eb_str, {"x", "y", "z", "t"});

    potential_xlo = potential_xlo_parser.compile<1>();
    potential_xhi = potential_xhi_parser.compile<1>();
    potential_ylo = potential_ylo_parser.compile<1>();
    potential_yhi = potential_yhi_parser.compile<1>();
    potential_zlo = potential_zlo_parser.compile<1>();
    potential_zhi = potential_zhi_parser.compile<1>();

    // check if the EB potential is a function of space or only of time
    std::set<std::string> eb_symbols = potential_eb_parser.symbols();
    if ((eb_symbols.count("x") != 0) || (eb_symbols.count("y") != 0)
            || (eb_symbols.count("z") != 0)) {
        potential_eb = potential_eb_parser.compile<4>();
        phi_EB_only_t = false;
    }
    else {
        potential_eb_parser = makeParser(potential_eb_str, {"t"});
        potential_eb_t = potential_eb_parser.compile<1>();
    }
}
