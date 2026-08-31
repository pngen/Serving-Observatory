#pragma once
// Serving Observatory — framed TCP protocol with strict decoding.
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// Wire frame:
//   [u32 len][u32 crc32][u16 version][u16 msg_type][payload...]
//   len = number of bytes following the len field (=> 8 + payload.size()).
//   crc32 = CRC-32 (IEEE) over [version][msg_type][payload].
// Decoding rejects: truncation, malformed lengths, checksum mismatch, trailing
// garbage, unsupported versions, and unknown message types.

#include "servingobs/core/types.hpp"
#include "servingobs/core/identity.hpp"

#include <string>

namespace servingobs {

constexpr u16 kProtocolVersion = 1;
constexpr u32 kFrameMagic = 0x534F4253u; // "SOBS"

enum class MsgType : u16 {
    HELLO = 1,       // source -> coordinator: register boot authority
    HELLO_ACK = 2,   // coordinator -> source
    OBS = 3,         // source -> coordinator: one observation
    OBS_ACK = 4,     // coordinator -> source: decision (accepted/rejected)
    STATUS = 5,      // health/status exchange
    BYE = 6,         // graceful close
    PING = 7,
    PONG = 8,
};
string_view msg_type_name(MsgType t);

struct Frame {
    u16 version = kProtocolVersion;
    MsgType type = MsgType::PING;
    bytes payload;

    // Encode the full wire frame bytes.
    static bytes encode(MsgType t, const bytes& payload, u16 version = kProtocolVersion);
    // Decode a complete frame (must be exactly the encoded bytes).
    static Result<Frame> decode(const byte* data, std::size_t n);
    static Result<Frame> decode(const bytes& b) { return decode(b.data(), b.size()); }

    // Total wire length that encode(..., payload) produces.
    static u32 wire_length(const bytes& payload);
};

// CRC-32 (IEEE 802.3, polynomial 0xEDB88320).
u32 crc32(const byte* data, std::size_t n);
u32 crc32(const bytes& b);

// ---- payload builders / parsers (deterministic binary) ----

// HELLO payload fields
struct HelloPayload {
    SourceId source;
    WorkerId worker;
    WorkerBootId boot;
    SourceGeneration src_gen = 0;
    ObservationGeneration obs_gen = 0;
    CoordinatorEpoch epoch = 0;
    string clock_domain;
    string role;      // e.g. "worker"/"source"
    string extra;
};
bytes encode_hello(const HelloPayload& h);
Result<HelloPayload> decode_hello(const bytes& b);

// OBS_ACK payload fields (decision is the raw u8 of AuthorityDecision)
struct ObsAckPayload {
    bool accepted = false;
    u8 decision = 0;
    u32 accepted_total = 0;
    string reason;
};
bytes encode_obs_ack(bool accepted, u8 decision, u32 accepted_total, const string& reason);
Result<ObsAckPayload> decode_obs_ack(const bytes& b);

// STATUS payload fields
struct StatusPayload {
    string health;   // source_health_name
    u32 online_sources = 0;
    u32 observations_total = 0;
    u32 dropped_stale = 0;
    CoordinatorEpoch epoch = 0;
};
bytes encode_status(const StatusPayload& s);
Result<StatusPayload> decode_status(const bytes& b);

} // namespace servingobs
