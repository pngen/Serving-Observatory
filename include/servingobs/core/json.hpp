#pragma once
// Serving Observatory — JSON value tree with deterministic output.
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// Object keys are stored in a sorted map so canonical serialization is
// deterministic. Doubles are emitted as shortest-round-trip decimal (17 sig
// digits) which round-trips IEEE-754 binary64 exactly and is deterministic for
// a given toolchain. Provide canonical (compact, sorted) and human (indented)
// writers.

#include "servingobs/core/types.hpp"

#include <cmath>
#include <charconv>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace servingobs {

class Json {
public:
    enum class Kind { Null, Bool, Int, UInt, Double, String, Array, Object };

    Json() : kind_(Kind::Null) {}
    Json(std::nullptr_t) : kind_(Kind::Null) {}
    Json(bool b) : kind_(Kind::Bool), b_(b) {}
    Json(i64 v) : kind_(Kind::Int), i_(v) {}
    Json(u64 v) : kind_(Kind::UInt), u_(v) {}
    Json(int v) : kind_(Kind::Int), i_(v) {}
    Json(f64 v) : kind_(Kind::Double), d_(v) {}
    Json(string v) : kind_(Kind::String), s_(std::move(v)) {}
    Json(const char* v) : kind_(Kind::String), s_(v) {}

    static Json array() { Json j; j.kind_ = Kind::Array; return j; }
    static Json object() { Json j; j.kind_ = Kind::Object; return j; }

    Kind kind() const { return kind_; }
    bool is_null() const { return kind_ == Kind::Null; }

    // Object access (keys stay sorted; re-setting a key overwrites).
    Json& set(string key, Json val) {
        ensure_object();
        obj_[std::move(key)] = std::move(val);
        return *this;
    }
    const Json* find(string_view key) const {
        if (kind_ != Kind::Object) return nullptr;
        auto it = obj_.find(string(key));
        return it == obj_.end() ? nullptr : &it->second;
    }
    const Json& get(string_view key) const {
        static Json null;
        const Json* p = find(key);
        return p ? *p : null;
    }
    const std::map<string, Json>& members() const { return obj_; }

    // Array access
    void push(Json v) { ensure_array(); arr_.push_back(std::move(v)); }
    const std::vector<Json>& items() const { return arr_; }

    // Value accessors
    bool as_bool(bool def = false) const { return kind_ == Kind::Bool ? b_ : def; }
    i64 as_int(i64 def = 0) const {
        if (kind_ == Kind::Int) return i_;
        if (kind_ == Kind::UInt) return static_cast<i64>(u_);
        return def;
    }
    u64 as_uint(u64 def = 0) const {
        if (kind_ == Kind::UInt) return u_;
        if (kind_ == Kind::Int) return static_cast<u64>(i_);
        return def;
    }
    f64 as_double(f64 def = 0.0) const {
        if (kind_ == Kind::Double) return d_;
        if (kind_ == Kind::Int) return static_cast<f64>(i_);
        if (kind_ == Kind::UInt) return static_cast<f64>(u_);
        return def;
    }
    const string& as_string(const string& def = {}) const {
        static const string empty;
        return kind_ == Kind::String ? s_ : def;
    }

    // Serialization --- canonical (compact, deterministic) and pretty
    string to_canonical() const;
    string to_pretty() const;

private:
    void ensure_object() { if (kind_ != Kind::Object) { *this = Json::object(); } }
    void ensure_array() { if (kind_ != Kind::Array) { *this = Json::array(); } }

    Kind kind_;
    bool b_ = false;
    i64 i_ = 0;
    u64 u_ = 0;
    f64 d_ = 0.0;
    string s_;
    std::vector<Json> arr_;
    std::map<string, Json> obj_;
};

inline Json json_object() { return Json::object(); }
inline Json json_array() { return Json::array(); }

} // namespace servingobs
