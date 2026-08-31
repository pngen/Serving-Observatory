#pragma once
// Serving Observatory — canonical, validated binary codec (deterministic).
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// All integers are written big-endian, fixed-width. Strings and byte arrays are
// length-prefixed (u32). The reader performs strict validation and rejects
// truncation, malformed lengths, invalid enums, NaN/Inf, and trailing garbage.
// A value writes at a canonical offset; identical logical values produce
// identical byte streams (no padding, no unspecified std::string internals).

#include "servingobs/core/types.hpp"
#include "servingobs/core/identity.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace servingobs {

// ================================================================ writer
class BinaryWriter {
public:
    BinaryWriter() = default;

    size_t size() const { return buf_.size(); }
    const bytes& data() const { return buf_; }

    void write_u8(u8 v)   { put(v); }
    void write_u16(u16 v) { put_be(v); }
    void write_u32(u32 v) { put_be(v); }
    void write_u64(u64 v) { put_be(v); }
    void write_i8(i8 v)   { put(static_cast<u8>(v)); }
    void write_i16(i16 v) { put_be(static_cast<u16>(v)); }
    void write_i32(i32 v) { put_be(static_cast<u32>(v)); }
    void write_i64(i64 v) { put_be(static_cast<u64>(v)); }

    void write_bool(bool v) { put(v ? 1 : 0); }

    void write_f32(f32 v) {
        u32 bits;
        std::memcpy(&bits, &v, sizeof(bits));
        put_be(bits);
    }
    void write_f64(f64 v) {
        u64 bits;
        std::memcpy(&bits, &v, sizeof(bits));
        put_be(bits);
    }

    void write_str(string_view s) {
        write_len(s.size());
        append(reinterpret_cast<const byte*>(s.data()), s.size());
    }

    void write_bytes(const bytes& b) {
        write_len(b.size());
        append(b.data(), b.size());
    }

    void write_id(const Id128& v) {
        append(v.to_bytes().data(), 16);
    }

    // Write a raw count (u32) — must be validated by the caller to be in range.
    void write_count(u64 n) {
        if (n > 0xFFFFFFFFULL) throw std::runtime_error("count overflow");
        put_be(static_cast<u32>(n));
    }

private:
    void write_len(u64 n) {
        if (n > 0xFFFFFFFFULL) throw std::runtime_error("length overflow");
        put_be(static_cast<u32>(n));
    }
    void put(u8 v) { buf_.push_back(static_cast<byte>(v)); }
    void put_be(u16 v) {
        buf_.push_back(static_cast<byte>(v >> 8));
        buf_.push_back(static_cast<byte>(v));
    }
    void put_be(u32 v) {
        buf_.push_back(static_cast<byte>(v >> 24));
        buf_.push_back(static_cast<byte>(v >> 16));
        buf_.push_back(static_cast<byte>(v >> 8));
        buf_.push_back(static_cast<byte>(v));
    }
    void put_be(u64 v) {
        buf_.push_back(static_cast<byte>(v >> 56));
        buf_.push_back(static_cast<byte>(v >> 48));
        buf_.push_back(static_cast<byte>(v >> 40));
        buf_.push_back(static_cast<byte>(v >> 32));
        buf_.push_back(static_cast<byte>(v >> 24));
        buf_.push_back(static_cast<byte>(v >> 16));
        buf_.push_back(static_cast<byte>(v >> 8));
        buf_.push_back(static_cast<byte>(v));
    }
    void append(const byte* p, std::size_t n) { buf_.insert(buf_.end(), p, p + n); }

    bytes buf_;
};

// ================================================================ reader
// Bounds-checked, error-accumulating reader. Every read advances only on
// success. A failed read leaves the reader in a failed state; further reads
// are no-ops returning default values. Track an error message for reporting.
class BinaryReader {
public:
    explicit BinaryReader(const byte* data, std::size_t n) : p_(data), end_(data + n) {}
    explicit BinaryReader(const bytes& b) : p_(b.data()), end_(b.data() + b.size()) {}

