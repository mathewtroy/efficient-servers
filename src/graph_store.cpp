#include "graph_store.hpp"

#include <mutex>
#include <queue>
#include <utility>
#include <vector>


void GraphStore::reset() {
    std::unique_lock lock(mutex_);
    nodes_.clear();
    grid_.clear();
}

bool GraphStore::points_match(const Point& a, const Point& b) {
    static constexpr uint64_t radius_squared =
        static_cast<uint64_t>(LOCATION_MERGE_RADIUS_MM) * LOCATION_MERGE_RADIUS_MM;
    return squared_distance(a, b) <= radius_squared;
}

std::pair<int32_t, int32_t> GraphStore::cell_of(const Point& point) {
    return {
        floor_div(point.x, GRID_CELL_SIZE_MM),
        floor_div(point.y, GRID_CELL_SIZE_MM)
    };
}

bool GraphStore::resolve_existing_node_locked(const Point& point, uint32_t& node_id) const {
    const auto [cell_x, cell_y] = cell_of(point);

    for (int32_t dx = -1; dx <= 1; ++dx) {
        for (int32_t dy = -1; dy <= 1; ++dy) {
            const int64_t key = pack_cell_key(cell_x + dx, cell_y + dy);
            const auto it = grid_.find(key);
            if (it == grid_.end()) {
                continue;
            }

            for (const uint32_t candidate_id : it->second) {
                if (points_match(point, nodes_[candidate_id].point)) {
                    node_id = candidate_id;
                    return true;
                }
            }
        }
    }

    return false;
}

uint32_t GraphStore::resolve_or_create_node_locked(const Point& point) {
    uint32_t existing_id = 0;
    if (resolve_existing_node_locked(point, existing_id)) {
        return existing_id;
    }

    const uint32_t new_id = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(Node{point, {}});

    const auto [cell_x, cell_y] = cell_of(point);
    grid_[pack_cell_key(cell_x, cell_y)].push_back(new_id);

    return new_id;
}

void GraphStore::add_edge_locked(uint32_t from, uint32_t to, uint32_t length) {
    nodes_[from].outgoing[to].add_sample(length);
}

bool GraphStore::add_walk(const Walk& walk) {
    if (walk.locations_size() < 2) {
        return false;
    }

    if (walk.lengths_size() != walk.locations_size() - 1) {
        return false;
    }

    std::unique_lock lock(mutex_);

    std::vector<uint32_t> node_ids;
    node_ids.reserve(static_cast<std::size_t>(walk.locations_size()));

    for (const auto& location : walk.locations()) {
        const Point point{location.x(), location.y()};
        node_ids.push_back(resolve_or_create_node_locked(point));
    }

    for (int i = 0; i < walk.lengths_size(); ++i) {
        add_edge_locked(node_ids[i], node_ids[i + 1], walk.lengths(i));
    }

    return true;
}

bool GraphStore::dijkstra_one_to_one_locked(uint32_t source,
                                             uint32_t target,
                                             uint64_t& result) const {
    using QueueItem = std::pair<uint64_t, uint32_t>;
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<>> pq;

    const std::size_t n = nodes_.size();
    std::vector<uint64_t> dist(n, INF_DISTANCE);
    dist[source] = 0;
    pq.emplace(0, source);

    while (!pq.empty()) {
        const auto [d, u] = pq.top();
        pq.pop();

        if (d != dist[u]) {
            continue;
        }

        if (u == target) {
            result = d;
            return true;
        }

        for (const auto& [v, stat] : nodes_[u].outgoing) {
            const uint64_t nd = d + stat.average_length();
            if (nd < dist[v]) {
                dist[v] = nd;
                pq.emplace(nd, v);
            }
        }
    }

    return false;
}

uint64_t GraphStore::dijkstra_one_to_all_locked(uint32_t source) const {
    using QueueItem = std::pair<uint64_t, uint32_t>;
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<>> pq;

    const std::size_t n = nodes_.size();
    std::vector<uint64_t> dist(n, INF_DISTANCE);
    dist[source] = 0;
    pq.emplace(0, source);

    while (!pq.empty()) {
        const auto [d, u] = pq.top();
        pq.pop();

        if (d != dist[u]) {
            continue;
        }

        for (const auto& [v, stat] : nodes_[u].outgoing) {
            const uint64_t nd = d + stat.average_length();
            if (nd < dist[v]) {
                dist[v] = nd;
                pq.emplace(nd, v);
            }
        }
    }

    uint64_t total = 0;
    for (const uint64_t value : dist) {
        if (value != INF_DISTANCE) {
            total += value;
        }
    }

    return total;
}

bool GraphStore::one_to_one(const Location& origin,
                             const Location& destination,
                             uint64_t& result) const {
    const Point origin_point{origin.x(), origin.y()};
    const Point destination_point{destination.x(), destination.y()};

    std::shared_lock lock(mutex_);

    uint32_t source = 0;
    uint32_t target = 0;

    if (!resolve_existing_node_locked(origin_point, source)) {
        return false;
    }

    if (!resolve_existing_node_locked(destination_point, target)) {
        return false;
    }

    return dijkstra_one_to_one_locked(source, target, result);
}

bool GraphStore::one_to_all(const Location& origin, uint64_t& result) const {
    const Point origin_point{origin.x(), origin.y()};

    std::shared_lock lock(mutex_);

    uint32_t source = 0;
    if (!resolve_existing_node_locked(origin_point, source)) {
        return false;
    }

    result = dijkstra_one_to_all_locked(source);
    return true;
}