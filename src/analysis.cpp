// Serving Observatory — tail, correlation, causality, explanation.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/trace/analysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace servingobs {

// ------------------------------------------------------------ percentile
u64 percentile_of(const std::vector<u64>& values, double p) {
    if (values.empty()) return 0;
    std::vector<u64> v = values;
    std::sort(v.begin(), v.end());
    // Nearest-rank percentile: the smallest value at or above p% of the set.
    double rank = (p / 100.0) * static_cast<double>(v.size());
    size_t idx = static_cast<size_t>(std::ceil(rank));
    if (idx < 1) idx = 1;
    if (idx > v.size()) idx = v.size();
    return v[idx - 1];
}

TailMetrics tail_metrics(const std::vector<u64>& latencies) {
    TailMetrics m;
    m.count = latencies.size();
    if (latencies.empty()) return m;
    m.min_ns = *std::min_element(latencies.begin(), latencies.end());
    m.max_ns = *std::max_element(latencies.begin(), latencies.end());
    m.p50_ns = percentile_of(latencies, 50.0);
    m.p90_ns = percentile_of(latencies, 90.0);
    m.p95_ns = percentile_of(latencies, 95.0);
    m.p99_ns = percentile_of(latencies, 99.0);
    m.p999_ns = percentile_of(latencies, 99.9);
    double sum = 0.0;
    for (auto v : latencies) sum += static_cast<double>(v);
    m.mean_ns = sum / static_cast<double>(latencies.size());
    double ss = 0.0;
    for (auto v : latencies) { double d = static_cast<double>(v) - m.mean_ns; ss += d * d; }
    m.stddev_ns = std::sqrt(ss / static_cast<double>(latencies.size()));
    return m;
}

Json TailMetrics::to_json() const {
    Json j = Json::object();
    j.set("count", count);
    j.set("min_ns", min_ns); j.set("max_ns", max_ns);
    j.set("p50_ns", p50_ns); j.set("p90_ns", p90_ns); j.set("p95_ns", p95_ns);
    j.set("p99_ns", p99_ns); j.set("p999_ns", p999_ns);
    j.set("mean_ns", mean_ns); j.set("stddev_ns", stddev_ns);
    return j;
}

