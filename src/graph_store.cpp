#include "graph_store.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

bool GraphStore::points_match(const Point& a, const Point& b) {
    static constexpr uint64_t R2 =
        static_cast<uint64_t>(LOCATION_MERGE_RADIUS_MM) * LOCATION_MERGE_RADIUS_MM;
    return squared_distance(a, b) <= R2;
}

std::pair<int32_t, int32_t> GraphStore::cell_of(const Point& p) {
    return { floor_div(p.x, GRID_CELL_SIZE_MM),
             floor_div(p.y, GRID_CELL_SIZE_MM) };
}

GraphStore::GraphStore() { reset(); }

void GraphStore::reset() {
    std::unique_lock lock(mutex_);
    nodes_.clear();
    nodes_.reserve(2 << 20);            // 2 M nodes upfront
    edge_pool_.clear();
    edge_pool_.reserve(8 << 20);        // 8 M edge slots (amortised doubling)
    csr_.row_start.clear();
    csr_.col.clear();
    csr_.weight.clear();
    csr_dirty_.store(true, std::memory_order_relaxed);
    grid_.clear_and_reserve(2 << 20);   // 4 M hash slots
}

bool GraphStore::resolve_existing_node_locked(const Point& point,
                                               uint32_t& node_id) const {
    const auto [cx, cy] = cell_of(point);
    const int32_t lx = point.x - cx * GRID_CELL_SIZE_MM;
    const int32_t ly = point.y - cy * GRID_CELL_SIZE_MM;
    const int32_t nx = (lx < LOCATION_MERGE_RADIUS_MM) ? -1 : 1;
    const int32_t ny = (ly < LOCATION_MERGE_RADIUS_MM) ? -1 : 1;

    for (const int32_t dx : {0, nx}) {
        for (const int32_t dy : {0, ny}) {
            const auto* slot = grid_.find(pack_cell_key(cx + dx, cy + dy));
            if (!slot) continue;
            for (uint32_t k = 0; k < slot->count; ++k) {
                const uint32_t id = slot->ids[k];
                if (points_match(point, nodes_[id].point)) {
                    node_id = id;
                    return true;
                }
            }
        }
    }
    return false;
}

uint32_t GraphStore::resolve_or_create_node_locked(const Point& point) {
    uint32_t existing = 0;
    if (resolve_existing_node_locked(point, existing)) return existing;

    const uint32_t new_id = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(Node{point, 0, 0, 0});

    const auto [cx, cy] = cell_of(point);
    grid_.add(pack_cell_key(cx, cy), new_id);
    return new_id;
}

void GraphStore::add_edge_locked(uint32_t from, uint32_t to, uint32_t length) {
    Node& nd = nodes_[from];

    for (uint16_t i = 0; i < nd.edge_count; ++i) {
        Edge& e = edge_pool_[nd.edge_start + i];
        if (e.to == to) {
            e.weight_sum += length;
            ++e.sample_count;
            return;
        }
    }

    if (nd.edge_count == nd.edge_capacity) {
        const uint16_t new_cap = (nd.edge_capacity == 0) ? 2u : nd.edge_capacity * 2u;
        const uint32_t new_start = static_cast<uint32_t>(edge_pool_.size());
        edge_pool_.resize(new_start + new_cap);   // no realloc if reserved
        if (nd.edge_count > 0) {
            std::memcpy(&edge_pool_[new_start],
                        &edge_pool_[nd.edge_start],
                        nd.edge_count * sizeof(Edge));
        }
        nd.edge_start    = new_start;
        nd.edge_capacity = new_cap;
    }

    edge_pool_[nd.edge_start + nd.edge_count++] = Edge{to, length, 1u};
}

bool GraphStore::add_walk(const Walk& walk) {
    if (walk.locations_size() < 2) return false;
    if (walk.lengths_size() != walk.locations_size() - 1) return false;

    thread_local std::vector<uint32_t> node_ids;
    node_ids.clear();
    node_ids.reserve(static_cast<std::size_t>(walk.locations_size()));

    std::unique_lock lock(mutex_);

    for (const auto& loc : walk.locations()) {
        node_ids.push_back(
            resolve_or_create_node_locked(Point{loc.x(), loc.y()}));
    }
    for (int i = 0; i < walk.lengths_size(); ++i) {
        add_edge_locked(node_ids[i], node_ids[i + 1], walk.lengths(i));
    }

    csr_dirty_.store(true, std::memory_order_relaxed);
    return true;
}

