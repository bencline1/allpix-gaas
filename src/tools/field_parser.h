/**
 * @file
 * @brief Utility to parse INIT-format field files
 * @copyright Copyright (c) 2017 CERN and the Allpix Squared authors.
 * This software is distributed under the terms of the MIT License, copied verbatim in the file "LICENSE.md".
 * In applying this license, CERN does not waive the privileges and immunities granted to it by virtue of its status as an
 * Intergovernmental Organization or submit itself to any jurisdiction.
 */

#ifndef ALLPIX_FIELD_PARSER_H
#define ALLPIX_FIELD_PARSER_H

#include <fstream>
#include <iostream>
#include <map>

#include "core/utils/log.h"
#include "core/utils/unit.h"

#include <cereal/archives/portable_binary.hpp>

#include <cereal/types/array.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

namespace allpix {

    /**
     * @brief Field quantities
     */
    enum class FieldQuantity : size_t {
        UNKNOWN = 0, ///< Unknown fiield quantity
        SCALAR = 1,  ///< Scalar field, i.e. one entry per field position
        VECTOR = 3,  ///< Vector field, i.e. three entries per field position
    };

    /**
     * @brief Type of file formats
     */
    enum class FileType {
        UNKNOWN = 0, ///< Unknown file format
        INIT,        ///< Leagcy file format, values stored in plain-text ASCII
        APF,         ///< Binary Allpix Squared format serialized using the cereal library
    };

    /**
     * Class to hold raw, three-dimensional field data with N components, containing
     * * The actual field data as shared pointer to vector
     * * An array specifying the number of bins in each dimension
     * * An array containing the physical extent of the field in each dimension, as specified in the file
     */
    template <typename T = double> class FieldData {
    public:
        FieldData() = default;
        FieldData(std::string header,
                  std::array<size_t, 3> dimensions,
                  std::array<T, 3> size,
                  std::shared_ptr<std::vector<T>> data)
            : header_(header), dimensions_(dimensions), size_(size), data_(data){};

        std::string getHeader() const { return header_; }
        std::array<size_t, 3> getDimensions() const { return dimensions_; }
        std::array<T, 3> getSize() const { return size_; }
        std::shared_ptr<std::vector<T>> getData() const { return data_; }

    private:
        std::string header_;
        std::array<size_t, 3> dimensions_{};
        std::array<T, 3> size_{};
        std::shared_ptr<std::vector<T>> data_;

        friend class cereal::access;

        template <class Archive> void serialize(Archive& archive) {
            archive(header_);
            archive(dimensions_);
            archive(size_);
            archive(data_);
        }
    };

