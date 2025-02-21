#include "ParticleDiag.H"

#include "Diagnostics/ParticleDiag/ParticleDiag.H"
#include "Particles/WarpXParticleContainer.H"
#include "Utils/TextMsg.H"
#include "Utils/WarpXUtil.H"
#include "WarpX.H"

#include <AMReX_ParmParse.H>

#include <map>
#include <vector>

using namespace amrex;

ParticleDiag::ParticleDiag(std::string diag_name, std::string name, WarpXParticleContainer* pc, PinnedMemoryParticleContainer* pinned_pc)
    : m_diag_name(diag_name), m_name(name), m_pc(pc), m_pinned_pc(pinned_pc)
{
    ParmParse pp_diag_name_species_name(diag_name + "." + name);
    if (!pp_diag_name_species_name.queryarr("variables", variables)){
        variables = {"ux", "uy", "uz", "w"};
    }

    //variable to set plot_flags size
    int plot_flag_size = PIdx::nattribs;
    if(WarpX::do_back_transformed_diagnostics && m_pc->doBackTransformedDiagnostics())
        // Also output old values for position and momenta
        plot_flag_size += 6;

#ifdef WARPX_QED
    if(m_pc->DoQED()){
        // plot_flag will have an entry for the optical depth
        plot_flag_size++;
    }
#endif

    // Set plot_flag to 0 for all attribs
    plot_flags.resize(plot_flag_size, 0);

    // If not none, set plot_flags values to 1 for elements in variables.
    if (variables[0] != "none"){
        for (const auto& var : variables){
            // The string "rho" is needed to dump rho per species, which is generated
            // on the fly from existing species variables. Hence, "rho" is not part
            // of the species' PIdx variables.
            if (var != "rho") {
                // Return error if var not in PIdx.
                WARPX_ALWAYS_ASSERT_WITH_MESSAGE(
                    ParticleStringNames::to_index.count(var),
                    "variables argument '" + var
                    +"' not in ParticleStringNames");
                plot_flags[ParticleStringNames::to_index.at(var)] = 1;
            }
        }
    }

#ifdef WARPX_DIM_RZ
    // Always write out theta, whether or not it's requested,
    // to be consistent with always writing out r and z.
    // TODO: openPMD does a reconstruction to Cartesian, so we can now skip force-writing this
    plot_flags[ParticleStringNames::to_index.at("theta")] = 1;
#endif

#ifdef WARPX_QED
    if(m_pc->DoQED()){
        // TODO: cleaner handling of particle attributes. This should probably be done in
        // the particle container classes, either by adding "opt_depth_BW" to ParticleStringNames
        // or by using a std::map for particle attributes.
        // Optical depths is always plotted if QED is on.
        plot_flags[plot_flag_size-1] = 1;
    }
#endif

    // build filter functors
    m_do_random_filter = queryWithParser(pp_diag_name_species_name, "random_fraction",
                                                                    m_random_fraction);
    m_do_uniform_filter = queryWithParser(pp_diag_name_species_name, "uniform_stride",
                                                                     m_uniform_stride);
    std::string buf;
    m_do_parser_filter = pp_diag_name_species_name.query("plot_filter_function(t,x,y,z,ux,uy,uz)",
                                                         buf);

    if (m_do_parser_filter) {
        std::string function_string = "";
        Store_parserString(pp_diag_name_species_name,"plot_filter_function(t,x,y,z,ux,uy,uz)",
                           function_string);
        m_particle_filter_parser = std::make_unique<amrex::Parser>(
            makeParser(function_string,{"t","x","y","z","ux","uy","uz"}));
    }
}
