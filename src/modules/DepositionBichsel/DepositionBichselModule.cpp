/**
 * @file
 * @brief Implementation of Bichsel's straggeling in silicon
 * @remarks Based on code from Daniel Pitzel and - in turn - H. Bichsel and M. Mazziotta.
 * @copyright Copyright (c) 2021 CERN and the Allpix Squared authors.
 * This software is distributed under the terms of the MIT License, copied verbatim in the file "LICENSE.md".
 * In applying this license, CERN does not waive the privileges and immunities granted to it by virtue of its status as an
 * Intergovernmental Organization or submit itself to any jurisdiction.
 */

#include "DepositionBichselModule.hpp"
#include "MazziottaIonizer.hpp"

#include <deque>
#include <fstream>
#include <sstream>
#include <stack>

#include <TCanvas.h>
#include <TH3F.h>

#include "core/messenger/Messenger.hpp"
#include "core/utils/file.h"
#include "core/utils/log.h"
#include "objects/DepositedCharge.hpp"
#include "objects/MCParticle.hpp"

#include "tools/ROOT.h"

using namespace allpix;

DepositionBichselModule::DepositionBichselModule(Configuration& config, Messenger* messenger, GeometryManager* geo_manager)
    : Module(config), geo_manager_(geo_manager), messenger_(messenger) {

    // Seed the random generator with the global seed
    random_generator_.seed(getRandomSeed());

    config_.setDefault("source_position", ROOT::Math::XYZPoint(0., 0., 0.));
    config_.setDefault("source_energy_spread", 0.);
    config_.setDefault("beam_size", 0.);
    config_.setDefault("beam_divergence", ROOT::Math::XYVector(0., 0.));

    config_.setDefault<double>("temperature", 293.15);
    config_.setDefault("delta_energy_cut", 0.009);
    config_.setDefault<bool>("fast", true);

    config_.setDefault<bool>("output_plots", false);
    config_.setDefault<bool>("output_event_displays", false);
    config_.setDefault<bool>("output_plots_align_pixels", false);
    config_.setDefault<double>("output_plots_theta", 0.0f);
    config_.setDefault<double>("output_plots_phi", 0.0f);

    explicit_delta_energy_cut_ = config_.get<double>("delta_energy_cut");
    fast_ = config_.get<bool>("fast");
    output_plots_ = config_.get<bool>("output_plots");
    output_event_displays_ = config_.get<bool>("output_event_displays");

    source_position_ = config_.get<ROOT::Math::XYZPoint>("source_position");
    source_energy_ = config_.get<double>("source_energy");
    source_energy_spread_ = config.get<double>("source_energy_spread");
    beam_direction_ = config_.get<ROOT::Math::XYZVector>("beam_direction");
    if(fabs(beam_direction_.Mag2() - 1.0) > std::numeric_limits<double>::epsilon()) {
        LOG(WARNING) << "Momentum direction is not a unit vector: magnitude is ignored";
    }
    beam_size_ = config_.get<double>("beam_size");
    beam_divergence_ = config_.get<ROOT::Math::XYVector>("beam_divergence");

    // EGAP = GAP ENERGY IN eV
    // EMIN = THRESHOLD ENERGY (ALIG ET AL., PRB22 (1980), 5565)
    auto temperature = config_.get<double>("temperature");
    energy_threshold_ =
        config_.get<double>("energy_threshold", 1.5 * 1.17 - 4.73e-4 * temperature * temperature / (636 + temperature));

    // FIXME make sure particle exists
    particle_type_ = static_cast<ParticleType>(config_.get<unsigned int>("particle_type", 4));

    // Register lookup paths for cross-section and oscillator strength data files:
    if(config_.has("data_paths")) {
        auto extra_paths = config_.getPathArray("data_paths", true);
        data_paths_.insert(data_paths_.end(), extra_paths.begin(), extra_paths.end());
        LOG(TRACE) << "Registered data paths from configuration.";
    }
    if(path_is_directory(ALLPIX_BICHSEL_DATA_DIRECTORY)) {
        data_paths_.emplace_back(ALLPIX_BICHSEL_DATA_DIRECTORY);
        LOG(TRACE) << "Registered data path: " << ALLPIX_BICHSEL_DATA_DIRECTORY;
    }
    const char* data_dirs_env = std::getenv("XDG_DATA_DIRS");
    if(data_dirs_env == nullptr || strlen(data_dirs_env) == 0) {
        data_dirs_env = "/usr/local/share/:/usr/share/:";
    }
    std::vector<std::string> data_dirs = split<std::string>(data_dirs_env, ":");
    for(auto data_dir : data_dirs) {
        if(data_dir.back() != '/') {
            data_dir += "/";
        }

        data_dir += std::string(ALLPIX_PROJECT_NAME) + std::string("/data");
        if(path_is_directory(data_dir)) {
            data_paths_.emplace_back(data_dir);
            LOG(TRACE) << "Registered global data path: " << data_dir;
        }
    }
}

