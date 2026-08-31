#define NOMINMAX

// Serving Observatory — obscli command-line interface (single TU).
// Copyright 2026 Summon Software Labs. Apache-2.0.

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
#include "servingobs/store/network.hpp"
#include "servingobs/store/coordinator.hpp"
#include "servingobs/store/source.hpp"
#include "servingobs/store/persistence.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(SOBS_HAVE_CUDA)
#include "servingobs/cuda/cuda_proof.hpp"
#endif

namespace servingobs {
namespace cli {

using namespace servingobs;

// ================================================================== utils
struct Args {
    std::vector<std::string> all;
    std::map<std::string, std::string> opts;
    std::vector<std::string> positional;
    std::string program;
};

Args parse_args(int argc, char** argv) {
    Args a; if (argc > 0) a.program = argv[0];
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        a.all.push_back(s);
        if (!s.empty() && s[0] == '-') {
            std::size_t eq = s.find('=');
            if (eq != std::string::npos) a.opts[s.substr(2, eq - 2)] = s.substr(eq + 1);
            else if (i + 1 < argc && argv[i + 1][0] != '-') { a.opts[s.substr(2)] = argv[++i]; }
            else a.opts[s.substr(2)] = "true";
        } else a.positional.push_back(s);
    }
    return a;
}
std::string opt(const Args& a, const std::string& k, const std::string& def = "") {
    auto it = a.opts.find(k); return it == a.opts.end() ? def : it->second;
}
bool flag(const Args& a, const std::string& k) {
    auto it = a.opts.find(k); return it != a.opts.end() && it->second != "false" && it->second != "0";
}
u64 oid(const Args& a, const std::string& k, u64 def = 0) {
    auto v = opt(a, k); if (v.empty() || v == "true") return def;
    return std::strtoull(v.c_str(), nullptr, 0);
}
f64 of(const Args& a, const std::string& k, f64 def = 0.0) {
    auto v = opt(a, k); if (v.empty() || v == "true") return def;
    return std::atof(v.c_str());
}
Id128 parse_id(const std::string& s) {
    auto r = Id128::parse(s);
    return r.ok() ? r.value() : Id128(0, std::strtoull(s.c_str(), nullptr, 0));
}
void print_usage(const std::string& program) {
    std::printf(
        "Serving Observatory %s\n"
        "usage: %s <command> [options]\n"
        "commands: serve source ingest list inspect trace timeline explain latency tail\n"
        "          compare counterfactual replay recover save batch worker device snapshot\n"
        "          multiprocess cuda benchmark help\n",
        SOBS_VERSION_STRING, program.c_str());
}

struct ArchiveView { ArchiveContents contents; bool ok = false; std::string error; };
ArchiveView load_view(const std::string& path) {
    ArchiveView v;
    auto a = load_archive(path, true);
    if (a.ok()) { v.contents = a.value(); v.ok = true; } else { v.error = a.error(); }
    return v;
}
std::optional<RequestTrace> find_trace(const ArchiveContents& a, const Id128& rid) {
    ReplayResult rp = replay(a.accepted);
    for (const auto& t : rp.traces) if (t.request_id.raw() == rid) return t;
    return std::nullopt;
}

// ================================================================== script
struct ScriptEntry {
    enum class K { OBS, SLEEP } kind = K::OBS;
    ObsType type = ObsType::UNKNOWN;
    u64 obs_ts = 0;
    std::map<std::string, std::string> fields;
    u64 sleep_ms = 0;
};
Result<std::vector<ScriptEntry>> parse_script(const std::string& path) {
    std::ifstream f(path);
    if (!f) return Result<std::vector<ScriptEntry>>::Err("cannot open script: " + path);
    std::vector<ScriptEntry> out;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        std::istringstream ss(line);
        std::string kw; ss >> kw;
        if (kw.empty() || kw[0] == '#') continue;
        if (kw == "SLEEP") { u64 ms; ss >> ms; ScriptEntry e; e.kind = ScriptEntry::K::SLEEP; e.sleep_ms = ms; out.push_back(e); continue; }
        if (kw != "OBS") continue;
        ScriptEntry e; e.kind = ScriptEntry::K::OBS;
        std::string ty; ss >> ty; e.type = obs_type_from_code(ty);
        ss >> e.obs_ts;
        std::string kv;
        while (ss >> kv) {
            auto p = kv.find('=');
            if (p != std::string::npos) e.fields[kv.substr(0, p)] = kv.substr(p + 1);
        }
        out.push_back(std::move(e));
    }
    return Result<std::vector<ScriptEntry>>::Ok(std::move(out));
}

Observation apply_entry(const Source& src, const ScriptEntry& e, SeqNum& seq) {
    Observation o;
    o.type = e.type;
    o.source = src.config().source; o.worker = src.config().worker; o.boot = src.config().boot;
    o.src_gen = src.config().src_gen; o.obs_gen = src.config().obs_gen; o.epoch = src.config().epoch;
    o.clock_domain = src.config().clock_domain;
    o.seq = ++seq;
    o.provenance = Provenance::MEASURED;
    if (e.obs_ts != 0) { o.has_obs_ts = true; o.obs_ts = e.obs_ts; }
    for (const auto& [k, v] : e.fields) {
        if (k == "request") o.set_id(FieldKeys::RequestId, parse_id(v));
        else if (k == "recv") { o.has_recv_ts = true; o.recv_ts = std::strtoull(v.c_str(), nullptr, 0); }
        else if (k == "is_warm") o.set_bool(FieldKeys::IsWarm, v == "true" || v == "1");
        else {
            if (v.size() == 32 && v.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) o.set_id(k, parse_id(v));
            else if (v == "true" || v == "false" || v == "1" || v == "0") o.set_bool(k, v == "true" || v == "1");
            else if (v.find_first_of(".eE") != std::string::npos && v.find_first_not_of("0123456789.+-eE") == std::string::npos) o.set_f64(k, std::atof(v.c_str()));
            else if (v.find_first_not_of("0123456789") == std::string::npos) o.set_u64(k, std::strtoull(v.c_str(), nullptr, 0));
            else o.set_str(k, v);
        }
    }
    o.id = derive_observation_id(o.source.raw(), o.worker.raw(), o.boot.raw(), o.seq, o.type);
    return o;
}

