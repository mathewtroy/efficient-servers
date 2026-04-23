#include "server.hpp"
#include "protocol.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "server.pb.h"

Server::Server(int port) : port_(port) {}

static void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static void set_blocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
}

void Server::run() {
    const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Failed to create socket\n";
        return;
    }

    int reuse_addr = 1;
    if (::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                     &reuse_addr, sizeof(reuse_addr)) < 0) {
        std::cerr << "setsockopt(SO_REUSEADDR) failed\n";
        ::close(server_fd);
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind failed: " << std::strerror(errno) << '\n';
        ::close(server_fd);
        return;
    }

    if (::listen(server_fd, 256) < 0) {
        std::cerr << "listen failed: " << std::strerror(errno) << '\n';
        ::close(server_fd);
        return;
    }

    set_nonblocking(server_fd);

    const int epoll_fd = ::epoll_create1(0);
    if (epoll_fd < 0) {
        std::cerr << "epoll_create1 failed: " << std::strerror(errno) << '\n';
        ::close(server_fd);
        return;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        std::cerr << "epoll_ctl failed: " << std::strerror(errno) << '\n';
        ::close(epoll_fd);
        ::close(server_fd);
        return;
    }

    std::cout << "Server listening on port " << port_ << std::endl;

    constexpr int MAX_EVENTS = 128;
    std::array<epoll_event, MAX_EVENTS> events{};

    while (true) {
        const int n = ::epoll_wait(epoll_fd, events.data(), MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "epoll_wait failed: " << std::strerror(errno) << '\n';
            break;
        }

        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd != server_fd) {
                continue;
            }

            while (true) {
                const int client_fd = ::accept(server_fd, nullptr, nullptr);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    std::cerr << "accept failed: " << std::strerror(errno) << '\n';
                    break;
                }

                set_blocking(client_fd);

                const int nodelay = 1;
                ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                             &nodelay, sizeof(nodelay));

                thread_pool_.submit([this, client_fd] {
                    handle_client(client_fd);
                });
            }
        }
    }

    ::close(epoll_fd);
    ::close(server_fd);
}

void Server::handle_client(int client_fd) {
    std::vector<uint8_t> buffer;

    while (true) {
        if (!Protocol::read_message(client_fd, buffer)) {
            break;
        }

        Request request;
        if (!request.ParseFromArray(buffer.data(),
                                    static_cast<int>(buffer.size()))) {
            Response response;
            response.set_status(Response::ERROR);
            response.set_errmsg("Failed to parse request");

            std::vector<uint8_t> out(response.ByteSizeLong());
            response.SerializeToArray(out.data(), static_cast<int>(out.size()));
            Protocol::write_message(client_fd, out);
            break;
        }

        Response response;
        response.set_status(Response::OK);

        switch (request.msg_case()) {
            case Request::kReset: {
                graph_store_.reset();
                break;
            }

            case Request::kWalk: {
                if (!graph_store_.add_walk(request.walk())) {
                    response.set_status(Response::ERROR);
                    response.set_errmsg("Invalid Walk request");
                }
                break;
            }

            case Request::kOneToOne: {
                uint64_t result = 0;
                if (graph_store_.one_to_one(
                        request.onetoone().origin(),
                        request.onetoone().destination(),
                        result)) {
                    response.set_shortest_path_length(result);
                }
                break;
            }

            case Request::kOneToAll: {
                uint64_t result = 0;
                if (graph_store_.one_to_all(
                        request.onetoall().origin(), result)) {
                    response.set_total_length(result);
                }
                break;
            }

            case Request::MSG_NOT_SET:
            default: {
                response.set_status(Response::ERROR);
                response.set_errmsg("Empty request");
                break;
            }
        }

        std::vector<uint8_t> out(response.ByteSizeLong());
        if (!response.SerializeToArray(out.data(),
                                       static_cast<int>(out.size()))) {
            break;
        }

        if (!Protocol::write_message(client_fd, out)) {
            break;
        }
    }

    ::close(client_fd);
}