void DepositionBichselModule::init() {

    // Booking histograms:
    if(output_plots_) {
        source_energy = new TH1D("source_energy",
                                 "source energy;energy [MeV];particles",
                                 500,
                                 source_energy_ - 3 * source_energy_spread_,
                                 source_energy_ + 3 * source_energy_spread_);

        for(auto& detector : geo_manager_->getDetectors()) {
            auto model = detector->getModel();
            auto name = detector->getName();
            auto depth = static_cast<int>(Units::convert(model->getSensorSize().z(), "um"));

            auto pitch_x = static_cast<double>(Units::convert(model->getPixelSize().x(), "um"));
            auto pitch_y = static_cast<double>(Units::convert(model->getPixelSize().y(), "um"));

            TDirectory* directory = getROOTDirectory();
            TDirectory* local_directory = directory->mkdir(name.c_str());

            if(local_directory == nullptr) {
                throw RuntimeError("Cannot create or access local ROOT directory for module " + this->getUniqueName());
            }
            local_directory->cd();
            directories[name] = local_directory;

            elvse[name] = new TProfile("elvse", "elastic mfp;log_{10}(E_{kin}[MeV]);elastic mfp [#mum]", 140, -3, 4);
            invse[name] = new TProfile("invse", "inelastic mfp;log_{10}(E_{kin}[MeV]);inelastic mfp [#mum]", 140, -3, 4);

            hstep5[name] = new TH1I("step5", "step length;step length [#mum];steps", 500, 0, 5);
            hstep0[name] = new TH1I("step0", "step length;step length [#mum];steps", 500, 0, 0.05);
            hzz[name] = new TH1I("zz", "z;depth z [#mum];steps", depth, -1 / 2 * depth, depth / 2);

            hde0[name] = new TH1I("de0", "step E loss;step E loss [eV];steps", 200, 0, 200);
            hde1[name] = new TH1I("de1", "step E loss;step E loss [eV];steps", 100, 0, 5000);
            hde2[name] = new TH1I("de2", "step E loss;step E loss [keV];steps", 200, 0, 20);
            hdel[name] = new TH1I("del", "log step E loss;log_{10}(step E loss [eV]);steps", 140, 0, 7);
            htet[name] = new TH1I("tet", "delta emission angle;delta emission angle [deg];inelasic steps", 180, 0, 90);
            hnprim[name] = new TH1I("nprim", "primary eh;primary e-h;scatters", 21, -0.5, 20.5);
            hlogE[name] = new TH1I("logE", "log Eeh;log_{10}(E_{eh}) [eV]);eh", 140, 0, 7);
            hlogn[name] = new TH1I("logn", "log neh;log_{10}(n_{eh});clusters", 80, 0, 4);
            hscat[name] = new TH1I("scat", "elastic scattering angle;scattering angle [deg];elastic steps", 180, 0, 180);
            hncl[name] = new TH1I("ncl", "clusters;e-h clusters;tracks", 4 * depth * 5, 0, 4 * depth * 5);

            double lastbin = source_energy_ < 1.1 ? 1.05 * source_energy_ * 1e3 : 5 * 0.35 * depth; // 350 eV/micron
            htde[name] =
                new TH1I("tde", "sum E loss;sum E loss [keV];tracks / keV", std::max(100, int(lastbin)), 0, int(lastbin));
            htde0[name] = new TH1I("tde0",
                                   "sum E loss, no delta;sum E loss [keV];tracks, no delta",
                                   std::max(100, int(lastbin)),
                                   0,
                                   int(lastbin));
            htde1[name] = new TH1I("tde1",
                                   "sum E loss, with delta;sum E loss [keV];tracks, with delta",
                                   std::max(100, int(lastbin)),
                                   0,
                                   int(lastbin));

            hteh[name] = new TH1I("total_eh",
                                  "total e-h;total charge [ke];tracks",
                                  std::max(100, int(50 * 0.1 * depth)),
                                  0,
                                  std::max(1, int(10 * 0.1 * depth)));
            hq0[name] = new TH1I("q0",
                                 "normal charge;normal charge [ke];tracks",
                                 std::max(100, int(50 * 0.1 * depth)),
                                 0,
                                 std::max(1, int(10 * 0.1 * depth)));
            hrms[name] = new TH1I("rms", "RMS e-h;charge RMS [e];tracks", 100, 0, 50 * depth);

            h2xy[name] = new TH2I("xy",
                                  "x-y eh-pairs;x_{particle} - x_{eh} [#mum];y_{particle} - y_{eh} [#mum];eh-pairs",
                                  static_cast<int>(4 * pitch_x),
                                  -2 * pitch_x,
                                  2 * pitch_x,
                                  static_cast<int>(4 * pitch_y),
                                  -2 * pitch_y,
                                  2 * pitch_y);
            h2zx[name] = new TH2I("zx",
                                  "z-x eh-pairs;z [#mum];x_{particle} - x_{eh} [#mum];eh-pairs",
                                  depth,
                                  -1 / 2 * depth,
                                  depth / 2,
                                  static_cast<int>(4 * pitch_x),
                                  -2 * pitch_x,
                                  2 * pitch_x);
            h2zr[name] = new TH2I("zr",
                                  "z-r eh-pairs;z [#mum];r_{eh} [#mum];eh-pairs",
                                  depth,
                                  -1 / 2 * depth,
                                  depth / 2,
                                  static_cast<int>(4 * sqrt(pitch_x * pitch_x + pitch_y * pitch_y)),
                                  0.,
                                  2 * sqrt(pitch_x * pitch_x + pitch_y * pitch_y));
        }
    }
    // = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
    // INITIALIZE ENERGY BINS

    double u = log(2.0) / N2;
    double um = exp(u);
    int ken = static_cast<int>(log(1839.0 / 1.5) / u);
    double Emin = 1839.0 / pow(2, ken / N2); // integer division intended

    // EMIN is chosen to give an E-value exactly at the K-shell edge, 1839 eV
    E[1] = Emin;
    for(size_t j = 2; j < E.size(); ++j) {
        E[j] = E[j - 1] * um; // must agree with heps.tab
        dE[j - 1] = E[j] - E[j - 1];
    }

    LOG(DEBUG) << std::endl << "  n2 " << N2 << ", Emin " << Emin << ", um " << um << ", E[nume] " << E.back();

    // = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
    // READ DIELECTRIC CONSTANTS
    read_hepstab();

    //= = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
    // READ INTEGRAL OVER MOMENTUM TRANSFER OF THE GENERALIZED OSCILLATOR STRENGTH
    read_macomtab();

    read_emerctab();
}

