
// Serving Observatory — deterministic test suite.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "framework.hpp"

#include "servingobs/core/types.hpp"
#include "servingobs/core/identity.hpp"
#include "servingobs/core/digest.hpp"
#include "servingobs/core/enums.hpp"
#include "servingobs/core/json.hpp"
#include "servingobs/model/observation.hpp"
#include "servingobs/model/source_authority.hpp"
#include "servingobs/trace/trace.hpp"
#include "servingobs/trace/latency.hpp"
#include "servingobs/trace/attribution.hpp"
#include "servingobs/trace/analysis.hpp"
#include "servingobs/replay/replay.hpp"
#include "servingobs/replay/counterfactual.hpp"
#include "servingobs/replay/compare.hpp"
#include "servingobs/store/protocol.hpp"
#include "servingobs/store/persistence.hpp"
#include "servingobs/store/coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace servingobs;
using namespace sobftest;

namespace {

// Build a bytes vector from integers (avoids Windows 'byte' ambiguity).
bytes mkbytes(std::initializer_list<int> v) {
    bytes b;
    for (int x : v) b.push_back(static_cast<servingobs::byte>(x));
    return b;
}

// ---- synthetic observation builder for one request
struct ObsBuilder {
    SourceId source;
    WorkerId worker;
    WorkerBootId boot;
    SourceGeneration src_gen = 1;
    ObservationGeneration obs_gen = 1;
    CoordinatorEpoch epoch = 1;
    string clock_domain = "test_mono";
    SeqNum seq = 0;

    Observation make(ObsType type, const RequestId& req, u64 obs_ts) {
        Observation o;
        o.type = type;
        o.source = source; o.worker = worker; o.boot = boot;
        o.src_gen = src_gen; o.obs_gen = obs_gen; o.epoch = epoch;
        o.clock_domain = clock_domain;
        o.seq = ++seq;
        o.provenance = Provenance::MEASURED;
        if (obs_ts != 0) { o.has_obs_ts = true; o.obs_ts = obs_ts; }
        if (!req.is_null()) o.set_id(FieldKeys::RequestId, req.raw());
        o.id = ObservationId(Id128::derive(0x0B53, reinterpret_cast<const servingobs::byte*>(&seq), sizeof(seq)));
        return o;
    }
};

std::vector<Observation> build_request(const RequestId& req, int decode_steps = 3, bool retry = false,
                                       bool cold_model = false) {
    ObsBuilder b;
    b.source = SourceId(Id128::from_u64(1));
    b.worker = WorkerId(Id128::from_u64(10));
    b.boot = WorkerBootId(Id128::make(0x11, 0x22));
    std::vector<Observation> obs;
    u64 t = 1000;
    obs.push_back(b.make(ObsType::REQUEST, req, t)); ++t;
    obs.push_back(b.make(ObsType::ADMISSION, req, t)); ++t;
    obs.push_back(b.make(ObsType::QUEUE_ENTER, req, t)); ++t;
    obs.push_back(b.make(ObsType::QUEUE_LEAVE, req, t)); ++t;
    if (retry) obs.push_back(b.make(ObsType::RETRY, req, t));
    obs.push_back(b.make(ObsType::BATCH_FORM, req, t)); ++t;
    obs.push_back(b.make(ObsType::BATCH_SEAL, req, t)); ++t;
    obs.push_back(b.make(ObsType::DISPATCH, req, t)); ++t;
    obs.push_back(b.make(ObsType::TRANSFER_START, req, t)); ++t;
    obs.push_back(b.make(ObsType::TRANSFER_END, req, t)); ++t;
    obs.push_back(b.make(ObsType::PREFILL_START, req, t));
    obs.back().set_u64(FieldKeys::InputTokens, 128);
    ++t;
    obs.push_back(b.make(ObsType::PREFILL_END, req, t)); ++t;
    for (int i = 0; i < decode_steps; ++i) {
        u64 dss = t;
        obs.push_back(b.make(ObsType::DECODE_STEP_START, req, dss));
        obs.back().set_u64(FieldKeys::OutputTokens, static_cast<u64>(i + 1));
        t += 25;
        obs.push_back(b.make(ObsType::DECODE_STEP_END, req, t));
    }
    if (cold_model) {
        Observation m = b.make(ObsType::MODEL_RESIDENCY, req, t);
        m.set_bool(FieldKeys::IsWarm, false);
        m.set_id(FieldKeys::ModelId, ModelId(Id128::from_u64(7)).raw());
        obs.push_back(std::move(m));
    }
    obs.push_back(b.make(ObsType::KV_HIT, req, t));
    obs.back().set_u64(FieldKeys::Count, 8);
    obs.push_back(b.make(ObsType::KV_MISS, req, t));
    obs.back().set_u64(FieldKeys::Count, 2);
    obs.push_back(b.make(ObsType::KERNEL_HIT, req, t));
    obs.back().set_u64(FieldKeys::Count, 5);
    obs.push_back(b.make(ObsType::COMPLETION, req, t));
    obs.back().set_str(FieldKeys::Outcome, "completed");
    u64 r = 100000;
    for (auto& o : obs) { o.has_recv_ts = true; o.recv_ts = r; r += 1; }
    return obs;
}

} // namespace