    /**
     * @brief Class to parse Allpix Squared field data from files
     *
     * This class can be used to deserialize and parse FieldData objects from files of different format. The FieldData
     * objects read from file are cached, and a cache hit will be returned when trying to re-read a file with the same
     * canoncial path.
     */
    template <typename T = double> class FieldParser {
    public:
        /**
         * Construct a FieldParser
         * @param quantity Quantity of individual field points, vector (three values per point) or scalar (one value per
         * point)
         */
        FieldParser(const FieldQuantity quantity) {
            // Store quantity: vector or scalar field:
            N_ = static_cast<std::underlying_type<FieldQuantity>::type>(quantity);
        };
        ~FieldParser() = default;

        /**
         * @brief Parse a file and retrieve the field data.
         * @param file_name  File name (as canonical path) of the input file to be parsed
         * @param file_type  Type of file (file format) to be parsed
         * @param units      Optional units to convert the field from after reading from file. Only used by some formats.
         * @return           Field data object read from file or internal cache
         */
        FieldData<T>
        get_by_file_name(const std::string& file_name, const FileType& file_type, const std::string units = std::string()) {
            // Search in cache (NOTE: the path reached here is always a canonical name)
            auto iter = field_map_.find(file_name);
            if(iter != field_map_.end()) {
                LOG(INFO) << "Using cached field data";
                return iter->second;
            }

            switch(file_type) {
            case FileType::INIT:
                return parse_init_file(file_name, units);
            case FileType::APF:
                if(!units.empty()) {
                    LOG(WARNING) << "Units will be ignored, APF file content is interpreted in internal units.";
                }
                return parse_apf_file(file_name);
            default:
                throw std::runtime_error("unknown file format");
            }
        }

    private:
        /**
         * @brief Function to deserialize FieldData from an APF file, using the cereal library. This does not convert any
         * units, i.e. all values stored in APF files are given framework-internal base units. This includes the field data
         * itself as well as the field size.
         * @param file_name  File name (as canonical path) of the input file to be parsed
         */
        FieldData<T> parse_apf_file(const std::string& file_name) {
            std::ifstream file(file_name, std::ios::binary);
            FieldData<double> field_data;

            // Parse the file with cereal, add manual scope to ensure flushing:
            {
                cereal::PortableBinaryInputArchive archive(file);
                archive(field_data);
            }

            // Check that we have the right number of vector entries
            auto dimensions = field_data.getDimensions();
            if(field_data.getData()->size() != dimensions[0] * dimensions[1] * dimensions[2] * N_) {
                throw std::runtime_error("invalid data");
            }

            return field_data;
        }

        /**
         * @brief Function to read FieldData from INIT-formatted ASCII files. Values are interpreted in the units provided by
         * the argument and converted to the framework-internal base units. The size of the field given in the file is always
         * interpreted as micrometers.
         * @param file_name  File name (as canonical path) of the input file to be parsed
         * @param units      Units to convert the values of the field data from
         */
        FieldData<T> parse_init_file(const std::string& file_name, const std::string units) {
            // Load file
            std::ifstream file(file_name);
            std::string header;
            std::getline(file, header);
            LOG(TRACE) << "Header of file " << file_name << " is " << std::endl << header;

            // Read the header
            std::string tmp;
            file >> tmp >> tmp;        // ignore the init seed and cluster length
            file >> tmp >> tmp >> tmp; // ignore the incident pion direction
            file >> tmp >> tmp >> tmp; // ignore the magnetic field (specify separately)
            double thickness, xpixsz, ypixsz;
            file >> thickness >> xpixsz >> ypixsz;
            thickness = Units::get(thickness, "um");
            xpixsz = Units::get(xpixsz, "um");
            ypixsz = Units::get(ypixsz, "um");
            file >> tmp >> tmp >> tmp >> tmp; // ignore temperature, flux, rhe (?) and new_drde (?)
            size_t xsize, ysize, zsize;
            file >> xsize >> ysize >> zsize;
            file >> tmp;

            if(file.fail()) {
                throw std::runtime_error("invalid data or unexpected end of file");
            }
            auto field = std::make_shared<std::vector<double>>();
            auto vertices = xsize * ysize * zsize;
            field->resize(vertices * N_);

            // Loop through all the field data
            for(size_t i = 0; i < vertices; ++i) {
                if(i % (vertices / 100) == 0) {
                    LOG_PROGRESS(INFO, "read_init") << "Reading field data: " << (100 * i / vertices) << "%";
                }

                if(file.eof()) {
                    throw std::runtime_error("unexpected end of file");
                }

                // Get index of field
                size_t xind, yind, zind;
                file >> xind >> yind >> zind;

                if(file.fail() || xind > xsize || yind > ysize || zind > zsize) {
                    throw std::runtime_error("invalid data");
                }
                xind--;
                yind--;
                zind--;

                // Loop through components of field
                for(size_t j = 0; j < N_; ++j) {
                    double input;
                    file >> input;

                    // Set the field at a position
                    (*field)[xind * ysize * zsize * N_ + yind * zsize * N_ + zind * N_ + j] = Units::get(input, units);
                }
            }
            LOG_PROGRESS(INFO, "read_init") << "Reading field data: finished.";

            FieldData<T> field_data(
                header, std::array<size_t, 3>{{xsize, ysize, zsize}}, std::array<T, 3>{{xpixsz, ypixsz, thickness}}, field);

            // Store the parsed field data for further reference:
            field_map_[file_name] = field_data;
            return field_data;
        }

        size_t N_;
        std::map<std::string, FieldData<T>> field_map_;
    };