// ------------------------------------------------------------ explanation
Explanation explain_request(const RequestTrace& t) {
    Explanation e;
    e.request_id = t.request_id;
    e.breakdown = decompose_latency(t);
    e.attribution = attribute_resources(t);

    const double total = e.breakdown.total_ns ? static_cast<double>(e.breakdown.total_ns) : 1.0;
    auto add = [&](string claim, ExplanationClass cls, Provenance prov, string ev, string ph, double sev) {
        e.statements.push_back({ std::move(claim), cls, prov, std::move(ev), std::move(ph), sev });
    };

    // Dominating phase.
    for (const auto& c : e.breakdown.components) {
        if (c.duration_ns == 0 && c.exclusive_ns == 0) continue;
        double frac = static_cast<double>(c.exclusive_ns) / total;
        ExplanationClass cls = c.on_critical_path ? ExplanationClass::STRUCTURALLY_DERIVED : ExplanationClass::TEMPORALLY_CORRELATED;
        add("phase " + c.name + " accounts for " +
            std::to_string(static_cast<int>(frac * 100.0)) + "% of latency",
            cls, c.provenance, "exclusive_ns=" + std::to_string(c.exclusive_ns), c.name, frac);
    }
    // Direct evidence: queue length / retries / restarts / cold caches.
    if (t.boots.size() > 1) {
        add("request spans " + std::to_string(t.boots.size()) + " worker boots (worker restart/failover)",
            ExplanationClass::DIRECTLY_EVIDENCED, Provenance::MEASURED,
            "boots=" + std::to_string(t.boots.size()) + " epochs=" + std::to_string(t.min_epoch) + ".." + std::to_string(t.max_epoch),
            "recovery", 0.0);
    }
    if (t.spans_multiple_epochs) {
        add("request spans multiple coordinator epochs (epoch " + std::to_string(t.min_epoch) + ".." + std::to_string(t.max_epoch) + ")",
            ExplanationClass::DIRECTLY_EVIDENCED, Provenance::MEASURED, "", "total", 0.0);
    }
    for (const auto& o : t.evidence) {
        if (o.type == ObsType::RETRY) {
            add("a retry occurred (" + o.str_field(FieldKeys::Reason) + ")", ExplanationClass::DIRECTLY_EVIDENCED,
                o.provenance, "retry " + o.id.to_hex(), "retry", 0.0);
        } else if (o.type == ObsType::FAILURE) {
            add("a failure occurred (" + o.str_field(FieldKeys::Reason) + ")", ExplanationClass::DIRECTLY_EVIDENCED,
                o.provenance, "failure " + o.id.to_hex(), "retry", 0.0);
        } else if (o.type == ObsType::WORKER_RESTART) {
            add("a worker restart occurred", ExplanationClass::DIRECTLY_EVIDENCED, o.provenance,
                "boot=" + o.boot.to_hex(), "recovery", 0.0);
        } else if (o.type == ObsType::MODEL_RESIDENCY && o.bool_field(FieldKeys::IsWarm, true) == false) {
            add("model was cold (not resident)", ExplanationClass::DIRECTLY_EVIDENCED, o.provenance,
                "model=" + o.id_field(FieldKeys::ModelId).to_hex(), "prefill", 0.0);
        } else if (o.type == ObsType::ADAPTER_RESIDENCY && o.bool_field(FieldKeys::IsWarm, true) == false) {
            add("adapter was cold (not resident)", ExplanationClass::DIRECTLY_EVIDENCED, o.provenance,
                "adapter=" + o.id_field(FieldKeys::AdapterId).to_hex(), "prefill", 0.0);
        }
    }
    // KV reuse.
    if (e.attribution.kv_lookups > 0) {
        double hit_ratio = static_cast<double>(e.attribution.kv_hits) / static_cast<double>(e.attribution.kv_lookups);
        if (hit_ratio < 0.5) {
            add("low KV reuse (hit ratio " + std::to_string(static_cast<int>(hit_ratio * 100.0)) + "%)",
                ExplanationClass::STRUCTURALLY_DERIVED, Provenance::DERIVED,
                "kv_hits=" + std::to_string(e.attribution.kv_hits) + " kv_lookups=" + std::to_string(e.attribution.kv_lookups),
                "prefill", 0.0);
        }
    }
    // Queueing fraction.
    if (e.attribution.has("queue") && e.attribution.queue_time > 0) {
        double qfrac = static_cast<double>(e.attribution.queue_time) / total;
        add("queueing contributed " + std::to_string(static_cast<int>(qfrac * 100.0)) + "%",
            ExplanationClass::STRUCTURALLY_DERIVED, e.attribution.prov("queue"), "queue_ns=" + std::to_string(e.attribution.queue_time),
            "queue", qfrac);
    }
    if (e.breakdown.gap_ns > 0) {
        double gfrac = static_cast<double>(e.breakdown.gap_ns) / total;
        add("unattributed latency " + std::to_string(static_cast<int>(gfrac * 100.0)) + "%",
            ExplanationClass::TEMPORALLY_CORRELATED, Provenance::UNKNOWN, "gap_ns=" + std::to_string(e.breakdown.gap_ns), "total", gfrac);
    }
    // Terminal outcome.
    if (t.outcome != Outcome::UNKNOWN) {
        add("terminal outcome " + string(outcome_name(t.outcome)), ExplanationClass::DIRECTLY_EVIDENCED,
            Provenance::MEASURED, t.terminal_reason, "completion", 0.0);
    }
    return e;
}

Json Explanation::to_json() const {
    Json j = Json::object();
    j.set("request_id", request_id.to_hex());
    j.set("latency", breakdown.to_json());
    j.set("attribution", attribution.to_json());
    Json st = Json::array();
    for (const auto& s : statements) {
        Json o = Json::object();
        o.set("claim", s.claim);
        o.set("class", string(explanation_class_name(s.cls)));
        o.set("provenance", string(provenance_name(s.provenance)));
        o.set("evidence", s.evidence);
        o.set("phase", s.phase);
        o.set("severity", s.severity);
        st.push(std::move(o));
    }
    j.set("statements", std::move(st));
    return j;
}

string explanation_text(const Explanation& e) {
    string out = "explanation for request " + e.request_id.to_hex() + "\n";
    for (const auto& s : e.statements) {
        out += "  [" + string(explanation_class_name(s.cls)) + "/" + string(provenance_name(s.provenance)) + "] " +
               s.claim + (s.evidence.empty() ? "" : "  (" + s.evidence + ")") + "\n";
    }
    return out;
}

