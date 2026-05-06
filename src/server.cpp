#include "server.hpp"
#include "fast_protocol.hpp"
#include "protocol.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
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

namespace {

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

class BufferedWriter {
public:
    explicit BufferedWriter(int fd) : fd_(fd) {
        buffer_.reserve(256 * 1024);
    }

    bool append(std::span<const uint8_t> data) {
        if (buffer_.size() + data.size() > buffer_.capacity() && !flush()) {
            return false;
        }
        buffer_.insert(buffer_.end(), data.begin(), data.end());
        return buffer_.size() < buffer_.capacity() || flush();
    }

    bool flush() {
        if (buffer_.empty()) return true;
        const bool ok = write_raw(fd_, buffer_);
        buffer_.clear();
        return ok;
    }

private:
    int fd_;
    std::vector<uint8_t> buffer_;
};

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
        ::close(server_fd);
        return;
    }
    if (::listen(server_fd, 1024) < 0) {
        std::cerr << "listen failed\n";
        ::close(server_fd);
        return;
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
    BufferedReader reader(client_fd, 1024 * 1024);
    BufferedWriter writer(client_fd);
    std::vector<uint8_t> input;
    std::vector<uint8_t> out;
    Request request;
    Response response;
    FastRequest fast_request;
    PendingWalkBatch batch;

    {
        std::unique_lock lock(pending_mutex_);
        pending_batches_.push_back(&batch);
    }

    auto flush_all_walks_locked = [&]() -> bool {
        for (PendingWalkBatch* pending : pending_batches_) {
            if (pending->point_offsets.size() <= 1) continue;

            if (!graph_store_.add_walks_flat(pending->points,
                                             pending->lengths,
                                             pending->point_offsets,
                                             pending->length_offsets)) {
                return false;
            }
            pending->points.clear();
            pending->lengths.clear();
            pending->point_offsets.clear();
            pending->length_offsets.clear();
            pending->point_offsets.push_back(0);
            pending->length_offsets.push_back(0);
        }

        return true;
    };

    auto flush_current_walks_locked = [&]() -> bool {
        if (batch.point_offsets.size() <= 1) return true;
        const bool ok = graph_store_.add_walks_flat(batch.points,
                                                    batch.lengths,
                                                    batch.point_offsets,
                                                    batch.length_offsets);
        batch.points.clear();
        batch.lengths.clear();
        batch.point_offsets.clear();
        batch.length_offsets.clear();
        batch.point_offsets.push_back(0);
        batch.length_offsets.push_back(0);
        return ok;
    };

    auto write_response = [&]() -> bool {
        out.clear();
        append_serialized_response_frame(out, response);
        return writer.append(out);
    };

    while (Protocol::read_message(reader, input)) {
        if (!input.empty() && input[0] == 0x0a) {
            out.clear();
            const std::size_t point_start = batch.points.size();
            const std::size_t length_start = batch.lengths.size();
            bool parsed = false;
            {
                std::shared_lock pending_lock(pending_mutex_);
                parsed = parse_fast_walk_into(input.data(), input.size(), batch.points, batch.lengths);
                if (parsed &&
                    batch.points.size() - point_start >= 2 &&
                    batch.lengths.size() - length_start == batch.points.size() - point_start - 1) {
                    batch.point_offsets.push_back(static_cast<uint32_t>(batch.points.size()));
                    batch.length_offsets.push_back(static_cast<uint32_t>(batch.lengths.size()));
                } else {
                    batch.points.resize(point_start);
                    batch.lengths.resize(length_start);
                    parsed = false;
                }
            }

            if (parsed) {
                append_empty_ok_frame(out);
                if (!writer.append(out)) goto close_client;
                if (!reader.has_buffered_data() && !writer.flush()) goto close_client;
                continue;
            }
        }

        if (parse_fast_request(input.data(), input.size(), fast_request)) {
            uint64_t result = 0;
            out.clear();

            switch (fast_request.kind) {
                case FastKind::Reset: {
                    std::unique_lock pending_lock(pending_mutex_);
                    for (PendingWalkBatch* pending : pending_batches_) {
                        pending->points.clear();
                        pending->lengths.clear();
                        pending->point_offsets.clear();
                        pending->length_offsets.clear();
                        pending->point_offsets.push_back(0);
                        pending->length_offsets.push_back(0);
                    }
                    graph_store_.reset();
                    append_empty_ok_frame(out);
                    if (!writer.append(out) || !writer.flush()) goto close_client;
                    continue;
                }

                case FastKind::Walk:
                    if (fast_request.locations.size() < 2 ||
                        fast_request.lengths.size() != fast_request.locations.size() - 1) {
                        response.Clear();
                        response.set_status(Response::ERROR);
                        response.set_errmsg("Invalid Walk request");
                        append_serialized_response_frame(out, response);
                    } else {
                        {
                            std::shared_lock pending_lock(pending_mutex_);
                            batch.points.insert(batch.points.end(),
                                                fast_request.locations.begin(),
                                                fast_request.locations.end());
                            batch.lengths.insert(batch.lengths.end(),
                                                 fast_request.lengths.begin(),
                                                 fast_request.lengths.end());
                            batch.point_offsets.push_back(static_cast<uint32_t>(batch.points.size()));
                            batch.length_offsets.push_back(static_cast<uint32_t>(batch.lengths.size()));
                        }

                        append_empty_ok_frame(out);
                        if (!writer.append(out)) goto close_client;
                        if (!reader.has_buffered_data() && !writer.flush()) goto close_client;
                        continue;
                    }
                    break;

                case FastKind::OneToOne: {
                    std::unique_lock pending_lock(pending_mutex_);
                    if (!flush_all_walks_locked()) {
                        response.Clear();
                        response.set_status(Response::ERROR);
                        response.set_errmsg("Invalid pending Walk request");
                        append_serialized_response_frame(out, response);
                    } else if (graph_store_.one_to_one(
                            fast_request.origin,
                            fast_request.destination,
                            result)) {
                        append_uint64_response_frame(out, 3, result);
                    } else {
                        append_empty_ok_frame(out);
                    }
                    if (!writer.append(out) || !writer.flush()) goto close_client;
                    continue;
                }

                case FastKind::OneToAll: {
                    std::unique_lock pending_lock(pending_mutex_);
                    if (!flush_all_walks_locked()) {
                        response.Clear();
                        response.set_status(Response::ERROR);
                        response.set_errmsg("Invalid pending Walk request");
                        append_serialized_response_frame(out, response);
                    } else if (graph_store_.one_to_all(
                            fast_request.origin,
                            result)) {
                        append_uint64_response_frame(out, 4, result);
                    } else {
                        append_empty_ok_frame(out);
                    }
                    if (!writer.append(out) || !writer.flush()) goto close_client;
                    continue;
                }

                case FastKind::Unknown:
                    response.Clear();
                    response.set_status(Response::ERROR);
                    response.set_errmsg("Invalid request");
                    append_serialized_response_frame(out, response);
                    break;
            }

            if (!writer.append(out)) break;
            if (!reader.has_buffered_data() && !writer.flush()) break;
            continue;
        }

        request.Clear();

        if (!request.ParseFromArray(input.data(), static_cast<int>(input.size()))) {
            response.Clear();
            response.set_status(Response::ERROR);
            response.set_errmsg("Failed to parse request");
            if (!write_response()) break;
            break;
        }

        response.Clear();
        response.set_status(Response::OK);

        switch (request.msg_case()) {

            case Request::kReset:
                {
                    std::unique_lock pending_lock(pending_mutex_);
                    for (PendingWalkBatch* pending : pending_batches_) {
                        pending->points.clear();
                        pending->lengths.clear();
                        pending->point_offsets.clear();
                        pending->length_offsets.clear();
                        pending->point_offsets.push_back(0);
                        pending->length_offsets.push_back(0);
                    }
                    graph_store_.reset();
                    if (!write_response() || !writer.flush()) goto close_client;
                }
                continue;

            case Request::kWalk: {
                if (request.walk().locations_size() < 2 ||
                    request.walk().lengths_size() != request.walk().locations_size() - 1) {

                    response.set_status(Response::ERROR);
                    response.set_errmsg("Invalid Walk request");
                } else {
                    std::vector<Point> pts;
                    std::vector<uint32_t> lens;

                    for (const auto& loc : request.walk().locations()) {
                        pts.push_back(Point{loc.x(), loc.y()});
                    }
                    for (auto l : request.walk().lengths()) {
                        lens.push_back(l);
                    }

                    {
                        std::shared_lock pending_lock(pending_mutex_);
                        batch.points.insert(batch.points.end(), pts.begin(), pts.end());
                        batch.lengths.insert(batch.lengths.end(), lens.begin(), lens.end());
                        batch.point_offsets.push_back(static_cast<uint32_t>(batch.points.size()));
                        batch.length_offsets.push_back(static_cast<uint32_t>(batch.lengths.size()));
                    }
                    if (!write_response()) goto close_client;
                    if (!reader.has_buffered_data() && !writer.flush()) goto close_client;
                    continue;
                }
                break;
            }

            case Request::kOneToOne: {
                std::unique_lock pending_lock(pending_mutex_);
                if (!flush_all_walks_locked()) {
                    response.set_status(Response::ERROR);
                    response.set_errmsg("Invalid pending Walk request");
                } else {
                    uint64_t result = 0;
                    if (graph_store_.one_to_one(
                        request.onetoone().origin(),
                        request.onetoone().destination(),
                        result)) {
                        response.set_shortest_path_length(result);
                    }
                }
                if (!write_response()) goto close_client;
                if (!writer.flush()) goto close_client;
                continue;
            }

            case Request::kOneToAll: {
                std::unique_lock pending_lock(pending_mutex_);
                if (!flush_all_walks_locked()) {
                    response.set_status(Response::ERROR);
                    response.set_errmsg("Invalid pending Walk request");
                } else {
                    uint64_t result = 0;
                    if (graph_store_.one_to_all(
                        request.onetoall().origin(),
                        result)) {
                        response.set_total_length(result);
                    }
                }
                if (!write_response()) goto close_client;
                if (!writer.flush()) goto close_client;
                continue;
            }

            default:
                response.set_status(Response::ERROR);
                response.set_errmsg("Empty request");
                break;
        }

        if (!write_response()) break;
        if (!reader.has_buffered_data() && !writer.flush()) break;
    }

close_client:
    writer.flush();
    {
        std::unique_lock pending_lock(pending_mutex_);
        flush_current_walks_locked();
        const auto it = std::find(pending_batches_.begin(), pending_batches_.end(), &batch);
        if (it != pending_batches_.end()) {
            pending_batches_.erase(it);
        }
    }
    ::close(client_fd);
}