SourceConfig cfg_from_hello(const HelloPayload& h);


// ================================================================== serve
int cmd_serve(const Args& a) {
    std::string host = opt(a, "host", "127.0.0.1");
    u16 port = static_cast<u16>(oid(a, "port", 0));
    std::string out = opt(a, "out", "coord.sobs");
    std::string manifest = opt(a, "manifest", "");
    std::string stop_file = opt(a, "stop-file", "");
    u64 stop_obs = oid(a, "stop-obs", 0);
    u64 epoch = oid(a, "epoch", 1);
    u64 max_ms = oid(a, "max-ms", 0);
    Coordinator coord; coord.set_epoch(static_cast<CoordinatorEpoch>(epoch));
    CoordinatorServer server(coord, host, port, 2000);
    if (!server.start()) { std::fprintf(stderr, "serve: bind failed\n"); return 2; }
    std::printf("PORT=%u\n", server.port()); std::fflush(stdout);
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        if (!stop_file.empty() && std::ifstream(stop_file).good()) break;
        if (stop_obs > 0 && coord.accepted_total() >= stop_obs) break;
        if (max_ms > 0 && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() >= (i64)max_ms) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    server.stop();
    auto saved = save_archive(coord, out);
    Json m = Json::object();
    m.set("port", server.port());
    m.set("accepted", coord.accepted_total());
    m.set("stale", coord.stale_total());
    m.set("duplicates", coord.duplicate_total());
    m.set("sources", coord.source_count());
    m.set("saved", saved.ok());
    if (saved.ok()) {
        m.set("evidence_digest", digest_hex(saved.value().stored_evidence_digest));
        m.set("trace_digest", digest_hex(saved.value().stored_trace_digest));
        m.set("aggregate_digest", digest_hex(saved.value().stored_aggregate_digest));
    }
    if (!manifest.empty()) { std::ofstream out_f(manifest, std::ios::trunc); out_f << m.to_pretty(); }
    std::printf("MANIFEST=%s\n", m.to_canonical().c_str()); std::fflush(stdout);
    return saved.ok() ? 0 : 3;
}

