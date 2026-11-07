// SPDX-License-Identifier: Apache-2.0
#pragma once

// What every command of groomlab-helper reads its arguments with (docs/BLENDER.md, "Protocol"):
// typed arguments that answer bad_request when wrong, arrays given as JSON lists or shared memory
// references, and arrays answered in the helper's own shared memory, points turned into the
// host's space at the boundary (host_space.h).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

#include "helper.h"
#include "host_space.h"
#include "shared_memory.h"

namespace groomlab::helper {

using nlohmann::json;

[[noreturn]] inline void fail(const std::string& code, const std::string& message) { throw RequestError(code, message); }

// ---- Arguments ----------------------------------------------------------------------------------

inline const json& need(const json& args, const char* key) {
    const auto it = args.find(key);
    if (it == args.end() || it->is_null()) fail("bad_request", std::string("missing \"") + key + "\"");
    return *it;
}

inline std::string needString(const json& args, const char* key) {
    const json& v = need(args, key);
    if (!v.is_string()) fail("bad_request", std::string("\"") + key + "\" must be a string");
    return v.get<std::string>();
}

inline std::string optString(const json& args, const char* key, const std::string& fallback) {
    const auto it = args.find(key);
    if (it == args.end() || it->is_null()) return fallback;
    if (!it->is_string()) fail("bad_request", std::string("\"") + key + "\" must be a string");
    return it->get<std::string>();
}

inline double optNumber(const json& args, const char* key, double fallback) {
    const auto it = args.find(key);
    if (it == args.end() || it->is_null()) return fallback;
    if (!it->is_number()) fail("bad_request", std::string("\"") + key + "\" must be a number");
    const double v = it->get<double>();
    if (!std::isfinite(v)) fail("bad_request", std::string("\"") + key + "\" must be finite");
    return v;
}

inline bool optBool(const json& args, const char* key, bool fallback) {
    const auto it = args.find(key);
    if (it == args.end() || it->is_null()) return fallback;
    if (!it->is_boolean()) fail("bad_request", std::string("\"") + key + "\" must be true or false");
    return it->get<bool>();
}

// ---- Arrays in ----------------------------------------------------------------------------------

// The client's blocks a request names, each opened once for it.
class BlockReader {
public:
    const SharedBlock& get(const std::string& name) {
        auto it = open_.find(name);
        if (it == open_.end()) {
            try {
                it = open_.emplace(name, SharedBlock::open(name)).first;
            } catch (const std::exception& e) {
                fail("shared_memory", e.what());
            }
        }
        return *it->second;
    }

private:
    std::map<std::string, std::unique_ptr<SharedBlock>> open_;
};

inline std::size_t dtypeSize(const std::string& dtype) {
    if (dtype == "<f4" || dtype == "<i4" || dtype == "<u4") return 4;
    if (dtype == "<f8" || dtype == "<i8" || dtype == "<u8") return 8;
    fail("bad_request", "dtype \"" + dtype + "\" is not one of <f4 <f8 <i4 <u4 <i8 <u8");
}

template <class T>
T element(const std::uint8_t* p, const std::string& dtype) {
    auto get = [p](auto v) {
        std::memcpy(&v, p, sizeof v);
        return v;
    };
    if (dtype == "<f4") return static_cast<T>(get(float{}));
    if (dtype == "<f8") return static_cast<T>(get(double{}));
    if (dtype == "<i4") return static_cast<T>(get(std::int32_t{}));
    if (dtype == "<u4") return static_cast<T>(get(std::uint32_t{}));
    if (dtype == "<i8") return static_cast<T>(get(std::int64_t{}));
    return static_cast<T>(get(std::uint64_t{}));
}

template <class T>
void flatten(const json& v, std::vector<T>& out, const char* what) {
    if (v.is_array()) {
        for (const json& e : v) flatten(e, out, what);
    } else if (v.is_number()) {
        if constexpr (std::is_floating_point_v<T>) {
            out.push_back(static_cast<T>(v.get<double>()));
        } else if (v.is_number_integer()) {
            out.push_back(v.is_number_unsigned() ? static_cast<T>(v.get<std::uint64_t>()) : static_cast<T>(v.get<std::int64_t>()));
        } else {
            fail("bad_request", std::string(what) + " holds a fraction where an integer goes");
        }
    } else {
        fail("bad_request", std::string(what) + " holds something that is not a number");
    }
}

// An array argument: a JSON list (nested or flat) or {"shm", "offset", "dtype", "shape"} naming a
// shared memory block. Its values flat, `components` per element.
template <class T>
std::vector<T> readArray(const json& value, const char* what, std::size_t components, BlockReader& blocks) {
    std::vector<T> out;
    if (value.is_array()) {
        flatten(value, out, what);
    } else if (value.is_object()) {
        const std::string block = needString(value, "shm");
        const std::string dtype = optString(value, "dtype", "<f4");
        const std::size_t item = dtypeSize(dtype);
        const double offsetValue = optNumber(value, "offset", 0.0);
        if (offsetValue < 0.0) fail("bad_request", std::string(what) + ": a negative offset");
        const auto offset = static_cast<std::size_t>(offsetValue);
        std::size_t count = 1;
        const json& shape = need(value, "shape");
        if (!shape.is_array() || shape.empty()) fail("bad_request", std::string(what) + ": shape must be a list");
        for (const json& d : shape) {
            if (!d.is_number_unsigned() && !(d.is_number_integer() && d.get<std::int64_t>() >= 0)) {
                fail("bad_request", std::string(what) + ": shape holds a bad size");
            }
            count *= d.get<std::size_t>();
        }
        if (shape.size() > 1 && shape.back().get<std::size_t>() != components) {
            fail("bad_request", std::string(what) + " has " + std::to_string(shape.back().get<std::size_t>()) +
                                    " components, not " + std::to_string(components));
        }
        const SharedBlock& b = blocks.get(block);
        if (offset > b.size() || count > (b.size() - offset) / item) {
            fail("bad_request", std::string(what) + " runs past the end of shared memory '" + block + "'");
        }
        out.resize(count);
        const std::uint8_t* p = b.data() + offset;
        for (std::size_t i = 0; i < count; ++i) out[i] = element<T>(p + i * item, dtype);
    } else {
        fail("bad_request", std::string(what) + " must be a list or a shared memory reference");
    }
    if (components > 1 && out.size() % components != 0) {
        fail("bad_request", std::string(what) + " holds " + std::to_string(out.size()) + " values: not a multiple of " +
                                std::to_string(components));
    }
    if constexpr (std::is_floating_point_v<T>) {
        for (const T v : out) {
            if (!std::isfinite(v)) fail("bad_request", std::string(what) + " holds a NaN or an infinity");
        }
    }
    return out;
}

// ---- Arrays out ---------------------------------------------------------------------------------

inline json refJson(const ArrayRef& r) { return {{"shm", r.block}, {"offset", r.offset}, {"dtype", r.dtype}, {"shape", r.shape}}; }

template <class T>
const char* dtypeOf() {
    if constexpr (std::is_same_v<T, float>) {
        return "<f4";
    } else if constexpr (std::is_same_v<T, double>) {
        return "<f8";
    } else if constexpr (std::is_same_v<T, std::int32_t>) {
        return "<i4";
    } else if constexpr (std::is_same_v<T, std::uint32_t>) {
        return "<u4";
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
        return "<i8";
    } else if constexpr (std::is_same_v<T, std::uint8_t>) {
        return "|u1";
    } else {
        return "<u8";
    }
}

template <class T>
inline json put(Arena& arena, const T* data, std::size_t count, std::size_t components) {
    ArrayRef ref;
    std::vector<std::size_t> shape{count};
    if (components > 1) shape.push_back(components);
    void* dst = arena.add(count * components, sizeof(T), dtypeOf<T>(), std::move(shape), &ref);
    if (count) std::memcpy(dst, data, count * components * sizeof(T));
    return refJson(ref);
}

// Points into the host's space (or as they are, `groom`), into the arena.
inline json putPoints(Arena& arena, const std::vector<Vec3f>& points, const HostSpace& space, bool groom) {
    ArrayRef ref;
    auto* dst = static_cast<float*>(arena.add(points.size() * 3, sizeof(float), "<f4", {points.size(), 3}, &ref));
    if (groom) {
        if (!points.empty()) std::memcpy(dst, points.data(), points.size() * sizeof(Vec3f));
    } else {
        for (std::size_t i = 0; i < points.size(); ++i) space.toHost(points[i], dst + 3 * i);
    }
    return refJson(ref);
}

inline std::vector<Vec3f> pointsToGroom(const std::vector<float>& xyz, const HostSpace& space) {
    std::vector<Vec3f> out(xyz.size() / 3);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = space.toGroom(xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]);
    return out;
}

}  // namespace groomlab::helper
