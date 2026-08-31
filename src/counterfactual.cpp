
// Serving Observatory — bounded counterfactual comparison.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/replay/counterfactual.hpp"

#include <cstdio>
#include <string>

namespace servingobs {

string_view counterfactual_rule_name(CounterfactualRule r) {
    switch (r) {
        case CounterfactualRule::REMOVE_QUEUE_WAIT: return "remove_queue_wait";
        case CounterfactualRule::REMOVE_RETRY: return "remove_retry";
        case CounterfactualRule::WARM_RESIDENCY: return "warm_residency";
        case CounterfactualRule::REDUCE_TRANSFER_BY_HALF: return "reduce_transfer_by_half";
    }
    return "unknown";
}

CounterfactualResult counterfactual(const RequestTrace& t, CounterfactualRule rule) {
    CounterfactualResult r;
    r.request_id = t.request_id;
    r.rule = rule;
    const PhaseInterval& tot = t.get_phase(Phase::TOTAL);
    r.original_total = tot.exists ? (tot.end - tot.start) : 0;
    r.counterfactual_total = r.original_total;

    switch (rule) {
        case CounterfactualRule::REMOVE_QUEUE_WAIT: {
            const PhaseInterval& q = t.get_phase(Phase::QUEUE);
            if (q.exists) {
                u64 dur = q.end >= q.start ? q.end - q.start : 0;
                r.changed_inputs.push_back("queue duration " + std::to_string(dur) + " ns removed");
                r.counterfactual_total = r.original_total >= dur ? r.original_total - dur : 0;
                r.derived_differences.push_back("total latency reduced by queue residence");
            }
            break;
        }
        case CounterfactualRule::REMOVE_RETRY: {
            const PhaseInterval& rt = t.get_phase(Phase::RETRY);
            if (rt.exists) {
                u64 dur = rt.end >= rt.start ? rt.end - rt.start : 0;
                r.changed_inputs.push_back("retry interval " + std::to_string(dur) + " ns removed");
                r.counterfactual_total = r.original_total >= dur ? r.original_total - dur : 0;
                r.derived_differences.push_back("retry time removed from total");
            } else {
                r.changed_inputs.push_back("no measurable retry interval; retry retained");
                r.derived_differences.push_back("no delta (missing retry duration)");
            }
            break;
        }
        case CounterfactualRule::WARM_RESIDENCY: {
            bool cold = false;
            for (const auto& o : t.evidence) {
                if (o.type == ObsType::MODEL_RESIDENCY && !o.bool_field(FieldKeys::IsWarm, true)) cold = true;
                if (o.type == ObsType::ADAPTER_RESIDENCY && !o.bool_field(FieldKeys::IsWarm, true)) cold = true;
            }
            const PhaseInterval& pf = t.get_phase(Phase::PREFILL);
            if (cold && pf.exists) {
                u64 prefill = pf.end >= pf.start ? pf.end - pf.start : 0;
                u64 saved = static_cast<u64>(static_cast<double>(prefill) * 0.7);
                r.changed_inputs.push_back("cold residency replaced with warm residency (stated model: 70% prefill saving)");
                r.counterfactual_total = r.original_total >= saved ? r.original_total - saved : 0;
                r.derived_differences.push_back("prefill reduced by estimated " + std::to_string(saved) + " ns");
                r.provenance = Provenance::ESTIMATED;
                r.cls = ExplanationClass::PLAUSIBLE_BUT_UNPROVEN;
            } else {
                r.changed_inputs.push_back("residency already warm (or unknown); no counterfactual applied");
                r.derived_differences.push_back("no delta");
            }
            break;
        }
        case CounterfactualRule::REDUCE_TRANSFER_BY_HALF: {
            const PhaseInterval& tr = t.get_phase(Phase::TRANSFER);
            if (tr.exists) {
                u64 dur = tr.end >= tr.start ? tr.end - tr.start : 0;
                u64 saved = dur / 2;
                r.changed_inputs.push_back("transfer duration halved (stated transform)");
                r.counterfactual_total = r.original_total >= saved ? r.original_total - saved : 0;
                r.derived_differences.push_back("transfer reduced by " + std::to_string(saved) + " ns");
            } else {
                r.derived_differences.push_back("no transfer evidence; no delta");
            }
            break;
        }
    }
    r.delta_ns = static_cast<i64>(r.counterfactual_total) - static_cast<i64>(r.original_total);
    r.delta_percent = r.original_total ? (static_cast<double>(r.delta_ns) / static_cast<double>(r.original_total)) * 100.0 : 0.0;
    return r;
}

Json CounterfactualResult::to_json() const {
    Json j = Json::object();
    j.set("request_id", request_id.to_hex());
    j.set("rule", string(counterfactual_rule_name(rule)));
    j.set("original_total_ns", original_total);
    j.set("counterfactual_total_ns", counterfactual_total);
    j.set("delta_ns", delta_ns);
    j.set("delta_percent", delta_percent);
    j.set("provenance", string(provenance_name(provenance)));
    j.set("class", string(explanation_class_name(cls)));
    Json ci = Json::array();
    for (const auto& s : changed_inputs) ci.push(s);
    j.set("changed_inputs", std::move(ci));
    Json dd = Json::array();
    for (const auto& s : derived_differences) dd.push(s);
    j.set("derived_differences", std::move(dd));
    return j;
}

string counterfactual_text(const CounterfactualResult& r) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "counterfactual rule=%s original_ns=%llu cf_ns=%llu delta_ns=%lld (%.2f%%)\n",
                  string(counterfactual_rule_name(r.rule)).c_str(),
                  static_cast<unsigned long long>(r.original_total),
                  static_cast<unsigned long long>(r.counterfactual_total),
                  static_cast<long long>(r.delta_ns), r.delta_percent);
    string out = buf;
    for (const auto& s : r.changed_inputs) out += "  changed: " + s + "\n";
    for (const auto& s : r.derived_differences) out += "  derived: " + s + "\n";
    out += "  provenance=" + string(provenance_name(r.provenance)) + " class=" + string(explanation_class_name(r.cls)) + "\n";
    return out;
}

} // namespace servingobs