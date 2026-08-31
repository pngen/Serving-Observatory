
// Serving Observatory — source/worker mode engine.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/store/source.hpp"

#include <string>

namespace servingobs {

Source::Source(SourceConfig cfg) : cfg_(std::move(cfg)) {}

bool Source::connect(const string& host, u16 port, int timeout_ms) {
    auto s = TcpStream::connect(host, port, timeout_ms);
    if (!s.ok()) return false;
    stream_ = std::move(s.value());
    session_ = SessionStatus::SOBS_CONNECTED;
    return true;
}

bool Source::send_hello() {
    HelloPayload h;
    h.source = cfg_.source;
    h.worker = cfg_.worker;
    h.boot = cfg_.boot;
    h.src_gen = cfg_.src_gen;
    h.obs_gen = cfg_.obs_gen;
    h.epoch = cfg_.epoch;
    h.clock_domain = cfg_.clock_domain;
    h.role = cfg_.role;
    auto frame = Frame::encode(MsgType::HELLO, encode_hello(h));
    auto send = stream_.send_all(frame);
    if (!send.ok()) return false;
    // Wait for the ack.
    auto fr = stream_.recv_frame();
    if (!fr.ok() || !fr.value()) return false;
    auto decoded = Frame::decode(fr.value().value());
    if (!decoded.ok() || decoded.value().type != MsgType::HELLO_ACK) return false;
    auto ack = decode_obs_ack(decoded.value().payload);
    if (!ack.ok()) return false;
    if (!ack.value().accepted) return false;
    registered_ = true;
    session_ = SessionStatus::SOBS_REGISTERED;
    return true;
}

Result<SourceAck> Source::send_observation(const Observation& o) {
    SourceAck a;
    if (session_ != SessionStatus::SOBS_REGISTERED) { a.ok = false; return Result<SourceAck>::Err("not registered"); }
    bytes buf = encode_observation_payload(o);
    auto frame = Frame::encode(MsgType::OBS, buf);
    auto send = stream_.send_all(frame);
    if (!send.ok()) return Result<SourceAck>::Err("send failed: " + send.error());
    auto ar = recv_ack();
    if (!ar.ok()) return Result<SourceAck>::Err("ack recv failed: " + ar.error());
    a.ok = true;
    a.payload = ar.value();
    return Result<SourceAck>::Ok(std::move(a));
}

bool Source::send_observation_fire(const Observation& o) {
    if (session_ != SessionStatus::SOBS_REGISTERED) return false;
    bytes buf = encode_observation_payload(o);
    auto frame = Frame::encode(MsgType::OBS, buf);
    return stream_.send_all(frame).ok();
}

Result<ObsAckPayload> Source::recv_ack() {
    auto fr = stream_.recv_frame();
    if (!fr.ok() || !fr.value()) return Result<ObsAckPayload>::Err("no ack frame");
    auto decoded = Frame::decode(fr.value().value());
    if (!decoded.ok()) return Result<ObsAckPayload>::Err("ack decode failed");
    return decode_obs_ack(decoded.value().payload);
}

void Source::disconnect() {
    auto frame = Frame::encode(MsgType::BYE, {});
    stream_.send_all(frame);
    stream_.shutdown_send();
    stream_.close();
    session_ = SessionStatus::SOBS_DISCONNECTED;
}

WorkerBootId Source::restart_boot() {
    // Deterministic fresh boot: derive from serial counter so the proof and
    // tests are reproducible.
    WorkerBootId nb = WorkerBootId::derive(0x424F4F54, reinterpret_cast<const byte*>(&serial_), sizeof(serial_));
    cfg_.boot = nb;
    cfg_.src_gen += 1;
    cfg_.obs_gen += 1;
    cfg_.epoch += 0;
    serial_ += 1;
    return nb;
}

Observation Source::make_observation(ObsType type) const {
    Observation o;
    o.type = type;
    o.source = cfg_.source;
    o.worker = cfg_.worker;
    o.boot = cfg_.boot;
    o.src_gen = cfg_.src_gen;
    o.obs_gen = cfg_.obs_gen;
    o.epoch = cfg_.epoch;
    o.seq = seq_ + 1;
    o.clock_domain = cfg_.clock_domain;
    o.provenance = Provenance::MEASURED;
    // deterministic observation id
    u64 ns = 0;
    // combine source + seq
    ns = static_cast<u64>(type) ^ (seq_ * 0x9e3779b97f4a7c15ULL) ^ serial_;
    o.id = ObservationId(Id128::derive(0x4F4253, reinterpret_cast<const byte*>(&ns), sizeof(ns)));
    return o;
}

Observation Source::make_observation(ObsType type, RequestId req, TimestampNs obs_ts) const {
    Observation o = make_observation(type);
    if (!req.is_null()) o.set_id(FieldKeys::RequestId, req.raw());
    if (obs_ts != 0) { o.has_obs_ts = true; o.obs_ts = obs_ts; }
    return o;
}

} // namespace servingobs
