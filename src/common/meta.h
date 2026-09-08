//
// Created by ubuntu on 1/18/23.
//

#pragma once
#include <string>
#include <stdint.h>

namespace minigraph
{
    class MetaData {
    public:
        uint64_t num_vertex{0};
        uint64_t num_edge{0};
        uint64_t num_triangle{0};
        uint64_t max_degree{0};
        uint64_t max_offset{0};
        uint64_t max_triangle{0};
        double scheduler_avg_degree{0.0};

        MetaData() = default;
        MetaData(uint64_t _num_vertex, uint64_t _num_edge, uint64_t _num_triangle,
                 uint64_t _max_degree, uint64_t _max_offset, uint64_t _max_triangle,
                 double _scheduler_avg_degree = -1.0):
                num_vertex{_num_vertex}, num_edge{_num_edge}, num_triangle{_num_triangle},
                max_degree{_max_degree}, max_offset{_max_offset}, max_triangle{_max_triangle},
                scheduler_avg_degree{(_scheduler_avg_degree >= 0.0)
                                     ? _scheduler_avg_degree
                                     : ((_num_vertex == 0)
                                        ? 0.0
                                        : static_cast<double>(_num_edge) / static_cast<double>(_num_vertex))}{};

        void save(std::string in_dir);
        void read(std::string in_dir);;
    };
}
