#pragma once

#include "graph_store.hpp"
#include "thread_pool.hpp"

#include <algorithm>
#include <shared_mutex>
#include <thread>
#include <vector>

class Server {
public:
    explicit Server(int port);
    void run();

private:
    struct PendingWalkBatch {
        std::vector<Point> points;
        std::vector<uint32_t> lengths;
        std::vector<uint32_t> point_offsets{0};
        std::vector<uint32_t> length_offsets{0};
    };

    void handle_client(int client_fd);

    int port_;
    GraphStore graph_store_;
    std::shared_mutex pending_mutex_;
    std::vector<PendingWalkBatch*> pending_batches_;

    ThreadPool thread_pool_{
        std::max(4u, std::thread::hardware_concurrency())
    };
};
