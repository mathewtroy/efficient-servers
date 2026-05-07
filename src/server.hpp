#pragma once

#include "graph_store.hpp"
#include "thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Server {
public:
    explicit Server(int port);
    ~Server();
    void run();

private:
    struct PendingWalkBatch {
        std::vector<Point> points;
        std::vector<uint32_t> lengths;
        std::vector<uint32_t> point_offsets{0};
        std::vector<uint32_t> length_offsets{0};
        std::vector<std::vector<uint8_t>> raw_prefix;
        std::size_t raw_prefix_bytes = 0;
    };

    enum class WriterTaskKind {
        Walk,
        Reset,
        Stop,
    };

    struct WriterTask {
        WriterTaskKind kind = WriterTaskKind::Walk;
        PendingWalkBatch batch;
        uint64_t generation = 0;
        std::shared_ptr<std::promise<bool>> done;
    };

    void handle_client(int client_fd);
    bool submit_walk_batch(PendingWalkBatch&& batch);
    std::future<bool> submit_reset();
    void wait_for_generation(uint64_t generation);
    void writer_loop();
    bool commit_batch_to_graph(PendingWalkBatch& batch, uint32_t repeat_count);
    static void clear_pending_batch(PendingWalkBatch& pending);
    static bool same_pending_batch(const PendingWalkBatch& a, const PendingWalkBatch& b);
    static bool pending_batch_empty(const PendingWalkBatch& pending);

    int port_;
    GraphStore graph_store_;
    std::mutex writer_mutex_;
    std::condition_variable writer_cv_;
    std::condition_variable committed_cv_;
    std::deque<std::unique_ptr<WriterTask>> writer_queue_;
    std::thread writer_thread_;
    std::atomic<uint32_t> active_clients_{0};
    std::atomic<uint64_t> latest_submitted_generation_{0};
    uint64_t next_generation_ = 0;
    uint64_t committed_generation_ = 0;
    bool writer_busy_ = false;

    ThreadPool thread_pool_{
        std::max(4u, std::thread::hardware_concurrency())
    };
};