void DepositionBichselModule::create_output_plots(unsigned int event_num,
                                                  const std::shared_ptr<const Detector>& detector,
                                                  const std::vector<Cluster>& clusters) {
    LOG(TRACE) << "Writing output plots";
    auto model = detector->getModel();
    auto name = detector->getName();

    // Calculate the axis limits
    double minX = FLT_MAX, maxX = FLT_MIN;
    double minY = FLT_MAX, maxY = FLT_MIN;
    for(const auto& point : clusters) {
        minX = std::min(minX, point.position().x());
        maxX = std::max(maxX, point.position().x());

        minY = std::min(minY, point.position().y());
        maxY = std::max(maxY, point.position().y());
    }

    // Compute frame axis sizes if equal scaling is requested
    if(config_.get<bool>("output_plots_use_equal_scaling", true)) {
        double centerX = (minX + maxX) / 2.0;
        double centerY = (minY + maxY) / 2.0;
        minX = centerX - model->getSensorSize().z() / 2.0;
        maxX = centerX + model->getSensorSize().z() / 2.0;

        minY = centerY - model->getSensorSize().z() / 2.0;
        maxY = centerY + model->getSensorSize().z() / 2.0;
    }

    // Align on pixels if requested
    if(config_.get<bool>("output_plots_align_pixels")) {
        double div = minX / model->getPixelSize().x();
        minX = (std::floor(div - 0.5) + 0.5) * model->getPixelSize().x();
        div = minY / model->getPixelSize().y();
        minY = (std::floor(div - 0.5) + 0.5) * model->getPixelSize().y();
        div = maxX / model->getPixelSize().x();
        maxX = (std::ceil(div + 0.5) - 0.5) * model->getPixelSize().x();
        div = maxY / model->getPixelSize().y();
        maxY = (std::ceil(div + 0.5) - 0.5) * model->getPixelSize().y();
    }

    // Use a histogram to create the underlying frame
    auto* histogram_frame = new TH3F(("frame_" + name + "_" + std::to_string(event_num)).c_str(),
                                     "",
                                     100,
                                     minX,
                                     maxX,
                                     100,
                                     minY,
                                     maxY,
                                     100,
                                     model->getSensorCenter().z() - model->getSensorSize().z() / 2.0,
                                     model->getSensorCenter().z() + model->getSensorSize().z() / 2.0);
    histogram_frame->SetDirectory(directories[name]);

    // Create the canvas for the line plot and set orientation
    auto canvas = std::make_unique<TCanvas>(("event_" + std::to_string(event_num)).c_str(),
                                            ("Particle trajectories for event " + std::to_string(event_num)).c_str(),
                                            1280,
                                            1024);
    canvas->cd();
    canvas->SetTheta(config_.get<float>("output_plots_theta") * 180.0f / ROOT::Math::Pi());
    canvas->SetPhi(config_.get<float>("output_plots_phi") * 180.0f / ROOT::Math::Pi());

    for(const auto& point : clusters) {
        histogram_frame->Fill(point.position().X(), point.position().Y(), point.position().Z(), point.ehpairs());
    }

    // Draw the frame on the canvas
    histogram_frame->GetXaxis()->SetTitle((std::string("x ") + "(mm)").c_str());
    histogram_frame->GetYaxis()->SetTitle((std::string("y ") + "(mm)").c_str());
    histogram_frame->GetZaxis()->SetTitle("z (mm)");
    histogram_frame->Draw("BOX2");

    // Draw and write canvas to module output file
    canvas->Draw();
    directories[name]->WriteTObject(canvas.get());
}

void DepositionBichselModule::run(unsigned int event) {
    std::uniform_real_distribution<double> unirnd(0, 1);

    // Add energy spread from Gaussian:
    std::normal_distribution<double> energy_spread(0, source_energy_spread_);
    double particle_energy = source_energy_ + energy_spread(random_generator_);

    if(output_plots_) {
        source_energy->Fill(particle_energy);
    }

    // Lambda for smearing the initial particle position with the beam size
    auto beam_pos_smearing = [&](auto size) {
        double dx = std::normal_distribution<double>(0, size)(random_generator_);
        double dy = std::normal_distribution<double>(0, size)(random_generator_);
        return ROOT::Math::DisplacementVector3D<ROOT::Math::Cartesian3D<double>>(dx, dy, 0);
    };

    // Generate particle position and direction:
    auto particle_position = source_position_ + beam_pos_smearing(beam_size_);
    // FIXME divergence missing
    auto particle_direction = beam_direction_;

    // P is the line origin, D is the unit-length line direction, and e contains the box extents. The box center has been
    // translated to the origin and the line has been translated accordingly.
    // It returns true if an intersection has been found and false if box and line do not intersect or the intersection is
    // not in the direction of the line vector. If an intersection is found, the closest intersection point in the direction
    // of the line vector is stored in the "intersection" parameter.
    auto localTrackEntrance = [&](const std::shared_ptr<const Detector>& detector,
                                  const ROOT::Math::XYZPoint& position_global,
                                  const ROOT::Math::XYZVector& direction_global,
                                  double& distance,
                                  ROOT::Math::XYZPoint& position_local,
                                  ROOT::Math::XYZVector& direction_local) {
        // Obtain total sensor size
        auto sensor = detector->getModel()->getSensorSize();

        // Transformation from locally centered into global coordinate system, consisting of
        // * The rotation into the global coordinate system
        // * The shift from the origin to the detector position
        ROOT::Math::Rotation3D rotation_center(detector->getOrientation());
        ROOT::Math::Translation3D translation_center(static_cast<ROOT::Math::XYZVector>(detector->getPosition()));
        ROOT::Math::Transform3D transform_center(rotation_center, translation_center);
        auto position = transform_center.Inverse()(position_global);

        // Direction vector can directly be rotated
        direction_local = detector->getOrientation().Inverse()(direction_global);

        // Liang–Barsky clipping of a line against faces of a box
        auto clip = [](double denominator, double numerator, double& t0, double& t1) {
            if(denominator > 0) {
                if(numerator > denominator * t1) {
                    return false;
                }
                if(numerator > denominator * t0) {
                    t0 = numerator / denominator;
                }
                return true;
            } else if(denominator < 0) {
                if(numerator > denominator * t0) {
                    return false;
                }
                if(numerator > denominator * t1) {
                    t1 = numerator / denominator;
                }
                return true;
            } else {
                return numerator <= 0;
            }
        };

        // Clip the particle track against the six possible box faces
        double t0 = std::numeric_limits<double>::lowest(), t1 = std::numeric_limits<double>::max();
        bool intersect = clip(direction_local.X(), -position.X() - sensor.X() / 2, t0, t1) &&
                         clip(-direction_local.X(), position.X() - sensor.X() / 2, t0, t1) &&
                         clip(direction_local.Y(), -position.Y() - sensor.Y() / 2, t0, t1) &&
                         clip(-direction_local.Y(), position.Y() - sensor.Y() / 2, t0, t1) &&
                         clip(direction_local.Z(), -position.Z() - sensor.Z() / 2, t0, t1) &&
                         clip(-direction_local.Z(), position.Z() - sensor.Z() / 2, t0, t1);

        // The intersection is a point P + t * D with t = t0. Return if positive (i.e. in direction of track vector)
        if(intersect && t0 > 0) {
            // Transform point from local-centered coordinate system to local coordinate system of the detector
            ROOT::Math::Translation3D translation_local(
                static_cast<ROOT::Math::XYZVector>(detector->getModel()->getCenter()));
            ROOT::Math::Transform3D transform_local(translation_local);
            position_local = transform_local(position + t0 * direction_local);
            distance = t0;
            return true;
        } else {
            // Otherwise: The line does not intersect the box.
            return false;
        }
    };

    LOG(INFO) << "Initial particle position  (global): " << Units::display(particle_position, {"mm", "um"});
    std::deque<Particle> global_particles;
    global_particles.emplace_back(particle_energy, particle_position, particle_direction, particle_type_);

    // Loop until no particle hits any detector anymore:
    while(!global_particles.empty()) {
        LOG(WARNING) << "Have " << global_particles.size() << " more particles to treat";

        // Get one particle:
        auto particle = global_particles.front();
        global_particles.pop_front();

        // Let's get intersection with closest sensor.
        ROOT::Math::XYZPoint position_local;
        ROOT::Math::XYZVector direction_local;
        double distance = std::numeric_limits<double>::max();
        std::shared_ptr<const Detector> detector = nullptr;

        // Check collision for all detectors and get the closest one
        for(const auto& det : geo_manager_->getDetectors()) {
            ROOT::Math::XYZPoint this_position;
            ROOT::Math::XYZVector this_direction;
            double this_distance;
            if(!localTrackEntrance(
                   det, particle.position(), particle.direction(), this_distance, this_position, this_direction)) {
                // No intersection with sensor
                LOG(WARNING) << "Particle has no intersection with sensor of detector " << det->getName();
                continue;
            }

            // This intersection is closer than the previous:
            if(this_distance < distance) {
                LOG(WARNING) << "Found close hit on detector \"" << det->getName() << "\"";
                distance = this_distance;
                position_local = std::move(this_position);
                direction_local = std::move(this_direction);
                detector = det;
            } else {
                LOG(WARNING) << "Hit on detector " << det->getName() << " is further away";
            }
        }

        if(detector == nullptr) {
            LOG(WARNING) << "Particle has no intersection with sensor any detector";
            continue;
        }

        LOG(ERROR) << "Particle enters detector \"" << detector->getName() << "\" at "
                   << Units::display(position_local, {"um", "mm"}) << " (local) / "
                   << Units::display(detector->getGlobalPosition(position_local), {"um", "mm"}) << " (global)";

        Particle incoming(particle_energy, position_local, direction_local, particle_type_);
        auto outgoing = stepping(std::move(incoming), detector, event);

        for(const auto& out : outgoing) {
            LOG(ERROR) << "Particle leaving detector \"" << detector->getName() << "\" at "
                       << Units::display(out.position(), {"um", "mm"}) << " (local) / "
                       << Units::display(detector->getGlobalPosition(out.position()), {"um", "mm"}) << " (global)";
            global_particles.emplace_back(out.E(),
                                          detector->getGlobalPosition(out.position()),
                                          detector->getOrientation()(out.direction()),
                                          out.type());
        }
    }
}

