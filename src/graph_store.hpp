#pragma once

#include "flat_grid.hpp"
#include "server.pb.h"
#include "types.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <shared_mutex>

class GraphStore {
public:
    GraphStore();  

    void reset();

    bool add_walk(const Walk& walk);
    bool add_walk(std::span<const Point> locations,
                  std::span<const uint32_t> lengths);

    bool add_walks(const std::vector<std::vector<Point>>& walks_points,
                   const std::vector<std::vector<uint32_t>>& walks_lengths);
    bool add_walks_flat(std::span<const Point> points,
                        std::span<const uint32_t> lengths,
                        std::span<const uint32_t> point_offsets,
                        std::span<const uint32_t> length_offsets);

    bool one_to_one(const Location& origin, const Location& destination, uint64_t& result) const;
    bool one_to_one(const Point& origin, const Point& destination, uint64_t& result) const;
    bool one_to_all(const Location& origin, uint64_t& result) const;
    bool one_to_all(const Point& origin, uint64_t& result) const;

private:
    struct Edge {
        uint32_t to;
        uint64_t weight_sum;
        uint32_t sample_count;
        uint32_t average;

        [[nodiscard]] uint32_t average_weight() const noexcept {
            return average;
        }
    };

    struct Node {
        Point    point;
        uint32_t edge_start    = 0;
        uint32_t cell_next     = FlatGrid::INVALID_NODE;
        uint16_t edge_count    = 0;
        uint16_t edge_capacity = 0;
    };

    struct CsrEdge {
        uint32_t to;
        uint32_t weight;
    };

    struct Snapshot {
        uint32_t node_count = 0;
        std::vector<uint32_t> row_start;
        std::vector<CsrEdge> edges;
        std::vector<uint32_t> reverse_row_start;
        std::vector<CsrEdge> reverse_edges;
    };

    uint32_t resolve_or_create_node_locked(const Point& point);
    bool     resolve_existing_node_locked(const Point& point, uint32_t& node_id) const;
    bool     resolve_existing_node_in_cell_locked(const Point& point,
                                                  int32_t cx,
                                                  int32_t cy,
                                                  uint32_t& node_id) const;

    void add_edge_locked(uint32_t from, uint32_t to, uint32_t length, uint32_t samples = 1);
    void compact_edges_locked();
    void mark_dirty_locked() noexcept;
    std::shared_ptr<const Snapshot> snapshot_for_query() const;
    void rebuild_snapshot_locked() const;

    static bool points_match(const Point& a, const Point& b);
    static std::pair<int32_t, int32_t> cell_of(const Point& point);

    static bool dijkstra_one_to_one_snapshot(const Snapshot& snapshot,
                                             uint32_t source,
                                             uint32_t target,
                                             uint64_t& result);
    static uint64_t dijkstra_one_to_all_snapshot(const Snapshot& snapshot, uint32_t source);

    mutable std::shared_mutex mutex_;

    std::vector<Node> nodes_;
    std::vector<Edge> edge_pool_;    
    FlatGrid          grid_;
    mutable bool snapshot_dirty_ = false;
    mutable std::shared_ptr<const Snapshot> snapshot_;
};
