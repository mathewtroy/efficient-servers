#pragma once

class Server {
public:
    explicit Server(int port);
    void run();

private:
    int port_;
};