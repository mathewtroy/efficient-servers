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
    auto& edges = nodes_[from].outgoing;
    for (auto& e : edges) {
        if (e.to == to) {
            e.weight_sum += length;
            ++e.sample_count;
            return;
        }
    }
    edges.push_back(Edge{to, length, 1u});
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
        node_ids.push_back(
            resolve_or_create_node_locked(Point{location.x(), location.y()})
        );
    }

    for (int i = 0; i < walk.lengths_size(); ++i) {
        add_edge_locked(node_ids[i], node_ids[i + 1], walk.lengths(i));
    }

    return true;
}

GraphStore::Snapshot GraphStore::build_snapshot_from(const std::vector<Node>& nodes) {
    Snapshot snap;
    snap.adj.resize(nodes.size());

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& edges = nodes[i].outgoing;
        snap.adj[i].reserve(edges.size());
        for (const auto& e : edges) {
            snap.adj[i].push_back(Snapshot::SnapEdge{e.to, e.average_weight()});
        }
    }

    return snap;
}

bool GraphStore::dijkstra_one_to_one(const Snapshot& snap,
                                      uint32_t source,
                                      uint32_t target,
                                      uint64_t& result) {
    using QI = std::pair<uint64_t, uint32_t>;
    std::priority_queue<QI, std::vector<QI>, std::greater<>> pq;

    std::vector<uint64_t> dist(snap.size(), INF_DISTANCE);
    dist[source] = 0;
    pq.emplace(0ULL, source);

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

        for (const auto& e : snap.adj[u]) {
            const uint64_t nd = d + e.weight;
            if (nd < dist[e.to]) {
                dist[e.to] = nd;
                pq.emplace(nd, e.to);
            }
        }
    }

    return false;
}

uint64_t GraphStore::dijkstra_one_to_all(const Snapshot& snap, uint32_t source) {
    using QI = std::pair<uint64_t, uint32_t>;
    std::priority_queue<QI, std::vector<QI>, std::greater<>> pq;

    std::vector<uint64_t> dist(snap.size(), INF_DISTANCE);
    dist[source] = 0;
    pq.emplace(0ULL, source);

    while (!pq.empty()) {
        const auto [d, u] = pq.top();
        pq.pop();

        if (d != dist[u]) {
            continue;
        }

        for (const auto& e : snap.adj[u]) {
            const uint64_t nd = d + e.weight;
            if (nd < dist[e.to]) {
                dist[e.to] = nd;
                pq.emplace(nd, e.to);
            }
        }
    }

    uint64_t total = 0;
    for (const uint64_t v : dist) {
        if (v != INF_DISTANCE) {
            total += v;
        }
    }
    return total;
}

bool GraphStore::one_to_one(const Location& origin,
                             const Location& destination,
                             uint64_t& result) const {
    const Point op{origin.x(), origin.y()};
    const Point dp{destination.x(), destination.y()};

    uint32_t source = 0;
    uint32_t target = 0;
    std::vector<Node> nodes_copy;

    {
        std::shared_lock lock(mutex_);

        if (!resolve_existing_node_locked(op, source)) {
            return false;
        }
        if (!resolve_existing_node_locked(dp, target)) {
            return false;
        }

        nodes_copy = nodes_;
    } 

    const Snapshot snap = build_snapshot_from(nodes_copy);
    return dijkstra_one_to_one(snap, source, target, result);
}

bool GraphStore::one_to_all(const Location& origin, uint64_t& result) const {
    const Point op{origin.x(), origin.y()};

    uint32_t source = 0;
    std::vector<Node> nodes_copy;

    {
        std::shared_lock lock(mutex_);

        if (!resolve_existing_node_locked(op, source)) {
            return false;
        }

        nodes_copy = nodes_;
    } 

    const Snapshot snap = build_snapshot_from(nodes_copy);
    result = dijkstra_one_to_all(snap, source);
    return true;
}