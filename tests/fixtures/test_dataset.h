
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

#pragma once

#include <utility>

#include "vsag/dataset.h"
namespace fixtures {

class TestDataset {
public:
    using DatasetPtr = vsag::DatasetPtr;

    TestDataset(DatasetPtr base, DatasetPtr query, DatasetPtr ground_truth)
        : base_(base), query_(query), ground_truth_(ground_truth){};

    DatasetPtr base_{nullptr};
    DatasetPtr query_{nullptr};
    DatasetPtr ground_truth_{nullptr};
};
}  // namespace fixtures
