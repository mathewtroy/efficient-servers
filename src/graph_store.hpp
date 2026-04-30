#pragma once

#include "flat_grid.hpp"
#include "server.pb.h"
#include "types.hpp"

#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <vector>

class GraphStore {
public:
    GraphStore();  

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
        Point    point;
        uint32_t edge_start    = 0;
        uint16_t edge_count    = 0;
        uint16_t edge_capacity = 0;
    };

    struct CSR {
        std::vector<uint32_t> row_start;  
        std::vector<uint32_t> col;       
        std::vector<uint32_t> weight;     
    };

    uint32_t resolve_or_create_node_locked(const Point& point);
    bool     resolve_existing_node_locked(const Point& point, uint32_t& node_id) const;
    void     add_edge_locked(uint32_t from, uint32_t to, uint32_t length);

    static bool points_match(const Point& a, const Point& b);
    static std::pair<int32_t, int32_t> cell_of(const Point& point);

    void build_csr_locked() const;

    bool     dijkstra_one_to_one_locked(uint32_t source, uint32_t target, uint64_t& result) const;
    uint64_t dijkstra_one_to_all_locked(uint32_t source) const;

    mutable std::shared_mutex mutex_;

    std::vector<Node> nodes_;
    std::vector<Edge> edge_pool_;    
    FlatGrid          grid_;

    mutable CSR              csr_;
    mutable std::atomic<bool> csr_dirty_{true};
};
