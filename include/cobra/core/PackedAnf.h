#pragma once

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace cobra {

    class PackedAnf
    {
        std::vector< uint64_t > words_;
        size_t size_;

      public:
        explicit PackedAnf(size_t n) : words_((n + 63) / 64, 0), size_(n) {}

        PackedAnf(std::initializer_list< uint8_t > bits)
            : words_((bits.size() + 63) / 64, 0), size_(bits.size()) {
            size_t i = 0;
            for (auto b : bits) {
                if (b != 0u) {
                    Set(i);
                }
                ++i;
            }
        }

        uint8_t operator[](size_t i) const {
            return static_cast< uint8_t >((words_[i / 64] >> (i % 64)) & 1);
        }

        void Set(size_t i) { words_[i / 64] |= (1ULL << (i % 64)); }

        void Flip(size_t i) { words_[i / 64] ^= (1ULL << (i % 64)); }

        size_t size() const { return size_; }

        bool Empty() const { return size_ == 0; }

        uint64_t &Word(size_t w) { return words_[w]; }

        uint64_t Word(size_t w) const { return words_[w]; }

        size_t WordCount() const { return words_.size(); }
    };

} // namespace cobra
