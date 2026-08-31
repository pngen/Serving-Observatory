
// Serving Observatory — comparison.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/replay/compare.hpp"
#include "servingobs/trace/analysis.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace servingobs {

namespace {
u64 phase_dur(const RequestTrace& t, Phase p) {
    const PhaseInterval& pi = t.get_phase(p);
    return pi.exists && pi.end >= pi.start ? pi.end - pi.start : 0;
}
u64 count_retry(const RequestTrace& t) { u64 c = 0; for (const auto& o : t.evidence) if (o.type == ObsType::RETRY) ++c; return c; }
u64 count_obs_of(const RequestTrace& t, ObsType ty) { u64 c = 0; for (const auto& o : t.evidence) if (o.type == ty) ++c; return c; }
bool any_cold(const RequestTrace& t) {
    for (const auto& o : t.evidence)
        if ((o.type == ObsType::MODEL_RESIDENCY || o.type == ObsType::ADAPTER_RESIDENCY) && !o.bool_field(FieldKeys::IsWarm, true)) return true;
    return false;
}
}

TraceComparison compare_traces(const RequestTrace& a, const RequestTrace& b) {
    TraceComparison c;
    c.request_a = a.request_id; c.request_b = b.request_id;
    c.total_a = phase_dur(a, Phase::TOTAL); c.total_b = phase_dur(b, Phase::TOTAL);
    c.delta_total = static_cast<i64>(c.total_b) - static_cast<i64>(c.total_a);
    const Phase phases[] = {Phase::QUEUE, Phase::BATCH_WAIT, Phase::DISPATCH, Phase::TRANSFER,
                            Phase::PREFILL, Phase::FIRST_TOKEN, Phase::DECODE, Phase::SPECULATION,
                            Phase::RETRY, Phase::RECOVERY};
    for (auto p : phases) {
        i64 da = static_cast<i64>(phase_dur(a, p));
        i64 db = static_cast<i64>(phase_dur(b, p));
        if (da != 0 || db != 0) c.phase_deltas.push_back({ string(phase_name(p)), db - da });
    }
    c.retries_a = count_retry(a); c.retries_b = count_retry(b);
    c.kv_hits_a = count_obs_of(a, ObsType::KV_HIT); c.kv_hits_b = count_obs_of(b, ObsType::KV_HIT);
    c.kernel_hits_a = count_obs_of(a, ObsType::KERNEL_HIT); c.kernel_hits_b = count_obs_of(b, ObsType::KERNEL_HIT);
    c.spec_accepted_a = count_obs_of(a, ObsType::SPECULATION_VERIFY); c.spec_accepted_b = count_obs_of(b, ObsType::SPECULATION_VERIFY);
    for (const auto& o : a.evidence) c.bytes_h2d_a += o.u64_field(FieldKeys::BytesH2D);
    for (const auto& o : b.evidence) c.bytes_h2d_b += o.u64_field(FieldKeys::BytesH2D);
    c.worker_restart_a = a.boots.size() > 1; c.worker_restart_b = b.boots.size() > 1;
    c.cold_model_a = any_cold(a); c.cold_model_b = any_cold(b);
    return c;
}

Json TraceComparison::to_json() const {
    Json j = Json::object();
    j.set("request_a", request_a.to_hex()); j.set("request_b", request_b.to_hex());
    j.set("total_a_ns", total_a); j.set("total_b_ns", total_b); j.set("delta_total_ns", delta_total);
    Json pd = Json::object();
    for (const auto& [k, v] : phase_deltas) pd.set(k, v);
    j.set("phase_deltas_ns", std::move(pd));
    j.set("retries_a", retries_a); j.set("retries_b", retries_b);
    j.set("kv_hits_a", kv_hits_a); j.set("kv_hits_b", kv_hits_b);
    j.set("kernel_hits_a", kernel_hits_a); j.set("kernel_hits_b", kernel_hits_b);
    j.set("spec_accepted_a", spec_accepted_a); j.set("spec_accepted_b", spec_accepted_b);
    j.set("bytes_h2d_a", bytes_h2d_a); j.set("bytes_h2d_b", bytes_h2d_b);
    j.set("worker_restart_a", worker_restart_a); j.set("worker_restart_b", worker_restart_b);
    j.set("cold_model_a", cold_model_a); j.set("cold_model_b", cold_model_b);
    return j;
}

string comparison_text(const TraceComparison& c) {
    char buf[200];
    std::snprintf(buf, sizeof buf,
        "compare %s vs %s: total %llu -> %llu ns (delta %lld ns)\n",
        c.request_a.to_hex().c_str(), c.request_b.to_hex().c_str(),
        static_cast<unsigned long long>(c.total_a), static_cast<unsigned long long>(c.total_b),
        static_cast<long long>(c.delta_total));
    string out = buf;
    for (const auto& [k, v] : c.phase_deltas) {
        std::snprintf(buf, sizeof buf, "  phase %-14s delta_ns=%lld\n", k.c_str(), static_cast<long long>(v));
        out += buf;
    }
    return out;
}

WindowComparison compare_windows(const std::vector<RequestTrace>& a, const std::vector<RequestTrace>& b,
                                 const string& label_a, const string& label_b) {
    WindowComparison w;
    w.label_a = label_a; w.label_b = label_b;
    std::vector<u64> la, lb;
    for (const auto& t : a) {
        const PhaseInterval& tot = t.get_phase(Phase::TOTAL);
        la.push_back(tot.exists ? tot.end - tot.start : 0); w.total_latency_a += la.back();
        for (const auto& o : t.evidence) {
            if (o.type == ObsType::RETRY) w.retries_a++;
            else if (o.type == ObsType::WORKER_RESTART || o.type == ObsType::WORKER_DOWN) w.restarts_a++;
            else if (o.type == ObsType::KV_HIT) w.kv_hits_a++;
        }
    }
    for (const auto& t : b) {
        const PhaseInterval& tot = t.get_phase(Phase::TOTAL);
        lb.push_back(tot.exists ? tot.end - tot.start : 0); w.total_latency_b += lb.back();
        for (const auto& o : t.evidence) {
            if (o.type == ObsType::RETRY) w.retries_b++;
            else if (o.type == ObsType::WORKER_RESTART || o.type == ObsType::WORKER_DOWN) w.restarts_b++;
            else if (o.type == ObsType::KV_HIT) w.kv_hits_b++;
        }
    }
    w.count_a = a.size(); w.count_b = b.size();
    w.p90_a = percentile_of(la, 90.0); w.p90_b = percentile_of(lb, 90.0);
    return w;
}

Json WindowComparison::to_json() const {
    Json j = Json::object();
    j.set("label_a", label_a); j.set("label_b", label_b);
    j.set("count_a", count_a); j.set("count_b", count_b);
    j.set("total_latency_a_ns", total_latency_a); j.set("total_latency_b_ns", total_latency_b);
    j.set("p90_a_ns", p90_a); j.set("p90_b_ns", p90_b);
    j.set("retries_a", retries_a); j.set("retries_b", retries_b);
    j.set("restarts_a", restarts_a); j.set("restarts_b", restarts_b);
    j.set("kv_hits_a", kv_hits_a); j.set("kv_hits_b", kv_hits_b);
    return j;
}

} // namespace servingobs
