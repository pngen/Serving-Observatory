// Serving Observatory — core implementation (identity, digest, enums, json).
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/core/types.hpp"
#include "servingobs/core/identity.hpp"
#include "servingobs/core/digest.hpp"
#include "servingobs/core/enums.hpp"
#include "servingobs/core/json.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace servingobs {

// ------------------------------------------------------------------ trim
string_view trim(string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

// ------------------------------------------------------------------ identity
static const char* kHex = "0123456789abcdef";

string Id128::to_hex() const {
    string out(32, '0');
    for (int i = 0; i < 16; ++i) {
        int shift = (i < 8) ? ((7 - i) * 8) : ((15 - i) * 8);
        u64 word = (i < 8) ? hi_ : lo_;
        u8 b = static_cast<u8>((word >> shift) & 0xFF);
        out[i * 2] = kHex[b >> 4];
        out[i * 2 + 1] = kHex[b & 0xF];
    }
    return out;
}

Result<Id128> Id128::parse(string_view s) {
    string cleaned;
    cleaned.reserve(32);
    string_view t = trim(s);
    if (t.size() >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t.remove_prefix(2);
    for (char c : t) {
        if (c == '-' || c == ':' || c == '_' || c == ' ') continue;
        if (c >= '0' && c <= '9') cleaned.push_back(c);
        else if (c >= 'a' && c <= 'f') cleaned.push_back(c);
        else if (c >= 'A' && c <= 'F') cleaned.push_back(static_cast<char>(c - 'A' + 'a'));
        else return Result<Id128>::Err("invalid hex character");
    }
    if (cleaned.size() != 32) {
        return Result<Id128>::Err("expected 32 hex chars, got " + std::to_string(cleaned.size()));
    }
    u64 hi = 0, lo = 0;
    for (int i = 0; i < 32; ++i) {
        u8 nib = static_cast<u8>((cleaned[i] <= '9') ? (cleaned[i] - '0') : (cleaned[i] - 'a' + 10));
        if (i < 16) hi = (hi << 4) | nib;
        else lo = (lo << 4) | nib;
    }
    return Result<Id128>::Ok(Id128(hi, lo));
}

bytes Id128::to_bytes() const {
    bytes out;
    out.reserve(16);
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<byte>((hi_ >> (i * 8)) & 0xFF));
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<byte>((lo_ >> (i * 8)) & 0xFF));
    return out;
}

Id128 Id128::derive(u64 namespace_tag, const byte* data, std::size_t n) {
    constexpr u64 kFnv = 14695981039346656037ULL;
    u64 h1 = kFnv ^ namespace_tag;
    u64 h2 = kFnv ^ (namespace_tag ^ 0x9e3779b97f4a7c15ULL);
    for (std::size_t i = 0; i < n; ++i) {
        u8 b = static_cast<u8>(data[i]);
        h1 ^= b; h1 *= 1099511628211ULL;
        h2 ^= static_cast<u8>(b + 1); h2 *= 1099511628211ULL;
    }
    auto mix = [](u64 x) { x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL; x ^= x >> 27; x *= 0x94d049bb133111ebULL; x ^= x >> 31; return x; };
    u64 hi = mix(h1) ^ 0x00ECB00000000000ULL;
    u64 lo = mix(h2);
    if (hi == 0) hi = 1;
    return Id128(hi, lo);
}

// ------------------------------------------------------------------ sha256
namespace {
constexpr u32 K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
inline u32 rr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
}

Sha256::Sha256() {
    h_[0]=0x6a09e667; h_[1]=0xbb67ae85; h_[2]=0x3c6ef372; h_[3]=0xa54ff53a;
    h_[4]=0x510e527f; h_[5]=0x9b05688c; h_[6]=0x1f83d9ab; h_[7]=0x5be0cd19;
    total_len_ = 0; buf_len_ = 0;
}

void Sha256::update(const byte* data, std::size_t n) {
    total_len_ += n;
    const u8* p = reinterpret_cast<const u8*>(data);
    while (n > 0) {
        if (buf_len_ == 64) { process_block(buf_); buf_len_ = 0; }
        const std::size_t take = std::min<std::size_t>(64 - buf_len_, n);
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take; p += take; n -= take;
    }
}

