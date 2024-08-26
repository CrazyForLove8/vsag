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


#include "vsag/dataset.h"
#include "../simd/simd.h"
#include <vector>
#include <random>
#include <iostream>
#include "../utils.h"
namespace vsag {

struct Node {
     bool old = false;
     uint32_t id;
     float distance;

     Node(uint32_t id, float distance) {
         this->id = id;
         this->distance = distance;
     }

     Node(uint32_t id, float distance, bool old) {
         this->id = id;
         this->distance = distance;
         this->old = old;
     }
     Node(){}

     bool operator<(const Node& other) const {
         return distance < other.distance;
     }

     bool operator==(const Node& other) const {
         return id == other.id;
     }
};

struct Linklist {
    std::vector<Node> neigbors;
};

class Graph {

public:

    Graph(int64_t max_degree, int64_t turn, DistanceFunc distance): max_degree_(max_degree), turn_(turn), distance_(distance) {

    }

    bool Build(const DatasetPtr dataset) {
        if (is_build_) {
            return false;
        }
        is_build_ = true;
        dim_ = dataset->GetDim();
        data_num_ = dataset->GetNumElements();
        data_ = dataset->GetFloat32Vectors();
        init_graph();
        check_turn();
        {
            SlowTaskTimer t("hnsw graph");
            for (int i = 0; i < 10; ++i) {
                update_neighbors();
                check_turn();
                if (i != 9) {
                    add_reverse_edges();
                }
            }
            for (int i = 0; i < data_num_; ++i) {
                reduce_graph(i);
            }
            check_turn();
        }
        return true;
    }

    std::vector<std::vector<uint32_t>>
    GetGraph(){
        std::vector<std::vector<uint32_t>> extract_graph;
        extract_graph.resize(data_num_);
        for (int i = 0; i < data_num_; ++i) {
            extract_graph[i].resize(graph[i].neigbors.size());
            for (int j = 0; j < graph[i].neigbors.size(); ++j) {
                extract_graph[i][j] = graph[i].neigbors[j].id;
            }
        }

        return extract_graph;
    }






private:
    inline float get_distance(uint32_t loc1, uint32_t loc2) {
        return distance_(get_data_by_loc(loc1), get_data_by_loc(loc2), &dim_);
    }

    inline const float* get_data_by_loc(uint32_t loc) {
        return data_ + loc * dim_;
    }

    void init_graph() {
        graph.resize(data_num_);
        std::random_device rd;
        std::uniform_int_distribution<int> k_generate(0, data_num_ - 1);
#pragma omp for
        for (int i = 0; i < data_num_; ++i) {
            std::mt19937 rng(rd());
            for (int j = 0; j < max_degree_; ++j) {
                auto id = k_generate(rng);
                graph[i].neigbors.emplace_back(id, get_distance(i, id));
            }
        }
    }
    
    
    void update_neighbors() {
        for (int i = 0; i < data_num_; ++i) {
            std::vector<Node> old_neighbors;
            {
                graph[i].neigbors.swap(old_neighbors);
            }
            std::sort(old_neighbors.begin(), old_neighbors.end());
            std::vector<Node> new_neighbors;
            uint32_t last_id = -1;
            for (int j = 0; j < old_neighbors.size(); ++j) {
                bool flag = true;
                if (j > 0 && last_id == old_neighbors[j].id) {
                    continue;
                }
                last_id = old_neighbors[j].id;
                for (int k = 0; k < new_neighbors.size(); ++k) {
                    if ((old_neighbors[j].old && new_neighbors[k].old)) {
                        continue;
                    }
                    if (old_neighbors[j].id == new_neighbors[k].id) {
                        break;
                    }
                    float d = get_distance(old_neighbors[j].id, new_neighbors[k].id);
                    if (d < old_neighbors[j].distance) {
                        flag = false;
                        {
                            graph[new_neighbors[k].id].neigbors.emplace_back(old_neighbors[j].id, d);
                        }
                        break;
                    }
                }
                if (flag) {
                    new_neighbors.push_back(old_neighbors[j]);
                }
            }
            for (int j = 0; j < new_neighbors.size(); ++j) {
                new_neighbors[j].old = true;
            }
            {
                graph[i].neigbors.insert(graph[i].neigbors.end(), new_neighbors.begin(), new_neighbors.end());
                reduce_graph(i);
            }
        }
    }


    void add_reverse_edges() {
        std::vector<Linklist> reverse_graph;
        reverse_graph.resize(data_num_);
        for (int i = 0; i < data_num_; ++i) {
            for (int j = 0; j < graph[i].neigbors.size(); ++j) {
                auto& node = graph[i].neigbors[j];
                reverse_graph[node.id].neigbors.emplace_back(i, node.distance);
            }
        }
        for (int i = 0; i < data_num_; ++i) {
            graph[i].neigbors.insert(graph[i].neigbors.end(), reverse_graph[i].neigbors.begin(), reverse_graph[i].neigbors.end());
            reduce_graph(i);
        }
    }

    void reduce_graph(uint32_t loc) {
        std::sort(graph[loc].neigbors.begin(), graph[loc].neigbors.end());
        graph[loc].neigbors.erase(std::unique(graph[loc].neigbors.begin(), graph[loc].neigbors.end()), graph[loc].neigbors.end());
        if (graph[loc].neigbors.size() > max_degree_) {
            graph[loc].neigbors.resize(max_degree_);
        }
    }

    void check_turn() {
        int edge_count = 0;
        float loss = 0;
        for (int i = 0; i < data_num_; ++i) {
            for (int j = 0; j < graph[i].neigbors.size(); ++j) {
                loss += graph[i].neigbors[j].distance;
//                std::cout << graph[i].neigbors[j].distance << " ";
            }
//            std::cout << std::endl;
            edge_count += graph[i].neigbors.size();
        }
        loss /= edge_count;
        std::cout << "loss:" << loss << "  edge_count:" << edge_count << std::endl;
    }

private:
    size_t dim_;
    int64_t data_num_;
    int64_t is_build_ = false;
    const float* data_;

    int64_t max_degree_;
    int64_t turn_;
    std::vector<Linklist> graph;

    DistanceFunc distance_;


};


} // namespace vsag
