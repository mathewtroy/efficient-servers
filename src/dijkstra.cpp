#include "dijkstra.hpp"

#include <functional>
#include <queue>
#include <utility>
#include <vector>

bool Dijkstra::shortest_path(
    const Graph& graph,
    uint32_t source,
    uint32_t target,
    uint64_t& result
) {
    result = 0;

    if (source >= graph.size() || target >= graph.size()) {
        return false;
    }

    using QueueItem = std::pair<uint64_t, uint32_t>;
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<>> queue;

    std::vector<uint64_t> dist(graph.size(), INF_DISTANCE);
    dist[source] = 0;
    queue.emplace(0, source);

    while (!queue.empty()) {
        const auto [current_dist, node_id] = queue.top();
        queue.pop();

        if (current_dist != dist[node_id]) {
            continue;
        }

        if (node_id == target) {
            result = current_dist;
            return true;
        }

        for (const auto& edge : graph[node_id]) {
            const uint64_t next_dist = current_dist + edge.weight;
            if (next_dist < dist[edge.to]) {
                dist[edge.to] = next_dist;
                queue.emplace(next_dist, edge.to);
            }
        }
    }

    return false;
}

uint64_t Dijkstra::sum_of_shortest_paths(
    const Graph& graph,
    uint32_t source
) {
    if (source >= graph.size()) {
        return 0;
    }

    using QueueItem = std::pair<uint64_t, uint32_t>;
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<>> queue;

    std::vector<uint64_t> dist(graph.size(), INF_DISTANCE);
    dist[source] = 0;
    queue.emplace(0, source);

    while (!queue.empty()) {
        const auto [current_dist, node_id] = queue.top();
        queue.pop();

        if (current_dist != dist[node_id]) {
            continue;
        }

        for (const auto& edge : graph[node_id]) {
            const uint64_t next_dist = current_dist + edge.weight;
            if (next_dist < dist[edge.to]) {
                dist[edge.to] = next_dist;
                queue.emplace(next_dist, edge.to);
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