std::deque<Particle> DepositionBichselModule::stepping(Particle primary,
                                                       const std::shared_ptr<const Detector>& detector,
                                                       unsigned int event) { // NOLINT

    std::deque<Particle> incoming;
    incoming.push_back(primary);
    MazziottaIonizer ionizer(&random_generator_);
    std::uniform_real_distribution<double> unirnd(0, 1);

    std::vector<MCParticle> mcparticles;
    std::vector<int> mcparticles_parent_id;
    std::vector<DepositedCharge> charges;

    std::deque<Particle> outgoing;
    std::vector<Cluster> clusters;

    auto name = detector->getName();

    // Statistics:
    unsigned ndelta = 0; // number of deltas generated
    unsigned nsteps = 0; // number of steps for full event
    unsigned nscat = 0;  // elastic scattering
    unsigned nloss = 0;  // ionization
    double total_energy_loss = 0.0;
    unsigned nehpairs = 0;
    unsigned sumeh2 = 0;

    while(!incoming.empty()) {
        double Ekprev = 9e9; // update flag for next delta

        auto particle = incoming.front();
        incoming.pop_front();
        LOG(TRACE) << "Picked up particle of type " << particle.type();

        auto nlast = E.size() - 1;

        double inv_collision_length_inelastic = 1;
        double inv_collision_length_elastic = 1;
        double screening_parameter = 1;
        table totsig;

        LOG(DEBUG) << "  delta " << Units::display(particle.E(), {"keV", "MeV", "GeV"}) << ", cost "
                   << particle.direction().Z() << ", u " << particle.direction().X() << ", v " << particle.direction().Y()
                   << ", z " << particle.position().Z() << " v " << Units::display(particle.velocity(), {"m/s"}) << " t "
                   << Units::display(particle.time(), {"ns", "ps"});

        while(true) { // steps
            LOG(TRACE) << "Stepping...";
            if(particle.E() < 0.9 * Ekprev) { // update
                LOG(TRACE) << "Updating...";
                // Emax = maximum energy loss, see Uehling, also Sternheimer & Peierls Eq.(53)
                double Emax =
                    particle.mass() * (particle.gamma() * particle.gamma() - 1) /
                    (0.5 * particle.mass() / electron_mass_ + 0.5 * electron_mass_ / particle.mass() + particle.gamma());

                // maximum energy loss for incident electrons
                if(particle.type() == ParticleType::ELECTRON) {
                    Emax = 0.5 * particle.E();
                }
                Emax = 1e6 * Emax; // eV

                // Define parameters and calculate Inokuti"s sums,
                // Sect 3.3 in Rev Mod Phys 43, 297 (1971)

                double dec = zi_ * zi_ * atnu_ * fac_ / particle.betasquared();
                double EkeV = particle.E() * 1e6; // [eV]

                // Generate collision spectrum sigma(E) from df/dE, epsilon and AE.
                // sig(*,j) actually is E**2 * sigma(E)

                std::array<double, 6> tsig;
                tsig.fill(0);
                table H;

                // Statistics:
                double stpw = 0;

                std::array<table, 6> sig;
                for(unsigned j = 1; j < E.size(); ++j) {

                    if(E[j] > Emax) {
                        break;
                    }

                    // Eq. (3.1) in RMP and red notebook CCS-33, 39 & 47
                    double Q1 = rydberg_constant_;
                    if(E[j] < 11.9) {
                        Q1 = pow(xkmn[j], 2) * rydberg_constant_;
                    } else if(E[j] < 100.0) {
                        Q1 = pow(0.025, 2) * rydberg_constant_;
                    }

                    double qmin = E[j] * E[j] / (2 * electron_mass_ * 1e6 * particle.betasquared());
                    if(E[j] < 11.9 && Q1 < qmin) {
                        sig[1][j] = 0;
                    } else {
                        sig[1][j] = E[j] * dfdE[j] * log(Q1 / qmin);
                    }
                    // longitudinal excitation, Eq. (46) in Fano; Eq. (2.9) in RMP

                    double epbe = std::max(1 - particle.betasquared() * dielectric_const_real[j], 1e-20); // Fano Eq. (47)
                    double sgg = E[j] * dfdE[j] * (-0.5) *
                                 log(epbe * epbe + pow(particle.betasquared() * dielectric_const_imag[j], 2));

                    double thet = atan(dielectric_const_imag[j] * particle.betasquared() / epbe);
                    if(thet < 0) {
                        thet = thet + M_PI; // plausible-otherwise I"d have a jump
                    }
                    // Fano says [p 21]: "arctan approaches pi for betasq*eps1 > 1 "

                    double sgh = 0.0092456 * E[j] * E[j] * thet *
                                 (particle.betasquared() - dielectric_const_real[j] / (pow(dielectric_const_real[j], 2) +
                                                                                       pow(dielectric_const_imag[j], 2)));

                    sig[2][j] = sgg;
                    sig[3][j] = sgh; // small, negative

                    // uef from  Eqs. 9 & 2 in Uehling, Ann Rev Nucl Sci 4, 315 (1954)
                    double uef = 1 - E[j] * particle.betasquared() / Emax;
                    if(particle.type() == ParticleType::ELECTRON) {
                        uef = 1 + pow(E[j] / (EkeV - E[j]), 2) +
                              pow((particle.gamma() - 1) / particle.gamma() * E[j] / EkeV, 2) -
                              (2 * particle.gamma() - 1) * E[j] / (particle.gamma() * particle.gamma() * (EkeV - E[j]));
                    }
                    // there is a factor of 2 because the integral was over d(lnK) rather than d(lnQ)
                    sig[4][j] = 2 * oscillator_strength_ae[j] * uef;

                    sig[5][j] = 0;
                    for(unsigned i = 1; i <= 4; ++i) {
                        sig[5][j] += sig[i][j]; // sum

                        // divide by E**2 to get the differential collision cross section sigma
                        // Tsig = integrated total collision cross section
                        tsig[i] = tsig[i] + sig[i][j] * dE[j] / (E[j] * E[j]);
                    }                                             // i
                    tsig[5] += sig[5][j] * dE[j] / (E[j] * E[j]); // running sum

                    double HE2 = sig[5][j] * dec;
                    H[j] = HE2 / (E[j] * E[j]);
                    stpw += H[j] * E[j] * dE[j]; // dE/dx
                    nlast = j;
                }
                inv_collision_length_inelastic = tsig[5] * dec; // 1/path

                // Statistics:
                double sst = H[1] * dE[1]; // total cross section (integral)

                totsig[1] = H[1] * dE[1]; // running integral
                for(unsigned j = 2; j <= nlast; ++j) {
                    totsig[j] = totsig[j - 1] + H[j] * dE[j];
                    sst += H[j] * dE[j];
                }

                // NORMALIZE running integral:
                for(unsigned j = 1; j <= nlast; ++j) {
                    totsig[j] /= totsig[nlast]; // norm
                }

                // elastic:
                update_elastic_collision_parameters(inv_collision_length_elastic, screening_parameter, particle);

                if(output_plots_) {
                    elvse[name]->Fill(log(particle.E()) / log(10), 1e4 / inv_collision_length_elastic);
                    invse[name]->Fill(log(particle.E()) / log(10), 1e4 / inv_collision_length_inelastic);
                }

                Ekprev = particle.E();

                LOG(TRACE) << "type " << particle.type() << ", Ekin " << particle.E() * 1e3 << " keV"
                           << ", beta " << sqrt(particle.betasquared()) << ", gam " << particle.gamma() << std::endl
                           << "  Emax " << Emax << ", nlast " << nlast << ", Elast " << E[nlast] << ", norm "
                           << totsig[nlast] << std::endl
                           << "  inelastic " << 1e4 / inv_collision_length_inelastic << "  " << 1e4 / sst << ", elastic "
                           << 1e4 / inv_collision_length_elastic << " um"
                           << ", mean dE " << stpw * detector->getModel()->getSensorSize().Z() * 1e-3 << " keV";
            } // update

            // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
            // step:

            double tlam =
                1 / (inv_collision_length_inelastic + inv_collision_length_elastic); // [cm] TOTAL MEAN FREE PATH (MFP)
            double step = -log(1 - unirnd(random_generator_)) * tlam * 10;           // exponential step length, in MM

            // Update position after step
            particle.step(step);

            if(particle.E() < 1) {
                LOG(TRACE) << "step " << step << ", z " << particle.position().Z();
            }

            if(output_plots_) {
                hstep5[name]->Fill(step);
                hstep0[name]->Fill(step);
                hzz[name]->Fill(particle.position().Z());
            }

            // Outside the sensor
            if(!detector->isWithinSensor(particle.position())) {
                LOG(DEBUG) << "Left the sensor at " << Units::display(particle.position(), {"mm", "um"});
                outgoing.emplace_back(
                    particle.E(), particle.position(), particle.direction(), particle.type(), particle.time());
                break;
            }

            ++nsteps;

            // INELASTIC (ionization) PROCESS
            if(unirnd(random_generator_) > tlam * inv_collision_length_elastic) {
                LOG(TRACE) << "Inelastic scattering";
                ++nloss;

                // GENERATE VIRTUAL GAMMA:
                double yr = unirnd(random_generator_); // inversion method
                unsigned je = 2;
                for(; je <= nlast; ++je) {
                    if(yr < totsig[je]) {
                        break;
                    }
                }

                double energy_gamma = E[je - 1] + (E[je] - E[je - 1]) * unirnd(random_generator_); // [eV]

                if(output_plots_) {
                    hde0[name]->Fill(energy_gamma); // M and L shells
                    hde1[name]->Fill(energy_gamma); // K shell
                    hde2[name]->Fill(energy_gamma * 1e-3);
                    hdel[name]->Fill(log(energy_gamma) / log(10));
                }

                double residual_kin_energy = particle.E() - energy_gamma * 1E-6; // [ MeV]

                // cut off for further movement: [MeV]
                if(residual_kin_energy < explicit_delta_energy_cut_) {
                    energy_gamma = particle.E() * 1E6;                 // [eV]
                    residual_kin_energy = particle.E() - energy_gamma; // zero
                    // LOG(TRACE) << "LAST ENERGY LOSS" << energy_gamma << residual_kin_energy
                }

                total_energy_loss += energy_gamma; // [eV]

                // emission angle from delta:

                // PRIMARY SCATTERING ANGLE:
                // SINT = SQRT(ER*1.E-6/EK) ! Mazziotta = deflection angle
                // COST = SQRT(1.-SINT*SINT)
                // STORE INFORMATION ABOUT DELTA-RAY:
                // SINT = COST ! flip
                // COST = SQRT(1.-SINT**2) ! sqrt( 1 - ER*1e-6 / particle.E() ) ! wrong

                // double cost = sqrt( energy_gamma / (2*electron_mass_ev + energy_gamma) ); // M. Swartz
                double cost = sqrt(energy_gamma / (2 * electron_mass_ * 1e6 + energy_gamma) *
                                   (particle.E() + 2 * electron_mass_) / particle.E());
                // Penelope, Geant4
                double sint = 0;
                if(cost * cost <= 1) {
                    sint = sqrt(1 - cost * cost); // mostly 90 deg
                }
                double phi = 2 * M_PI * unirnd(random_generator_);

                // G4PenelopeIonisationModel.cc

                // rb = kineticEnergy + 2*electron_mass_c2;

                // kineticEnergy1 = kineticEnergy - deltaE;
                // cosThetaPrimary = sqrt( kineticEnergy1 * rb /
                // ( kineticEnergy * ( rb - deltaE ) ) );

                // cosThetaSecondary = sqrt( deltaE * rb /
                // ( kineticEnergy * ( deltaE + 2*electron_mass_c2 ) ) );

                // penelope.f90
                // Energy and scattering angle ( primary electron ).
                // EP = E - DE
                // TME = 2 * ME
                // RB = E + TME
                // CDT = SQRT( EP * RB / ( E * ( RB - DE ) ) )

                // emission angle of the delta ray:
                // CDTS = SQRT( DE * RB / ( E * ( DE + TME ) ) ) // like Geant

                std::vector<double> din{sint * cos(phi), sint * sin(phi), cost};

                if(output_plots_) {
                    htet[name]->Fill(180 / M_PI * asin(sint)); // peak at 90, tail to 45, elastic forward
                }

                // transform into detector system:
                double cz = particle.direction().Z(); // delta direction
                double sz = sqrt(1 - cz * cz);
                double phif = atan2(particle.direction().Y(), particle.direction().X());
                ROOT::Math::XYZVector delta_direction(cz * cos(phif) * din[0] - sin(phif) * din[1] + sz * cos(phif) * din[2],
                                                      cz * sin(phif) * din[0] + cos(phif) * din[1] + sz * sin(phif) * din[2],
                                                      -sz * din[0] + cz * din[2]);

                // GENERATE PRIMARY e-h:
                std::stack<double> veh;
                if(energy_gamma > energy_threshold_) {
                    veh = ionizer.getIonization(energy_gamma);
                }

                if(output_plots_) {
                    hnprim[name]->Fill(static_cast<double>(veh.size()));
                }

                double sumEeh{0};
                unsigned neh{0};

                // PROCESS e and h
                while(!veh.empty()) {
                    double Eeh = veh.top();
                    veh.pop();

                    if(output_plots_) {
                        hlogE[name]->Fill(Eeh > 1 ? log(Eeh) / log(10) : 0);
                    }

                    if(Eeh > explicit_delta_energy_cut_ * 1e6) {
                        // Put new delta in FIFO, use current number of MCParticles as reference to parent:
                        LOG(DEBUG) << "Generated secondary at " << Units::display(particle.position(), {"um", "mm"}) << " t "
                                   << Units::display(particle.time(), {"ns", "ps"});
                        incoming.emplace_back(Eeh * 1E-6,
                                              particle.position(),
                                              delta_direction,
                                              ParticleType::ELECTRON,
                                              particle.time(),
                                              mcparticles.size());

                        ++ndelta;
                        total_energy_loss -= Eeh; // [eV], avoid double counting

                        continue; // next ieh
                    }             // new delta

                    sumEeh += Eeh;

                    // slow down low energy e and h: 95% of CPU time
                    while(!fast_ && Eeh > energy_threshold_) {

                        const double eom0 = 0.063; // phonons
                        const double aaa = 5.2;    // Alig 1980

                        // for e and h
                        double p_ionization =
                            1 / (1 + aaa * 105 / 2 / M_PI * sqrt(Eeh - eom0) / pow(Eeh - energy_threshold_, 3.5));

                        if(unirnd(random_generator_) < p_ionization) { // ionization
                            ++neh;
                            double E1 = gena1() * (Eeh - energy_threshold_);
                            double E2 = gena2() * (Eeh - energy_threshold_ - E1);

                            if(E1 > energy_threshold_) {
                                veh.push(E1);
                            }
                            if(E2 > energy_threshold_) {
                                veh.push(E2);
                            }

                            Eeh = Eeh - E1 - E2 - energy_threshold_;
                        } else {
                            Eeh -= eom0; // phonon emission
                        }
                    } // slow: while Eeh
                }     // while veh

                if(fast_) {
                    std::poisson_distribution<unsigned int> poisson(sumEeh / 3.645);
                    neh = poisson(random_generator_);
                }

                nehpairs += neh;
                sumeh2 += neh * neh;

                LOG(TRACE) << "  dE " << energy_gamma << " eV, neh " << neh;

                // Store charge cluster:
                if(neh > 0) {
                    // Using number of already created MCParticles as reference
                    clusters.emplace_back(neh, particle.position(), mcparticles.size(), particle.time());

                    auto position_x =
                        static_cast<double>(Units::convert(particle.position_start().X() - particle.position().X(), "um"));
                    auto position_y =
                        static_cast<double>(Units::convert(particle.position_start().Y() - particle.position().Y(), "um"));
                    auto position_z = static_cast<double>(Units::convert(particle.position().Z(), "um"));
                    auto position_r = sqrt(position_x * position_x + position_y * position_y);

                    if(output_plots_) {
                        hlogn[name]->Fill(log(neh) / log(10));
                        h2xy[name]->Fill(position_x, position_y, neh);
                        h2zx[name]->Fill(position_z, position_x, neh);
                        h2zr[name]->Fill(position_z, position_r, neh);
                    }
                }

                // Update particle energy
                particle.setE(particle.E() - energy_gamma * 1E-6); // [MeV]

                if(particle.E() < 1) {
                    LOG(TRACE) << "    Ek " << particle.E() * 1e3 << " keV, z " << particle.position().Z() << ", neh " << neh
                               << ", steps " << nsteps << ", ion " << nloss << ", elas " << nscat << ", cl "
                               << clusters.size();
                }

                if(particle.E() < 1E-6 || residual_kin_energy < 1E-6) {
                    LOG(DEBUG) << "Absorbed at " << Units::display(particle.position(), {"mm", "um"});
                    break;
                }

                // For electrons, update elastic cross section at new energy
                if(particle.type() == ParticleType::ELECTRON) {
                    update_elastic_collision_parameters(inv_collision_length_elastic, screening_parameter, particle);
                }

            } else { // ELASTIC SCATTERING: Chaoui 2006
                LOG(TRACE) << "Elastic scattering";
                ++nscat;

                double r = unirnd(random_generator_);
                double cost = 1 - 2 * screening_parameter * r / (2 + screening_parameter - 2 * r);
                double sint = sqrt(1 - cost * cost);
                double phi = 2 * M_PI * unirnd(random_generator_);
                std::vector<double> din{sint * cos(phi), sint * sin(phi), cost};

                if(output_plots_) {
                    hscat[name]->Fill(180 / M_PI * asin(sint)); // forward peak, tail to 90
                }

                // Change direction of particle:
                double cz = particle.direction().Z(); // delta direction
                double sz = sqrt(1.0 - cz * cz);
                double phif = atan2(particle.direction().Y(), particle.direction().X());
                particle.setDirection(
                    ROOT::Math::XYZVector(cz * cos(phif) * din[0] - sin(phif) * din[1] + sz * cos(phif) * din[2],
                                          cz * sin(phif) * din[0] + cos(phif) * din[1] + sz * sin(phif) * din[2],
                                          -sz * din[0] + cz * din[2]));
            } // elastic
        }     // while steps

        // Start and end position of MCParticle:
        auto start_global = detector->getGlobalPosition(particle.position_start());
        auto end_global = detector->getGlobalPosition(particle.position());

        // Finished treating this particle, let's store it:
        // Create MCParticle:
        // FIXME global time missing.
        mcparticles.emplace_back(particle.position_start(),
                                 start_global,
                                 particle.position(),
                                 end_global,
                                 static_cast<std::underlying_type<ParticleType>::type>(particle.type()),
                                 particle.time(),
                                 0.);
        // Store MCParticle ID of the parent particle
        mcparticles_parent_id.push_back(particle.getParentID());
        LOG(DEBUG) << "Generated MCParticle with start " << Units::display(start_global, {"um", "mm"}) << " and end "
                   << Units::display(end_global, {"um", "mm"}) << " in detector " << name;
        LOG(DEBUG) << "                    local start " << Units::display(particle.position_start(), {"um", "mm"})
                   << " and end " << Units::display(particle.position(), {"um", "mm"});
    } // while deltas

    LOG(INFO) << "  steps " << nsteps << ", ion " << nloss << ", elas " << nscat << ", dE " << total_energy_loss * 1e-3
              << " keV"
              << ", eh " << nehpairs << ", cl " << clusters.size();

    if(output_plots_) {
        hncl[name]->Fill(static_cast<double>(clusters.size()));
        htde[name]->Fill(total_energy_loss * 1e-3); // [keV] energy conservation - binding energy
        if(ndelta > 0) {
            htde1[name]->Fill(total_energy_loss * 1e-3); // [keV]
        } else {
            htde0[name]->Fill(total_energy_loss * 1e-3); // [keV]
        }
        hteh[name]->Fill(nehpairs * 1e-3); // [ke]
        hq0[name]->Fill(nehpairs * 1e-3);  // [ke]
        hrms[name]->Fill(sqrt(sumeh2));
    }

    // Only now that we have all MCParticles generated, we can assign the fixed memory address of the parent:
    for(size_t i = 0; i < mcparticles.size(); i++) {
        auto id = static_cast<size_t>(mcparticles_parent_id.at(i));
        if(mcparticles_parent_id.at(i) >= 0) {
            LOG(DEBUG) << "MCParticle at " << &(mcparticles.at(i)) << " has parent ID " << id << ", linking MCParticle at "
                       << &(mcparticles.at(id));
            mcparticles.at(i).setParent(&(mcparticles.at(id)));
        } else {
            LOG(DEBUG) << "MCParticle at " << &(mcparticles.at(i)) << " is a primary particle";
        }
    }

    // Generate deposited charges
    for(const auto& cluster : clusters) {
        auto position_global = detector->getGlobalPosition(cluster.position());

        // FIXME global time missing
        charges.emplace_back(cluster.position(),
                             position_global,
                             CarrierType::ELECTRON,
                             cluster.ehpairs(),
                             cluster.time(),
                             0.,
                             &(mcparticles.at(cluster.particleID())));
        charges.emplace_back(cluster.position(),
                             position_global,
                             CarrierType::HOLE,
                             cluster.ehpairs(),
                             cluster.time(),
                             0.,
                             &(mcparticles.at(cluster.particleID())));
        LOG(TRACE) << "Deposited " << cluster.ehpairs() << " charge carriers of both types at global position "
                   << Units::display(position_global, {"um", "mm"}) << " in detector " << name;
    }

    // Dispatch the messages to the framework
    auto mcparticle_message = std::make_shared<MCParticleMessage>(std::move(mcparticles), detector);
    messenger_->dispatchMessage(this, mcparticle_message);

    auto deposit_message = std::make_shared<DepositedChargeMessage>(std::move(charges), detector);
    messenger_->dispatchMessage(this, deposit_message);

    if(output_event_displays_) {
        create_output_plots(event, detector, clusters);
    }

    LOG(INFO) << outgoing.size() << " particles leaving the sensor";
    return outgoing;
}

