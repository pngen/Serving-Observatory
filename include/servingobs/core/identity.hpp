#pragma once
// Serving Observatory — strongly typed 128-bit identities.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/core/types.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

namespace servingobs {

// ------------------------------------------------------------ Id128
// A 128-bit value stored as two 64-bit words (hi, lo). Serialized big-endian
// to 16 bytes and as a fixed 32-hex-character string. Round-trips exactly.
class Id128 {
public:
    constexpr Id128() : hi_(0), lo_(0) {}
    constexpr Id128(u64 hi, u64 lo) : hi_(hi), lo_(lo) {}

    static Id128 from_u64(u64 v) { return Id128(0, v); }
    static Id128 make(u64 hi, u64 lo) { return Id128(hi, lo); }

    u64 hi() const { return hi_; }
    u64 lo() const { return lo_; }

    // Deterministic string: exactly 32 lowercase hex characters.
    string to_hex() const;

    // Parse a 32-char (optionally 0x-prefixed or hyphen/collon-grouped) hex.
    static Result<Id128> parse(string_view s);

    // Deterministic 16-byte big-endian representation.
    bytes to_bytes() const;

    // Derive an id deterministically from a namespace tag and payload bytes.
    static Id128 derive(u64 namespace_tag, const byte* data, std::size_t n);

    bool operator==(const Id128& o) const { return hi_ == o.hi_ && lo_ == o.lo_; }
    bool operator!=(const Id128& o) const { return !(*this == o); }
    bool operator<(const Id128& o) const {
        return hi_ < o.hi_ || (hi_ == o.hi_ && lo_ < o.lo_);
    }

private:
    u64 hi_;
    u64 lo_;
};

// ------------------------------------------------------------ typed ids
template <class Tag>
class Id {
public:
    constexpr Id() : v_() {}
    constexpr explicit Id(Id128 v) : v_(v) {}

    static Id from_u64(u64 v) { return Id(Id128::from_u64(v)); }
    static Id from_raw(u64 hi, u64 lo) { return Id(Id128(hi, lo)); }
    static Id from_hex(string_view s) {
        auto r = Id128::parse(s);
        return r.ok() ? Id(r.value()) : Id();
    }
    static Id derive(u64 ns, const byte* data, std::size_t n) {
        return Id(Id128::derive(ns, data, n));
    }

    // NOTE: empty (zero) id is never a valid authority. Callers must check.
    bool is_null() const { return v_ == Id128(); }
    const Id128& raw() const { return v_; }

    string to_hex() const { return v_.to_hex(); }

    bool operator==(const Id& o) const { return v_ == o.v_; }
    bool operator!=(const Id& o) const { return !(*this == o); }
    bool operator<(const Id& o) const { return v_ < o.v_; }

private:
    Id128 v_;
};

template <class Tag>
string to_string(const Id<Tag>& id) { return id.to_hex(); }

// Deterministic hash functor for typed ids (use as the Hash template argument
// of unordered containers; std::hash cannot be partially specialized).
template <class Tag>
struct IdHash {
    std::size_t operator()(const Id<Tag>& id) const noexcept {
        const Id128 r = id.raw();
        u64 x = r.hi();
        x ^= r.lo() + 0x9e3779b97f4a7c15ULL + (x << 6) + (x >> 2);
        return static_cast<std::size_t>(x);
    }
};

// ------------------------------------------------------------ named identities
struct RequestIdTag {};
struct TenantIdTag {};
struct WorkloadIdTag {};
struct ModelIdTag {};
struct ModelRevisionIdTag {};
struct AdapterIdTag {};
struct SequenceIdTag {};
struct BatchIdTag {};
struct PrefillIdTag {};
struct DecodeStepIdTag {};
struct AttemptIdTag {};
struct DispatchIdTag {};
struct WorkerIdTag {};
struct WorkerBootIdTag {};
struct NodeIdTag {};
struct DeviceIdTag {};
struct FlowIdTag {};
struct TransferIdTag {};
struct ReservationIdTag {};
struct ObservationIdTag {};
struct TraceIdTag {};
struct SnapshotIdTag {};
struct EventIdTag {};
struct SourceIdTag {};

using RequestId            = Id<RequestIdTag>;
using TenantId             = Id<TenantIdTag>;
using WorkloadId           = Id<WorkloadIdTag>;
using ModelId              = Id<ModelIdTag>;
using ModelRevisionId      = Id<ModelRevisionIdTag>;
using AdapterId            = Id<AdapterIdTag>;
using SequenceId           = Id<SequenceIdTag>;
using BatchId              = Id<BatchIdTag>;
using PrefillId            = Id<PrefillIdTag>;
using DecodeStepId         = Id<DecodeStepIdTag>;
using AttemptId            = Id<AttemptIdTag>;
using DispatchId           = Id<DispatchIdTag>;
using WorkerId             = Id<WorkerIdTag>;
using WorkerBootId         = Id<WorkerBootIdTag>;
using NodeId               = Id<NodeIdTag>;
using DeviceId             = Id<DeviceIdTag>;
using FlowId               = Id<FlowIdTag>;
using TransferId           = Id<TransferIdTag>;
using ReservationId        = Id<ReservationIdTag>;
using ObservationId        = Id<ObservationIdTag>;
using TraceId              = Id<TraceIdTag>;
using SnapshotId           = Id<SnapshotIdTag>;
using EventId              = Id<EventIdTag>;
using SourceId             = Id<SourceIdTag>;

// Epoch / generation counters are 64-bit monotonic values scoped to a source
// or the coordinator. They are not 128-bit identities.
using CoordinatorEpoch = u64;
using SourceGeneration = u64;
using ObservationGeneration = u64;

} // namespace servingobs
