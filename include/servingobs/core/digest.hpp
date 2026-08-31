#pragma once
// Serving Observatory — deterministic SHA-256 digests.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/core/types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace servingobs {

using Digest = std::array<u8, 32>;

// Incremental SHA-256 (FIPS 180-4).
class Sha256 {
public:
    Sha256();
    void update(const byte* data, std::size_t n);
    void update(string_view s) { update(reinterpret_cast<const byte*>(s.data()), s.size()); }
    void update(const bytes& b) { update(b.data(), b.size()); }
    Digest finalize();

    static Digest hash(const byte* data, std::size_t n);
    static Digest hash(string_view s);
    static Digest hash(const bytes& b);
    static Digest hash_ns(u64 v);

private:
    void process_block(const u8* p);
    u32 h_[8];
    u64 total_len_;
    u8 buf_[64];
    std::size_t buf_len_;
};

string digest_hex(const Digest& d);
Result<Digest> digest_from_hex(string_view s);
bool digest_equal(const Digest& a, const Digest& b);

} // namespace servingobs