void DepositionBichselModule::update_elastic_collision_parameters(double& inv_collision_length_elastic,
                                                                  double& screening_parameter,
                                                                  const Particle& particle) const {
    if(particle.type() == ParticleType::ELECTRON) {
        // screening_parameter = 2*2.61 * pow( atomic_number, 2.0/3.0 ) / EkeV; // Mazziotta
        // Moliere
        screening_parameter = 2 * 2.61 * pow(atomic_number_, 2.0 / 3.0) / (particle.momentum() * particle.momentum()) * 1e-6;
        double E2 = 14.4e-14; // [MeV*cm]
        double FF = 0.5 * M_PI * E2 * E2 * atomic_number_ * atomic_number_ / (particle.E() * particle.E());
        // Elastic total cross section  [cm2/atom]
        double S0EL = 2 * FF / (screening_parameter * (2 + screening_parameter));
        inv_collision_length_elastic = atnu_ * S0EL; // ATNU = N_A * density / A = atoms/cm3
    } else {
        double getot = particle.E() + particle.mass();
        inv_collision_length_elastic =
            std::min(2232.0 * radiation_length * pow(particle.momentum() * particle.momentum() / (getot * zi_), 2),
                     10.0 * radiation_length);
        // units ?
    }
}

std::ifstream DepositionBichselModule::open_data_file(const std::string& file_name) {

    std::string file_path{};
    for(auto& path : data_paths_) {
        // Check if file or directory
        if(allpix::path_is_directory(path)) {
            std::vector<std::string> sub_paths = allpix::get_files_in_directory(path);
            for(auto& sub_path : sub_paths) {
                auto name = allpix::get_file_name_extension(sub_path);

                // Accept only with correct suffix and file name
                std::string suffix(ALLPIX_BICHSEL_DATA_SUFFIX);
                if(name.first != file_name || name.second != suffix) {
                    continue;
                }

                file_path = sub_path;
                break;
            }
        } else {
            // Always a file because paths are already checked
            file_path = path;
            break;
        }
    }

    LOG(TRACE) << "Reading data file " << file_path;
    std::ifstream file(file_path);
    if(file.bad() || !file.is_open()) {
        throw ModuleError("Error opening data file \"" + file_name + "\"");
    }

    return file;
}

