
#pragma once
// Serving Observatory — coordinator: real-time ingestion + source authority.
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// The coordinator owns reconstruction state. It accepts observations from
// registered sources, validates them against source authority, deduplicates by
// ObservationId, and retains rejected/stale evidence as historical without
// mutating current state. Thread-safety: ingest() mutates state under an
// internal lock; network I/O occurs on the caller's thread outside that lock.
//
// Serving Observatory never makes scheduling/admission/residency decisions. It
// only records and explains what was observed.

#include "servingobs/core/types.hpp"
#include "servingobs/core/identity.hpp"
#include "servingobs/core/enums.hpp"
#include "servingobs/model/observation.hpp"
#include "servingobs/model/source_authority.hpp"
#include "servingobs/store/protocol.hpp"
#include "servingobs/store/network.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace servingobs {

struct IngestResult {
    bool accepted = false;
    u8 decision = 0;           // AuthorityDecision code (or 0xFF for dedup)
    string reason;
    u32 accepted_total = 0;
    u32 rejected_total = 0;
};

enum class RejectKind : u8 { DUPLICATE_ID = 0xFF, AUTHORITY = 0xFE };

// An Ingestion engine with an index of accepted observations and a history of
// rejected/stale evidence. Because this type owns reconstructable state it is
// the core coordinator data structure.
class Coordinator {
public:
    Coordinator() { set_epoch(1); }

    void set_epoch(CoordinatorEpoch e);
    CoordinatorEpoch epoch() const { return epoch_; }

    // Register a source (from a HELLO). Returns the authority or an error.
    Result<SourceAuthority> register_hello(const HelloPayload& h);

    // Thread-safe ingestion. Dedups by id, validates authority, and on ACCEPTED
    // mutates current state; on a stale/duplicate decision it retains the
    // observation as historical evidence only.
    IngestResult ingest(const Observation& o);

    // Snapshot accessors (copy under lock).
    size_t accepted_count() const;
    size_t stale_count() const;
    size_t source_count() const;
    u32 accepted_total() const { return accepted_total_; }
    u32 stale_total() const { return stale_total_; }
    u32 duplicate_total() const { return duplicate_total_; }

    std::vector<Observation> all_accepted() const;
    std::map<RequestId, std::vector<Observation>> by_request() const;
    std::vector<std::pair<Observation, string>> stale_history() const;
    std::vector<SourceAuthority> registry_snapshot() const;
    SourceRegistry& registry() { return registry_; }

    // Clear all state (for a fresh run).
    void reset();

private:
    mutable std::mutex mu_;
    CoordinatorEpoch epoch_ = 1;
    SourceRegistry registry_;
    std::map<ObservationId, Observation> accepted_;
    std::map<RequestId, std::vector<Observation>> by_request_;
    std::vector<std::pair<Observation, string>> stale_;
    u32 accepted_total_ = 0;
    u32 stale_total_ = 0;
    u32 duplicate_total_ = 0;
};

// ------------------------------------------------------------------ server
// Accepts TCP connections, runs a handler thread per client, and keeps all
// network I/O off the coordinator's state lock.
class CoordinatorServer {
public:
    CoordinatorServer(Coordinator& coord, const string& host, u16 port, int accept_timeout_ms = 3000);
    ~CoordinatorServer();

    bool start();
    void stop();
    u16 port() const { return port_; }
    bool running() const { return running_.load(); }

private:
    void accept_loop();
    void handle_client(TcpStream stream);

    Coordinator& coord_;
    string host_;
    u16 port_;
    int accept_timeout_ms_;
    TcpListener listener_;
    std::thread accept_thread_;
    std::vector<std::thread> clients_;
    std::mutex clients_mu_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
};
} // namespace servingobs
