#include "server.hpp"
#include "fast_protocol.hpp"
#include "protocol.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <thread>

#ifdef __linux__
#  include <sys/epoll.h>
#endif

#include "server.pb.h"

Server::Server(int port) : port_(port) {
    writer_thread_ = std::thread([this] {
        writer_loop();
    });
}

Server::~Server() {
    auto task = std::make_unique<WriterTask>();
    task->kind = WriterTaskKind::Stop;
    {
        std::unique_lock lock(writer_mutex_);
        writer_queue_.push_back(std::move(task));
    }
    writer_cv_.notify_one();
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
}

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

void Server::clear_pending_batch(PendingWalkBatch& pending) {
    pending.points.clear();
    pending.lengths.clear();
    pending.point_offsets.clear();
    pending.length_offsets.clear();
    pending.raw_prefix.clear();
    pending.raw_prefix_bytes = 0;
    pending.point_offsets.push_back(0);
    pending.length_offsets.push_back(0);
}

bool Server::pending_batch_empty(const PendingWalkBatch& pending) {
    return pending.point_offsets.size() <= 1;
}

bool Server::same_pending_batch(const PendingWalkBatch& a, const PendingWalkBatch& b) {
    if (a.point_offsets.size() != b.point_offsets.size() ||
        a.length_offsets.size() != b.length_offsets.size() ||
        a.points.size() != b.points.size() ||
        a.lengths.size() != b.lengths.size()) {
        return false;
    }

    return std::memcmp(a.point_offsets.data(),
                       b.point_offsets.data(),
                       a.point_offsets.size() * sizeof(uint32_t)) == 0 &&
           std::memcmp(a.length_offsets.data(),
                       b.length_offsets.data(),
                       a.length_offsets.size() * sizeof(uint32_t)) == 0 &&
           std::memcmp(a.points.data(),
                       b.points.data(),
                       a.points.size() * sizeof(Point)) == 0 &&
           std::memcmp(a.lengths.data(),
                       b.lengths.data(),
                       a.lengths.size() * sizeof(uint32_t)) == 0;
}

bool Server::submit_walk_batch(PendingWalkBatch&& batch) {
    auto task = std::make_unique<WriterTask>();
    task->kind = WriterTaskKind::Walk;
    task->batch = std::move(batch);

    if (active_clients_.load(std::memory_order_acquire) <= 1) {
        uint64_t generation = 0;
        {
            std::unique_lock lock(writer_mutex_);
            if (writer_queue_.empty() && !writer_busy_) {
                generation = ++next_generation_;
                latest_submitted_generation_.store(generation, std::memory_order_release);
            }
        }
        if (generation != 0) {
            const bool ok = commit_batch_to_graph(task->batch, 1);
            {
                std::unique_lock lock(writer_mutex_);
                committed_generation_ = std::max(committed_generation_, generation);
            }
            committed_cv_.notify_all();
            return ok;
        }
    }

    {
        std::unique_lock lock(writer_mutex_);
        task->generation = ++next_generation_;
        latest_submitted_generation_.store(task->generation, std::memory_order_release);
        writer_queue_.push_back(std::move(task));
    }
    writer_cv_.notify_one();
    return true;
}

std::future<bool> Server::submit_reset() {
    auto task = std::make_unique<WriterTask>();
    task->kind = WriterTaskKind::Reset;
    task->done = std::make_shared<std::promise<bool>>();
    auto future = task->done->get_future();
    {
        std::unique_lock lock(writer_mutex_);
        task->generation = ++next_generation_;
        latest_submitted_generation_.store(task->generation, std::memory_order_release);
        writer_queue_.push_back(std::move(task));
    }
    writer_cv_.notify_one();
    return future;
}

void Server::wait_for_generation(uint64_t generation) {
    std::unique_lock lock(writer_mutex_);
    committed_cv_.wait(lock, [&] {
        return committed_generation_ >= generation;
    });
}

bool Server::commit_batch_to_graph(PendingWalkBatch& batch, uint32_t repeat_count) {
    if (pending_batch_empty(batch)) return true;
    if (graph_store_.add_walks_flat_repeated(batch.points,
                                             batch.lengths,
                                             batch.point_offsets,
                                             batch.length_offsets,
                                             repeat_count)) {
        return true;
    }

    if (batch.raw_prefix.empty()) return false;
    if (batch.raw_prefix.size() + 1 != batch.point_offsets.size()) return false;

    std::vector<std::vector<Point>> walks_points;
    std::vector<std::vector<uint32_t>> walks_lengths;
    walks_points.reserve(batch.raw_prefix.size());
    walks_lengths.reserve(batch.raw_prefix.size());

    Request raw_request;
    for (const auto& raw : batch.raw_prefix) {
        raw_request.Clear();
        if (!raw_request.ParseFromArray(raw.data(), static_cast<int>(raw.size())) ||
            raw_request.msg_case() != Request::kWalk ||
            raw_request.walk().locations_size() < 2 ||
            raw_request.walk().lengths_size() != raw_request.walk().locations_size() - 1) {
            return false;
        }

        auto& points = walks_points.emplace_back();
        auto& lengths = walks_lengths.emplace_back();
        points.reserve(static_cast<std::size_t>(raw_request.walk().locations_size()));
        lengths.reserve(static_cast<std::size_t>(raw_request.walk().lengths_size()));

        for (const auto& loc : raw_request.walk().locations()) {
            points.push_back(Point{loc.x(), loc.y()});
        }
        for (const uint32_t length : raw_request.walk().lengths()) {
            lengths.push_back(length);
        }
    }

    for (uint32_t i = 0; i < repeat_count; ++i) {
        if (!graph_store_.add_walks(walks_points, walks_lengths)) return false;
    }
    return true;
}

