#include "graph_store.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <shared_mutex>
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
    nodes_.reserve(1 << 20);
    edge_pool_.clear();
    edge_pool_.reserve(2 << 20);
    grid_.clear_and_reserve(1 << 18);
    snapshot_ = std::make_shared<Snapshot>();
    reverse_pos_scratch_.clear();
    snapshot_dirty_ = false;
}

bool GraphStore::resolve_existing_node_locked(const Point& point,
                                               uint32_t& node_id) const {
    const auto [cx, cy] = cell_of(point);
    return resolve_existing_node_in_cell_locked(point, cx, cy, node_id);
}

bool GraphStore::resolve_existing_node_in_cell_locked(const Point& point,
                                                       int32_t cx,
                                                       int32_t cy,
                                                       uint32_t& node_id) const {
    const int64_t cell_x0 = static_cast<int64_t>(cx) * GRID_CELL_SIZE_MM;
    const int64_t cell_y0 = static_cast<int64_t>(cy) * GRID_CELL_SIZE_MM;
    const int32_t lx = static_cast<int32_t>(static_cast<int64_t>(point.x) - cell_x0);
    const int32_t ly = static_cast<int32_t>(static_cast<int64_t>(point.y) - cell_y0);
    const int32_t dx0 = (lx <= LOCATION_MERGE_RADIUS_MM) ? -1 : 0;
    const int32_t dx1 = (lx >= GRID_CELL_SIZE_MM - LOCATION_MERGE_RADIUS_MM) ? 1 : 0;
    const int32_t dy0 = (ly <= LOCATION_MERGE_RADIUS_MM) ? -1 : 0;
    const int32_t dy1 = (ly >= GRID_CELL_SIZE_MM - LOCATION_MERGE_RADIUS_MM) ? 1 : 0;

    for (int32_t dx = dx0; dx <= dx1; ++dx) {
        for (int32_t dy = dy0; dy <= dy1; ++dy) {
            const uint32_t* head = grid_.find(pack_cell_key(cx + dx, cy + dy));
            if (!head) continue;
            for (uint32_t id = *head; id != FlatGrid::INVALID_NODE; id = nodes_[id].cell_next) {
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
    const auto [cx, cy] = cell_of(point);
    uint32_t existing = 0;
    if (resolve_existing_node_in_cell_locked(point, cx, cy, existing)) return existing;

    const uint32_t new_id = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(Node{point, 0, FlatGrid::INVALID_NODE, 0, 0});

    nodes_[new_id].cell_next = grid_.add(pack_cell_key(cx, cy), new_id);
    return new_id;
}

void GraphStore::add_edge_locked(uint32_t from, uint32_t to, uint32_t length, uint32_t samples) {
    Node& nd = nodes_[from];

    for (uint16_t i = 0; i < nd.edge_count; ++i) {
        Edge& e = edge_pool_[nd.edge_start + i];
        if (e.to == to) {
            e.weight_sum += static_cast<uint64_t>(length) * samples;
            e.sample_count += samples;
            e.average = static_cast<uint32_t>(e.weight_sum / e.sample_count);
            return;
        }
    }

    if (nd.edge_count == nd.edge_capacity) {
        const uint16_t new_cap = (nd.edge_capacity == 0) ? 2u : nd.edge_capacity * 2u;
        const uint32_t new_start = static_cast<uint32_t>(edge_pool_.size());
        edge_pool_.resize(new_start + new_cap);
        if (nd.edge_count > 0) {
            std::memcpy(&edge_pool_[new_start],
                        &edge_pool_[nd.edge_start],
                        nd.edge_count * sizeof(Edge));
        }
        nd.edge_start    = new_start;
        nd.edge_capacity = new_cap;
    }

    edge_pool_[nd.edge_start + nd.edge_count++] =
        Edge{to, static_cast<uint64_t>(length) * samples, samples, length};
}

void GraphStore::compact_edges_locked() {
    thread_local std::vector<Edge> compact;
    compact.clear();
    compact.reserve(edge_pool_.size());

    for (Node& node : nodes_) {
        if (node.edge_count == 0) {
            node.edge_start = 0;
            node.edge_capacity = 0;
            continue;
        }

        const uint32_t new_start = static_cast<uint32_t>(compact.size());
        const Edge* edges = edge_pool_.data() + node.edge_start;
        compact.insert(compact.end(), edges, edges + node.edge_count);
        node.edge_start = new_start;
        node.edge_capacity = node.edge_count;
    }

    edge_pool_.swap(compact);
}

void GraphStore::mark_dirty_locked() noexcept {
    snapshot_dirty_ = true;
}

std::shared_ptr<const GraphStore::Snapshot> GraphStore::snapshot_for_query() const {
    {
        std::shared_lock lock(mutex_);
        if (!snapshot_dirty_ && snapshot_) return snapshot_;
    }

    std::unique_lock lock(mutex_);
    if (snapshot_dirty_ || !snapshot_) {
        rebuild_snapshot_locked();
    }
    return snapshot_;
}

void GraphStore::rebuild_snapshot_locked() const {
    std::shared_ptr<Snapshot> snapshot;
    if (snapshot_ && snapshot_.use_count() == 1) {
        snapshot = std::const_pointer_cast<Snapshot>(snapshot_);
    } else {
        snapshot = std::make_shared<Snapshot>();
    }
    const uint32_t n = static_cast<uint32_t>(nodes_.size());
    uint32_t edge_count = 0;

    for (const Node& node : nodes_) {
        edge_count += node.edge_count;
    }

    snapshot->node_count = n;
    snapshot->row_start.assign(static_cast<std::size_t>(n) + 1, 0);
    snapshot->reverse_row_start.assign(static_cast<std::size_t>(n) + 1, 0);
    snapshot->edges.resize(edge_count);
    snapshot->reverse_edges.resize(edge_count);

    for (uint32_t u = 0; u < n; ++u) {
        snapshot->row_start[u + 1] = snapshot->row_start[u] + nodes_[u].edge_count;
        const Edge* edges = edge_pool_.data() + nodes_[u].edge_start;
        for (uint16_t j = 0; j < nodes_[u].edge_count; ++j) {
            ++snapshot->reverse_row_start[edges[j].to + 1];
        }
    }

    for (uint32_t i = 1; i <= n; ++i) {
        snapshot->reverse_row_start[i] += snapshot->reverse_row_start[i - 1];
    }

    reverse_pos_scratch_ = snapshot->reverse_row_start;
    for (uint32_t u = 0; u < n; ++u) {
        uint32_t out = snapshot->row_start[u];
        const Edge* edges = edge_pool_.data() + nodes_[u].edge_start;
        for (uint16_t j = 0; j < nodes_[u].edge_count; ++j) {
            const uint32_t v = edges[j].to;
            const uint32_t w = edges[j].average_weight();
            snapshot->edges[out++] = CsrEdge{v, w};
            snapshot->reverse_edges[reverse_pos_scratch_[v]++] = CsrEdge{u, w};
        }
    }

    snapshot_ = std::move(snapshot);
    snapshot_dirty_ = false;
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

    compact_edges_locked();
    mark_dirty_locked();
    return true;
}

bool GraphStore::add_walk(std::span<const Point> locations,
                          std::span<const uint32_t> lengths) {
    if (locations.size() < 2) return false;
    if (lengths.size() != locations.size() - 1) return false;

    thread_local std::vector<uint32_t> node_ids;
    node_ids.clear();
    node_ids.reserve(locations.size());

    std::unique_lock lock(mutex_);

    for (const Point& point : locations) {
        node_ids.push_back(resolve_or_create_node_locked(point));
    }
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        add_edge_locked(node_ids[i], node_ids[i + 1], lengths[i]);
    }

    mark_dirty_locked();
    return true;
}

bool GraphStore::add_walks(
    const std::vector<std::vector<Point>>& walks_points,
    const std::vector<std::vector<uint32_t>>& walks_lengths) {

    if (walks_points.size() != walks_lengths.size()) {
        return false;
    }

    for (std::size_t w = 0; w < walks_points.size(); ++w) {
        const auto& points = walks_points[w];
        const auto& lengths = walks_lengths[w];
        if (points.size() < 2 || lengths.size() != points.size() - 1) {
            return false;
        }
    }

    std::unique_lock lock(mutex_);

    thread_local std::vector<uint32_t> node_ids;

    for (std::size_t w = 0; w < walks_points.size(); ++w) {
        const auto& points  = walks_points[w];
        const auto& lengths = walks_lengths[w];

        node_ids.clear();
        node_ids.reserve(points.size());

        for (const Point& p : points) {
            node_ids.push_back(resolve_or_create_node_locked(p));
        }

        for (std::size_t i = 0; i < lengths.size(); ++i) {
            add_edge_locked(node_ids[i], node_ids[i + 1], lengths[i]);
        }
    }

    mark_dirty_locked();
    return true;
}

bool GraphStore::add_walks_flat(std::span<const Point> points,
                                std::span<const uint32_t> lengths,
                                std::span<const uint32_t> point_offsets,
                                std::span<const uint32_t> length_offsets) {
    return add_walks_flat_repeated(points, lengths, point_offsets, length_offsets, 1);
}

bool GraphStore::add_walks_flat_repeated(std::span<const Point> points,
                                         std::span<const uint32_t> lengths,
                                         std::span<const uint32_t> point_offsets,
                                         std::span<const uint32_t> length_offsets,
                                         uint32_t repeat_count) {
    if (repeat_count == 0) return true;
    if (point_offsets.size() != length_offsets.size()) return false;
    if (point_offsets.size() < 2) return true;
    if (point_offsets.front() != 0 || length_offsets.front() != 0) return false;
    if (point_offsets.back() != points.size()) return false;
    if (length_offsets.back() != lengths.size()) return false;

    const std::size_t walks = point_offsets.size() - 1;
    for (std::size_t w = 0; w < walks; ++w) {
        const uint32_t p0 = point_offsets[w];
        const uint32_t p1 = point_offsets[w + 1];
        const uint32_t l0 = length_offsets[w];
        const uint32_t l1 = length_offsets[w + 1];
        if (p1 <= p0 || p1 - p0 < 2 || l1 - l0 != p1 - p0 - 1) return false;
    }

    std::unique_lock lock(mutex_);

    thread_local std::vector<uint32_t> node_ids;
    node_ids.resize(points.size());

    for (std::size_t i = 0; i < points.size(); ++i) {
        node_ids[i] = resolve_or_create_node_locked(points[i]);
    }

    for (std::size_t w = 0; w < walks; ++w) {
        const uint32_t p0 = point_offsets[w];
        const uint32_t p1 = point_offsets[w + 1];
        const uint32_t l0 = length_offsets[w];

        for (uint32_t i = p0; i + 1 < p1; ++i) {
            add_edge_locked(node_ids[i], node_ids[i + 1], lengths[l0 + i - p0], repeat_count);
        }
    }

    mark_dirty_locked();
    return true;
}

struct HeapEntry {
    uint64_t dist;
    uint32_t node;
};

class MinHeap {
public:
    void clear() {
        data_.clear();
    }

    [[nodiscard]] bool empty() const {
        return data_.empty();
    }

    [[nodiscard]] uint64_t min_dist() const {
        return data_[0].dist;
    }

    void push(uint64_t dist, uint32_t node) {
        data_.push_back(HeapEntry{dist, node});
        std::size_t i = data_.size() - 1;
        while (i > 0) {
            const std::size_t parent = (i - 1) >> 1U;
            if (less_or_equal(data_[parent], data_[i])) break;
            std::swap(data_[parent], data_[i]);
            i = parent;
        }
    }

    HeapEntry pop() {
        HeapEntry result = data_[0];
        data_[0] = data_.back();
        data_.pop_back();

        std::size_t i = 0;
        while (true) {
            const std::size_t left = i * 2 + 1;
            const std::size_t right = left + 1;
            if (left >= data_.size()) break;
            std::size_t best = left;
            if (right < data_.size() && less_entry(data_[right], data_[left])) {
                best = right;
            }
            if (less_or_equal(data_[i], data_[best])) break;
            std::swap(data_[i], data_[best]);
            i = best;
        }

        return result;
    }

private:
    static bool less_entry(const HeapEntry& a, const HeapEntry& b) {
        return a.dist < b.dist || (a.dist == b.dist && a.node < b.node);
    }

    static bool less_or_equal(const HeapEntry& a, const HeapEntry& b) {
        return a.dist < b.dist || (a.dist == b.dist && a.node <= b.node);
    }

    std::vector<HeapEntry> data_;
};


bool GraphStore::dijkstra_one_to_one_snapshot(const Snapshot& snapshot,
                                               uint32_t source,
                                               uint32_t target,
                                               uint64_t& result) {
    if (source == target) {
        result = 0;
        return true;
    }

    const uint32_t n = snapshot.node_count;

    thread_local std::vector<uint64_t> dist_f;
    thread_local std::vector<uint64_t> dist_b;
    thread_local std::vector<uint32_t> seen_f;
    thread_local std::vector<uint32_t> seen_b;
    thread_local MinHeap pq_f;
    thread_local MinHeap pq_b;
    thread_local uint32_t generation = 0;

    if (++generation == 0) {
        std::fill(seen_f.begin(), seen_f.end(), 0);
        std::fill(seen_b.begin(), seen_b.end(), 0);
        generation = 1;
    }
    if (dist_f.size() < n) dist_f.resize(n);
    if (dist_b.size() < n) dist_b.resize(n);
    if (seen_f.size() < n) seen_f.resize(n, 0);
    if (seen_b.size() < n) seen_b.resize(n, 0);
    pq_f.clear();
    pq_b.clear();

    uint64_t best = INF_DISTANCE;

    dist_f[source] = 0;
    dist_b[target] = 0;
    seen_f[source] = generation;
    seen_b[target] = generation;
    pq_f.push(0ULL, source);
    pq_b.push(0ULL, target);

    while (!pq_f.empty() && !pq_b.empty()) {
        const uint64_t top_f = pq_f.min_dist();
        const uint64_t top_b = pq_b.min_dist();
        if (top_f + top_b >= best) break;

        if (top_f <= top_b) {
            const HeapEntry entry = pq_f.pop();
            const uint64_t d = entry.dist;
            const uint32_t u = entry.node;
            if (d != dist_f[u]) continue;
            if (seen_b[u] == generation) {
                best = std::min(best, d + dist_b[u]);
            }
            for (uint32_t i = snapshot.row_start[u]; i < snapshot.row_start[u + 1]; ++i) {
                const CsrEdge& edge = snapshot.edges[i];
                const uint32_t v = edge.to;
                const uint64_t nd = d + edge.weight;
                if (seen_f[v] != generation || nd < dist_f[v]) {
                    dist_f[v] = nd;
                    seen_f[v] = generation;
                    pq_f.push(nd, v);
                    if (seen_b[v] == generation) {
                        best = std::min(best, nd + dist_b[v]);
                    }
                }
            }
        } else {
            const HeapEntry entry = pq_b.pop();
            const uint64_t d = entry.dist;
            const uint32_t u = entry.node;
            if (d != dist_b[u]) continue;
            if (seen_f[u] == generation) {
                best = std::min(best, d + dist_f[u]);
            }
            for (uint32_t i = snapshot.reverse_row_start[u]; i < snapshot.reverse_row_start[u + 1]; ++i) {
                const CsrEdge& edge = snapshot.reverse_edges[i];
                const uint32_t v = edge.to;
                const uint64_t nd = d + edge.weight;
                if (seen_b[v] != generation || nd < dist_b[v]) {
                    dist_b[v] = nd;
                    seen_b[v] = generation;
                    pq_b.push(nd, v);
                    if (seen_f[v] == generation) {
                        best = std::min(best, nd + dist_f[v]);
                    }
                }
            }
        }
    }

    if (best != INF_DISTANCE) {
        result = best;
        return true;
    }
    return false;
}

uint64_t GraphStore::dijkstra_one_to_all_snapshot(const Snapshot& snapshot, uint32_t source) {
    const uint32_t n = snapshot.node_count;

    thread_local std::vector<uint64_t> dist;
    thread_local std::vector<uint32_t> seen;
    thread_local std::vector<uint32_t> touched;
    thread_local MinHeap pq;
    thread_local uint32_t generation = 0;

    if (++generation == 0) {
        std::fill(seen.begin(), seen.end(), 0);
        generation = 1;
    }
    if (dist.size() < n) dist.resize(n);
    if (seen.size() < n) seen.resize(n, 0);
    pq.clear();
    touched.clear();

    dist[source] = 0;
    seen[source] = generation;
    touched.push_back(source);
    pq.push(0ULL, source);

    while (!pq.empty()) {
        const HeapEntry entry = pq.pop();
        const uint64_t d = entry.dist;
        const uint32_t u = entry.node;

        if (d != dist[u]) continue;

        for (uint32_t i = snapshot.row_start[u]; i < snapshot.row_start[u + 1]; ++i) {
            const CsrEdge& edge = snapshot.edges[i];
            const uint32_t v = edge.to;
            const uint64_t nd = d + edge.weight;
            if (seen[v] != generation || nd < dist[v]) {
                if (seen[v] != generation) {
                    seen[v] = generation;
                    touched.push_back(v);
                }
                dist[v] = nd;
                pq.push(nd, v);
            }
        }
    }

    uint64_t total = 0;
    for (const uint32_t node : touched) {
        total += dist[node];
    }
    return total;
}

bool GraphStore::one_to_one(const Location& origin,
                             const Location& destination,
                             uint64_t& result) const {
    const Point op{origin.x(), origin.y()};
    const Point dp{destination.x(), destination.y()};

    return one_to_one(op, dp, result);
}

bool GraphStore::one_to_one(const Point& op,
                             const Point& dp,
                             uint64_t& result) const {
    std::shared_ptr<const Snapshot> snapshot;
    uint32_t source = 0, target = 0;

    {
        std::shared_lock lock(mutex_);
        if (!resolve_existing_node_locked(op, source)) return false;
        if (!resolve_existing_node_locked(dp, target)) return false;
    }

    snapshot = snapshot_for_query();
    return dijkstra_one_to_one_snapshot(*snapshot, source, target, result);
}

bool GraphStore::one_to_all(const Location& origin, uint64_t& result) const {
    const Point op{origin.x(), origin.y()};

    return one_to_all(op, result);
}

bool GraphStore::one_to_all(const Point& op, uint64_t& result) const {
    std::shared_ptr<const Snapshot> snapshot;
    uint32_t source = 0;

    {
        std::shared_lock lock(mutex_);
        if (!resolve_existing_node_locked(op, source)) return false;
    }

    snapshot = snapshot_for_query();
    result = dijkstra_one_to_all_snapshot(*snapshot, source);
    return true;
}