    bool ok() const { return !failed_; }
    const string& error() const { return err_; }
    bool at_end() const { return p_ == end_; }
    size_t remaining() const { return static_cast<size_t>(end_ - p_); }

    void fail(string msg) {
        if (!failed_) { failed_ = true; err_ = std::move(msg); }
    }

    // Require the reader to be fully consumed (no trailing garbage).
    bool require_end() {
        if (failed_) return false;
        if (!at_end()) { fail("trailing garbage: " + std::to_string(remaining()) + " bytes"); return false; }
        return true;
    }

    u8  read_u8()  { return r_u8(); }
    u16 read_u16() { return r_u16(); }
    u32 read_u32() { return r_u32(); }
    u64 read_u64() { return r_u64(); }
    i8  read_i8()  { return static_cast<i8>(r_u8()); }
    i16 read_i16() { return static_cast<i16>(r_u16()); }
    i32 read_i32() { return static_cast<i32>(r_u32()); }
    i64 read_i64() { return static_cast<i64>(r_u64()); }

    bool read_bool() { return r_u8() != 0; }

    f32 read_f32() { u32 bits = r_u32(); f32 v; std::memcpy(&v, &bits, sizeof(v)); return v; }
    f64 read_f64() {
        u64 bits = r_u64();
        f64 v; std::memcpy(&v, &bits, sizeof(v));
        if (std::isnan(v) || std::isinf(v)) fail("NaN/Inf floating value rejected");
        return v;
    }

    // Reads a length and validates it does not exceed remaining bytes.
    u32 read_length() {
        u32 len = r_u32();
        if (failed_) return 0;
        if (static_cast<u64>(len) > remaining()) {
            fail("declared length " + std::to_string(len) + " exceeds remaining " +
                 std::to_string(remaining()));
            return 0;
        }
        return len;
    }

    string read_str() {
        u32 len = read_length();
        if (failed_) return {};
        string s(reinterpret_cast<const char*>(p_), len);
        p_ += len;
        return s;
    }

    bytes read_bytes() {
        u32 len = read_length();
        if (failed_) return {};
        bytes b(p_, p_ + len);
        p_ += len;
        return b;
    }

    // Read exactly n raw bytes (no length prefix), bounds-checked.
    bytes read_raw(u32 n) {
        if (failed_) return {};
        if (static_cast<u64>(n) > remaining()) { fail("read_raw overruns buffer"); return {}; }
        bytes b(p_, p_ + n);
        p_ += n;
        return b;
    }

    Id128 read_id() {
        if (failed_) return Id128();
        if (remaining() < 16) { fail("truncated id"); return Id128(); }
        u64 hi = 0, lo = 0;
        for (int i = 0; i < 8; ++i) hi = (hi << 8) | r_u8();
        for (int i = 0; i < 8; ++i) lo = (lo << 8) | r_u8();
        return Id128(hi, lo);
    }

    u64 read_count() { return r_u32(); }

    // Read a sub-buffer of exactly len bytes and return a nested reader for
    // validated length-prefixed sections.
    BinaryReader sub(u32 len) {
        if (failed_) return BinaryReader(p_, 0);
        if (static_cast<u64>(len) > remaining()) { fail("sub-section overruns buffer"); return BinaryReader(p_, 0); }
        BinaryReader r(p_, len);
        p_ += len;
        return r;
    }

private:
    const byte* p_;
    const byte* end_;
    bool failed_ = false;
    string err_;

    u8 r_u8() {
        if (failed_) return 0;
        if (p_ == end_) { fail("unexpected end of buffer"); return 0; }
        return static_cast<u8>(*p_++);
    }
    u16 r_u16() {
        u16 v = 0;
        for (int i = 0; i < 2; ++i) v = static_cast<u16>((v << 8) | r_u8());
        return v;
    }
    u32 r_u32() {
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v = static_cast<u32>((v << 8) | r_u8());
        return v;
    }
    u64 r_u64() {
        u64 v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | r_u8();
        return v;
    }
};

} // namespace servingobs