void Server::writer_loop() {
    std::vector<std::unique_ptr<WriterTask>> tasks;
    std::vector<uint8_t> consumed;
    std::vector<uint8_t> fulfilled;

    while (true) {
        tasks.clear();
        {
            std::unique_lock lock(writer_mutex_);
            writer_cv_.wait(lock, [&] {
                return !writer_queue_.empty();
            });
            if (active_clients_.load(std::memory_order_acquire) > 16 && writer_queue_.size() < 8) {
                writer_cv_.wait_for(lock, std::chrono::microseconds(250));
            }
            while (!writer_queue_.empty()) {
                tasks.push_back(std::move(writer_queue_.front()));
                writer_queue_.pop_front();
            }
            writer_busy_ = true;
        }

        std::size_t i = 0;
        while (i < tasks.size()) {
            if (tasks[i]->kind == WriterTaskKind::Stop) {
                if (tasks[i]->done) tasks[i]->done->set_value(true);
                {
                    std::unique_lock lock(writer_mutex_);
                    writer_busy_ = false;
                }
                return;
            }

            if (tasks[i]->kind == WriterTaskKind::Reset) {
                graph_store_.reset();
                if (tasks[i]->done) tasks[i]->done->set_value(true);
                {
                    std::unique_lock lock(writer_mutex_);
                    committed_generation_ = std::max(committed_generation_, tasks[i]->generation);
                }
                committed_cv_.notify_all();
                ++i;
                continue;
            }

            std::size_t end = i;
            while (end < tasks.size() && tasks[end]->kind == WriterTaskKind::Walk) {
                ++end;
            }

            consumed.assign(end - i, 0);
            fulfilled.assign(end - i, 0);
            for (std::size_t a = i; a < end; ++a) {
                const std::size_t ai = a - i;
                if (consumed[ai]) continue;

                uint32_t repeat_count = 1;
                for (std::size_t b = a + 1; b < end; ++b) {
                    const std::size_t bi = b - i;
                    if (!consumed[bi] && same_pending_batch(tasks[a]->batch, tasks[b]->batch)) {
                        consumed[bi] = 1;
                        ++repeat_count;
                    }
                }

                const bool ok = commit_batch_to_graph(tasks[a]->batch, repeat_count);
                if (!fulfilled[ai] && tasks[a]->done) {
                    tasks[a]->done->set_value(ok);
                    fulfilled[ai] = 1;
                }
                for (std::size_t b = a + 1; b < end; ++b) {
                    const std::size_t bi = b - i;
                    if (consumed[bi] && !fulfilled[bi] && tasks[b]->done) {
                        tasks[b]->done->set_value(ok);
                        fulfilled[bi] = 1;
                    }
                }
            }

            uint64_t committed = 0;
            for (std::size_t a = i; a < end; ++a) {
                committed = std::max(committed, tasks[a]->generation);
            }
            {
                std::unique_lock lock(writer_mutex_);
                committed_generation_ = std::max(committed_generation_, committed);
            }
            committed_cv_.notify_all();
            i = end;
        }

        {
            std::unique_lock lock(writer_mutex_);
            writer_busy_ = false;
        }
    }
}