void DepositionBichselModule::read_hepstab() {
    auto heps = open_data_file("HEPS");

    std::string line;
    getline(heps, line);
    std::istringstream header(line);

    int n2t = 0;
    size_t numt = 0;
    header >> n2t >> numt;

    LOG(DEBUG) << "HEPS.TAB: n2t " << n2t << ", numt " << numt;
    if(N2 != n2t) {
        LOG(WARNING) << "HEPS: n2 & n2t differ";
    }
    if(E.size() - 1 != numt) {
        LOG(WARNING) << "HEPS: nume & numt differ";
    }
    if(numt > E.size() - 1) {
        numt = E.size() - 1;
    }

    unsigned jt = 1;
    while(!heps.eof() && jt < numt) {
        getline(heps, line);
        std::istringstream tokenizer(line);

        double etbl = NAN, rimt = NAN;
        tokenizer >> jt >> etbl >> dielectric_const_real[jt] >> dielectric_const_imag[jt] >> rimt;

        // The dipole oscillator strength df/dE is calculated, essentially Eq. (2.20)
        dfdE[jt] = rimt * 0.0092456 * E[jt];
    }

    LOG(INFO) << "Read " << jt << " data lines from HEPS.TAB";
    // MAZZIOTTA: 0.0 at 864
    // EP( 2, 864 ) = 0.5 * ( EP(2, 863) + EP(2, 865) )
    // RIM(864) = 0.5 * ( RIM(863) + RIM(865) )
    // DFDE(864) = RIM(864) * 0.0092456 * E(864)
    // DP: fixed in HEPS.TAB
}

