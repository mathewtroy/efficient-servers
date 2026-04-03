#pragma once

#include "graph_store.hpp"

class Server {
public:
    explicit Server(int port);
    void run();

private:
    void handle_client(int client_fd);

    int port_;
    GraphStore graph_store_;
};