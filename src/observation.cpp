// Serving Observatory — observation codec and accessors.
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/model/observation.hpp"

#include <limits>
#include <string>

namespace servingobs {

// ---------------------------------------------------------------- accessors
u64 Observation::u64_field(string_view k, u64 def) const {
    auto it = fields.find(string(k));
    if (it == fields.end()) return def;
    if (const auto* v = std::get_if<u64>(&it->second)) return *v;
    if (const auto* v = std::get_if<i64>(&it->second)) return static_cast<u64>(*v);
    return def;
}
i64 Observation::i64_field(string_view k, i64 def) const {
    auto it = fields.find(string(k));
    if (it == fields.end()) return def;
    if (const auto* v = std::get_if<i64>(&it->second)) return *v;
    if (const auto* v = std::get_if<u64>(&it->second)) return static_cast<i64>(*v);
    return def;
}
f64 Observation::f64_field(string_view k, f64 def) const {
    auto it = fields.find(string(k));
    if (it == fields.end()) return def;
    if (const auto* v = std::get_if<f64>(&it->second)) return *v;
    if (const auto* v = std::get_if<i64>(&it->second)) return static_cast<f64>(*v);
    if (const auto* v = std::get_if<u64>(&it->second)) return static_cast<f64>(*v);
    return def;
}
bool Observation::bool_field(string_view k, bool def) const {
    auto it = fields.find(string(k));
    if (it == fields.end()) return def;
    if (const auto* v = std::get_if<bool>(&it->second)) return *v;
    return def;
}
string Observation::str_field(string_view k, string def) const {
    auto it = fields.find(string(k));
    if (it == fields.end()) return def;
    if (const auto* v = std::get_if<string>(&it->second)) return *v;
    return def;
}
Id128 Observation::id_field(string_view k) const {
    auto it = fields.find(string(k));
    if (it == fields.end()) return Id128();
    if (const auto* v = std::get_if<Id128>(&it->second)) return *v;
    return Id128();
}

// ---------------------------------------------------------------- value codec
namespace {
void write_fv(BinaryWriter& w, const FieldValue& v) {
    std::visit([&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, NullVal>) { w.write_u8(0); }
        else if constexpr (std::is_same_v<T, bool>) { w.write_u8(1); w.write_bool(x); }
        else if constexpr (std::is_same_v<T, i64>) { w.write_u8(2); w.write_i64(x); }
        else if constexpr (std::is_same_v<T, u64>) { w.write_u8(3); w.write_u64(x); }
        else if constexpr (std::is_same_v<T, f64>) { w.write_u8(4); w.write_f64(x); }
        else if constexpr (std::is_same_v<T, string>) { w.write_u8(5); w.write_str(x); }
        else if constexpr (std::is_same_v<T, Id128>) { w.write_u8(6); w.write_id(x); }
        else if constexpr (std::is_same_v<T, bytes>) { w.write_u8(7); w.write_bytes(x); }
    }, v);
}

FieldValue read_fv(BinaryReader& r) {
    u8 tag = r.read_u8();
    if (!r.ok()) return NullVal{};
    switch (tag) {
        case 0: return NullVal{};
        case 1: return r.read_bool();
        case 2: return r.read_i64();
        case 3: return r.read_u64();
        case 4: return r.read_f64();
        case 5: return r.read_str();
        case 6: return r.read_id();
        case 7: return r.read_bytes();
        default:
            r.fail("invalid field-value tag " + std::to_string(tag));
            return NullVal{};
    }
}
}

void write_observation(BinaryWriter& w, const Observation& o) {
    w.write_id(o.id.raw());
    w.write_u16(static_cast<u16>(o.type));
    w.write_id(o.source.raw());
    w.write_id(o.worker.raw());
    w.write_id(o.boot.raw());
    w.write_u64(o.src_gen);
    w.write_u64(o.obs_gen);
    w.write_u64(o.epoch);
    w.write_u64(o.seq);
    w.write_str(o.clock_domain);
    u8 pres = 0;
    if (o.has_obs_ts) pres |= 1u << 0;
    if (o.has_recv_ts) pres |= 1u << 1;
    if (o.has_health) pres |= 1u << 2;
    if (o.has_confidence) pres |= 1u << 3;
    w.write_u8(pres);
    if (o.has_obs_ts) w.write_u64(o.obs_ts);
    if (o.has_recv_ts) w.write_u64(o.recv_ts);
    if (o.has_health) w.write_u8(static_cast<u8>(o.health));
    if (o.has_confidence) w.write_f32(o.confidence);
    w.write_u8(static_cast<u8>(o.provenance));
    w.write_count(o.fields.size());
    for (const auto& [k, v] : o.fields) {  // std::map => sorted keys
        w.write_str(k);
        write_fv(w, v);
    }
}