void Sha256::process_block(const u8* p) {
    u32 w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<u32>(p[i*4]) << 24) | (static_cast<u32>(p[i*4+1]) << 16) |
               (static_cast<u32>(p[i*4+2]) << 8) | static_cast<u32>(p[i*4+3]);
    }
    for (int i = 16; i < 64; ++i) {
        u32 s0 = rr(w[i-15],7) ^ rr(w[i-15],18) ^ (w[i-15] >> 3);
        u32 s1 = rr(w[i-2],17) ^ rr(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    u32 a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],hh=h_[7];
    for (int i = 0; i < 64; ++i) {
        u32 S1 = rr(e,6)^rr(e,11)^rr(e,25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = hh + S1 + ch + K[i] + w[i];
        u32 S0 = rr(a,2)^rr(a,13)^rr(a,22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h_[0]+=a; h_[1]+=b; h_[2]+=c; h_[3]+=d; h_[4]+=e; h_[5]+=f; h_[6]+=g; h_[7]+=hh;
}

Digest Sha256::finalize() {
    u64 bit_len = total_len_ * 8;
    u8 pad = 0x80;
    update(reinterpret_cast<const byte*>(&pad), 1);
    u8 zero = 0;
    while (buf_len_ != 56) update(reinterpret_cast<const byte*>(&zero), 1);
    u8 lenbytes[8];
    for (int i = 0; i < 8; ++i) lenbytes[i] = static_cast<u8>(bit_len >> (56 - i*8));
    for (int i = 0; i < 8; ++i) buf_[buf_len_++] = lenbytes[i];
    process_block(buf_);
    Digest d;
    for (int i = 0; i < 8; ++i) {
        d[i*4]   = static_cast<u8>(h_[i] >> 24);
        d[i*4+1] = static_cast<u8>(h_[i] >> 16);
        d[i*4+2] = static_cast<u8>(h_[i] >> 8);
        d[i*4+3] = static_cast<u8>(h_[i]);
    }
    return d;
}

Digest Sha256::hash(const byte* data, std::size_t n) { Sha256 s; s.update(data, n); return s.finalize(); }
Digest Sha256::hash(string_view s) { return hash(reinterpret_cast<const byte*>(s.data()), s.size()); }
Digest Sha256::hash(const bytes& b) { return hash(b.data(), b.size()); }

string digest_hex(const Digest& d) {
    string out(64, '0');
    for (int i = 0; i < 32; ++i) {
        out[i*2] = kHex[d[i] >> 4];
        out[i*2+1] = kHex[d[i] & 0xF];
    }
    return out;
}

Result<Digest> digest_from_hex(string_view s) {
    string_view t = trim(s);
    if (t.size() >= 2 && t[0]=='0' && (t[1]=='x'||t[1]=='X')) t.remove_prefix(2);
    if (t.size() != 64) return Result<Digest>::Err("digest must be 64 hex chars");
    Digest d;
    auto nib = [](char c) -> int {
        if (c>='0'&&c<='9') return c-'0';
        if (c>='a'&&c<='f') return c-'a'+10;
        if (c>='A'&&c<='F') return c-'A'+10;
        return -1;
    };
    for (int i = 0; i < 32; ++i) {
        int hi = nib(t[i*2]); int lo = nib(t[i*2+1]);
        if (hi < 0 || lo < 0) return Result<Digest>::Err("bad hex in digest");
        d[i] = static_cast<u8>((hi<<4) | lo);
    }
    return Result<Digest>::Ok(d);
}
bool digest_equal(const Digest& a, const Digest& b) { return a == b; }

// ------------------------------------------------------------------ enums
string_view obs_type_name(ObsType t) {
    switch (t) {
        case ObsType::UNKNOWN: return "unknown";
        case ObsType::REQUEST: return "request";
        case ObsType::ADMISSION: return "admission";
        case ObsType::QUEUE_ENTER: return "queue_enter";
        case ObsType::QUEUE_LEAVE: return "queue_leave";
        case ObsType::BATCH_FORM: return "batch_form";
        case ObsType::BATCH_SEAL: return "batch_seal";
        case ObsType::BATCH_SPLIT: return "batch_split";
        case ObsType::BATCH_MERGE: return "batch_merge";
        case ObsType::DISPATCH: return "dispatch";
        case ObsType::PREFILL_START: return "prefill_start";
        case ObsType::PREFILL_END: return "prefill_end";
        case ObsType::DECODE_STEP_START: return "decode_step_start";
        case ObsType::DECODE_STEP_END: return "decode_step_end";
        case ObsType::SPECULATION_START: return "speculation_start";
        case ObsType::SPECULATION_VERIFY: return "speculation_verify";
        case ObsType::KV_LOOKUP: return "kv_lookup";
        case ObsType::KV_HIT: return "kv_hit";
        case ObsType::KV_MISS: return "kv_miss";
        case ObsType::KV_GROWTH: return "kv_growth";
        case ObsType::MODEL_RESIDENCY: return "model_residency";
        case ObsType::ADAPTER_RESIDENCY: return "adapter_residency";
        case ObsType::KERNEL_LOOKUP: return "kernel_lookup";
        case ObsType::KERNEL_HIT: return "kernel_hit";
        case ObsType::KERNEL_MISS: return "kernel_miss";
        case ObsType::GRAPH_LOOKUP: return "graph_lookup";
        case ObsType::GRAPH_HIT: return "graph_hit";
        case ObsType::GRAPH_MISS: return "graph_miss";
        case ObsType::ALLOCATION: return "allocation";
        case ObsType::FREE: return "free";
        case ObsType::TRANSFER_START: return "transfer_start";
        case ObsType::TRANSFER_END: return "transfer_end";
        case ObsType::RETRY: return "retry";
        case ObsType::CANCEL: return "cancel";
        case ObsType::FAILURE: return "failure";
        case ObsType::WORKER_DOWN: return "worker_down";
        case ObsType::WORKER_RESTART: return "worker_restart";
        case ObsType::RECOVERY: return "recovery";
        case ObsType::COMPLETION: return "completion";
        case ObsType::SLO_STATE: return "slo_state";
        case ObsType::QUOTA_STATE: return "quota_state";
        case ObsType::CAPACITY_STATE: return "capacity_state";
    }
    return "unknown";
}
string_view obs_type_code(ObsType t) { return obs_type_name(t); }
ObsType obs_type_from_code(string_view code) {
#define X(name) if (code == #name) return ObsType::name;
    X(REQUEST) X(ADMISSION) X(QUEUE_ENTER) X(QUEUE_LEAVE) X(BATCH_FORM) X(BATCH_SEAL)
    X(BATCH_SPLIT) X(BATCH_MERGE) X(DISPATCH) X(PREFILL_START) X(PREFILL_END)
    X(DECODE_STEP_START) X(DECODE_STEP_END) X(SPECULATION_START) X(SPECULATION_VERIFY)
    X(KV_LOOKUP) X(KV_HIT) X(KV_MISS) X(KV_GROWTH) X(MODEL_RESIDENCY) X(ADAPTER_RESIDENCY)
    X(KERNEL_LOOKUP) X(KERNEL_HIT) X(KERNEL_MISS) X(GRAPH_LOOKUP) X(GRAPH_HIT) X(GRAPH_MISS)
    X(ALLOCATION) X(FREE) X(TRANSFER_START) X(TRANSFER_END) X(RETRY) X(CANCEL) X(FAILURE)
    X(WORKER_DOWN) X(WORKER_RESTART) X(RECOVERY) X(COMPLETION) X(SLO_STATE) X(QUOTA_STATE)
    X(CAPACITY_STATE)
#undef X
    return ObsType::UNKNOWN;
}

string_view provenance_name(Provenance p) {
    switch (p) {
        case Provenance::UNKNOWN: return "unknown";
        case Provenance::MEASURED: return "measured";
        case Provenance::REPORTED: return "reported";
        case Provenance::DERIVED: return "derived";
        case Provenance::ESTIMATED: return "estimated";
        case Provenance::RECONSTRUCTED: return "reconstructed";
        case Provenance::HEURISTIC: return "heuristic";
    }
    return "unknown";
}
Provenance provenance_from_code(string_view code) {
    if (code=="measured") return Provenance::MEASURED;
    if (code=="reported") return Provenance::REPORTED;
    if (code=="derived") return Provenance::DERIVED;
    if (code=="estimated") return Provenance::ESTIMATED;
    if (code=="reconstructed") return Provenance::RECONSTRUCTED;
    if (code=="heuristic") return Provenance::HEURISTIC;
    return Provenance::UNKNOWN;
}

string_view explanation_class_name(ExplanationClass c) {
    switch (c) {
        case ExplanationClass::UNKNOWN: return "unknown";
        case ExplanationClass::DIRECTLY_EVIDENCED: return "directly_evidenced";
        case ExplanationClass::STRUCTURALLY_DERIVED: return "structurally_derived";
        case ExplanationClass::TEMPORALLY_CORRELATED: return "temporally_correlated";
        case ExplanationClass::PLAUSIBLE_BUT_UNPROVEN: return "plausible_but_unproven";
        case ExplanationClass::CONTRADICTED: return "contradicted";
    }
    return "unknown";
}
ExplanationClass explanation_class_from_code(string_view code) {
    if (code=="directly_evidenced") return ExplanationClass::DIRECTLY_EVIDENCED;
    if (code=="structurally_derived") return ExplanationClass::STRUCTURALLY_DERIVED;
    if (code=="temporally_correlated") return ExplanationClass::TEMPORALLY_CORRELATED;
    if (code=="plausible_but_unproven") return ExplanationClass::PLAUSIBLE_BUT_UNPROVEN;
    if (code=="contradicted") return ExplanationClass::CONTRADICTED;
    return ExplanationClass::UNKNOWN;
}

string_view source_health_name(SourceHealth h) {
    switch (h) {
        case SourceHealth::UNKNOWN: return "unknown";
        case SourceHealth::HEALTHY: return "healthy";
        case SourceHealth::DEGRADED: return "degraded";
        case SourceHealth::STALE: return "stale";
        case SourceHealth::DOWN: return "down";
        case SourceHealth::RESTARTING: return "restarting";
        case SourceHealth::REJECTED: return "rejected";
    }
    return "unknown";
}
SourceHealth source_health_from_code(string_view code) {
    if (code=="healthy") return SourceHealth::HEALTHY;
    if (code=="degraded") return SourceHealth::DEGRADED;
    if (code=="stale") return SourceHealth::STALE;
    if (code=="down") return SourceHealth::DOWN;
    if (code=="restarting") return SourceHealth::RESTARTING;
    if (code=="rejected") return SourceHealth::REJECTED;
    return SourceHealth::UNKNOWN;
}

string_view outcome_name(Outcome o) {
    switch (o) {
        case Outcome::UNKNOWN: return "unknown";
        case Outcome::COMPLETED: return "completed";
        case Outcome::FAILED: return "failed";
        case Outcome::CANCELLED: return "cancelled";
        case Outcome::TIMED_OUT: return "timed_out";
        case Outcome::PREEMPTED: return "preempted";
        case Outcome::ABANDONED: return "abandoned";
        case Outcome::RETRIED: return "retried";
        case Outcome::PARTIAL: return "partial";
    }
    return "unknown";
}
Outcome outcome_from_code(string_view code) {
    if (code=="completed") return Outcome::COMPLETED;
    if (code=="failed") return Outcome::FAILED;
    if (code=="cancelled") return Outcome::CANCELLED;
    if (code=="timed_out") return Outcome::TIMED_OUT;
    if (code=="preempted") return Outcome::PREEMPTED;
    if (code=="abandoned") return Outcome::ABANDONED;
    if (code=="retried") return Outcome::RETRIED;
    if (code=="partial") return Outcome::PARTIAL;
    return Outcome::UNKNOWN;
}

string_view queue_kind_name(QueueKind k) {
    switch (k) {
        case QueueKind::UNKNOWN: return "unknown";
        case QueueKind::ADMISSION: return "admission";
        case QueueKind::SCHEDULER: return "scheduler";
        case QueueKind::BATCH: return "batch";
        case QueueKind::PREEMPTION: return "preemption";
    }
    return "unknown";
}
QueueKind queue_kind_from_code(string_view code) {
    if (code=="admission") return QueueKind::ADMISSION;
    if (code=="scheduler") return QueueKind::SCHEDULER;
    if (code=="batch") return QueueKind::BATCH;
    if (code=="preemption") return QueueKind::PREEMPTION;
    return QueueKind::UNKNOWN;
}

// ------------------------------------------------------------------ json serialization
namespace {
void escape_json(string_view s, string& out) {
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else out += static_cast<char>(c);
        }
    }
}
void write_json(const Json& j, string& out, bool pretty, int depth) {
    const string indent = pretty ? string(static_cast<size_t>(depth) * 2, ' ') : "";
    const string indent_in = pretty ? string(static_cast<size_t>(depth + 1) * 2, ' ') : "";
    const string nl = pretty ? "\n" : "";
    switch (j.kind()) {
        case Json::Kind::Null: out += "null"; break;
        case Json::Kind::Bool: out += j.as_bool() ? "true" : "false"; break;
        case Json::Kind::Int: out += std::to_string(j.as_int()); break;
        case Json::Kind::UInt: out += std::to_string(j.as_uint()); break;
        case Json::Kind::Double: {
            f64 d = j.as_double();
            if (std::isnan(d) || std::isinf(d)) { out += "null"; break; }
            char buf[64];
            int n = std::snprintf(buf, sizeof buf, "%.17g", d);
            if (n <= 0) { out += "0"; break; }
            out.append(buf, static_cast<size_t>(n));
            break;
        }
        case Json::Kind::String: {
            out += '"'; escape_json(j.as_string(), out); out += '"';
            break;
        }
        case Json::Kind::Array: {
            out += '[';
            bool first = true;
            for (const auto& e : j.items()) {
                if (!first) out += (pretty ? "," + nl : ",") + (pretty ? indent_in : "");
                first = false;
                write_json(e, out, pretty, depth + 1);
            }
            out += ']';
            break;
        }
        case Json::Kind::Object: {
            out += '{';
            bool first = true;
            for (const auto& [k, v] : j.members()) {
                if (!first) out += (pretty ? "," + nl : ",") + (pretty ? indent_in : "");
                first = false;
                out += '"'; escape_json(k, out); out += '"';
                out += pretty ? ": " : ":";
                write_json(v, out, pretty, depth + 1);
            }
            out += '}';
            break;
        }
    }
}
}
string Json::to_canonical() const { string out; write_json(*this, out, false, 0); return out; }
string Json::to_pretty() const { string out; write_json(*this, out, true, 0); return out; }

} // namespace servingobs
