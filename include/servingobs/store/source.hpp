
#pragma once
// Serving Observatory — source/worker mode engine.
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// A source connects to the coordinator, registers its boot authority, and
// streams typed observations over framed TCP, reading back per-observation
// acknowledgements. A restart mints a fresh WorkerBootId and bumps generations
// so a restarted worker never continues the old source authority.

#include "servingobs/core/types.hpp"
#include "servingobs/core/identity.hpp"
#include "servingobs/model/observation.hpp"
#include "servingobs/store/protocol.hpp"
#include "servingobs/store/network.hpp"

#include <string>

namespace servingobs {

struct SourceConfig {
    SourceId source;
    WorkerId worker;
    WorkerBootId boot;
    SourceGeneration src_gen = 1;
    ObservationGeneration obs_gen = 1;
    CoordinatorEpoch epoch = 1;
    string clock_domain = "mono_ns";
    string role = "worker";
};

struct SourceAck {
    bool ok = false;
    ObsAckPayload payload;
};

enum class SessionStatus { SOBS_DISCONNECTED = 0, SOBS_CONNECTED = 1, SOBS_REGISTERED = 2 };

// A logical source/worker that talks to a single coordinator over one TCP
// connection. Serializes writes and enforces a monotonic sequence number.
class Source {
public:
    explicit Source(SourceConfig cfg);

    bool connect(const string& host, u16 port, int timeout_ms = 10000);
    bool registered() const { return registered_; }
    bool send_hello();
    // Send one observation; returns the coordinator's ack.
    Result<SourceAck> send_observation(const Observation& o);
    // Send and forget a batch without waiting for acks (for throughput tests).
    bool send_observation_fire(const Observation& o);
    // Read a single ack frame (for when fire becomes synchronous).
    Result<ObsAckPayload> recv_ack();
    void disconnect();

    // Mint a fresh boot authority for a logical restart.
    WorkerBootId restart_boot();
    void next_seq() { ++seq_; }
    SeqNum next_seq_value() const { return seq_ + 1; }

    // Helpers to build a typed observation committed to this source.
    Observation make_observation(ObsType type) const;
    Observation make_observation(ObsType type, RequestId req, TimestampNs obs_ts) const;

    const SourceConfig& config() const { return cfg_; }

private:
    SourceConfig cfg_;
    TcpStream stream_;
    SessionStatus session_;
    bool registered_ = false;
    SeqNum seq_ = 0;
    u64 serial_ = 0;   // used to derive deterministic observation ids
};

} // namespace servingobs
