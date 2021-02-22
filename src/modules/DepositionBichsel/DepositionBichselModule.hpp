/**
 * @file
 * @brief Definition of a module to deposit charges using Hans Bichsel's stragelling description
 * @copyright Copyright (c) 2021 CERN and the Allpix Squared authors.
 * This software is distributed under the terms of the MIT License, copied verbatim in the file "LICENSE.md".
 * In applying this license, CERN does not waive the privileges and immunities granted to it by virtue of its status as an
 * Intergovernmental Organization or submit itself to any jurisdiction.
 */

#include <random>

#include "core/module/Module.hpp"

#include <TH1I.h>
#include <TProfile.h>

namespace allpix {
#define HEPS_ENTRIES 1251
#define N2 64

    /**
     * @brief Type of particles
     */
    enum class ParticleType : unsigned int {
        NONE = 0, ///< No particle
        PROTON,
        PION,
        KAON,
        ELECTRON,
        MUON,
        HELIUM,
        LITHIUM,
        CARBON,
        IRON,
    };

    inline std::ostream& operator<<(std::ostream& os, const ParticleType type) {
        os << static_cast<std::underlying_type<ParticleType>::type>(type);
        return os;
    }

    /**
     * @brief Particle
     */
    class Particle {
    public:
        /**
         * Constructor for new particle
         * @param energy        Kinetic energy of the particle
         * @param pos           Position of generation
         * @param dir           Direction of motion
         * @param particle_type Type of particle
         */
        Particle(double energy, ROOT::Math::XYZVector pos, ROOT::Math::XYZVector dir, ParticleType type)
            : position_start_(pos), position_end_(std::move(pos)), direction_(std::move(dir)), energy_(energy), type_(type) {
            update();
        };

        /**
         * Default constructor
         */
        Particle() = default;

        ROOT::Math::XYZVector position() const { return position_end_; }
        void setPosition(ROOT::Math::XYZVector pos) { position_end_ = pos; }

        ROOT::Math::XYZVector direction() const { return direction_; }
        void setDirection(ROOT::Math::XYZVector dir) { direction_ = dir; }

        double E() const { return energy_; }
        void setE(double energy) {
            energy_ = energy;
            update();
        }
        ParticleType type() const { return type_; }

        /**
         * Helper to obtain particle rest mass in units of MeV
         * @return Particle rest mass in MeV
         */
        double mass() const { return mass_.at(static_cast<std::underlying_type<ParticleType>::type>(type_)); };

        double gamma() const { return gamma_; }

        double betasquared() const { return betasquared_; }

        double momentum() const { return momentum_; }

    private:
        ROOT::Math::XYZVector position_start_;
        ROOT::Math::XYZVector position_end_;
        ROOT::Math::XYZVector direction_;

        double energy_;     // [MeV]
        ParticleType type_; // particle type

        void update() {
            gamma_ = energy_ / mass() + 1.0;                // W = total energy / restmass
            double betagamma = sqrt(gamma_ * gamma_ - 1.0); // bg = beta*gamma = p/m
            betasquared_ = betagamma * betagamma / (1 + betagamma * betagamma);
            momentum_ = mass() * betagamma; // [MeV/c]
        }

        double gamma_{};
        double betasquared_{};
        double momentum_{};

        std::vector<double> mass_{
            0,
            938.2723,   // proton
            139.578,    // pion
            493.67,     // K
            0.51099906, // e
            105.65932   // mu
        };
    };

    /**
     * @brief Deposited clusters of electron-hole pairs generated via ionization
     */
    class Cluster {
    public:
        /**
         * @ brief default constructor
         */
        Cluster() = default;

        /**
         * Constructor for e/h pair cluster
         * @param eh_pairs Number of electron-hole pairs
         * @param pos      Position of the cluster in local coordinates
         * @param energy   Energy of the generating particle
         */
        Cluster(int eh_pairs, ROOT::Math::XYZVector pos, double energy) : neh(eh_pairs), position(pos), E(energy){};
        int neh;
        ROOT::Math::XYZVector position;
        double E; // [eV] generating particle
    };

