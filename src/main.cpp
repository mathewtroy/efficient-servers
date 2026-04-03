#include "server.hpp"
#include <iostream>

int main(int argc, char** argv) {
    int port = 1234;

    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    Server server(port);
    server.run();

    return 0;
}