Result<Observation> read_observation(BinaryReader& r) {
    Observation o;
    o.id = ObservationId(r.read_id());
    u16 t = r.read_u16();
    if (t >= static_cast<u16>(ObsType::UNKNOWN) && t <= static_cast<u16>(ObsType::CAPACITY_STATE)) {
        o.type = static_cast<ObsType>(t);
    } else {
        return Result<Observation>::Err("invalid observation type " + std::to_string(t));
    }
    o.source = SourceId(r.read_id());
    o.worker = WorkerId(r.read_id());
    o.boot = WorkerBootId(r.read_id());
    o.src_gen = r.read_u64();
    o.obs_gen = r.read_u64();
    o.epoch = r.read_u64();
    o.seq = r.read_u64();
    o.clock_domain = r.read_str();
    u8 pres = r.read_u8();
    if (pres & (1u << 0)) { o.has_obs_ts = true; o.obs_ts = r.read_u64(); }
    if (pres & (1u << 1)) { o.has_recv_ts = true; o.recv_ts = r.read_u64(); }
    if (pres & (1u << 2)) {
        o.has_health = true;
        u8 h = r.read_u8();
        if (h <= static_cast<u8>(SourceHealth::REJECTED)) o.health = static_cast<SourceHealth>(h);
        else o.health = SourceHealth::UNKNOWN;
    }
    if (pres & (1u << 3)) { o.has_confidence = true; o.confidence = r.read_f32(); }
    u8 prov = r.read_u8();
    if (prov <= static_cast<u8>(Provenance::HEURISTIC)) o.provenance = static_cast<Provenance>(prov);
    u64 fcount = r.read_count();
    if (!r.ok()) return Result<Observation>::Err("corrupt observation header: " + r.error());
    if (fcount > 16777216u) return Result<Observation>::Err("field count overflow");
    for (u64 i = 0; i < fcount; ++i) {
        string key = r.read_str();
        if (!r.ok()) return Result<Observation>::Err("corrupt field key: " + r.error());
        FieldValue v = read_fv(r);
        if (!r.ok()) return Result<Observation>::Err("corrupt field value for '" + key + "': " + r.error());
        o.fields.emplace(std::move(key), std::move(v));
    }
    return Result<Observation>::Ok(std::move(o));
}

Digest observation_digest(const Observation& o) {
    BinaryWriter w;
    write_observation(w, o);
    return Sha256::hash(w.data());
}

bytes encode_observation_payload(const Observation& o) {
    BinaryWriter w;
    write_observation(w, o);
    return w.data();
}
Result<Observation> read_observation_payload(const bytes& b) {
    BinaryReader r(b);
    auto res = read_observation(r);
    if (!res.ok()) return res;
    if (!r.require_end()) return Result<Observation>::Err("observation payload trailing garbage");
    return res;
}

string field_value_to_string(const FieldValue& v) {
    return std::visit([](const auto& x) -> string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, NullVal>) return "null";
        else if constexpr (std::is_same_v<T, bool>) return x ? "true" : "false";
        else if constexpr (std::is_same_v<T, i64>) return std::to_string(x);
        else if constexpr (std::is_same_v<T, u64>) return std::to_string(x);
        else if constexpr (std::is_same_v<T, f64>) { char b[64]; std::snprintf(b, sizeof b, "%.17g", x); return b; }
        else if constexpr (std::is_same_v<T, string>) return x;
        else if constexpr (std::is_same_v<T, Id128>) return x.to_hex();
        else if constexpr (std::is_same_v<T, bytes>) { return "bytes[" + std::to_string(x.size()) + "]"; }
    }, v);
}

Json observation_to_json(const Observation& o) {
    Json j = Json::object();
    j.set("kind", string(obs_type_name(o.type)));
    j.set("type", static_cast<u64>(o.type));
    j.set("id", o.id.to_hex());
    j.set("source", o.source.to_hex());
    j.set("worker", o.worker.to_hex());
    j.set("boot", o.boot.to_hex());
    j.set("src_gen", o.src_gen);
    j.set("obs_gen", o.obs_gen);
    j.set("epoch", o.epoch);
    j.set("seq", o.seq);
    j.set("clock_domain", o.clock_domain);
    if (o.has_obs_ts) j.set("obs_ts", o.obs_ts);
    if (o.has_recv_ts) j.set("recv_ts", o.recv_ts);
    j.set("provenance", string(provenance_name(o.provenance)));
    if (o.has_health) j.set("health", string(source_health_name(o.health)));
    if (o.has_confidence) j.set("confidence", static_cast<f64>(o.confidence));
    if (!o.fields.empty()) {
        Json fj = Json::object();
        for (const auto& [k, v] : o.fields) {
            std::visit([&](const auto& x) {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, NullVal>) fj.set(k, nullptr);
                else if constexpr (std::is_same_v<T, bool>) fj.set(k, x);
                else if constexpr (std::is_same_v<T, i64>) fj.set(k, static_cast<i64>(x));
                else if constexpr (std::is_same_v<T, u64>) fj.set(k, x);
                else if constexpr (std::is_same_v<T, f64>) fj.set(k, static_cast<f64>(x));
                else if constexpr (std::is_same_v<T, string>) fj.set(k, x);
                else if constexpr (std::is_same_v<T, Id128>) fj.set(k, x.to_hex());
                else if constexpr (std::is_same_v<T, bytes>) fj.set(k, "bytes[" + std::to_string(x.size()) + "]");
            }, v);
        }
        j.set("fields", std::move(fj));
    }
    return j;
}

} // namespace servingobs
