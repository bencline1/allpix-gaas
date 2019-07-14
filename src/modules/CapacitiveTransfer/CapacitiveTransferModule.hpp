/*
 * @copyright Copyright (c) 2017-2019 CERN and the Allpix Squared authors.
 * This software is distributed under the terms of the MIT License, copied verbatim in the file "LICENSE.md".
 * In applying this license, CERN does not waive the privileges and immunities granted to it by virtue of its status as an
 * Intergovernmental Organization or submit itself to any jurisdiction.
 */

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <TFile.h>
#include <TGraph.h>
#include <TH1D.h>
#include <TH2D.h>

#include <Eigen/Geometry>

#include "core/config/Configuration.hpp"
#include "core/geometry/GeometryManager.hpp"
#include "core/messenger/Messenger.hpp"
#include "core/module/Event.hpp"
#include "core/module/Module.hpp"

#include "objects/Pixel.hpp"
#include "objects/PixelCharge.hpp"
#include "objects/PropagatedCharge.hpp"

namespace allpix {
    /**
     * @ingroup Modules
     * @brief Module that directly converts propagated charges to charges on a pixel and its neighbours (simulating the
     * cross-coupling in CCPDs)
     * This module is based on the SimpleTransferModule. It does a mapping of the propagated charges to the nearest pixel in
     * the grid and copies this propageted charge, scaled by the cross-coupling matrix, to the neighbouring pixels.
     * The coupling matrix must be provided in the configuration file as a Matrix or as a matrix file.
     * Like the SimpleTransferModule, it only considers propagated charges within a certain distance from the implants and
     * within the pixel grid, charges in the rest of the sensor are ignored.
     * The cross hit created in the neighbouring pixels keeps the history, saving from where the original charge came from.
     */

    class CapacitiveTransferModule : public Module {
    public:
        /**
         * @brief Constructor for this detector-specific module
         * @param config Configuration object for this module as retrieved from the steering file
         * @param messenger Pointer to the messenger object to allow binding to messages on the bus
         * @param detector Pointer to the detector for this module instance
         */
        CapacitiveTransferModule(Configuration& config, Messenger* messenger, std::shared_ptr<Detector> detector);

        /**
         * @brief Initialize the module, creating the cross-coupling matrixs
         */
        void init(std::mt19937_64&) override;

        /**
         * @brief Transfer the propagated charges to the pixels and its neighbours
         */
        void run(Event*) override;

        /**
         * @brief Display statistical summary
         */
        void finalize() override;

    private:
        // Configuration config_;
        Messenger* messenger_;
        std::shared_ptr<Detector> detector_;
        std::shared_ptr<DetectorModel> model_;

        /**
         * @brief Compare two pixels, necessary to store them in the a std::map
         */
        struct pixel_cmp {
            bool operator()(const Pixel::Index& p1, const Pixel::Index& p2) const {
                if(p1.x() == p2.x()) {
                    return p1.y() < p2.y();
                }
                return p1.x() < p2.x();
            }
        };

        // Statistical information
        unsigned int total_transferred_charges_{};
        std::set<Pixel::Index> unique_pixels_;

        // Matrix to store cross-coupling values
        std::vector<std::vector<double>> relative_coupling;
        unsigned int matrix_rows;
        unsigned int matrix_cols;
        unsigned int max_row;
        unsigned int max_col;
        bool cc_in;

        double normalization;
        double nominal_gap;
        double minimum_gap;

        int cross_coupling;

        void getCapacitanceScan(TFile* root_file);
        TGraph* capacitances[9];

        Eigen::Hyperplane<double, 3> plane;

        double center[2] = {0.0, 0.0};
        double angles[2] = {0.0, 0.0};

        TH2D* coupling_map;
        TH2D* gap_map;
        TH2D* capacitance_map;
        TH2D* relative_capacitance_map;
    };
} // namespace allpix