void DepositionBichselModule::read_macomtab() {
    auto macom = open_data_file("MACOM");

    std::string line;
    getline(macom, line);
    std::istringstream header(line);

    int n2t = 0;
    size_t numt = 0;
    header >> n2t >> numt;

    auto nume = E.size() - 1;
    LOG(DEBUG) << "MACOM.TAB: n2t " << n2t << ", numt " << numt;
    if(N2 != n2t) {
        LOG(WARNING) << "MACOM: n2 & n2t differ";
    }
    if(nume != numt) {
        LOG(WARNING) << "MACOM: nume & numt differ";
    }
    if(numt > nume) {
        numt = nume;
    }

    unsigned jt = 1;
    while(!macom.eof() && jt < numt) {
        getline(macom, line);
        std::istringstream tokenizer(line);

        double etbl = NAN;
        tokenizer >> jt >> etbl >> oscillator_strength_ae[jt];
    }
    LOG(INFO) << "Read " << jt << " data lines from MACOM.TAB";
}

void DepositionBichselModule::read_emerctab() {
    auto emerc = open_data_file("EMERC");

    std::string line;
    getline(emerc, line); // header lines
    getline(emerc, line);
    getline(emerc, line);
    getline(emerc, line);

    unsigned jt = 1;
    while(!emerc.eof() && jt < 200) {
        getline(emerc, line);
        std::istringstream tokenizer(line);

        double etbl = NAN;
        tokenizer >> jt >> etbl >> oscillator_strength_ae[jt] >> xkmn[jt];
    }
    LOG(INFO) << "Read " << jt << " data lines from EMERC.TAB";
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
double DepositionBichselModule::gena1() {
    std::uniform_real_distribution<double> uniform_dist(0, 1);

    double r1 = 0, r2 = 0, alph1 = 0;
    do {
        r1 = uniform_dist(random_generator_);
        r2 = uniform_dist(random_generator_);
        alph1 = 105. / 16. * (1. - r1) * (1 - r1) * sqrt(r1); // integral = 1, max = 1.8782971
    } while(alph1 > 1.8783 * r2);                             // rejection method

    return r1;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
double DepositionBichselModule::gena2() {
    std::uniform_real_distribution<double> uniform_dist(0, 1);

    double r1 = 0, r2 = 0, alph2 = 0;
    do {
        r1 = uniform_dist(random_generator_);
        r2 = uniform_dist(random_generator_);
        alph2 = 8 / M_PI * sqrt(r1 * (1 - r1));
    } while(alph2 > 1.27324 * r2); // rejection method

    return r1;
}

void DepositionBichselModule::finalize() {
    if(output_plots_) {
        source_energy->Write();

        for(auto& detector : geo_manager_->getDetectors()) {
            auto name = detector->getName();
            directories[name]->cd();

            elvse[name]->Write();
            invse[name]->Write();
            hstep5[name]->Write();
            hstep0[name]->Write();
            hzz[name]->Write();
            hde0[name]->Write();
            hde1[name]->Write();
            hde2[name]->Write();
            hdel[name]->Write();
            htet[name]->Write();
            hnprim[name]->Write();
            hlogE[name]->Write();
            hlogn[name]->Write();
            hscat[name]->Write();
            hncl[name]->Write();
            htde[name]->Write();
            htde0[name]->Write();
            htde1[name]->Write();
            hteh[name]->Write();
            hq0[name]->Write();
            hrms[name]->Write();
            h2xy[name]->SetOption("colz");
            h2xy[name]->Write();
            h2zx[name]->SetOption("colz");
            h2zx[name]->Write();
            h2zr[name]->SetOption("colz");
            h2zr[name]->Write();
        }
    }
}