// ================================================================== source
int cmd_source(const Args& a) {
    std::string host = opt(a, "host", "127.0.0.1");
    u16 port = static_cast<u16>(oid(a, "port", 0));
    SourceConfig cfg;
    cfg.source = SourceId(parse_id(opt(a, "source", "00000000000000000000000000000001")));
    cfg.worker = WorkerId(parse_id(opt(a, "worker", "00000000000000000000000000000002")));
    cfg.boot = WorkerBootId(parse_id(opt(a, "boot", "00000000000000000000000000000003")));
    cfg.src_gen = oid(a, "src-gen", 1);
    cfg.obs_gen = oid(a, "obs-gen", 1);
    cfg.epoch = oid(a, "epoch", 1);
    cfg.clock_domain = opt(a, "clock", "mono_ns");
    cfg.role = opt(a, "role", "worker");
    std::string script = opt(a, "script", "");
    Source src(cfg);
    if (!src.connect(host, port)) { std::fprintf(stderr, "source: connect failed\n"); return 2; }
    if (!src.send_hello()) { std::fprintf(stderr, "source: hello rejected\n"); return 2; }
    for (const char* which : {"epoch", "boot", "srcgen", "obsgen"}) {
        std::string key = std::string("stale-") + which;
        if (a.opts.count(key)) {
            Observation o = src.make_observation(ObsType::REQUEST);
            RequestId req = RequestId(parse_id(opt(a, "stale-request", "000000000000000000000000000000aa")));
            o.set_id(FieldKeys::RequestId, req.raw());
            if (std::string(which) == "epoch") o.epoch = oid(a, key, 0);
            else if (std::string(which) == "boot") o.boot = WorkerBootId(parse_id(opt(a, key)));
            else if (std::string(which) == "srcgen") o.src_gen = oid(a, key, 0);
            else if (std::string(which) == "obsgen") o.obs_gen = oid(a, key, 0);
            auto ar = src.send_observation(o);
            if (ar.ok()) { std::printf("STALE_%s=%s\n", which, ar.value().payload.accepted ? "ACCEPTED_BUG" : "REJECTED"); std::fflush(stdout); }
            else { std::printf("STALE_%s=NOACK\n", which); std::fflush(stdout); }
        }
    }
    u64 seq = 0;
    if (!script.empty()) {
        auto entries = parse_script(script);
        if (!entries.ok()) { std::fprintf(stderr, "source: %s\n", entries.error().c_str()); return 2; }
        bool fire = flag(a, "fire");
        for (const auto& e : entries.value()) {
            if (e.kind == ScriptEntry::K::SLEEP) { std::this_thread::sleep_for(std::chrono::milliseconds(e.sleep_ms)); continue; }
            Observation o = apply_entry(src, e, seq);
            if (fire) { src.send_observation_fire(o); }
            else {
                auto ar = src.send_observation(o);
                if (ar.ok()) {
                    bool acc = ar.value().payload.accepted;
                    std::printf("ACK %s %s\n", std::string(obs_type_name(e.type)).c_str(), acc ? "ACCEPTED" : ("REJECTED:" + ar.value().payload.reason).c_str());
                    std::fflush(stdout);
                } else { std::printf("ACK %s NOFRAME\n", std::string(obs_type_name(e.type)).c_str()); std::fflush(stdout); }
            }
        }
    }
    u64 hold_ms = oid(a, "hold-ms", 0);
    if (hold_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    src.disconnect();
    return 0;
}

// ================================================================== ingest
int cmd_ingest(const Args& a) {
    std::string script = opt(a, "script");
    std::string out = opt(a, "out", "coord.sobs");
    u64 epoch = oid(a, "epoch", 1);
    SourceConfig cfg;
    cfg.source = SourceId(parse_id(opt(a, "source", "00000000000000000000000000000001")));
    cfg.worker = WorkerId(parse_id(opt(a, "worker", "00000000000000000000000000000002")));
    cfg.boot = WorkerBootId(parse_id(opt(a, "boot", "00000000000000000000000000000003")));
    cfg.src_gen = oid(a, "src-gen", 1);
    cfg.obs_gen = oid(a, "obs-gen", 1);
    cfg.epoch = static_cast<CoordinatorEpoch>(epoch);
    Coordinator coord; coord.set_epoch(static_cast<CoordinatorEpoch>(epoch));
    HelloPayload h; h.source=cfg.source; h.worker=cfg.worker; h.boot=cfg.boot;
    h.src_gen=cfg.src_gen; h.obs_gen=cfg.obs_gen; h.epoch=cfg.epoch; h.clock_domain="mono_ns";
    coord.register_hello(h);
    Source src(cfg);
    // Build a synthetic Source (no connection) just to reuse apply_entry? Build directly instead.
    auto entries = parse_script(script);
    if (!entries.ok()) { std::fprintf(stderr, "ingest: %s\n", entries.error().c_str()); return 2; }
    u64 seq = 0; u64 accepted = 0, stale = 0;
    for (const auto& e : entries.value()) {
        if (e.kind == ScriptEntry::K::OBS) {
            // Build an observation with the cfg authority using a local Source object.
            Observation o;
            o.type = e.type; o.source = cfg.source; o.worker = cfg.worker; o.boot = cfg.boot;
            o.src_gen = cfg.src_gen; o.obs_gen = cfg.obs_gen; o.epoch = cfg.epoch; o.clock_domain = "mono_ns";
            o.seq = ++seq; o.provenance = Provenance::MEASURED;
            if (e.obs_ts != 0) { o.has_obs_ts = true; o.obs_ts = e.obs_ts; }
            for (const auto& [k, v] : e.fields) {
                if (k == "request") o.set_id(FieldKeys::RequestId, parse_id(v));
                else if (k == "recv") { o.has_recv_ts = true; o.recv_ts = std::strtoull(v.c_str(), nullptr, 0); }
                else if (k == "is_warm") o.set_bool(FieldKeys::IsWarm, v == "true" || v == "1");
                else if (v.size() == 32 && v.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) o.set_id(k, parse_id(v));
                else if (v == "true" || v == "false" || v == "1" || v == "0") o.set_bool(k, v == "true" || v == "1");
                else if (v.find_first_of(".eE") != std::string::npos && v.find_first_not_of("0123456789.+-eE") == std::string::npos) o.set_f64(k, std::atof(v.c_str()));
                else if (v.find_first_not_of("0123456789") == std::string::npos) o.set_u64(k, std::strtoull(v.c_str(), nullptr, 0));
                else o.set_str(k, v);
            }
            o.id = derive_observation_id(o.source.raw(), o.worker.raw(), o.boot.raw(), o.seq, o.type);
            IngestResult ir = coord.ingest(o);
            if (ir.accepted) ++accepted; else ++stale;
        }
    }
    auto saved = save_archive(coord, out);
    std::printf("accepted=%llu stale=%llu saved=%d\n", (unsigned long long)accepted, (unsigned long long)stale, (int)saved.ok());
    if (saved.ok()) std::printf("evidence_digest=%s\n", digest_hex(saved.value().stored_evidence_digest).c_str());
    return saved.ok() ? 0 : 3;
}

// ================================================================== read-only commands
int cmd_list(const Args& a) {
    auto v = load_view(opt(a, "archive"));
    if (!v.ok) { std::fprintf(stderr, "list: %s\n", v.error.c_str()); return 2; }
    std::printf("magic=%08x version=%u epoch=%llu accepted=%zu stale=%zu sources=%zu\n",
                kArchiveMagic, kArchiveVersion, (unsigned long long)v.contents.epoch,
                v.contents.accepted.size(), v.contents.stale.size(), v.contents.registry.size());
    for (std::size_t i = 0; i < v.contents.accepted.size(); ++i) {
        const auto& o = v.contents.accepted[i];
        std::printf("  %zu obs=%s type=%-20s seq=%llu boot=%s src_gen=%llu obs_gen=%llu epoch=%llu prov=%s req=%s\n",
                    i, o.id.to_hex().c_str(), std::string(obs_type_name(o.type)).c_str(),
                    (unsigned long long)o.seq, o.boot.to_hex().c_str(),
                    (unsigned long long)o.src_gen, (unsigned long long)o.obs_gen, (unsigned long long)o.epoch,
                    std::string(provenance_name(o.provenance)).c_str(), o.request_id().to_hex().c_str());
    }
    return 0;
}
int cmd_inspect(const Args& a) {
    auto v = load_view(opt(a, "archive"));
    if (!v.ok) { std::fprintf(stderr, "inspect: %s\n", v.error.c_str()); return 2; }
    Id128 id = parse_id(opt(a, "id"));
    for (const auto& o : v.contents.accepted) if (o.id.raw() == id) { std::printf("%s\n", observation_to_json(o).to_pretty().c_str()); return 0; }
    std::fprintf(stderr, "inspect: id not found\n"); return 1;
}
int cmd_trace(const Args& a) {
    auto v = load_view(opt(a, "archive"));
    if (!v.ok) { std::fprintf(stderr, "trace: %s\n", v.error.c_str()); return 2; }
    auto rt = find_trace(v.contents, parse_id(opt(a, "request")));
    if (!rt) { std::fprintf(stderr, "trace: request not found\n"); return 1; }
    if (flag(a, "json")) std::printf("%s\n", trace_to_json(*rt).to_pretty().c_str());
    else {
        std::printf("request %s outcome=%s epochs=%llu..%llu boots=%zu\n", rt->request_id.to_hex().c_str(),
                    std::string(outcome_name(rt->outcome)).c_str(), (unsigned long long)rt->min_epoch,
                    (unsigned long long)rt->max_epoch, rt->boots.size());
        for (const auto& e : rt->timeline)
            std::printf("  #%-3u %-20s ts=%llu seq=%llu prov=%s src=%s\n",
                        e.ordinal, std::string(obs_type_name(e.type)).c_str(),
                        (unsigned long long)e.ts, (unsigned long long)e.seq,
                        std::string(provenance_name(e.provenance)).c_str(), e.source.to_hex().c_str());
    }
    return 0;
}
int cmd_timeline(const Args& a) {
    auto v = load_view(opt(a, "archive"));
    if (!v.ok) { std::fprintf(stderr, "timeline: %s\n", v.error.c_str()); return 2; }
    auto rt = find_trace(v.contents, parse_id(opt(a, "request")));
    if (!rt) { std::fprintf(stderr, "timeline: request not found\n"); return 1; }
    for (const auto& e : rt->timeline)
        std::printf("%llu ns  %-22s  prov=%-12s  boot=%s\n", (unsigned long long)e.ts,
                    std::string(obs_type_name(e.type)).c_str(), std::string(provenance_name(e.provenance)).c_str(),
                    e.boot.to_hex().c_str());
    return 0;
}
int cmd_explain(const Args& a) {
    auto v = load_view(opt(a, "archive"));
    if (!v.ok) { std::fprintf(stderr, "explain: %s\n", v.error.c_str()); return 2; }
    auto rt = find_trace(v.contents, parse_id(opt(a, "request")));
    if (!rt) { std::fprintf(stderr, "explain: request not found\n"); return 1; }
    Explanation e = explain_request(*rt);
    if (flag(a, "json")) std::printf("%s\n", e.to_json().to_pretty().c_str());
    else std::printf("%s", explanation_text(e).c_str());
    return 0;
}
int cmd_latency(const Args& a) {
    auto v = load_view(opt(a, "archive"));
    if (!v.ok) { std::fprintf(stderr, "latency: %s\n", v.error.c_str()); return 2; }
    auto rt = find_trace(v.contents, parse_id(opt(a, "request")));
    if (!rt) { std::fprintf(stderr, "latency: request not found\n"); return 1; }
    LatencyBreakdown b = decompose_latency(*rt);
    if (flag(a, "json")) std::printf("%s\n", b.to_json().to_pretty().c_str());
    else std::printf("%s", latency_text(b).c_str());
    ResourceAttribution at = attribute_resources(*rt);
    std::printf("%s", attribution_text(at).c_str());
    return 0;
}
int cmd_tail(const Args& a) {
    auto v = load_view(opt(a, "archive"));
    if (!v.ok) { std::fprintf(stderr, "tail: %s\n", v.error.c_str()); return 2; }
    ReplayResult rp = replay(v.contents.accepted);
    double pct = of(a, "pct", 90.0);
    auto tails = explain_tail(rp.traces, pct);
    if (flag(a, "json")) {
        Json j = Json::object(); Json arr = Json::array();
        for (const auto& t : tails) arr.push(t.to_json());
        j.set("metrics", rp.ungrouped_metrics.to_json()); j.set("tail", std::move(arr));
        std::printf("%s\n", j.to_pretty().c_str());
    } else {
        std::printf("tail: p50=%llu p90=%llu p95=%llu p99=%llu p999=%llu n=%llu\n",
                    (unsigned long long)rp.ungrouped_metrics.p50_ns, (unsigned long long)rp.ungrouped_metrics.p90_ns,
                    (unsigned long long)rp.ungrouped_metrics.p95_ns, (unsigned long long)rp.ungrouped_metrics.p99_ns,
                    (unsigned long long)rp.ungrouped_metrics.p999_ns, (unsigned long long)rp.ungrouped_metrics.count);
        for (const auto& t : tails) std::printf("%s", tail_text(t).c_str());
    }
    return 0;
}
int cmd_compare(const Args& a) {
    auto v = load_view(opt(a, "archive"));
    if (!v.ok) { std::fprintf(stderr, "compare: %s\n", v.error.c_str()); return 2; }
    auto ta = find_trace(v.contents, parse_id(opt(a, "a")));
    auto tb = find_trace(v.contents, parse_id(opt(a, "b")));
    if (!ta || !tb) { std::fprintf(stderr, "compare: request missing\n"); return 1; }
    TraceComparison c = compare_traces(*ta, *tb);
    if (flag(a, "json")) std::printf("%s\n", c.to_json().to_pretty().c_str());
    else std::printf("%s", comparison_text(c).c_str());
    return 0;
}
int cmd_counterfactual(const Args& a) {
    auto v = load_view(opt(a, "archive"));
    if (!v.ok) { std::fprintf(stderr, "counterfactual: %s\n", v.error.c_str()); return 2; }
    auto rt = find_trace(v.contents, parse_id(opt(a, "request")));
    if (!rt) { std::fprintf(stderr, "counterfactual: request not found\n"); return 1; }
    std::string rule = opt(a, "rule", "remove_queue_wait");
    CounterfactualRule cr = CounterfactualRule::REMOVE_QUEUE_WAIT;
    if (rule == "remove_retry") cr = CounterfactualRule::REMOVE_RETRY;
    else if (rule == "warm_residency") cr = CounterfactualRule::WARM_RESIDENCY;
    else if (rule == "reduce_transfer_by_half") cr = CounterfactualRule::REDUCE_TRANSFER_BY_HALF;
    auto cf = counterfactual(*rt, cr);
    if (flag(a, "json")) std::printf("%s\n", cf.to_json().to_pretty().c_str());
    else std::printf("%s", counterfactual_text(cf).c_str());
    return 0;
}
int cmd_replay(const Args& a) {
    auto v = load_view(opt(a, "archive"));
    if (!v.ok) { std::fprintf(stderr, "replay: %s\n", v.error.c_str()); return 2; }
    bool match = v.contents.replay_match;
    std::printf("obs=%zu traces=%zu replay_match=%d\n", v.contents.accepted.size(), v.contents.verified_replay.request_trace_count, (int)match);
    std::printf("evidence=%s\n", digest_hex(v.contents.stored_evidence_digest).c_str());
    std::printf("trace=%s\n", digest_hex(v.contents.stored_trace_digest).c_str());
    std::printf("explanation=%s\n", digest_hex(v.contents.stored_explanation_digest).c_str());
    std::printf("aggregate=%s\n", digest_hex(v.contents.stored_aggregate_digest).c_str());
    return match ? 0 : 1;
}
int cmd_recover(const Args& a) {
    auto v = load_view(opt(a, "archive"));
    if (!v.ok) { std::fprintf(stderr, "recover: REJECTED %s\n", v.error.c_str()); return 1; }
    std::printf("RECOVERED epoch=%llu accepted=%zu stale=%zu replay_match=%d\n",
                (unsigned long long)v.contents.epoch, v.contents.accepted.size(), v.contents.stale.size(), (int)v.contents.replay_match);
    return 0;
}
int cmd_batch(const Args& a) {
    auto v = load_view(opt(a, "archive"));
    if (!v.ok) { std::fprintf(stderr, "batch: %s\n", v.error.c_str()); return 2; }
    std::map<std::string, u64> bsize;
    for (const auto& o : v.contents.accepted)
        if (o.type == ObsType::BATCH_FORM) bsize[o.batch_id().to_hex()] = std::max(bsize[o.batch_id().to_hex()], o.u64_field(FieldKeys::BatchSize));
    std::printf("batches=%zu\n", bsize.size());
    for (const auto& [b, s] : bsize) std::printf("  batch=%s size=%llu\n", b.c_str(), (unsigned long long)s);
    return 0;
}
int cmd_worker(const Args& a) {
    auto v = load_view(opt(a, "archive"));
    if (!v.ok) { std::fprintf(stderr, "worker: %s\n", v.error.c_str()); return 2; }
    std::printf("registry=%zu\n", v.contents.registry.size());
    for (const auto& o : v.contents.registry)
        std::printf("  worker=%s boot=%s src_gen=%llu obs_gen=%llu last_seq=%llu restart=%d\n",
                    o.worker.to_hex().c_str(), o.boot.to_hex().c_str(), (unsigned long long)o.src_gen,
                    (unsigned long long)o.obs_gen, (unsigned long long)o.last_seq, o.restart_count);
    return 0;
}
int cmd_snapshot(const Args& a) {
    auto v = load_view(opt(a, "archive"));
    if (!v.ok) { std::fprintf(stderr, "snapshot: %s\n", v.error.c_str()); return 2; }
    ReplayResult rp = replay(v.contents.accepted);
    Json s = Json::object();
    s.set("epoch", v.contents.epoch); s.set("accepted", v.contents.accepted.size()); s.set("stale", v.contents.stale.size());
    s.set("trace_count", rp.request_trace_count); s.set("obs_count", rp.obs_count);
    s.set("tail", rp.ungrouped_metrics.to_json()); s.set("digest", digest_hex(v.contents.stored_aggregate_digest));
    std::printf("%s\n", s.to_pretty().c_str());
    return 0;
}
int cmd_device(const Args& a) { (void)a; std::printf("{\"device\":\"RTX 5090\"}\n"); return 0; }
#if defined(SOBS_HAVE_CUDA)
int cmd_cuda(const Args& a) {
    (void)a;
    try {
        CudaScenario sc = run_cuda_proof();
        std::printf("%s\n", sc.kpi_json().to_pretty().c_str());
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "cuda: %s\n", e.what()); return 1; }
}
#else
int cmd_cuda(const Args& a) { (void)a; std::fprintf(stderr, "cuda: built without CUDA\n"); return 1; }
#endif