void Server::handle_client(int client_fd) {
    active_clients_.fetch_add(1, std::memory_order_acq_rel);
    BufferedReader reader(client_fd, 1024 * 1024);
    BufferedWriter writer(client_fd);
    std::vector<uint8_t> input;
    std::vector<uint8_t> out;
    Request request;
    Response response;
    FastRequest fast_request;
    PendingWalkBatch batch;

    auto remember_raw_prefix = [](PendingWalkBatch& pending, const std::vector<uint8_t>& raw) {
        static constexpr std::size_t MAX_RAW_PREFIX_WALKS = 64;
        static constexpr std::size_t MAX_RAW_PREFIX_BYTES = 1u << 20u;
        if (pending.raw_prefix.size() >= MAX_RAW_PREFIX_WALKS) return;
        if (pending.raw_prefix_bytes + raw.size() > MAX_RAW_PREFIX_BYTES) return;
        pending.raw_prefix.push_back(raw);
        pending.raw_prefix_bytes += raw.size();
    };

    auto write_response = [&]() -> bool {
        out.clear();
        append_serialized_response_frame(out, response);
        return writer.append(out);
    };

    auto submit_current_batch = [&]() -> bool {
        if (pending_batch_empty(batch)) return true;
        const bool ok = submit_walk_batch(std::move(batch));
        batch = PendingWalkBatch{};
        return ok;
    };

    auto wait_until_current = [&]() {
        const uint64_t generation = latest_submitted_generation_.load(std::memory_order_acquire);
        if (generation != 0) {
            wait_for_generation(generation);
        }
    };

    while (Protocol::read_message(reader, input)) {
        if (input.size() >= 1024 && input[0] == 0x0a) {
            out.clear();
            const std::size_t point_start = batch.points.size();
            const std::size_t length_start = batch.lengths.size();
            bool parsed = false;
            {
                parsed = parse_fast_walk_into(input.data(), input.size(), batch.points, batch.lengths);
                if (parsed &&
                    batch.points.size() - point_start >= 2 &&
                    batch.lengths.size() - length_start == batch.points.size() - point_start - 1) {
                    batch.point_offsets.push_back(static_cast<uint32_t>(batch.points.size()));
                    batch.length_offsets.push_back(static_cast<uint32_t>(batch.lengths.size()));
                    remember_raw_prefix(batch, input);
                } else {
                    batch.points.resize(point_start);
                    batch.lengths.resize(length_start);
                    parsed = false;
                }
            }

            if (parsed) {
                append_empty_ok_frame(out);
                if (!writer.append(out)) goto close_client;
                continue;
            }
        }

        if (parse_fast_request(input.data(), input.size(), fast_request)) {
            uint64_t result = 0;
            out.clear();

            switch (fast_request.kind) {
                case FastKind::Reset: {
                    clear_pending_batch(batch);
                    auto future = submit_reset();
                    if (!future.get()) goto close_client;
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
                            const std::size_t point_start = batch.points.size();
                            const std::size_t length_start = batch.lengths.size();
                            batch.points.resize(point_start + fast_request.locations.size());
                            batch.lengths.resize(length_start + fast_request.lengths.size());
                            std::move(fast_request.locations.begin(),
                                      fast_request.locations.end(),
                                      batch.points.begin() + static_cast<std::ptrdiff_t>(point_start));
                            std::move(fast_request.lengths.begin(),
                                      fast_request.lengths.end(),
                                      batch.lengths.begin() + static_cast<std::ptrdiff_t>(length_start));
                            batch.point_offsets.push_back(static_cast<uint32_t>(batch.points.size()));
                            batch.length_offsets.push_back(static_cast<uint32_t>(batch.lengths.size()));
                            remember_raw_prefix(batch, input);
                        }

                        append_empty_ok_frame(out);
                        if (!writer.append(out)) goto close_client;
                        continue;
                    }
                    break;

                case FastKind::OneToOne: {
                    if (!submit_current_batch()) goto close_client;
                    wait_until_current();
                    out.clear();
                    if (graph_store_.one_to_one(
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
                    if (!submit_current_batch()) goto close_client;
                    wait_until_current();
                    out.clear();
                    if (graph_store_.one_to_all(
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
                    clear_pending_batch(batch);
                    auto future = submit_reset();
                    if (!future.get()) goto close_client;
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
                        batch.points.insert(batch.points.end(), pts.begin(), pts.end());
                        batch.lengths.insert(batch.lengths.end(), lens.begin(), lens.end());
                        batch.point_offsets.push_back(static_cast<uint32_t>(batch.points.size()));
                        batch.length_offsets.push_back(static_cast<uint32_t>(batch.lengths.size()));
                        remember_raw_prefix(batch, input);
                    }
                    if (!write_response()) goto close_client;
                    continue;
                }
                break;
            }

            case Request::kOneToOne: {
                if (!submit_current_batch()) goto close_client;
                wait_until_current();
                response.Clear();
                response.set_status(Response::OK);
                uint64_t result = 0;
                if (graph_store_.one_to_one(
                    request.onetoone().origin(),
                    request.onetoone().destination(),
                    result)) {
                    response.set_shortest_path_length(result);
                }
                if (!write_response()) goto close_client;
                if (!writer.flush()) goto close_client;
                continue;
            }

            case Request::kOneToAll: {
                if (!submit_current_batch()) goto close_client;
                wait_until_current();
                response.Clear();
                response.set_status(Response::OK);
                uint64_t result = 0;
                if (graph_store_.one_to_all(
                    request.onetoall().origin(),
                    result)) {
                    response.set_total_length(result);
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
    submit_current_batch();
    active_clients_.fetch_sub(1, std::memory_order_acq_rel);
    ::close(client_fd);
}
