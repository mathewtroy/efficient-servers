#pragma once

#include <cstdint>
#include <vector>

#include "types.hpp"

class Dijkstra {
public:
    struct Edge {
        uint32_t to;
        uint32_t weight;
    };

    using Graph = std::vector<std::vector<Edge>>;

    static bool shortest_path(
        const Graph& graph,
        uint32_t source,
        uint32_t target,
        uint64_t& result
    );

    static uint64_t sum_of_shortest_paths(
        const Graph& graph,
        uint32_t source
    );
};