// ================================================================== multiprocess proof
struct Child { PROCESS_INFORMATION pi{}; HANDLE out_read = NULL; };
bool child_alive_try(Child& c) { return WaitForSingleObject(c.pi.hProcess, 0) == WAIT_OBJECT_0 ? false : true; }
SourceConfig cfg_from_hello(const HelloPayload& h) {
    SourceConfig c; c.source=h.source; c.worker=h.worker; c.boot=h.boot; c.src_gen=h.src_gen;
    c.obs_gen=h.obs_gen; c.epoch=h.epoch; c.clock_domain="mono_ns"; return c;
}
bool spawn_self(const std::string& args, Child& c, bool capture_out) {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    std::string cmd = std::string("\"") + exe + "\" " + args;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rpipe = NULL, wpipe = NULL;
    if (capture_out) {
        if (!CreatePipe(&rpipe, &wpipe, &sa, 0)) return false;
        SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);
    }
    STARTUPINFOA si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    if (capture_out) { si.hStdOutput = wpipe; si.hStdError = GetStdHandle(STD_ERROR_HANDLE); si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); }
    if (!CreateProcessA(NULL, cmd.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &c.pi)) {
        if (capture_out) { CloseHandle(rpipe); CloseHandle(wpipe); }
        return false;
    }
    if (capture_out) { CloseHandle(wpipe); c.out_read = rpipe; }
    return true;
}
void kill_child(Child& c) { if (c.pi.hProcess) TerminateProcess(c.pi.hProcess, 1); }
void wait_child(Child& c, u32 timeout_ms) { if (c.pi.hProcess) WaitForSingleObject(c.pi.hProcess, timeout_ms); }
void close_child(Child& c) {
    if (c.pi.hThread) CloseHandle(c.pi.hThread);
    if (c.pi.hProcess) CloseHandle(c.pi.hProcess);
    if (c.out_read) CloseHandle(c.out_read);
    c.pi.hThread = c.pi.hProcess = NULL; c.out_read = NULL;
}
bool child_done(Child& c) { return WaitForSingleObject(c.pi.hProcess, 0) == WAIT_OBJECT_0; }
std::string read_pipe(Child& c, u32 timeout_ms, bool& closed) {
    std::string out; DWORD read_avail = 0; DWORD start = GetTickCount(); char buf[4096];
    for (;;) {
        if (PeekNamedPipe(c.out_read, NULL, 0, NULL, &read_avail, NULL)) {
            if (read_avail > 0) {
                DWORD got = 0;
                if (ReadFile(c.out_read, buf, (DWORD)std::min<std::size_t>(sizeof(buf), read_avail), &got, NULL) && got > 0) { out.append(buf, got); continue; }
            }
        }
        if (GetTickCount() - start > timeout_ms) { closed = false; return out; }
        if (!child_alive_try(c)) { closed = true; return out; }
        if (child_done(c)) { closed = true; }
        Sleep(20);
    }
}

