#pragma once

#include "dijkstra.hpp"
#include "server.pb.h"
#include "types.hpp"

#include <cstdint>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

class GraphStore {
public:
    GraphStore() = default;

    void reset();
    bool add_walk(const Walk& walk);
    bool one_to_one(const Location& origin, const Location& destination, uint64_t& result) const;
    bool one_to_all(const Location& origin, uint64_t& result) const;

private:
    struct Node {
        Point point;
        std::unordered_map<uint32_t, EdgeStat> outgoing;
    };

    uint32_t resolve_or_create_node_locked(const Point& point);
    bool resolve_existing_node_locked(const Point& point, uint32_t& node_id) const;
    void add_edge_locked(uint32_t from, uint32_t to, uint32_t length);

    static bool points_match(const Point& a, const Point& b);
    static std::pair<int32_t, int32_t> cell_of(const Point& point);

    Dijkstra::Graph build_graph_snapshot_locked() const;

    mutable std::shared_mutex mutex_;
    std::vector<Node> nodes_;
    std::unordered_map<int64_t, std::vector<uint32_t>> grid_;
};