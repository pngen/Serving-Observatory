// Serving Observatory — framed TCP protocol.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/store/protocol.hpp"
#include "servingobs/core/binary_codec.hpp"

#include <cstring>
#include <string>

namespace servingobs {

string_view msg_type_name(MsgType t) {
    switch (t) {
        case MsgType::HELLO: return "hello";
        case MsgType::HELLO_ACK: return "hello_ack";
        case MsgType::OBS: return "obs";
        case MsgType::OBS_ACK: return "obs_ack";
        case MsgType::STATUS: return "status";
        case MsgType::BYE: return "bye";
        case MsgType::PING: return "ping";
        case MsgType::PONG: return "pong";
    }
    return "unknown";
}

// ------------------------------------------------------------------ crc32
u32 crc32(const byte* data, std::size_t n) {
    u32 crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= static_cast<u8>(data[i]);
        for (int k = 0; k < 8; ++k) {
            u32 mask = static_cast<u32>(-static_cast<i32>(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}
u32 crc32(const bytes& b) { return crc32(b.data(), b.size()); }

// ------------------------------------------------------------------ frame
u32 Frame::wire_length(const bytes& payload) {
    // len field counts bytes after it: crc(4) + version(2) + type(2) + payload.
    return static_cast<u32>(8 + payload.size());
}

bytes Frame::encode(MsgType t, const bytes& payload, u16 version) {
    BinaryWriter body;
    body.write_u16(version);
    body.write_u16(static_cast<u16>(t));
    for (auto b : payload) body.write_u8(static_cast<u8>(b));

    BinaryWriter w;
    w.write_u32(wire_length(payload));
    w.write_u32(crc32(body.data()));
    // append body
    for (auto b : body.data()) w.write_u8(static_cast<u8>(b));
    return w.data();
}

Result<Frame> Frame::decode(const byte* data, std::size_t n) {
    BinaryReader r(data, n);
    u32 len = r.read_u32();
    if (!r.ok()) return Result<Frame>::Err("truncated frame header");
    if (len != n - 4) {
        return Result<Frame>::Err("frame length mismatch: declared " + std::to_string(len) +
                                  " but " + std::to_string(n - 4) + " bytes follow");
    }
    if (len < 8 || len > (64u * 1024u * 1024u)) {
        return Result<Frame>::Err("frame length out of range");
    }
    u32 expected_crc = r.read_u32();
    if (len < 4) return Result<Frame>::Err("frame body truncated");
    // body = remaining (len - 4) bytes
    BinaryReader body = r.sub(len - 4);
    if (!body.ok()) return Result<Frame>::Err("frame body overruns");
    // verify crc over body bytes (before the sub reader consumed them we need
    // the raw bytes; recompute from original).
    u32 actual_crc = crc32(data + 8, len - 4);
    if (actual_crc != expected_crc) {
        return Result<Frame>::Err("checksum mismatch");
    }
    Frame f;
    f.version = body.read_u16();
    if (f.version != kProtocolVersion) {
        return Result<Frame>::Err("unsupported protocol version " + std::to_string(f.version));
    }
    u16 t = body.read_u16();
    if (t >= static_cast<u16>(MsgType::HELLO) && t <= static_cast<u16>(MsgType::PONG)) {
        f.type = static_cast<MsgType>(t);
    } else {
        return Result<Frame>::Err("unknown message type " + std::to_string(t));
    }
    f.payload = body.read_raw(static_cast<u32>(body.remaining()));
    if (!body.require_end()) return Result<Frame>::Err("trailing garbage in frame: " + body.error());
    return Result<Frame>::Ok(std::move(f));
}

// ------------------------------------------------------------------ hello
bytes encode_hello(const HelloPayload& h) {
    BinaryWriter w;
    w.write_id(h.source.raw());
    w.write_id(h.worker.raw());
    w.write_id(h.boot.raw());
    w.write_u64(h.src_gen);
    w.write_u64(h.obs_gen);
    w.write_u64(h.epoch);
    w.write_str(h.clock_domain);
    w.write_str(h.role);
    w.write_str(h.extra);
    return w.data();
}
Result<HelloPayload> decode_hello(const bytes& b) {
    BinaryReader r(b);
    HelloPayload h;
    h.source = SourceId(r.read_id());
    h.worker = WorkerId(r.read_id());
    h.boot = WorkerBootId(r.read_id());
    h.src_gen = r.read_u64();
    h.obs_gen = r.read_u64();
    h.epoch = r.read_u64();
    h.clock_domain = r.read_str();
    h.role = r.read_str();
    h.extra = r.read_str();
    if (!r.ok()) return Result<HelloPayload>::Err("bad hello payload: " + r.error());
    if (!r.require_end()) return Result<HelloPayload>::Err("hello trailing garbage");
    return Result<HelloPayload>::Ok(std::move(h));
}

// ------------------------------------------------------------------ obs_ack
bytes encode_obs_ack(bool accepted, u8 decision, u32 accepted_total, const string& reason) {
    BinaryWriter w;
    w.write_u8(accepted ? 1 : 0);
    w.write_u8(decision);
    w.write_u32(accepted_total);
    w.write_str(reason);
    return w.data();
}
Result<ObsAckPayload> decode_obs_ack(const bytes& b) {
    BinaryReader r(b);
    ObsAckPayload a;
    a.accepted = r.read_u8() != 0;
    a.decision = r.read_u8();
    a.accepted_total = r.read_u32();
    a.reason = r.read_str();
    if (!r.ok()) return Result<ObsAckPayload>::Err("bad obs_ack payload: " + r.error());
    if (!r.require_end()) return Result<ObsAckPayload>::Err("obs_ack trailing garbage");
    return Result<ObsAckPayload>::Ok(std::move(a));
}

// ------------------------------------------------------------------ status
bytes encode_status(const StatusPayload& s) {
    BinaryWriter w;
    w.write_str(s.health);
    w.write_u32(s.online_sources);
    w.write_u32(s.observations_total);
    w.write_u32(s.dropped_stale);
    w.write_u64(s.epoch);
    return w.data();
}
Result<StatusPayload> decode_status(const bytes& b) {
    BinaryReader r(b);
    StatusPayload s;
    s.health = r.read_str();
    s.online_sources = r.read_u32();
    s.observations_total = r.read_u32();
    s.dropped_stale = r.read_u32();
    s.epoch = r.read_u64();
    if (!r.ok()) return Result<StatusPayload>::Err("bad status payload: " + r.error());
    if (!r.require_end()) return Result<StatusPayload>::Err("status trailing garbage");
    return Result<StatusPayload>::Ok(std::move(s));
}

} // namespace servingobs