// ------------------------------------------------------------ tail explanation
TailExplanation explain_one_tail(const RequestTrace& t, const TailMetrics& metrics) {
    TailExplanation te;
    te.request_id = t.request_id;
    te.metrics = metrics;
    const PhaseInterval& total = t.get_phase(Phase::TOTAL);
    te.latency_ns = total.exists ? (total.end - total.start) : 0;
    LatencyBreakdown bd = decompose_latency(t);
    double total_d = bd.total_ns ? static_cast<double>(bd.total_ns) : 1.0;
    for (const auto& c : bd.components) {
        // Tail-relevant factors only.
        if (c.name == "arrival" || c.name == "completion" || c.name == "admission") continue;
        double frac = static_cast<double>(c.exclusive_ns) / total_d;
        if (frac < 0.05) continue; // only material factors
        TailFactor f;
        f.name = c.name;
        f.severity = frac;
        f.cls = c.on_critical_path ? ExplanationClass::STRUCTURALLY_DERIVED : ExplanationClass::TEMPORALLY_CORRELATED;
        f.note = "exclusive_ns=" + std::to_string(c.exclusive_ns) + " prov=" + string(provenance_name(c.provenance));
        te.factors.push_back(std::move(f));
    }
    if (t.boots.size() > 1) {
        TailFactor f; f.name = "worker_restart"; f.severity = 0.0;
        f.cls = ExplanationClass::DIRECTLY_EVIDENCED; f.note = "boots=" + std::to_string(t.boots.size());
        te.factors.push_back(std::move(f));
    }
    for (const auto& o : t.evidence) {
        if (o.type == ObsType::RETRY) {
            TailFactor f; f.name = "retry"; f.severity = 0.0;
            f.cls = ExplanationClass::DIRECTLY_EVIDENCED; f.note = o.str_field(FieldKeys::Reason);
            te.factors.push_back(std::move(f));
        } else if (o.type == ObsType::MODEL_RESIDENCY && !o.bool_field(FieldKeys::IsWarm, true)) {
            TailFactor f; f.name = "cold_model"; f.severity = 0.0;
            f.cls = ExplanationClass::DIRECTLY_EVIDENCED; f.note = "cold";
            te.factors.push_back(std::move(f));
        }
    }
    return te;
}

std::vector<TailExplanation> explain_tail(const std::vector<RequestTrace>& traces, double percentile) {
    std::vector<u64> lats;
    for (const auto& t : traces) {
        const PhaseInterval& tot = t.get_phase(Phase::TOTAL);
        lats.push_back(tot.exists ? tot.end - tot.start : 0);
    }
    TailMetrics m = tail_metrics(lats);
    u64 threshold = percentile_of(lats, percentile);
    std::vector<TailExplanation> out;
    std::vector<RequestTrace> ordered = traces;
    // sort by latency desc
    std::sort(ordered.begin(), ordered.end(), [](const RequestTrace& a, const RequestTrace& b) {
        const PhaseInterval& x = a.get_phase(Phase::TOTAL);
        const PhaseInterval& y = b.get_phase(Phase::TOTAL);
        u64 la = x.exists ? x.end - x.start : 0;
        u64 lb = y.exists ? y.end - y.start : 0;
        return la > lb;
    });
    for (const auto& t : ordered) {
        const PhaseInterval& tot = t.get_phase(Phase::TOTAL);
        u64 l = tot.exists ? tot.end - tot.start : 0;
        if (l >= threshold) out.push_back(explain_one_tail(t, m));
    }
    return out;
}

Json TailExplanation::to_json() const {
    Json j = Json::object();
    j.set("request_id", request_id.to_hex());
    j.set("latency_ns", latency_ns);
    j.set("metrics", metrics.to_json());
    Json fs = Json::array();
    for (const auto& f : factors) {
        Json o = Json::object();
        o.set("factor", f.name);
        o.set("severity", f.severity);
        o.set("class", string(explanation_class_name(f.cls)));
        o.set("note", f.note);
        fs.push(std::move(o));
    }
    j.set("factors", std::move(fs));
    return j;
}