void GraphStore::build_csr_locked() const {
    const uint32_t n = static_cast<uint32_t>(nodes_.size());

    csr_.row_start.resize(n + 1);
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        csr_.row_start[i] = total;
        total += nodes_[i].edge_count;
    }
    csr_.row_start[n] = total;

    csr_.col.resize(total);
    csr_.weight.resize(total);

    for (uint32_t i = 0; i < n; ++i) {
        const Node& nd = nodes_[i];
        for (uint16_t j = 0; j < nd.edge_count; ++j) {
            const Edge& e = edge_pool_[nd.edge_start + j];
            const uint32_t idx = csr_.row_start[i] + j;
            csr_.col[idx]    = e.to;
            csr_.weight[idx] = e.average_weight();
        }
    }
}

using QI = std::pair<uint64_t, uint32_t>;
static constexpr std::greater<QI> QI_CMP{};

struct AEntry {
    uint64_t f, g;
    uint32_t node;
    bool operator>(const AEntry& o) const noexcept { return f > o.f; }
};
static constexpr std::greater<AEntry> AE_CMP{};

bool GraphStore::dijkstra_one_to_one_locked(uint32_t source,
                                             uint32_t target,
                                             uint64_t& result) const {
    const uint32_t n = static_cast<uint32_t>(nodes_.size());
    const Point& tp  = nodes_[target].point;

    auto h = [&](uint32_t v) -> uint64_t {
        return static_cast<uint64_t>(std::sqrt(
            static_cast<double>(squared_distance(nodes_[v].point, tp))));
    };

    thread_local std::vector<uint64_t>  dist;
    thread_local std::vector<AEntry>    pq;

    dist.assign(n, INF_DISTANCE);
    pq.clear();

    dist[source] = 0;
    pq.push_back({h(source), 0ULL, source});

    while (!pq.empty()) {
        std::pop_heap(pq.begin(), pq.end(), AE_CMP);
        const auto [f, g, u] = pq.back();
        pq.pop_back();

        if (g != dist[u]) continue;   // stale (lazy deletion)

        if (u == target) {
            result = g;
            return true;
        }

        for (uint32_t j = csr_.row_start[u], end = csr_.row_start[u + 1]; j < end; ++j) {
            const uint32_t v  = csr_.col[j];
            const uint64_t ng = g + csr_.weight[j];
            if (ng < dist[v]) {
                dist[v] = ng;
                pq.push_back({ng + h(v), ng, v});
                std::push_heap(pq.begin(), pq.end(), AE_CMP);
            }
        }
    }
    return false;
}

uint64_t GraphStore::dijkstra_one_to_all_locked(uint32_t source) const {
    const uint32_t n = static_cast<uint32_t>(nodes_.size());

    thread_local std::vector<uint64_t> dist;
    thread_local std::vector<QI>       pq;

    dist.assign(n, INF_DISTANCE);
    pq.clear();

    dist[source] = 0;
    pq.push_back({0ULL, source});

    while (!pq.empty()) {
        std::pop_heap(pq.begin(), pq.end(), QI_CMP);
        const auto [d, u] = pq.back();
        pq.pop_back();

        if (d != dist[u]) continue;

        for (uint32_t j = csr_.row_start[u], end = csr_.row_start[u + 1]; j < end; ++j) {
            const uint32_t v  = csr_.col[j];
            const uint64_t nd = d + csr_.weight[j];
            if (nd < dist[v]) {
                dist[v] = nd;
                pq.push_back({nd, v});
                std::push_heap(pq.begin(), pq.end(), QI_CMP);
            }
        }
    }

    uint64_t total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (dist[i] != INF_DISTANCE) total += dist[i];
    }
    return total;
}

bool GraphStore::one_to_one(const Location& origin,
                             const Location& destination,
                             uint64_t& result) const {
    const Point op{origin.x(), origin.y()};
    const Point dp{destination.x(), destination.y()};

    if (csr_dirty_.load(std::memory_order_acquire)) {
        std::unique_lock wl(mutex_);
        if (csr_dirty_.load(std::memory_order_relaxed)) {
            build_csr_locked();
            csr_dirty_.store(false, std::memory_order_release);
        }
    }

    std::shared_lock rl(mutex_);

    uint32_t source = 0, target = 0;
    if (!resolve_existing_node_locked(op, source)) return false;
    if (!resolve_existing_node_locked(dp, target)) return false;

    return dijkstra_one_to_one_locked(source, target, result);
}

bool GraphStore::one_to_all(const Location& origin, uint64_t& result) const {
    const Point op{origin.x(), origin.y()};

    if (csr_dirty_.load(std::memory_order_acquire)) {
        std::unique_lock wl(mutex_);
        if (csr_dirty_.load(std::memory_order_relaxed)) {
            build_csr_locked();
            csr_dirty_.store(false, std::memory_order_release);
        }
    }

    std::shared_lock rl(mutex_);

    uint32_t source = 0;
    if (!resolve_existing_node_locked(op, source)) return false;

    result = dijkstra_one_to_all_locked(source);
    return true;
}
