#pragma once

#include "graph_store.hpp"
#include "thread_pool.hpp"

#include <thread>

class Server {
public:
    explicit Server(int port);
    void run();

private:
    void handle_client(int client_fd);

    int port_;
    GraphStore graph_store_;

    ThreadPool thread_pool_{
        std::max(4u, std::thread::hardware_concurrency())
    };
};