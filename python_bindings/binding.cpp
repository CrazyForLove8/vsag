// Copyright 2024-present the vsag project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <filesystem>
#include <fstream>
#include <map>

#include "iostream"
#include "vsag/dataset.h"
#include "vsag/vsag.h"

namespace py = pybind11;

void
SetLoggerOff() {
    vsag::Options::Instance().logger()->SetLevel(vsag::Logger::Level::kOFF);
}

void
SetLoggerInfo() {
    vsag::Options::Instance().logger()->SetLevel(vsag::Logger::Level::kINFO);
}

void
SetLoggerDebug() {
    vsag::Options::Instance().logger()->SetLevel(vsag::Logger::Level::kDEBUG);
}

template <typename T>
static void
writeBinaryPOD(std::ostream& out, const T& podRef) {
    out.write((char*)&podRef, sizeof(T));
}

template <typename T>
static void
readBinaryPOD(std::istream& in, T& podRef) {
    in.read((char*)&podRef, sizeof(T));
}

py::array_t<float>
kmeans(py::array_t<float, py::array::c_style | py::array::forcecast>& datas,
       int clusters,
       const std::string& dis_type) {
    auto data_shape = datas.shape();
    py::ssize_t py_clusters(clusters);
    auto data_size = data_shape[0];
    auto dimension = data_shape[1];
    auto centroids = py::array_t<float>(py::array::ShapeContainer{py_clusters, dimension});
    vsag::kmeans_clustering(
        dimension, data_size, clusters, datas.data(), centroids.mutable_data(), dis_type);
    return centroids;
}

class Index {
public:
    Index(std::string name, const std::string& parameters) {
        if (auto index = vsag::Factory::CreateIndex(name, parameters)) {
            index_ = index.value();
        } else {
            vsag::Error error_code = index.error();
            if (error_code.type == vsag::ErrorType::UNSUPPORTED_INDEX) {
                throw std::runtime_error("error type: UNSUPPORTED_INDEX");
            } else if (error_code.type == vsag::ErrorType::INVALID_ARGUMENT) {
                throw std::runtime_error("error type: invalid_parameter");
            } else {
                throw std::runtime_error("error type: unexpectedError");
            }
        }
    }

public:
    void
    Build(py::array_t<float> vectors, py::array_t<int64_t> ids, size_t num_elements, size_t dim) {
        auto dataset = vsag::Dataset::Make();
        dataset->Owner(false)
            ->Dim(dim)
            ->NumElements(num_elements)
            ->Ids(ids.mutable_data())
            ->Float32Vectors(vectors.mutable_data());
        index_->Build(dataset);
    }

    py::object
    KnnSearch(py::array_t<float> vector, size_t k, std::string& parameters) {
        auto query = vsag::Dataset::Make();
        size_t data_num = 1;
        query->NumElements(data_num)
            ->Dim(vector.size())
            ->Float32Vectors(vector.mutable_data())
            ->Owner(false);

        auto labels = py::array_t<int64_t>(k);
        auto dists = py::array_t<float>(k);
        if (auto result = index_->KnnSearch(query, k, parameters); result.has_value()) {
            auto labels_data = labels.mutable_data();
            auto dists_data = dists.mutable_data();
            auto ids = result.value()->GetIds();
            auto distances = result.value()->GetDistances();
            for (int i = 0; i < data_num * k; ++i) {
                labels_data[i] = ids[i];
                dists_data[i] = distances[i];
            }
        }

        return py::make_tuple(labels, dists);
    }

    py::object
    RangeSearch(py::array_t<float> point, float threshold, std::string& parameters) {
        auto query = vsag::Dataset::Make();
        size_t data_num = 1;
        query->NumElements(data_num)
            ->Dim(point.size())
            ->Float32Vectors(point.mutable_data())
            ->Owner(false);

        py::array_t<int64_t> labels;
        py::array_t<float> dists;
        if (auto result = index_->RangeSearch(query, threshold, parameters); result.has_value()) {
            auto ids = result.value()->GetIds();
            auto distances = result.value()->GetDistances();
            auto k = result.value()->GetDim();
            labels.resize({k});
            dists.resize({k});
            auto labels_data = labels.mutable_data();
            auto dists_data = dists.mutable_data();
            for (int i = 0; i < data_num * k; ++i) {
                labels_data[i] = ids[i];
                dists_data[i] = distances[i];
            }
        }

        return py::make_tuple(labels, dists);
    }