int cmd_multiprocess(const Args& a) {
    std::string dir = opt(a, "dir", "proof_run");
    CreateDirectoryA(dir.c_str(), nullptr);
    u64 epoch = oid(a, "epoch", 1);
    Id128 sourceA = parse_id(opt(a, "source-a", "aa000000000000000000000000000001"));
    Id128 sourceB = parse_id(opt(a, "source-b", "bb000000000000000000000000000002"));
    Id128 bootA1 = parse_id(opt(a, "boot-a1", "aaa10000000000000000000000000001"));
    Id128 bootA2 = parse_id(opt(a, "boot-a2", "aaa20000000000000000000000000002"));
    Id128 bootB  = parse_id(opt(a, "boot-b",  "bbb00000000000000000000000000003"));
    std::string reqR1 = "c1111111111111111111111111111111";
    std::string reqR2 = "c2222222222222222222222222222222";
    std::string reqR3 = "c3333333333333333333333333333333";
    std::string reqR4 = "c4444444444444444444444444444444";

    std::string a1 = dir + "/a1.jsonl";
    { std::ofstream f(a1);
      f << "OBS REQUEST 1000 request=" << reqR1 << "\n";
      f << "OBS ADMISSION 1200 request=" << reqR1 << "\n";
      f << "OBS QUEUE_ENTER 1300 request=" << reqR1 << "\n";
      f << "OBS QUEUE_LEAVE 1600 request=" << reqR1 << "\n";
      f << "OBS DISPATCH 1700 request=" << reqR1 << "\n";
      f << "OBS PREFILL_START 1800 request=" << reqR1 << " input_tokens=64\n";
      f << "OBS PREFILL_END 2300 request=" << reqR1 << " gpu_ms=0.4\n";
      f << "OBS COMPLETION 2600 request=" << reqR1 << " outcome=completed\n";
      f << "OBS REQUEST 3000 request=" << reqR2 << "\n";
      f << "OBS ADMISSION 3100 request=" << reqR2 << "\n";
      f << "OBS PREFILL_START 3200 request=" << reqR2 << " input_tokens=128\n"; }
    std::string b = dir + "/b.jsonl";
    { std::ofstream f(b);
      f << "OBS REQUEST 5000 request=" << reqR3 << "\n";
      f << "OBS ADMISSION 5100 request=" << reqR3 << "\n";
      f << "OBS QUEUE_ENTER 5200 request=" << reqR3 << "\n";
      f << "OBS QUEUE_LEAVE 5300 request=" << reqR3 << "\n";
      f << "OBS DISPATCH 5400 request=" << reqR3 << "\n";
      f << "OBS PREFILL_START 5500 request=" << reqR3 << " input_tokens=96\n";
      f << "OBS PREFILL_END 5900 request=" << reqR3 << " gpu_ms=0.3\n";
      f << "OBS DECODE_STEP_START 6000 request=" << reqR3 << " output_tokens=1\n";
      f << "OBS DECODE_STEP_END 6050 request=" << reqR3 << " gpu_ms=0.05\n";
      f << "OBS COMPLETION 6100 request=" << reqR3 << " outcome=completed\n"; }
    std::string a2 = dir + "/a2.jsonl";
    { std::ofstream f(a2);
      f << "OBS PREFILL_END 3300 request=" << reqR2 << " gpu_ms=0.1\n";
      f << "OBS DECODE_STEP_START 3400 request=" << reqR2 << " output_tokens=1\n";
      f << "OBS DECODE_STEP_END 3450 request=" << reqR2 << " gpu_ms=0.05\n";
      f << "OBS COMPLETION 3500 request=" << reqR2 << " outcome=completed\n";
      f << "OBS REQUEST 9000 request=" << reqR4 << "\n";
      f << "OBS ADMISSION 9100 request=" << reqR4 << "\n";
      f << "OBS DISPATCH 9200 request=" << reqR4 << "\n";
      f << "OBS PREFILL_START 9300 request=" << reqR4 << " input_tokens=32\n";
      f << "OBS PREFILL_END 9600 request=" << reqR4 << " gpu_ms=0.2\n";
      f << "OBS COMPLETION 9800 request=" << reqR4 << " outcome=completed\n"; }

    std::string coord_out = dir + "/coord.sobs";
    std::string manifest_path = dir + "/manifest.json";
    std::string stop_file = dir + "/stop.marker";
    u16 port = static_cast<u16>(oid(a, "port", 43822));
    std::string coord_args = "serve --host 127.0.0.1 --port " + std::to_string(port) + " --epoch " + std::to_string(epoch) +
                             " --out " + coord_out + " --manifest " + manifest_path +
                             " --stop-file " + stop_file + " --stop-obs 0";
    Child coord;
    if (!spawn_self(coord_args, coord, false)) { std::fprintf(stderr, "multiprocess: cannot spawn coordinator\n"); return 2; }
    Sleep(500);

    std::string a1args = "source --host 127.0.0.1 --port " + std::to_string(port) +
                         " --source " + sourceA.to_hex() + " --worker " + sourceA.to_hex() +
                         " --boot " + bootA1.to_hex() + " --src-gen 1 --obs-gen 1 --epoch " + std::to_string(epoch) +
                         " --script " + a1 + " --hold-ms 60000";
    Child a1c; if (!spawn_self(a1args, a1c, false)) { std::fprintf(stderr, "multiprocess: cannot spawn A1\n"); kill_child(coord); close_child(coord); return 2; }
    std::string barg = "source --host 127.0.0.1 --port " + std::to_string(port) +
                       " --source " + sourceB.to_hex() + " --worker " + sourceB.to_hex() +
                       " --boot " + bootB.to_hex() + " --src-gen 1 --obs-gen 1 --epoch " + std::to_string(epoch) +
                       " --script " + b;
    Child bc; if (!spawn_self(barg, bc, false)) { std::fprintf(stderr, "multiprocess: cannot spawn B\n"); kill_child(coord); close_child(coord); return 2; }
    Sleep(1500);

    kill_child(a1c); wait_child(a1c, 3000); close_child(a1c);

    std::string a2args = "source --host 127.0.0.1 --port " + std::to_string(port) +
                         " --source " + sourceA.to_hex() + " --worker " + sourceA.to_hex() +
                         " --boot " + bootA2.to_hex() + " --src-gen 2 --obs-gen 2 --epoch " + std::to_string(epoch) +
                         " --stale-epoch 0 --stale-boot " + bootA1.to_hex() + " --stale-srcgen 1 --stale-obsgen 1" +
                         " --stale-request " + reqR1 + " --script " + a2;
    Child a2c; if (!spawn_self(a2args, a2c, true)) { std::fprintf(stderr, "multiprocess: cannot spawn A2\n"); kill_child(coord); close_child(coord); return 2; }
    std::string a2out; DWORD t2 = GetTickCount(); bool a2done = false;
    while (!a2done && (GetTickCount() - t2) < 15000) {
        bool closed = false; a2out += read_pipe(a2c, 200, closed);
        if (a2out.find("ACK COMPLETION ACCEPTED") != std::string::npos || a2out.find("ACK completion ACCEPTED") != std::string::npos) a2done = true;
        if (child_done(a2c)) a2done = true;
        Sleep(50);
    }
    if (flag(a, "verbose")) { std::printf("--- A2 stdout ---\n%s\n-----------------\n", a2out.c_str()); }
    int stale_rejected = 0;
    for (const char* w : {"epoch", "boot", "srcgen", "obsgen"}) {
        std::string mark = std::string("STALE_") + w + "=";
        std::size_t pos = a2out.find(mark);
        if (pos != std::string::npos && a2out.find("REJECTED", pos) != std::string::npos) ++stale_rejected;
    }
    wait_child(a2c, 5000); close_child(a2c);
    wait_child(bc, 3000); close_child(bc);

    std::ofstream mk(stop_file); mk << "stop";
    // Signal the coordinator and wait for it to save + exit (it reads stop-file).
    wait_child(coord, 25000);
    close_child(coord);

    auto arch = load_archive(coord_out, true);
    if (!arch.ok()) { std::fprintf(stderr, "multiprocess: archive FAILED: %s\n", arch.error().c_str()); return 1; }
    const ArchiveContents& ac = arch.value();
    std::printf("MULTIPROCESS PROOF\n");
    std::printf("  archive: epoch=%llu accepted=%zu stale=%zu replay_match=%d\n",
                (unsigned long long)ac.epoch, ac.accepted.size(), ac.stale.size(), (int)ac.replay_match);
    std::printf("  evidence_digest=%s\n", digest_hex(ac.stored_evidence_digest).c_str());
    std::printf("  trace_digest=%s\n", digest_hex(ac.stored_trace_digest).c_str());
    u64 stale_old_boot = 0;
    for (const auto& [o, r] : ac.stale) if (o.boot.raw() == bootA1) stale_old_boot++;
    std::printf("  stale_old_boot_evidence=%llu\n", (unsigned long long)stale_old_boot);
    std::printf("  stale_rejected_over_tcp=%d\n", stale_rejected);
    auto tR2 = find_trace(ac, parse_id(reqR2));
    auto tR4 = find_trace(ac, parse_id(reqR4));
    bool r2_span = tR2.has_value() && tR2->boots.size() >= 2;
    bool r4_fresh = tR4.has_value() && tR4->boots.size() == 1 && tR4->boots[0].raw() == bootA2;
    std::printf("  r2_spans_sources=%d r4_uses_fresh_boot_only=%d\n", (int)r2_span, (int)r4_fresh);
    int verdict = (ac.replay_match && stale_rejected >= 4 && stale_old_boot > 0 && r2_span && r4_fresh) ? 0 : 1;
    std::printf("multiprocess verdict=%s\n", verdict == 0 ? "PASS" : "FAIL");
    return verdict;
}

