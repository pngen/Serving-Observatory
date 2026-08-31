// Serving Observatory — deterministic replay and stable digests.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/replay/replay.hpp"

#include <algorithm>
#include <map>
#include <string>

namespace servingobs {

namespace {
// deterministic canonical bytes for a request trace.
string canonical_trace(const RequestTrace& t) { return trace_to_json(t).to_canonical(); }
string canonical_explanation(const RequestTrace& t) { return explain_request(t).to_json().to_canonical(); }
}

Digest trace_digest_of(const RequestTrace& t) {
    return Sha256::hash(canonical_trace(t));
}

Digest evidence_digest_of(const std::vector<Observation>& observations) {
    std::vector<Observation> sorted = observations;
    std::sort(sorted.begin(), sorted.end(),
              [](const Observation& a, const Observation& b) { return a.id.raw() < b.id.raw(); });
    BinaryWriter w;
    w.write_u64(sorted.size());
    for (const auto& o : sorted) w.write_bytes(encode_observation_payload(o));
    return Sha256::hash(w.data());
}

ReplayResult replay(const std::vector<Observation>& observations) {
    ReplayResult result;
    result.obs_count = observations.size();
    result.evidence_digest = evidence_digest_of(observations);

    // Group observations by request id (deterministic). Non-request-scoped
    // observations are recorded but do not form a per-request trace.
    std::map<RequestId, std::vector<Observation>> groups;
    for (const auto& o : observations) {
        RequestId rid = o.request_id();
        if (!rid.is_null()) groups[rid].push_back(o);
    }
    result.request_trace_count = groups.size();

    for (auto& [rid, obs] : groups) {
        result.traces.push_back(reconstruct_trace(obs, rid));
    }
    // Deterministic digest over traces in request-id order.
    // (std::map iterates in sorted operator< order.)
    BinaryWriter tw, xw;
    std::vector<u64> lats;
    for (const auto& t : result.traces) {
        const string ct = canonical_trace(t);
        tw.write_bytes(make_bytes(ct.data(), ct.size()));
        const string xex = canonical_explanation(t);
        xw.write_bytes(make_bytes(xex.data(), xex.size()));
        const PhaseInterval& tot = t.get_phase(Phase::TOTAL);
        lats.push_back(tot.exists ? (tot.end - tot.start) : 0);
    }
    result.trace_digest = Sha256::hash(tw.data());
    result.explanation_digest = Sha256::hash(xw.data());

    // Aggregate digest: stable snapshot of derived aggregates.
    result.ungrouped_metrics = tail_metrics(lats);
    Json agg = Json::object();
    agg.set("obs_count", result.obs_count);
    agg.set("request_trace_count", result.request_trace_count);
    agg.set("metrics", result.ungrouped_metrics.to_json());
    u64 retries = 0, kvhits = 0, kvmiss = 0, bytesh2d = 0, total_lat = 0, gpu = 0;
    for (const auto& t : result.traces) {
        auto at = attribute_resources(t);
        retries += at.spec_proposed + at.spec_accepted + at.spec_rejected;  // speculation counts
        for (const auto& o : t.evidence) {
            if (o.type == ObsType::RETRY) retries++;
            else if (o.type == ObsType::KV_HIT) kvhits++;
            else if (o.type == ObsType::KV_MISS) kvmiss++;
            bytesh2d += o.u64_field(FieldKeys::BytesH2D);
            gpu += o.u64_field(FieldKeys::GpuMs) > 0 ? static_cast<u64>(o.f64_field(FieldKeys::GpuMs) * 1e6) : 0;
        }
        total_lat += at.total_latency;
    }
    agg.set("retry_events", retries);
    agg.set("kv_hits", kvhits);
    agg.set("kv_misses", kvmiss);
    agg.set("bytes_h2d", bytesh2d);
    agg.set("total_latency_ns", total_lat);
    agg.set("gpu_exec_ns", gpu);
    result.aggregate_digest = Sha256::hash(agg.to_canonical());
    return result;
}

Json ReplayResult::to_json() const {
    Json j = Json::object();
    j.set("obs_count", obs_count);
    j.set("request_trace_count", request_trace_count);
    j.set("evidence_digest", digest_hex(evidence_digest));
    j.set("trace_digest", digest_hex(trace_digest));
    j.set("explanation_digest", digest_hex(explanation_digest));
    j.set("aggregate_digest", digest_hex(aggregate_digest));
    j.set("tail", ungrouped_metrics.to_json());
    return j;
}

} // namespace servingobs
