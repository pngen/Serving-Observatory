
// Serving Observatory — coordinator: real-time ingestion + server.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/store/coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace servingobs {

// ------------------------------------------------------------------ coordinator
void Coordinator::set_epoch(CoordinatorEpoch e) {
    std::lock_guard<std::mutex> lk(mu_);
    epoch_ = e;
}

Result<SourceAuthority> Coordinator::register_hello(const HelloPayload& h) {
    std::lock_guard<std::mutex> lk(mu_);
    Observation o;
    o.type = ObsType::REQUEST;
    o.source = h.source;
    o.worker = h.worker;
    o.boot = h.boot;
    o.src_gen = h.src_gen;
    o.obs_gen = h.obs_gen;
    o.epoch = h.epoch;
    o.clock_domain = h.clock_domain;
    // Avoid advancing last_seq; registration is a separate authority step.
    return registry_.register_source(o, h.epoch);
}

IngestResult Coordinator::ingest(const Observation& o) {
    IngestResult r;
    std::lock_guard<std::mutex> lk(mu_);
    // Dedup by id.
    if (accepted_.count(o.id)) {
        r.accepted = false;
        r.decision = static_cast<u8>(RejectKind::DUPLICATE_ID);
        r.reason = "duplicate observation id " + o.id.to_hex();
        ++duplicate_total_;
        r.accepted_total = accepted_total_;
        r.rejected_total = stale_total_;
        // Retain as historical (non-mutating for current state).
        stale_.push_back({ o, r.reason });
        return r;
    }
    // Authority validation.
    string reason;
    AuthorityDecision d = registry_.validate(o, reason);
    if (d != AuthorityDecision::ACCEPTED) {
        r.accepted = false;
        r.decision = static_cast<u8>(d);
        r.reason = reason;
        ++stale_total_;
        r.accepted_total = accepted_total_;
        r.rejected_total = stale_total_;
        // Retain as historical evidence only — never mutates current state.
        stale_.push_back({ o, reason });
        return r;
    }
    // Accepted -> mutate current state.
    accepted_[o.id] = o;
    RequestId rid = o.request_id();
    if (!rid.is_null()) by_request_[rid].push_back(o);
    ++accepted_total_;
    r.accepted = true;
    r.decision = static_cast<u8>(AuthorityDecision::ACCEPTED);
    r.accepted_total = accepted_total_;
    r.rejected_total = stale_total_;
    return r;
}

size_t Coordinator::accepted_count() const { std::lock_guard<std::mutex> lk(mu_); return accepted_.size(); }
size_t Coordinator::stale_count() const { std::lock_guard<std::mutex> lk(mu_); return stale_.size(); }
size_t Coordinator::source_count() const { std::lock_guard<std::mutex> lk(mu_); return registry_.count(); }

std::vector<Observation> Coordinator::all_accepted() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Observation> out;
    out.reserve(accepted_.size());
    for (const auto& [k, v] : accepted_) out.push_back(v);
    return out;
}
std::map<RequestId, std::vector<Observation>> Coordinator::by_request() const {
    std::lock_guard<std::mutex> lk(mu_);
    return by_request_;
}
std::vector<std::pair<Observation, string>> Coordinator::stale_history() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stale_;
}
std::vector<SourceAuthority> Coordinator::registry_snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<SourceAuthority> out;
    for (const auto& [k, v] : registry_.all()) out.push_back(v);
    return out;
}
void Coordinator::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    accepted_.clear();
    by_request_.clear();
    stale_.clear();
    accepted_total_ = stale_total_ = duplicate_total_ = 0;
    registry_ = SourceRegistry{};
}

// ------------------------------------------------------------------ server
CoordinatorServer::CoordinatorServer(Coordinator& coord, const string& host, u16 port, int accept_timeout_ms)
    : coord_(coord), host_(host), port_(port), accept_timeout_ms_(accept_timeout_ms) {}
CoordinatorServer::~CoordinatorServer() { stop(); }

bool CoordinatorServer::start() {
    auto l = TcpListener::bind(host_, port_);
    if (!l.ok()) { std::fprintf(stderr, "coordinator: bind failed: %s\n", l.error().c_str()); return false; }
    listener_ = std::move(l.value());
    port_ = listener_.port();
    running_ = true;
    stop_requested_ = false;
    accept_thread_ = std::thread([this] { accept_loop(); });
    return true;
}

void CoordinatorServer::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    stop_requested_ = true;
    listener_.close();  // wakes accept with an error
    if (accept_thread_.joinable()) accept_thread_.join();
    std::lock_guard<std::mutex> lk(clients_mu_);
    for (auto& t : clients_) if (t.joinable()) t.join();
    clients_.clear();
}

void CoordinatorServer::accept_loop() {
    while (running_) {
        auto c = listener_.accept(accept_timeout_ms_);
        if (!c.ok()) {  // timeout or closed; check stop
            if (stop_requested_.load() || !running_.load()) break;
            continue;
        }
        TcpStream s = std::move(c.value());
        {
            std::lock_guard<std::mutex> lk(clients_mu_);
            clients_.emplace_back([this, s = std::move(s)]() mutable { handle_client(std::move(s)); });
        }
    }
}

void CoordinatorServer::handle_client(TcpStream stream) {
    // The connection's read loop performs network I/O (recv), decodes a frame,
    // then hands the decoded payload to the coordinator under its lock, and
    // finally performs network I/O (send) again — never while holding the lock.
    for (;;) {
        auto fr = stream.recv_frame();
        if (!fr.ok()) { break; }
        if (!fr.value()) {
            // peer closed or timeout.
            if (stop_requested_.load()) break;
            auto more = stream.recv_some(1);
            if (more.ok() && more.value().has_value()) continue; // transient
            break;
        }
        auto frame = Frame::decode(fr.value().value());
        if (!frame.ok()) {
            // A malformed frame is a protocol error; drop the connection.
            break;
        }
        const Frame& f = frame.value();
        switch (f.type) {
            case MsgType::HELLO: {
                auto hp = decode_hello(f.payload);
                if (!hp.ok()) break;
                auto auth = coord_.register_hello(hp.value());
                bytes ack = encode_obs_ack(auth.ok(), static_cast<u8>(AuthorityDecision::ACCEPTED),
                                           auth.ok() ? coord_.accepted_total() : 0, auth.ok() ? "" : auth.error());
                auto enc = Frame::encode(MsgType::HELLO_ACK, ack);
                stream.send_all(enc);
                break;
            }
            case MsgType::OBS: {
                auto op = read_observation_payload(f.payload);
                if (!op.ok()) break;
                IngestResult ir = coord_.ingest(op.value());
                bytes ack = encode_obs_ack(ir.accepted, ir.decision, ir.accepted_total, ir.reason);
                auto enc = Frame::encode(MsgType::OBS_ACK, ack);
                stream.send_all(enc);
                break;
            }
            case MsgType::STATUS: {
                StatusPayload sp;
                sp.health = "healthy";
                sp.online_sources = static_cast<u32>(coord_.source_count());
                sp.observations_total = coord_.accepted_total();
                sp.dropped_stale = coord_.stale_total();
                sp.epoch = coord_.epoch();
                auto enc = Frame::encode(MsgType::STATUS, encode_status(sp));
                stream.send_all(enc);
                break;
            }
            case MsgType::PING: {
                auto enc = Frame::encode(MsgType::PONG, {});
                stream.send_all(enc);
                break;
            }
            case MsgType::BYE: {
                stream.shutdown_send();
                return;
            }
            default: break;
        }
    }
}

} // namespace servingobs