    /**
     * @ingroup Modules
     * @brief Module to deposit charges at predefined positions in the sensor volume
     *
     * This module can deposit charge carriers at defined positions inside the sensitive volume of the detector
     */
    class DepositionBichselModule : public Module {
    public:
        /**
         * @brief Constructor for a module to deposit charges at a specific point in the detector's active sensor volume
         * @param config Configuration object for this module as retrieved from the steering file
         * @param messenger Pointer to the messenger object to allow binding to messages on the bus
         * @param detector Pointer to the detector for this module instance
         */
        DepositionBichselModule(Configuration& config, Messenger* messenger, std::shared_ptr<Detector> detector);

        /**
         * @brief Deposit charge carriers for every simulated event
         */
        void run(unsigned int) override;

        /**
         * @brief Initialize the histograms
         */
        void init() override;

    private:
        std::mt19937_64 random_generator_;
        std::shared_ptr<const Detector> detector_;

        Messenger* messenger_;

        std::vector<Cluster> stepping(const Particle& init, unsigned iev, double depth, unsigned& ndelta);

        using table = std::array<double, HEPS_ENTRIES>;
        table E, dE;
        table dielectric_const_real;
        table dielectric_const_imag;
        table dfdE;
        table oscillator_strength_ae;
        table xkmn;

        // FIXME possible config parameters
        bool fast;
        // delta ray range: 1 um at 10 keV (Mazziotta 2004)
        double explicit_delta_energy_cut_keV_;
        ParticleType particle_type_{};
        double temperature_{};

        // Constants
        const double electron_mass = 0.51099906; // e mass [MeV]
        const double rydberg_constant = 13.6056981;
        const double fac = 8.0 * M_PI * rydberg_constant * rydberg_constant * pow(0.529177e-8, 2) / electron_mass / 1e6;

        // = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
        // silicon:

        const double atomic_number = 14.0;                          // ZA = atomic number of absorber, Si
        const double atomic_weight = 28.086;                        // AW = atomic weight of absorber
        const double density = 2.329;                               // rho= density of absorber material
        const double radiation_length = 9.36;                       // [cm]
        const double atnu = 6.0221367e23 * density / atomic_weight; // atnu = # of atoms per cm**3

        const double eom0 = 0.063; // phonons
        const double aaa = 5.2;    // Alig 1980

        double energy_threshold_{};

        // Histograms:
        TProfile *elvse, *invse;
        TH1I *hstep5, *hstep0, *hzz, *hde0, *hde1, *hde2, *hdel, *htet, *hnprim, *hlogE, *hlogn, *hscat, *hncl, *htde,
            *htde0, *htde1, *hteh, *hq0, *hrms;

        /**
         * Reading HEPS.TAB data file
         *
         * HEPS.TAB is the table of the dielectric constant for solid Si, epsilon = ep(1,j) + i*ep(2,j), as a function of
         * energy loss E(j), section II.E in RMP, and rim is Im(-1/epsilon), Eq. (2.17), p.668. Print statements are included
         * to check that the file is read correctly.
         */
        void read_hepstab();

        /**
         * Reading MACOM.TAB data file
         *
         * MACOM.TAB is the table of the integrals over momentum transfer K of the generalized oscillator strength, summed
         * for all shells, i.e. the A(E) of Eq. (2.11), p. 667 of RMP
         */
        void read_macomtab();

        /**
         * Reading EMERC.TAB data file
         *
         * EMERC.TAB is the table of the integral over K of generalized oscillator strength for E < 11.9 eV with
         * Im(-1/epsilon) from equations in the Appendix of Emerson et al., Phys Rev B7, 1798 (1973) (also see CCS-63)
         */
        void read_emerctab();

        double gena1();
        double gena2();
    };
} // namespace allpix
