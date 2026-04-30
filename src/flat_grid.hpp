#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

class FlatGrid {
public:
    static constexpr std::size_t INLINE_CAPACITY = 4;

    struct alignas(32) Slot {
        int64_t  key;
        uint32_t count;
        uint32_t ids[INLINE_CAPACITY];
    };

    void clear_and_reserve(std::size_t expected_entries) {
        std::size_t buckets = 256;
        while (buckets < expected_entries * 2) buckets <<= 1;
        table_.assign(buckets, Slot{});
        mask_ = buckets - 1;
    }

    void clear() {
        if (!table_.empty()) {
            std::memset(table_.data(), 0, table_.size() * sizeof(Slot));
        }
    }

    [[nodiscard]] const Slot* find(int64_t key) const noexcept {
        if (table_.empty()) return nullptr;
        std::size_t i = hash(key) & mask_;
        for (;;) {
            const Slot& s = table_[i];
            if (s.count == 0) return nullptr;
            if (s.key == key) return &s;
            i = (i + 1) & mask_;
        }
    }

    void add(int64_t key, uint32_t id) {
        std::size_t i = hash(key) & mask_;
        for (;;) {
            Slot& s = table_[i];
            if (s.count == 0) {
                s.key = key;
                s.count = 1;
                s.ids[0] = id;
                return;
            }
            if (s.key == key) {
                if (s.count < INLINE_CAPACITY) {
                    s.ids[s.count++] = id;
                }
                return;
            }
            i = (i + 1) & mask_;
        }
    }

private:
    static std::size_t hash(int64_t key) noexcept {
        uint64_t x = static_cast<uint64_t>(key);
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return static_cast<std::size_t>(x ^ (x >> 31));
    }

    std::vector<Slot> table_;
    std::size_t mask_ = 0;
};