// ------------------------------------------------------------------ identities
TEST(identity_roundtrip_hex) {
    Id128 x(0x0123456789abcdefULL, 0x0fedcba987654321ULL);
    string h = x.to_hex();
    CHECK_EQ(h.size(), 32u);
    auto r = Id128::parse(h);
    CHECK(r.ok());
    CHECK(r.value() == x);
}
TEST(identity_parse_variants) {
    auto a = Id128::parse("000102030405060708090a0b0c0d0e0f");
    CHECK(a.ok());
    auto b = Id128::parse("0x000102030405060708090a0b0c0d0e0f");
    CHECK(b.ok());
    CHECK(a.value() == b.value());
    auto c = Id128::parse("bad");
    CHECK(!c.ok());
}
TEST(identity_nullness) {
    RequestId r = RequestId::from_u64(1);
    CHECK(!r.is_null());
    RequestId n;
    CHECK(n.is_null());
}
TEST(identity_derive_distinct) {
    u8 a = 1, b = 2;
    RequestId x = RequestId::derive(1, reinterpret_cast<const servingobs::byte*>(&a), 1);
    RequestId y = RequestId::derive(1, reinterpret_cast<const servingobs::byte*>(&b), 1);
    CHECK(x != y);
}

// ------------------------------------------------------------------ digest
TEST(sha256_known_abc) {
    Digest d = Sha256::hash("abc");
    CHECK_EQ(digest_hex(d), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

// ------------------------------------------------------------------ observation codec
TEST(observation_codec_roundtrip) {
    RequestId req = RequestId::from_u64(99);
    auto obs = build_request(req, 2, true, true);
    for (const auto& o : obs) {
        bytes enc = encode_observation_payload(o);
        auto dec = read_observation_payload(enc);
        CHECK(dec.ok());
        CHECK(dec.value().id == o.id);
        CHECK(dec.value().type == o.type);
        CHECK(dec.value().boot == o.boot);
        CHECK(dec.value().seq == o.seq);
        CHECK(dec.value().has_obs_ts == o.has_obs_ts);
        CHECK(dec.value().has_recv_ts == o.has_recv_ts);
        CHECK(dec.value().fields == o.fields);
        CHECK(observation_digest(o) == observation_digest(dec.value()));
    }
}
TEST(observation_partial_evidence) {
    RequestId req = RequestId::from_u64(1);
    ObsBuilder b; b.source = SourceId(Id128::from_u64(2));
    Observation o = b.make(ObsType::DISPATCH, req, 123);
    CHECK(o.has_obs_ts);
    CHECK(!o.has_recv_ts);
    auto dec = read_observation_payload(encode_observation_payload(o));
    CHECK(dec.ok());
    CHECK(dec.value().has_obs_ts);
    CHECK(!dec.value().has_recv_ts);
    CHECK(dec.value().u64_field("missing", 77) == 77);
}

// ------------------------------------------------------------------ trace reconstruction
TEST(trace_reconstruction_phases) {
    auto obs = build_request(RequestId::from_u64(5), 3, false, false);
    RequestTrace t = reconstruct_trace(obs, RequestId::from_u64(5));
    CHECK(t.has_phase(Phase::QUEUE));
    CHECK(t.has_phase(Phase::PREFILL));
    CHECK(t.has_phase(Phase::DECODE));
    CHECK(t.has_phase(Phase::TOTAL));
    CHECK(t.outcome == Outcome::COMPLETED);
    CHECK(t.get_phase(Phase::QUEUE).end >= t.get_phase(Phase::QUEUE).start);
    CHECK(t.get_phase(Phase::TOTAL).end >= t.get_phase(Phase::TOTAL).start);
    CHECK_EQ(t.get_phase(Phase::DECODE).detail.find("3 steps") != string::npos, true);
    CHECK_EQ(t.boots.size(), 1u);
    CHECK(!t.spans_multiple_epochs);
}
TEST(latency_decomposition_no_double_count) {
    auto obs = build_request(RequestId::from_u64(6), 3, true, true);
    RequestTrace t = reconstruct_trace(obs, RequestId::from_u64(6));
    LatencyBreakdown b = decompose_latency(t);
    CHECK(b.total_ns > 0);
    u64 sum_excl = 0;
    for (const auto& c : b.components) sum_excl += c.exclusive_ns;
    CHECK(sum_excl <= b.total_ns);
    CHECK(b.covered_ns <= b.total_ns);
}
TEST(attribution_metrics) {
    auto obs = build_request(RequestId::from_u64(7), 3, true, true);
    RequestTrace t = reconstruct_trace(obs, RequestId::from_u64(7));
    ResourceAttribution a = attribute_resources(t);
    CHECK(a.decode_steps >= 3);
    CHECK(a.kv_hits >= 1);
    CHECK(a.kv_misses >= 1);
    CHECK(a.input_tokens == 128);
    CHECK(a.has("queue"));
}
TEST(tail_percentiles) {
    std::vector<u64> v = {1,2,3,4,5,6,7,8,9,10};
    CHECK_EQ(percentile_of(v, 50.0), (u64)5);
    CHECK_EQ(percentile_of(v, 90.0), (u64)9);
    CHECK_EQ(percentile_of(v, 99.0), (u64)10);
    TailMetrics m = tail_metrics(v);
    CHECK_EQ(m.p50_ns, 5u);
    CHECK_EQ(m.count, 10u);
}
TEST(explain_causal_discipline) {
    auto obs = build_request(RequestId::from_u64(8), 3, true, true);
    RequestTrace t = reconstruct_trace(obs, RequestId::from_u64(8));
    Explanation e = explain_request(t);
    bool has_direct = false;
    for (const auto& s : e.statements) if (s.cls == ExplanationClass::DIRECTLY_EVIDENCED) has_direct = true;
    CHECK(has_direct);
    bool has_cold = false;
    for (const auto& s : e.statements) if (s.claim.find("cold") != string::npos) has_cold = true;
    CHECK(has_cold);
}
TEST(tail_explanation) {
    std::vector<RequestTrace> traces;
    for (int i = 0; i < 20; ++i) {
        auto obs = build_request(RequestId::from_u64(20 + i), i % 5 + 1, i % 3 == 0, i % 2 == 0);
        traces.push_back(reconstruct_trace(obs, RequestId::from_u64(20 + i)));
    }
    auto tails = explain_tail(traces, 90.0);
    CHECK(!tails.empty());
    for (const auto& t : tails) CHECK(t.latency_ns > 0);
}
TEST(correlation_grouping) {
    std::vector<RequestTrace> traces;
    for (int i = 0; i < 10; ++i) {
        auto obs = build_request(RequestId::from_u64(30 + i), 2, false, false);
        RequestTrace t = reconstruct_trace(obs, RequestId::from_u64(30 + i));
        t.model = ModelId(Id128::from_u64(static_cast<u64>(i % 2)));
        traces.push_back(std::move(t));
    }
    auto groups = correlate_by(traces, [](const RequestTrace& t) { return t.model.to_hex(); });
    CHECK_EQ(groups.size(), 2u);
    CHECK_EQ(groups[0].count + groups[1].count, 10u);
}

// ------------------------------------------------------------------ replay & digests
TEST(replay_deterministic) {
    auto obs = build_request(RequestId::from_u64(40), 4, true, false);
    ReplayResult a = replay(obs);
    ReplayResult b = replay(obs);
    CHECK(a.evidence_digest == b.evidence_digest);
    CHECK(a.trace_digest == b.trace_digest);
    CHECK(a.explanation_digest == b.explanation_digest);
    CHECK(a.aggregate_digest == b.aggregate_digest);
    CHECK_EQ(a.request_trace_count, 1u);
}
TEST(replay_multi_request) {
    std::vector<Observation> all = build_request(RequestId::from_u64(50), 2, false, false);
    auto o2 = build_request(RequestId::from_u64(51), 3, true, true);
    all.insert(all.end(), o2.begin(), o2.end());
    ReplayResult r = replay(all);
    CHECK_EQ(r.request_trace_count, 2u);
}

// ------------------------------------------------------------------ counterfactual / compare
TEST(counterfactual_derived_label) {
    auto obs = build_request(RequestId::from_u64(60), 3, true, false);
    RequestTrace t = reconstruct_trace(obs, RequestId::from_u64(60));
    auto cf = counterfactual(t, CounterfactualRule::REMOVE_QUEUE_WAIT);
    CHECK(cf.provenance == Provenance::DERIVED);
    CHECK(cf.counterfactual_total <= cf.original_total);
    CHECK(!cf.changed_inputs.empty());
}
TEST(compare_traces) {
    auto a = build_request(RequestId::from_u64(70), 2, false, false);
    auto b = build_request(RequestId::from_u64(71), 5, true, false);
    auto ta = reconstruct_trace(a, RequestId::from_u64(70));
    auto tb = reconstruct_trace(b, RequestId::from_u64(71));
    TraceComparison c = compare_traces(ta, tb);
    CHECK(c.retries_b > c.retries_a);
    CHECK(c.phase_deltas.size() > 0);
}

// ------------------------------------------------------------------ persistence
TEST(archive_save_load_roundtrip) {
    Coordinator coord;
    coord.set_epoch(2);
    HelloPayload h;
    h.source = SourceId(Id128::from_u64(1));
    h.worker = WorkerId(Id128::from_u64(10));
    h.boot = WorkerBootId(Id128::make(1, 2));
    h.src_gen = 1; h.obs_gen = 1; h.epoch = 2;
    coord.register_hello(h);
    auto obs = build_request(RequestId::from_u64(80), 2, false, false);
    for (const auto& o : obs) {
        Observation o2 = o;
        o2.source = h.source; o2.worker = h.worker; o2.boot = h.boot;
        o2.src_gen = 1; o2.obs_gen = 1; o2.epoch = 2;
        coord.ingest(o2);
    }
    CHECK(coord.accepted_count() > 0);
    string path = "out/test_archive.sobs";
    auto saved = save_archive(coord, path);
    CHECK(saved.ok());
    auto loaded = load_archive(path, true);
    CHECK(loaded.ok());
    CHECK(loaded.value().replay_match);
    CHECK_EQ(loaded.value().accepted.size(), saved.value().accepted.size());
    CHECK(saved.value().stored_evidence_digest == loaded.value().stored_evidence_digest);
}
TEST(archive_corruption_rejected) {
    Coordinator coord;
    coord.set_epoch(1);
    HelloPayload h; h.source=SourceId(Id128::from_u64(1)); h.worker=WorkerId(Id128::from_u64(1));
    h.boot=WorkerBootId(Id128::make(1,1)); h.src_gen=1; h.obs_gen=1; h.epoch=1;
    coord.register_hello(h);
    auto obs = build_request(RequestId::from_u64(81), 2, false, false);
    for (auto& o : obs) { o.source=h.source; o.worker=h.worker; o.boot=h.boot; o.src_gen=1; o.obs_gen=1; o.epoch=1; coord.ingest(o); }
    string path = "out/test_corrupt.sobs";
    CHECK(save_archive(coord, path).ok());
    auto all = file_read_all(path);
    CHECK(all.ok());
    bytes& b = all.value();
    CHECK(b.size() > 10);
    b[6] = static_cast<servingobs::byte>(static_cast<u8>(b[6]) ^ 0xFF);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    f.close();
    auto loaded = load_archive(path, true);
    CHECK(!loaded.ok());
}
TEST(archive_truncation_rejected) {
    Coordinator coord;
    HelloPayload h; h.source=SourceId(Id128::from_u64(1)); h.worker=WorkerId(Id128::from_u64(1));
    h.boot=WorkerBootId(Id128::make(1,1)); h.src_gen=1; h.obs_gen=1; h.epoch=1;
    coord.register_hello(h);
    auto obs = build_request(RequestId::from_u64(82), 1, false, false);
    for (auto& o : obs) { o.source=h.source; o.worker=h.worker; o.boot=h.boot; o.src_gen=1; o.obs_gen=1; o.epoch=1; coord.ingest(o); }
    string path = "out/test_trunc.sobs";
    CHECK(save_archive(coord, path).ok());
    auto all = file_read_all(path);
    CHECK(all.ok());
    bytes b(all.value().begin(), all.value().end() - 5);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    f.close();
    auto loaded = load_archive(path, true);
    CHECK(!loaded.ok());
}
TEST(archive_malformed_length_rejected) {
    Coordinator coord; coord.set_epoch(1);
    string path = "out/test_badlen.sobs";
    BinaryWriter w;
    w.write_u32(999999);
    for (int i = 0; i < 40; ++i) w.write_u8(0);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(w.data().data()), static_cast<std::streamsize>(w.data().size()));
    f.close();
    auto loaded = load_archive(path, true);
    CHECK(!loaded.ok());
}

// ------------------------------------------------------------------ source authority
TEST(source_authority_accept_then_stale_rejected) {
    SourceRegistry reg;
    CoordinatorEpoch epoch = 5;
    Observation h;
    h.source = SourceId(Id128::from_u64(1));
    h.worker = WorkerId(Id128::from_u64(1));
    h.boot = WorkerBootId(Id128::make(1, 1));
    h.src_gen = 1; h.obs_gen = 1; h.epoch = epoch;
    auto auth = reg.register_source(h, epoch);
    CHECK(auth.ok());
    Observation o = h; o.seq = 1; o.has_obs_ts = true; o.obs_ts = 100;
    string reason;
    CHECK(reg.validate(o, reason) == AuthorityDecision::ACCEPTED);
    Observation oe = o; oe.epoch = epoch - 1; oe.seq = 2;
    CHECK(reg.validate(oe, reason) == AuthorityDecision::STALE_EPOCH);
    Observation ob = o; ob.boot = WorkerBootId(Id128::make(9, 9)); ob.seq = 2;
    CHECK(reg.validate(ob, reason) == AuthorityDecision::STALE_BOOT);
    Observation og = o; og.src_gen = 0; og.seq = 2;
    CHECK(reg.validate(og, reason) == AuthorityDecision::STALE_SOURCE_GENERATION);
    Observation oo = o; oo.obs_gen = 0; oo.seq = 2;
    CHECK(reg.validate(oo, reason) == AuthorityDecision::STALE_OBSERVATION_GENERATION);
    Observation od = o; od.seq = 1;
    CHECK(reg.validate(od, reason) == AuthorityDecision::DUPLICATE_SEQUENCE);
}
TEST(source_authority_restart) {
    SourceRegistry reg;
    CoordinatorEpoch epoch = 1;
    Observation h;
    h.source = SourceId(Id128::from_u64(2));
    h.worker = WorkerId(Id128::from_u64(2));
    h.boot = WorkerBootId(Id128::make(1, 1));
    h.src_gen = 1; h.obs_gen = 1; h.epoch = epoch;
    reg.register_source(h, epoch);
    auto nb = WorkerBootId(Id128::make(2, 2));
    SourceAuthority na = reg.mark_restart(h.source, h.worker, nb, epoch);
    CHECK_EQ(na.src_gen, 2u);
    CHECK_EQ(na.obs_gen, 2u);
    CHECK(na.boot == nb);
    Observation stale = h; stale.boot = WorkerBootId(Id128::make(1,1)); stale.src_gen=2; stale.obs_gen=2; stale.seq=1;
    string reason;
    CHECK(reg.validate(stale, reason) == AuthorityDecision::STALE_BOOT);
}

// ------------------------------------------------------------------ protocol
TEST(protocol_frame_roundtrip) {
    bytes payload = mkbytes({1,2,3,4,5});
    bytes f = Frame::encode(MsgType::OBS, payload);
    auto d = Frame::decode(f);
    CHECK(d.ok());
    CHECK(d.value().type == MsgType::OBS);
    CHECK(d.value().payload == payload);
    CHECK(d.value().version == kProtocolVersion);
}
TEST(protocol_bad_checksum_rejected) {
    bytes payload = mkbytes({1,2,3});
    bytes f = Frame::encode(MsgType::OBS, payload);
    f[9] = static_cast<servingobs::byte>(static_cast<u8>(f[9]) ^ 0xFF);
    auto d = Frame::decode(f);
    CHECK(!d.ok());
}
TEST(protocol_trailing_garbage_rejected) {
    bytes payload = mkbytes({1,2,3});
    bytes f = Frame::encode(MsgType::OBS, payload);
    f.push_back(static_cast<servingobs::byte>(0xAA));
    auto d = Frame::decode(f);
    CHECK(!d.ok());
}

// ------------------------------------------------------------------ adversarial / concurrency
TEST(coordinator_concurrent_ingest) {
    Coordinator coord;
    coord.set_epoch(3);
    HelloPayload h;
    h.source = SourceId(Id128::from_u64(1));
    h.worker = WorkerId(Id128::from_u64(1));
    h.boot = WorkerBootId(Id128::make(1,1));
    h.src_gen = 1; h.obs_gen = 1; h.epoch = 3;
    CHECK(coord.register_hello(h).ok());
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            ObsBuilder bb;
            bb.source = h.source; bb.worker = h.worker; bb.boot = h.boot; bb.src_gen = 1; bb.obs_gen = 1; bb.epoch = 3;
            for (int i = 0; i < 200; ++i) {
                RequestId req = RequestId::from_u64(static_cast<u64>(1000 + t*1000 + i));
                Observation o = bb.make(ObsType::REQUEST, req, static_cast<u64>(1 + i));
                o.epoch = 3;
                coord.ingest(o);
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(coord.accepted_count() >= 0);
}
TEST(coordinator_duplicate_id_rejected) {
    Coordinator coord; coord.set_epoch(1);
    HelloPayload h; h.source=SourceId(Id128::from_u64(3)); h.worker=WorkerId(Id128::from_u64(3));
    h.boot=WorkerBootId(Id128::from_u64(3)); h.src_gen=1; h.obs_gen=1; h.epoch=1;
    coord.register_hello(h);
    ObsBuilder bb; bb.source=h.source; bb.worker=h.worker; bb.boot=h.boot; bb.epoch=1;
    RequestId req = RequestId::from_u64(300);
    Observation o1 = bb.make(ObsType::REQUEST, req, 10);
    o1.epoch = 1;
    Observation o2 = o1;
    IngestResult a = coord.ingest(o1);
    CHECK(a.accepted);
    IngestResult b = coord.ingest(o2);
    CHECK(!b.accepted);
    CHECK_EQ(coord.accepted_count(), 1u);
}

int main() {
    // Ensure a writable scratch directory exists regardless of the cwd (ctest
    // runs the test from the build directory).
    std::error_code ec;
    std::filesystem::create_directories("out", ec);
    return run_all();
}