    /**
     * @brief Class to write Allpix Squared field data to files
     *
     * This class can be used to serialize FieldData objects into files using different formats. Scalar as well as vector
     * fields are supported.
     */
    template <typename T = double> class FieldWriter {
    public:
        /**
         * @brief Construct a FileWriter
         * @param quantity Quantity of individual field points, vector (three values per point) or scalar (one value per
         * point)
         */
        FieldWriter(const FieldQuantity quantity) {
            // Store quantity: vector or scalar field:
            N_ = static_cast<std::underlying_type<FieldQuantity>::type>(quantity);
        };
        ~FieldWriter() = default;

        /**
         * @brief Write the field to a file
         * @param field_data Field data object to store
         * @param file_name  File name (as canonical path) of the output file to be created
         * @param file_type  Type of file (file format) to be produced
         * @param units      Optional units to convert the field into before writing. Only used by some formats.
         */
        void write_file(const FieldData<T>& field_data,
                        const std::string& file_name,
                        const FileType& file_type,
                        const std::string units = std::string()) {
            auto dimensions = field_data.getDimensions();
            if(field_data.getData()->size() != N_ * dimensions[0] * dimensions[1] * dimensions[2]) {
                throw std::runtime_error("invalid field dimensions");
            }

            switch(file_type) {
            case FileType::INIT:
                write_init_file(field_data, file_name, units);
                break;
            case FileType::APF:
                if(!units.empty()) {
                    LOG(WARNING) << "Units will be ignored, APF file content is written in internal units.";
                }
                write_apf_file(field_data, file_name);
                break;
            default:
                throw std::runtime_error("unknown file format");
            }
        }

    private:
        /**
         * @brief Function to serialize FieldData into an APF file, using the cereal library. This does not convert any
         * units, i.e. all values stored in APF files are given framework-internal base units. This includes the field data
         * itself as well as the field size.
         * @param field_data Field data object to store
         * @param file_name  File name (as canonical path) of the output file to be created
         */
        void write_apf_file(const FieldData<T>& field_data, const std::string& file_name) {
            std::ofstream file(file_name, std::ios::binary);

            // Write the file with cereal:
            cereal::PortableBinaryOutputArchive archive(file);
            archive(field_data);
        }

        /**
         * @brief Function to write FieldData objects out to INIT-formatted ASCII files. Values are converted from the
         * framework-internal base units in which the data is stored in FieldData into the units provided by the units
         * parameter. The size of the field is always converted to micrometers.
         * @param field_data Field data object to store
         * @param file_name  File name (as canonical path) of the output file to be created
         * @param units      Units to convert the values of the field data to.
         */
        void write_init_file(const FieldData<T>& field_data, const std::string& file_name, const std::string units) {
            std::ofstream file(file_name);

            LOG(TRACE) << "Writing INIT file \"" << file_name << "\"";

            // Write INIT file header
            file << field_data.getHeader() << std::endl;  // Header line
            file << "##SEED## ##EVENTS##" << std::endl;   // Unused
            file << "##TURN## ##TILT## 1.0" << std::endl; // Unused
            file << "0.0 0.0 0.0" << std::endl;           // Magnetic field (unused)

            auto size = field_data.getSize();
            file << Units::convert(size[2], "um") << " " << Units::convert(size[0], "um") << " "
                 << Units::convert(size[1], "um") << " "; // Field size: (z, x, y)
            file << "0.0 0.0 0.0 0.0 ";                   // Unused

            auto dimensions = field_data.getDimensions();
            file << dimensions[0] << " " << dimensions[1] << " " << dimensions[2] << " "; // Field grid dimensions (x, y, z)
            file << "0.0" << std::endl;                                                   // Unused

            // Write the data block:
            auto data = field_data.getData();
            auto max_points = data->size() / N_;

            for(size_t xind = 0; xind < dimensions[0]; ++xind) {
                for(size_t yind = 0; yind < dimensions[1]; ++yind) {
                    for(size_t zind = 0; zind < dimensions[2]; ++zind) {
                        // Write field point index
                        file << xind + 1 << " " << yind + 1 << " " << zind + 1;

                        // Vector or scalar field:
                        for(size_t j = 0; j < N_; j++) {
                            file << " " << Units::convert(data->at(xind * dimensions[1] * dimensions[2] * N_ +
                                                                   yind * dimensions[2] * N_ + zind * N_ + j),
                                                          units);
                        }
                        // End this line
                        file << std::endl;
                    }

                    auto curr_point = xind * dimensions[1] * dimensions[2] + yind * dimensions[2];
                    LOG_PROGRESS(INFO, "write_init") << "Writing field data: " << (100 * curr_point / max_points) << "%";
                }
            }
            LOG_PROGRESS(INFO, "write_init") << "Writing field data: finished.";
        }

        size_t N_;
    };
} // namespace allpix

#endif /* ALLPIX_FIELD_PARSER_H */
