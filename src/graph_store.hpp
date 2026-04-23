#pragma once

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
    struct Edge {
        uint32_t to;
        uint32_t weight_sum;
        uint32_t sample_count;

        [[nodiscard]] uint32_t average_weight() const noexcept {
            return sample_count == 0 ? 0U : weight_sum / sample_count;
        }
    };

    struct Node {
        Point point;
        std::vector<Edge> outgoing;
    };

    struct Snapshot {
        struct SnapEdge {
            uint32_t to;
            uint32_t weight; 
        };
        std::vector<std::vector<SnapEdge>> adj;

        std::size_t size() const noexcept { return adj.size(); }
    };

    uint32_t resolve_or_create_node_locked(const Point& point);
    bool resolve_existing_node_locked(const Point& point, uint32_t& node_id) const;
    void add_edge_locked(uint32_t from, uint32_t to, uint32_t length);

    static bool points_match(const Point& a, const Point& b);
    static std::pair<int32_t, int32_t> cell_of(const Point& point);

    static Snapshot build_snapshot_from(const std::vector<Node>& nodes);

    static bool dijkstra_one_to_one(const Snapshot& snap,
                                     uint32_t source, uint32_t target,
                                     uint64_t& result);
    static uint64_t dijkstra_one_to_all(const Snapshot& snap, uint32_t source);

    mutable std::shared_mutex mutex_;
    std::vector<Node> nodes_;
    std::unordered_map<int64_t, std::vector<uint32_t>> grid_;
};