    void
    Save(const std::string& filename) {
        std::fstream file(filename, std::ios::out | std::ios::binary);
        if (auto bs = index_->Serialize(); bs.has_value()) {
            auto keys = bs->GetKeys();
            std::vector<uint64_t> offsets;

            uint64_t offset = 0;
            for (auto key : keys) {
                // [len][data...][len][data...]...
                vsag::Binary b = bs->Get(key);
                writeBinaryPOD(file, b.size);
                file.write((const char*)b.data.get(), b.size);
                offsets.push_back(offset);
                offset += sizeof(b.size) + b.size;
            }
            // footer
            for (uint64_t i = 0; i < keys.size(); ++i) {
                // [len][key...][offset][len][key...][offset]...
                const auto& key = keys[i];
                int64_t len = key.length();
                writeBinaryPOD(file, len);
                file.write(key.c_str(), key.length());
                writeBinaryPOD(file, offsets[i]);
            }
            // [num_keys][footer_offset]$
            writeBinaryPOD(file, keys.size());
            writeBinaryPOD(file, offset);
            file.close();
        } else if (bs.error().type == vsag::ErrorType::NO_ENOUGH_MEMORY) {
            std::cerr << "no enough memory to serialize index" << std::endl;
        }
    }

    void
    Load(const std::string& filename) {
        std::ifstream file(filename, std::ios::in);
        file.seekg(-sizeof(uint64_t) * 2, std::ios::end);
        uint64_t num_keys, footer_offset;
        readBinaryPOD(file, num_keys);
        readBinaryPOD(file, footer_offset);
        file.seekg(footer_offset, std::ios::beg);

        std::vector<std::string> keys;  // todo: mem peak here
        std::vector<uint64_t> offsets;
        for (uint64_t i = 0; i < num_keys; ++i) {
            int64_t key_len;
            readBinaryPOD(file, key_len);
            char key_buf[key_len + 1];
            memset(key_buf, 0, key_len + 1);
            file.read(key_buf, key_len);
            keys.push_back(key_buf);

            uint64_t offset;
            readBinaryPOD(file, offset);
            offsets.push_back(offset);
        }

        vsag::BinarySet bs;
        for (uint64_t i = 0; i < num_keys; ++i) {
            file.seekg(offsets[i], std::ios::beg);
            vsag::Binary b;
            readBinaryPOD(file, b.size);
            b.data.reset(new int8_t[b.size]);
            file.read((char*)b.data.get(), b.size);
            bs.Set(keys[i], b);
        }

        index_->Deserialize(bs);
    }

private:
    std::shared_ptr<vsag::Index> index_;
};

PYBIND11_MODULE(_pyvsag, m) {
    m.def("kmeans", &kmeans, "Kmeans");
    m.def("set_logger_off", &SetLoggerOff, "SetLoggerOff");
    m.def("set_logger_info", &SetLoggerInfo, "SetLoggerInfo");
    m.def("set_logger_debug", &SetLoggerDebug, "SetLoggerDebug");
    py::class_<Index>(m, "Index")
        .def(py::init<std::string, std::string&>(), py::arg("name"), py::arg("parameters"))
        .def("build",
             &Index::Build,
             py::arg("vectors"),
             py::arg("ids"),
             py::arg("num_elements"),
             py::arg("dim"))
        .def(
            "knn_search", &Index::KnnSearch, py::arg("vector"), py::arg("k"), py::arg("parameters"))
        .def("range_search",
             &Index::RangeSearch,
             py::arg("vector"),
             py::arg("threshold"),
             py::arg("parameters"))
        .def("save", &Index::Save, py::arg("filename"))
        .def("load", &Index::Load, py::arg("filename"));
}
