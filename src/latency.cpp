// Serving Observatory — deterministic latency decomposition.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/trace/latency.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace servingobs {

namespace {
// Overlap-resolution priority (higher wins) so exclusive attribution is
// deterministic. Execution phases outrank queueing/transfer/overhead.
int phase_priority(Phase p) {
    switch (p) {
        case Phase::ARRIVAL: return 0;
        case Phase::ADMISSION: return 1;
        case Phase::QUEUE: return 2;
        case Phase::BATCH_WAIT: return 3;
        case Phase::DISPATCH: return 4;
        case Phase::TRANSFER: return 5;
        case Phase::SPECULATION: return 6;
        case Phase::PREFILL: return 7;
        case Phase::FIRST_TOKEN: return 8;
        case Phase::DECODE: return 9;
        case Phase::RETRY: return 10;
        case Phase::RECOVERY: return 11;
        case Phase::COMPLETION: return 12;
        case Phase::TOTAL: return -1;
    }
    return -1;
}
}

Json LatencyBreakdown::to_json() const {
    Json j = Json::object();
    j.set("total_ns", total_ns);
    j.set("covered_ns", covered_ns);
    j.set("gap_ns", gap_ns);
    Json arr = Json::array();
    for (const auto& c : components) {
        Json o = Json::object();
        o.set("phase", string(phase_name(c.phase)));
        o.set("duration_ns", c.duration_ns);
        o.set("exclusive_ns", c.exclusive_ns);
        o.set("percent_of_total", c.percent_of_total);
        o.set("on_critical_path", c.on_critical_path);
        o.set("provenance", string(provenance_name(c.provenance)));
        o.set("point_event", c.point_event);
        arr.push(std::move(o));
    }
    j.set("components", std::move(arr));
    return j;
}

LatencyBreakdown decompose_latency(const RequestTrace& t) {
    LatencyBreakdown b;
    const PhaseInterval& total = t.get_phase(Phase::TOTAL);
    if (!total.exists) return b;
    b.total_ns = total.end >= total.start ? total.end - total.start : 0;

    struct IV { Phase phase; TimestampNs s, e; Provenance prov; };
    std::vector<IV> ivs;
    for (size_t i = 0; i < t.phases.size(); ++i) {
        const auto& p = t.phases[i];
        if (!p.exists) continue;
        if (p.phase == Phase::TOTAL) continue;
        ivs.push_back({ p.phase, p.start, p.end, p.provenance });
    }

    // ---- exclusive sweep
    std::set<TimestampNs> coords;
    for (const auto& iv : ivs) { coords.insert(iv.s); coords.insert(iv.e); }
    // Build (component-index, exclusive_ns, duration_ns, point, prov).
    b.components.resize(ivs.size());
    for (size_t i = 0; i < ivs.size(); ++i) {
        auto& c = b.components[i];
        c.phase = ivs[i].phase;
        c.name = string(phase_name(ivs[i].phase));
        c.duration_ns = ivs[i].e >= ivs[i].s ? ivs[i].e - ivs[i].s : 0;
        c.point_event = c.duration_ns == 0;
        c.provenance = ivs[i].prov;
    }
    b.covered_ns = 0;
    if (coords.size() >= 2) {
        auto it = coords.begin();
        TimestampNs prev = *it;
        for (++it; it != coords.end(); ++it) {
            TimestampNs cur = *it;
            if (cur == prev) continue;
            TimestampNs mid = prev + (cur - prev) / 2;
            int best_pri = -1; size_t best = SIZE_MAX;
            for (size_t i = 0; i < ivs.size(); ++i) {
                if (ivs[i].s <= mid && mid < ivs[i].e) {
                    int pri = phase_priority(ivs[i].phase);
                    if (pri > best_pri || (pri == best_pri && ivs[i].phase < ivs[best].phase)) {
                        best_pri = pri; best = i;
                    }
                }
            }
            if (best != SIZE_MAX) {
                b.components[best].exclusive_ns += (cur - prev);
                b.covered_ns += (cur - prev);
            }
            prev = cur;
        }
    }
    b.gap_ns = b.total_ns >= b.covered_ns ? b.total_ns - b.covered_ns : 0;
    for (auto& c : b.components) {
        c.percent_of_total = b.total_ns ? (static_cast<double>(c.exclusive_ns) / static_cast<double>(b.total_ns)) * 100.0 : 0.0;
    }

    // ---- critical path (longest non-overlapping chain)
    if (!ivs.empty()) {
        std::vector<size_t> order(ivs.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (ivs[a].s != ivs[b].s) return ivs[a].s < ivs[b].s;
            if (ivs[a].e != ivs[b].e) return ivs[a].e < ivs[b].e;
            return a < b;
        });
        std::vector<u64> chain(ivs.size(), 0);
        std::vector<int> pred(ivs.size(), -1);
        u64 best = 0; int best_idx = -1;
        for (size_t oi = 0; oi < order.size(); ++oi) {
            size_t i = order[oi];
            u64 dur = ivs[i].e >= ivs[i].s ? ivs[i].e - ivs[i].s : 0;
            u64 bestprev = 0; int bestpred = -1;
            for (size_t oj = 0; oj < oi; ++oj) {
                size_t j = order[oj];
                if (ivs[j].e <= ivs[i].s) {
                    if (chain[j] > bestprev) { bestprev = chain[j]; bestpred = static_cast<int>(j); }
                }
            }
            chain[i] = bestprev + dur;
            pred[i] = bestpred;
            if (chain[i] > best) { best = chain[i]; best_idx = static_cast<int>(i); }
        }
        // Mark path via best_idx back to a root.
        if (best_idx >= 0) {
            int cur = best_idx;
            while (cur >= 0) { b.components[cur].on_critical_path = true; cur = pred[cur]; }
        }
    }
    return b;
}

string latency_text(const LatencyBreakdown& b, size_t indent) {
    string pad(static_cast<size_t>(indent) * 2, ' ');
    char buf[256];
    string out = pad + "total_ns: " + std::to_string(b.total_ns) + "  covered_ns: " +
                 std::to_string(b.covered_ns) + "  gap_ns: " + std::to_string(b.gap_ns) + "\n";
    for (const auto& c : b.components) {
        std::snprintf(buf, sizeof buf, "%-14s dur=%-12llu excl=%-12llu pct=%-8.3f crit=%d prov=%s\n",
                      c.name.c_str(),
                      static_cast<unsigned long long>(c.duration_ns),
                      static_cast<unsigned long long>(c.exclusive_ns),
                      c.percent_of_total,
                      c.on_critical_path ? 1 : 0,
                      string(provenance_name(c.provenance)).c_str());
        out += pad + string(buf);
    }
    return out;
}

} // namespace servingobs
