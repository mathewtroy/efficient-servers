#include "protocol.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>

bool BufferedReader::fill() {
    const std::size_t remaining = tail_ - head_;
    if (head_ > 0 && remaining > 0) {
        std::memmove(buf_.data(), buf_.data() + head_, remaining);
    }
    tail_ = remaining;
    head_ = 0;

    const auto n = ::read(fd_, buf_.data() + tail_, buf_.size() - tail_);
    if (n <= 0) {
        if (n < 0 && errno == EINTR) return fill();
        return false;
    }
    tail_ += static_cast<std::size_t>(n);
    return true;
}

bool BufferedReader::read_exact(void* out, std::size_t size) {
    auto* ptr = static_cast<uint8_t*>(out);
    std::size_t done = 0;

    while (done < size) {
        const std::size_t avail = tail_ - head_;
        if (avail == 0) {
            if (!fill()) return false;
            continue;
        }
        const std::size_t take = std::min(avail, size - done);
        std::memcpy(ptr + done, buf_.data() + head_, take);
        head_ += take;
        done  += take;
    }
    return true;
}

bool BufferedReader::read_exact_to_vector(std::vector<uint8_t>& out, std::size_t size) {
    out.resize(size);
    auto* ptr = out.data();
    std::size_t done = 0;

    while (done < size) {
        const std::size_t avail = tail_ - head_;
        if (avail > 0) {
            const std::size_t take = std::min(avail, size - done);
            std::memcpy(ptr + done, buf_.data() + head_, take);
            head_ += take;
            done += take;
            continue;
        }

        if (size - done >= buf_.size() / 2) {
            const auto n = ::read(fd_, ptr + done, size - done);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                return false;
            }
            done += static_cast<std::size_t>(n);
        } else if (!fill()) {
            return false;
        }
    }

    return true;
}

bool Protocol::write_exact(int fd, const void* buffer, std::size_t size) {
    auto* ptr = static_cast<const uint8_t*>(buffer);
    std::size_t total = 0;

    while (total < size) {
        const auto n = ::write(fd, ptr + total, size - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        total += static_cast<std::size_t>(n);
    }
    return true;
}

bool Protocol::read_message(BufferedReader& reader, std::vector<uint8_t>& buffer) {
    uint32_t length_be = 0;
    if (!reader.read_exact(&length_be, sizeof(length_be))) return false;
    const uint32_t length = ntohl(length_be);
    if (length == 0) {
        buffer.clear();
        return true;
    }
    return reader.read_exact_to_vector(buffer, length);
}

bool Protocol::write_message(int fd, const void* data, std::size_t size) {
    const uint32_t length_be = htonl(static_cast<uint32_t>(size));

    if (size <= 4096) {
        uint8_t tmp[4100];
        std::memcpy(tmp, &length_be, 4);
        if (size > 0) std::memcpy(tmp + 4, data, size);
        return write_exact(fd, tmp, 4 + size);
    }

    if (!write_exact(fd, &length_be, 4)) return false;
    if (size == 0) return true;
    return write_exact(fd, data, size);
}