// ================================================================== benchmark

int cmd_benchmark(const Args& a) {
    std::string mode = opt(a, "mode", "ingest");
    u64 n = oid(a, "count", 10000);
    if (mode == "ingest") {
        Coordinator coord; coord.set_epoch(1);
        HelloPayload h; h.source=SourceId(Id128::from_u64(1)); h.worker=WorkerId(Id128::from_u64(1));
        h.boot=WorkerBootId(Id128::make(1,1)); h.src_gen=1; h.obs_gen=1; h.epoch=1;
        coord.register_hello(h);
        u64 seq = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (u64 i = 0; i < n; ++i) {
            Observation o; o.type=ObsType::REQUEST; o.source=h.source; o.worker=h.worker; o.boot=h.boot;
            o.src_gen=1; o.obs_gen=1; o.epoch=1; o.clock_domain="m"; o.seq=++seq; o.provenance=Provenance::MEASURED;
            o.set_id(FieldKeys::RequestId, RequestId(Id128::from_u64(i)).raw());
            o.id=derive_observation_id(h.source.raw(), h.worker.raw(), h.boot.raw(), seq, o.type);
            coord.ingest(o);
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::printf("ingest count=%llu time_ms=%.2f ops_per_sec=%.0f\n", (unsigned long long)n, ms, ms > 0 ? n/(ms/1000.0) : 0.0);
    } else if (mode == "json") {
        u64 seq = 0; auto t0 = std::chrono::steady_clock::now();
        for (u64 i = 0; i < n; ++i) {
            Observation o; o.type=ObsType::REQUEST; o.seq=++seq; o.provenance=Provenance::MEASURED;
            o.set_id(FieldKeys::RequestId, RequestId(Id128::from_u64(i)).raw());
            o.id=derive_observation_id(Id128::from_u64(1), Id128::from_u64(1), Id128::from_u64(1), seq, o.type);
            std::string tmp = observation_to_json(o).to_canonical(); (void)tmp;
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::printf("json count=%llu time_ms=%.2f ops_per_sec=%.0f\n", (unsigned long long)n, ms, ms>0 ? n/(ms/1000.0) : 0.0);
    } else if (mode == "persist") {
        Coordinator coord; coord.set_epoch(1);
        HelloPayload h; h.source=SourceId(Id128::from_u64(1)); h.worker=WorkerId(Id128::from_u64(1));
        h.boot=WorkerBootId(Id128::make(1,1)); h.src_gen=1; h.obs_gen=1; h.epoch=1;
        coord.register_hello(h);
        u64 seq = 0;
        for (u64 i = 0; i < n; ++i) {
            Observation o; o.type=ObsType::REQUEST; o.source=h.source; o.worker=h.worker; o.boot=h.boot;
            o.src_gen=1; o.obs_gen=1; o.epoch=1; o.clock_domain="m"; o.seq=++seq; o.provenance=Provenance::MEASURED;
            o.set_id(FieldKeys::RequestId, RequestId(Id128::from_u64(i)).raw());
            o.id=derive_observation_id(h.source.raw(), h.worker.raw(), h.boot.raw(), seq, o.type);
            coord.ingest(o);
        }
        auto t0 = std::chrono::steady_clock::now();
        auto saved = save_archive(coord, "bench_data/bench.sobs");
        auto loaded = load_archive("bench_data/bench.sobs", true);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::printf("persist count=%llu save=%d load=%d time_ms=%.2f\n", (unsigned long long)n, (int)saved.ok(), (int)loaded.ok(), ms);
    } else if (mode == "concurrent") {
        u64 threads = oid(a, "threads", 8);
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> ths;
        for (u64 th = 0; th < threads; ++th) {
            ths.emplace_back([th, n, threads]() {
                Coordinator coord; coord.set_epoch(1);
                HelloPayload h; h.source=SourceId(Id128::from_u64(1)); h.worker=WorkerId(Id128::from_u64(1));
                h.boot=WorkerBootId(Id128::make(1,1)); h.src_gen=1; h.obs_gen=1; h.epoch=1;
                coord.register_hello(h);
                u64 seq = 0;
                for (u64 i = th; i < n; i += threads) {
                    Observation o; o.type=ObsType::REQUEST; o.source=h.source; o.worker=h.worker; o.boot=h.boot;
                    o.src_gen=1; o.obs_gen=1; o.epoch=1; o.clock_domain="m"; o.seq=++seq; o.provenance=Provenance::MEASURED;
                    o.set_id(FieldKeys::RequestId, RequestId(Id128::from_u64(i)).raw());
                    o.id=derive_observation_id(h.source.raw(), h.worker.raw(), h.boot.raw(), seq + 1000000000u * th, o.type);
                    coord.ingest(o);
                }
            });
        }
        for (auto& th : ths) th.join();
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::printf("concurrent count=%llu threads=%llu time_ms=%.2f ops_per_sec=%.0f\n", (unsigned long long)n, (unsigned long long)threads, ms, ms>0 ? n/(ms/1000.0) : 0.0);
    } else if (mode == "replay") {
        std::vector<Observation> ev; u64 seq = 0;
        for (u64 i = 0; i < n; ++i) {
            Observation o; o.type=ObsType::REQUEST; o.seq=++seq; o.provenance=Provenance::MEASURED;
            o.set_id(FieldKeys::RequestId, RequestId(Id128::from_u64(i)).raw());
            o.id=derive_observation_id(Id128::from_u64(1), Id128::from_u64(1), Id128::from_u64(1), seq, o.type);
            ev.push_back(o);
        }
        auto t0 = std::chrono::steady_clock::now();
        auto rp = replay(ev);
        auto t1 = std::chrono::steady_clock::now();
        std::printf("replay count=%llu traces=%llu time_ms=%.2f\n", (unsigned long long)n, (unsigned long long)rp.request_trace_count,
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
    } else { std::fprintf(stderr, "benchmark: unknown mode %s\n", mode.c_str()); return 2; }
    return 0;
}


int cli_main(int argc, char** argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }
    std::string cmd = argv[1];
    Args a = parse_args(argc - 1, argv + 1);
    a.positional.insert(a.positional.begin(), cmd);
    if (cmd == "serve") return cmd_serve(a);
    if (cmd == "source") return cmd_source(a);
    if (cmd == "ingest") return cmd_ingest(a);
    if (cmd == "list") return cmd_list(a);
    if (cmd == "inspect") return cmd_inspect(a);
    if (cmd == "trace") return cmd_trace(a);
    if (cmd == "timeline") return cmd_timeline(a);
    if (cmd == "explain") return cmd_explain(a);
    if (cmd == "latency") return cmd_latency(a);
    if (cmd == "tail") return cmd_tail(a);
    if (cmd == "compare") return cmd_compare(a);
    if (cmd == "counterfactual") return cmd_counterfactual(a);
    if (cmd == "replay") return cmd_replay(a);
    if (cmd == "recover") return cmd_recover(a);
    if (cmd == "batch") return cmd_batch(a);
    if (cmd == "worker") return cmd_worker(a);
    if (cmd == "snapshot") return cmd_snapshot(a);
    if (cmd == "device") return cmd_device(a);
    if (cmd == "multiprocess") return cmd_multiprocess(a);
    if (cmd == "cuda") return cmd_cuda(a);
    if (cmd == "benchmark") return cmd_benchmark(a);
    if (cmd == "help" || cmd == "--help" || cmd == "-h") { print_usage(argv[0]); return 0; }
    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    print_usage(argv[0]);
    return 1;
}

} // namespace cli
} // namespace servingobs

int main(int argc, char** argv) { return servingobs::cli::cli_main(argc, argv); }