#include "server.hpp"
#include "protocol.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#ifdef __linux__
#  include <sys/epoll.h>
#endif

#include "server.pb.h"

Server::Server(int port) : port_(port) {}

static void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void configure_client(int fd) {
    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int bufsize = 1 << 20;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
}

void Server::run() {
    const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { std::cerr << "socket failed\n"; return; }

    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<uint16_t>(port_));

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind failed: " << std::strerror(errno) << '\n';
        ::close(server_fd); return;
    }
    if (::listen(server_fd, 1024) < 0) {
        std::cerr << "listen failed\n";
        ::close(server_fd); return;
    }

    std::cout << "Server listening on port " << port_ << std::endl;

#ifdef __linux__

    set_nonblocking(server_fd);

    const int epoll_fd = ::epoll_create1(0);
    if (epoll_fd < 0) { ::close(server_fd); return; }

    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = server_fd;
    ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    constexpr int MAX_EVENTS = 128;
    std::array<epoll_event, MAX_EVENTS> events{};

    while (true) {
        const int n = ::epoll_wait(epoll_fd, events.data(), MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd != server_fd) continue;

            while (true) {
                const int client_fd = ::accept(server_fd, nullptr, nullptr);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    break;
                }

                configure_client(client_fd);

                thread_pool_.submit([this, client_fd] {
                    handle_client(client_fd);
                });
            }
        }
    }

    ::close(epoll_fd);
#else
    
    while (true) {
        const int client_fd = ::accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        configure_client(client_fd);
        thread_pool_.submit([this, client_fd] {
            handle_client(client_fd);
        });
    }
#endif

    ::close(server_fd);
}

void Server::handle_client(int client_fd) {
    BufferedReader reader(client_fd);
    std::vector<uint8_t> buffer;
    std::vector<uint8_t> out;

    Request  request;
    Response response;

    while (true) {
        if (!Protocol::read_message(reader, buffer)) break;

        request.Clear();
        if (!request.ParseFromArray(buffer.data(), static_cast<int>(buffer.size()))) {
            response.Clear();
            response.set_status(Response::ERROR);
            response.set_errmsg("Failed to parse request");
            out.resize(response.ByteSizeLong());
            response.SerializeToArray(out.data(), static_cast<int>(out.size()));
            Protocol::write_message(client_fd, out.data(), out.size());
            break;
        }

        response.Clear();
        response.set_status(Response::OK);

        switch (request.msg_case()) {
            case Request::kReset:
                graph_store_.reset();
                break;

            case Request::kWalk:
                if (!graph_store_.add_walk(request.walk())) {
                    response.set_status(Response::ERROR);
                    response.set_errmsg("Invalid Walk request");
                }
                break;

            case Request::kOneToOne: {
                uint64_t result = 0;
                if (graph_store_.one_to_one(
                        request.onetoone().origin(),
                        request.onetoone().destination(), result)) {
                    response.set_shortest_path_length(result);
                }
                break;
            }

            case Request::kOneToAll: {
                uint64_t result = 0;
                if (graph_store_.one_to_all(request.onetoall().origin(), result)) {
                    response.set_total_length(result);
                }
                break;
            }

            default:
                response.set_status(Response::ERROR);
                response.set_errmsg("Empty request");
                break;
        }

        out.resize(response.ByteSizeLong());
        if (!response.SerializeToArray(out.data(), static_cast<int>(out.size()))) break;
        if (!Protocol::write_message(client_fd, out.data(), out.size())) break;
    }

    ::close(client_fd);
}