string tail_text(const TailExplanation& e) {
    string out = "tail request " + e.request_id.to_hex() + " latency_ns=" + std::to_string(e.latency_ns) + "\n";
    for (const auto& f : e.factors) {
        char buf[128]; std::snprintf(buf, sizeof buf, "  factor=%-16s sev=%-8.3f class=%s\n",
                                     f.name.c_str(), f.severity, string(explanation_class_name(f.cls)).c_str());
        out += buf;
        if (!f.note.empty()) out += "      note: " + f.note + "\n";
    }
    return out;
}

// ------------------------------------------------------------ correlation
Json GroupSummary::to_json() const {
    Json j = Json::object();
    j.set("key", key);
    j.set("count", count);
    j.set("total_latency_ns", total_latency_ns);
    j.set("mean_latency_ns", mean_latency_ns);
    j.set("p50_ns", p50_ns); j.set("p90_ns", p90_ns); j.set("p99_ns", p99_ns);
    j.set("total_retries", total_retries);
    j.set("worker_restart_exposures", worker_restart_exposures);
    j.set("kv_hits", kv_hits); j.set("kv_misses", kv_misses);
    j.set("kernel_hits", kernel_hits); j.set("kernel_misses", kernel_misses);
    return j;
}

std::vector<GroupSummary> correlate_by(const std::vector<RequestTrace>& traces,
                                       const std::function<string(const RequestTrace&)>& key) {
    // First pass: accumulate per key.
    struct Acc { u64 count=0, total=0, retries=0, restarts=0, kvh=0, kvm=0, kh=0, km=0; std::vector<u64> lats; };
    std::map<string, Acc> acc;
    for (const auto& t : traces) {
        const PhaseInterval& tot = t.get_phase(Phase::TOTAL);
        u64 l = tot.exists ? tot.end - tot.start : 0;
        string k = key(t);
        Acc& a = acc[k];
        a.count++; a.total += l; a.lats.push_back(l);
        for (const auto& o : t.evidence) {
            if (o.type == ObsType::RETRY) a.retries++;
            else if (o.type == ObsType::WORKER_RESTART || o.type == ObsType::WORKER_DOWN) a.restarts++;
            else if (o.type == ObsType::KV_HIT) a.kvh++;
            else if (o.type == ObsType::KV_MISS) a.kvm++;
            else if (o.type == ObsType::KERNEL_HIT) a.kh++;
            else if (o.type == ObsType::KERNEL_MISS) a.km++;
        }
    }
    std::vector<GroupSummary> out;
    for (auto& [k, a] : acc) {
        GroupSummary g;
        g.key = k; g.count = a.count; g.total_latency_ns = a.total;
        g.mean_latency_ns = a.count ? static_cast<double>(a.total) / static_cast<double>(a.count) : 0.0;
        g.p50_ns = percentile_of(a.lats, 50.0);
        g.p90_ns = percentile_of(a.lats, 90.0);
        g.p99_ns = percentile_of(a.lats, 99.0);
        g.total_retries = a.retries;
        g.worker_restart_exposures = a.restarts;
        g.kv_hits = a.kvh; g.kv_misses = a.kvm;
        g.kernel_hits = a.kh; g.kernel_misses = a.km;
        out.push_back(std::move(g));
    }
    // Deterministic order by key.
    std::sort(out.begin(), out.end(), [](const GroupSummary& x, const GroupSummary& y) { return x.key < y.key; });
    return out;
}

SharedFactorResult which_share(const std::vector<RequestTrace>& traces,
                               const std::function<bool(const RequestTrace&)>& pred,
                               string factor_name) {
    SharedFactorResult r;
    r.factor = std::move(factor_name);
    for (const auto& t : traces) {
        if (pred(t)) {
            const PhaseInterval& tot = t.get_phase(Phase::TOTAL);
            u64 l = tot.exists ? tot.end - tot.start : 0;
            r.requests.push_back({ t.request_id, l });
        }
    }
    std::sort(r.requests.begin(), r.requests.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
    return r;
}

Json SharedFactorResult::to_json() const {
    Json j = Json::object();
    j.set("factor", factor);
    Json arr = Json::array();
    for (const auto& [id, l] : requests) {
        Json o = Json::object();
        o.set("request_id", id.to_hex()); o.set("latency_ns", l);
        arr.push(std::move(o));
    }
    j.set("requests", std::move(arr));
    return j;
}

} // namespace servingobs
