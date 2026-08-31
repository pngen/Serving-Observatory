#pragma once
// Serving Observatory — core primitive types.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <optional>
#include <utility>
#include <vector>
#include <functional>

namespace servingobs {

// ---------------------------------------------------------------- integers
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

using byte  = std::byte;
using bytes = std::vector<byte>;

using string  = std::string;
using string_view = std::string_view;

// Timestamps are unsigned 64-bit nanoseconds on a monotonic clock domain.
// The clock-domain name travels with the observation for provenance.
using TimestampNs = u64;

// A 64-bit monotonic sequence number scoped to a source.
using SeqNum = u64;

// ---------------------------------------------------------------- bytes
inline bytes make_bytes(const void* p, std::size_t n) {
    const auto* b = static_cast<const byte*>(p);
    return { b, b + n };
}

template <class T>
bytes object_bytes(const T& v) {
    return make_bytes(&v, sizeof(T));
}

// ---------------------------------------------------------------- Result
// A small Result<T> (std::expected is C++23). Holds T or a string error.
template <class T>
class Result {
public:
    Result() = default;
    Result(T v) : value_(std::move(v)) {}
    Result(string err) : error_(std::move(err)) {}

    bool ok() const { return value_.has_value(); }
    explicit operator bool() const { return value_.has_value(); }

    T& value() & {
        if (!value_) throw std::runtime_error("Result::value() called on an error result");
        return *value_;
    }
    const T& value() const& {
        if (!value_) throw std::runtime_error("Result::value() called on an error result");
        return *value_;
    }
    T&& value() && {
        if (!value_) throw std::runtime_error("Result::value() called on an error result");
        return std::move(*value_);
    }

    const string& error() const { return error_; }
    string& error() { return error_; }

    static Result<T> Ok(T v) { return Result(std::move(v)); }
    static Result<T> Err(string m) { return Result(std::move(m)); }

private:
    std::optional<T> value_;
    string error_;
};

// ---------------------------------------------------------------- string helpers
string_view trim(string_view s);

} // namespace servingobs
