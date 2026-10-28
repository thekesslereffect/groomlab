// SPDX-License-Identifier: Apache-2.0
#include "groomlab/instances.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>

#include "groomlab/color_patterns.h"
#include "groomlab/curve.h"
#include "groomlab/frames.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {

namespace {

// Strand IDs to slots 0, 1, ... in the order first added: open addressing (twice the IDs' room,
// rounded up to a power of two; linear probing). Finding never writes: safe in parallel.
class IdSlots {
public:
    static constexpr std::size_t kNone = ~std::size_t{0};
    explicit IdSlots(std::size_t most) {
        std::size_t room = 16;
        while (room < 2 * most) room *= 2;
        mask_ = room - 1;
        ids_.resize(room);
        slots_.assign(room, kNone);
    }
    // Its slot, given the next one when new.
    std::size_t add(std::uint64_t id) {
        for (std::size_t at = hash64(id) & mask_;; at = (at + 1) & mask_) {
            if (slots_[at] == kNone) {
                ids_[at] = id;
                return slots_[at] = count_++;
            }
            if (ids_[at] == id) return slots_[at];
        }
    }
    std::size_t find(std::uint64_t id) const {
        for (std::size_t at = hash64(id) & mask_;; at = (at + 1) & mask_) {
            if (slots_[at] == kNone) return kNone;
            if (ids_[at] == id) return slots_[at];
        }
    }
    std::size_t size() const { return count_; }

private:
    std::vector<std::uint64_t> ids_;
    std::vector<std::size_t> slots_;
    std::size_t mask_ = 0, count_ = 0;
};

}  // namespace

std::vector<std::string> instancePrototypeNames(const std::string& text) {
    std::string spaced = text;
    std::replace(spaced.begin(), spaced.end(), ',', ' ');
    std::vector<std::string> names;
    std::size_t at = 0;
    while (true) {
        at = spaced.find_first_not_of(" \t\r\n", at);
        if (at == std::string::npos) break;
        const std::size_t end = spaced.find_first_of(" \t\r\n", at);
        names.push_back(spaced.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (end == std::string::npos) break;
        at = end;
    }
    if (names.empty()) names.push_back(text);  // (an empty name stays one, as before)
    return names;
}

std::uint32_t InstanceSet::prototypeIndex(const std::string& name) {
    auto it = std::find(prototypes.begin(), prototypes.end(), name);
    if (it != prototypes.end()) return static_cast<std::uint32_t>(it - prototypes.begin());
    prototypes.push_back(name);
    return static_cast<std::uint32_t>(prototypes.size() - 1);
}

std::uint32_t InstanceSet::materialIndex(const std::string& name) {
    auto it = std::find(materials.begin(), materials.end(), name);
    if (it != materials.end()) return static_cast<std::uint32_t>(it - materials.begin());
    materials.push_back(name);
    return static_cast<std::uint32_t>(materials.size() - 1);
}

void InstanceSet::append(const InstanceSet& other) {
    std::vector<std::uint32_t> remap, remapMaterials;
    for (const std::string& name : other.prototypes) remap.push_back(prototypeIndex(name));
    for (const std::string& name : other.materials) remapMaterials.push_back(materialIndex(name));
    hidesStrands = hidesStrands || other.hidesStrands;
    instances.reserve(instances.size() + other.instances.size());
    for (Instance i : other.instances) {
        i.prototype = i.prototype < remap.size() ? remap[i.prototype] : 0;
        if (i.material >= 0) {
            const auto m = static_cast<std::size_t>(i.material);
            i.material = m < remapMaterials.size() ? static_cast<std::int32_t>(remapMaterials[m]) : -1;
        }
        instances.push_back(i);
    }
}

std::uint64_t InstanceSet::hashInstance(std::uint64_t h, const Instance& i) {
    float f[10] = {i.t,     i.direction.x, i.direction.y,  i.direction.z, i.spin,
                   i.scale, i.offset,      i.restDistance, i.aspect,      i.restLength};
    h = hashCombine(hashCombine(hashCombine(h, i.prototype), i.strand), i.id);
    h = hashCombine(h, static_cast<std::uint64_t>(i.orient) | (i.scaleByWidth ? 0x100u : 0u) |
                           (i.pinToRest ? 0x200u : 0u) | (i.fitStrand ? 0x400u : 0u));
    for (const float v : f) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        h = hashCombine(h, bits);
    }
    if (i.deform) h = hashCombine(h, 0xDEF0u + i.deformAxis);  // (undeformed ones hash as they always did)
    if (i.onSkin) {  // (instances on strands hash as they always did)
        h = hashCombine(h, static_cast<std::uint64_t>(i.variant) | (static_cast<std::uint64_t>(i.preview) << 16) |
                               (i.conform ? 1ull << 24 : 0ull) | (i.aimRoot ? 1ull << 25 : 0ull) |
                               (i.followStrand ? 1ull << 26 : 0ull) |
                               (static_cast<std::uint64_t>(static_cast<std::uint32_t>(i.material)) << 32));
        h = hashFloat(hashFloat(hashFloat(h, i.lift), i.tile), i.stack);
    }
    return h;
}

std::uint64_t InstanceSet::contentHash() const {
    std::uint64_t h = hash64(0x1A57);
    for (const std::string& name : prototypes) {
        for (const char c : name) h = hashCombine(h, static_cast<std::uint64_t>(c));
    }
    for (const std::string& name : materials) h = hashString(h, name);
    if (hidesStrands) h = hashCombine(h, 0x41DEu);
    for (const Instance& i : instances) h = hashInstance(h, i);
    return h;
}

void InstanceSet::Placed::matrix(float out[16]) const {
    const Vec3f rows[4] = {x, y, z, origin};
    for (int r = 0; r < 4; ++r) {
        out[r * 4 + 0] = rows[r].x;
        out[r * 4 + 1] = rows[r].y;
        out[r * 4 + 2] = rows[r].z;
        out[r * 4 + 3] = r == 3 ? 1.0f : 0.0f;
    }
}

namespace {

// Position and unit tangent `distance` along a polyline from its first point
// (clamped to its ends); *segment and *u say where (point segment-1 to segment, u
// along it), for anything else stored per point.
void sampleStrand(const Vec3f* pts, std::size_t n, float distance, Vec3f* pos, Vec3f* tangent, std::size_t* segment,
                  float* u) {
    *segment = 0;
    *u = 0.0f;
    if (n == 1) {
        *pos = pts[0];
        *tangent = {0, 1, 0};
        return;
    }
    float target = std::max(distance, 0.0f);
    for (std::size_t i = 1; i < n; ++i) {
        const Vec3f seg = pts[i] - pts[i - 1];
        const float len = seg.length();
        if (target <= len || i + 1 == n) {
            *u = len > 0.0f ? std::clamp(target / len, 0.0f, 1.0f) : 0.0f;
            *segment = i;
            *pos = pts[i - 1] + seg * *u;
            *tangent = len > 0.0f ? seg / len : Vec3f{0, 1, 0};
            return;
        }
        target -= len;
    }
}

// A scale at strand s's root, flat on the skin: +Z the skin's normal there (the scalp's at the
// root when the strand is rooted on it, else the one the node saw), +Y the strand's direction
// (root to tip, or near the root: aimRoot, followStrand) laid onto the skin and turned by the
// spin, +X across.
InstanceSet::Placed placeScale(const InstanceSet::Instance& i, const StrandSet& strands, std::size_t s, const ScalpMesh* scalp,
                               float length) {
    const Vec3f* pts = strands.strandPoints(s);
    const std::size_t n = strands.strandPointCount(s);
    const StrandRoot& root = strands.roots()[s];
    Vec3f normal = scalp && scalp->contains(root) ? scalp->normal(root) : i.direction;
    normal = normal.length() > 1e-12f ? normal.normalized() : Vec3f{0, 1, 0};
    Vec3f dir = n > 1 ? pts[n - 1] - pts[0] : Vec3f{};
    if (n > 1 && (i.aimRoot || i.followStrand)) {
        // A tenth of the way along, at least the first segment.
        Vec3f near, tangent;
        std::size_t segment = 0;
        float u = 0.0f;
        sampleStrand(pts, n, std::max(0.1f * length, (pts[1] - pts[0]).length()), &near, &tangent, &segment, &u);
        dir = near - pts[0];
    }
    dir = dir - normal * dir.dot(normal);
    // A strand standing straight up has no direction on the skin: any one.
    // (Within a twentieth of a degree of the normal: its rounding is no direction, nor the same at
    // another scale.)
    Vec3f y = dir.length() > 1e-3f * length ? dir.normalized() : anyPerpendicular(normal).normalized();
    const float c = pm::cos(i.spin), sn = pm::sin(i.spin);
    y = (y * c + normal.cross(y) * sn).normalized();  // spin about the normal
    const Vec3f x = y.cross(normal).normalized();      // x, y, normal right-handed
    // Along: the rest length (the length now until it is known); across: the root width, or a
    // ratio of the length without widths.
    const float along = i.scale * (i.restLength >= 0.0f ? i.restLength : length);
    const std::vector<float>& widths = strands.widths();
    const std::size_t begin = strands.strandBegin(s);
    const float rootWidth = begin < widths.size() ? widths[begin] : 0.0f;
    // Widths of 1 all along are none set (StrandSet's default, no Width node: not a scene unit).
    bool unset = true;
    for (std::size_t k = 0; unset && k < n && begin + k < widths.size(); ++k) unset = widths[begin + k] == 1.0f;
    const float across = !unset && rootWidth > 0.0f && std::isfinite(rootWidth) ? i.scale * rootWidth : along * i.aspect;
    InstanceSet::Placed p;
    p.prototype = i.prototype;
    p.id = i.id;
    p.onSkin = true;
    p.x = x * across;
    p.y = y * along;
    p.z = normal * across;
    p.origin = pts[0];
    return p;
}

}  // namespace

bool InstanceSet::needsRestPose() const {
    return std::any_of(instances.begin(), instances.end(),
                       [](const Instance& i) { return i.pinToRest || i.fitStrand || i.onSkin; });
}

bool InstanceSet::hasMeshes() const {
    return std::any_of(instances.begin(), instances.end(), [](const Instance& i) { return i.meshed(); });
}

bool InstanceSet::hasScales() const {
    return std::any_of(instances.begin(), instances.end(), [](const Instance& i) { return i.onSkin; });
}

std::vector<InstanceSet::Placed> InstanceSet::resolve(const StrandSet& strands, const ScalpMesh* scalp) const {
    std::vector<Placed> out;
    if (instances.empty()) return out;
    // Each instance's strand (the first holding its ID), found through the IDs the instances name
    // (fewer than the strands), then every instance placed in parallel and kept in order.
    // (Each ID's slot, then the strands looked up in parallel blocks, each slot taking its least
    // strand.)
    constexpr std::size_t kUnfound = ~std::size_t{0};
    IdSlots slotOf(instances.size());
    for (const Instance& i : instances) slotOf.add(i.strand);
    std::unique_ptr<std::atomic<std::size_t>[]> firstOf(new std::atomic<std::size_t>[slotOf.size()]);
    for (std::size_t k = 0; k < slotOf.size(); ++k) firstOf[k].store(kUnfound, std::memory_order_relaxed);
    parallelFor(0, strands.strandCount(), [&](std::size_t s) {
        const std::size_t slot = slotOf.find(strands.ids()[s]);
        if (slot == IdSlots::kNone) return;
        std::atomic<std::size_t>& first = firstOf[slot];
        std::size_t seen = first.load(std::memory_order_relaxed);
        while (s < seen && !first.compare_exchange_weak(seen, s, std::memory_order_relaxed)) {
        }
    }, 4096);
    std::vector<Placed> all(instances.size());
    std::vector<char> placedOne(instances.size(), 0);
    const std::vector<float>& widths = strands.widths();
    // Bent along their strands: +Z faces the vane (Propagate's vane normals), else the skin.
    std::array<const std::vector<float>*, 3> vaneNormals{};
    bool hasVaneNormals = false;
    if (hasMeshes()) {
        hasVaneNormals = true;
        for (std::size_t c = 0; c < 3; ++c) {
            vaneNormals[c] = strands.strandChannel(kVaneNormalChannels[c]);
            hasVaneNormals = hasVaneNormals && vaneNormals[c] && vaneNormals[c]->size() == strands.strandCount();
        }
    }
    parallelFor(0, instances.size(), [&](std::size_t k) {
        const Instance& i = instances[k];
        const std::size_t s = firstOf[slotOf.find(i.strand)].load(std::memory_order_relaxed);
        if (s == kUnfound) return;
        const Vec3f* pts = strands.strandPoints(s);
        const std::size_t n = strands.strandPointCount(s);
        if (n == 0) return;
        const float length = n > 1 ? arcLength(pts, n) : 0.0f;
        if (i.onSkin) {
            all[k] = placeScale(i, strands, s, scalp, length);
            all[k].index = k;
            all[k].meshed = true;
            placedOne[k] = 1;
            return;
        }
        // Pinned: its rest distance from the root, up to the tip; else a fraction of the length now.
        const float distance = i.pinToRest && i.restDistance >= 0.0f ? std::min(i.restDistance, length)
                                                                      : std::clamp(i.t, 0.0f, 1.0f) * length;
        Vec3f pos, tangent;
        std::size_t segment = 0;
        float u = 0.0f;
        sampleStrand(pts, n, distance, &pos, &tangent, &segment, &u);
        // Fixed: `scale` on every axis. Fitted: a fraction of the rest length (the length now
        // until it is known) along +Y, `aspect` of that across; the width factor goes across.
        float along = i.scale;
        if (i.fitStrand) along *= i.restLength >= 0.0f ? i.restLength : length;
        float across = i.fitStrand ? along * i.aspect : along;
        const std::size_t begin = strands.strandBegin(s);
        if (i.scaleByWidth && begin + n <= widths.size()) {
            const float root = widths[begin];
            const float here = segment == 0 ? root : widths[begin + segment - 1] * (1.0f - u) + widths[begin + segment] * u;
            if (root > 0.0f && std::isfinite(here)) across *= std::max(here, 0.0f) / root;
        }
        if (!i.fitStrand) along = across;
        Vec3f y;
        switch (i.orient) {
            case InstanceOrient::Strand: y = tangent; break;
            case InstanceOrient::Up: y = {0, 1, 0}; break;
            case InstanceOrient::Root: y = n > 1 ? (pts[1] - pts[0]).normalized() : tangent; break;
            case InstanceOrient::Random: y = i.direction.normalized(); break;
        }
        if (y.length() < 1e-6f) y = {0, 1, 0};
        Vec3f x = anyPerpendicular(y).normalized();
        if (!i.deform && (i.orient == InstanceOrient::Strand || i.orient == InstanceOrient::Root) && n > 1) {
            // The roll from the root: across the root segment's lean off the skin (any way across
            // when it stands straight up), carried to y by the least turn. anyPerpendicular(y) alone
            // picks its axis by y's smallest world component, so the roll would jump 90 degrees as
            // a strand turns past one (every frame under Wind or Simulate); the root segment barely
            // moves.
            const Vec3f t0 = (pts[1] - pts[0]).normalized();
            if (t0.length() > 0.5f) {
                Vec3f side = anyPerpendicular(t0).normalized();
                const StrandRoot& root = strands.roots()[s];
                if (scalp && scalp->contains(root)) {
                    const Vec3f lean = t0.cross(scalp->normal(root));
                    if (lean.length() > 1e-3f) side = lean.normalized();
                }
                const Vec3f axis = t0.cross(y);
                const float sine = axis.length(), cosine = std::clamp(t0.dot(y), -1.0f, 1.0f);
                if (sine > 1e-6f) {
                    const Vec3f k = axis * (1.0f / sine);  // Rodrigues: side turned about k by the angle t0 to y
                    side = side * cosine + k.cross(side) * sine + k * (k.dot(side) * (1.0f - cosine));
                }
                side = side - y * side.dot(y);
                if (side.length() > 1e-6f) x = side.normalized();
            }
        }
        if (i.deform) {
            // Bent along the strand: +Y its tangent, +Z the vane's normal (the skin's at the root
            // without one), so a leaf or a feather's vane faces the way the strand's own does.
            y = tangent.length() > 1e-6f ? tangent : Vec3f{0, 1, 0};
            Vec3f up;
            if (hasVaneNormals) up = {(*vaneNormals[0])[s], (*vaneNormals[1])[s], (*vaneNormals[2])[s]};
            const StrandRoot& root = strands.roots()[s];
            if (!(up.length() > 1e-6f) && scalp && scalp->contains(root)) up = scalp->normal(root);
            up = up - y * up.dot(y);
            if (up.length() > 1e-6f) x = y.cross(up.normalized()).normalized();
        }
        const float c = pm::cos(i.spin), sn = pm::sin(i.spin);
        x = (x * c + y.cross(x) * sn).normalized();  // spin about y
        const Vec3f z = x.cross(y).normalized();
        Placed p;
        p.prototype = i.prototype;
        p.id = i.id;
        p.index = k;
        p.x = x * across;
        p.y = y * along;
        p.z = z * across;
        p.origin = pos + x * i.offset;
        p.meshed = i.deform;
        p.distance = distance;
        all[k] = p;
        placedOne[k] = 1;
    }, 256);
    out.reserve(instances.size());
    for (std::size_t k = 0; k < all.size(); ++k) {
        if (placedOne[k]) out.push_back(all[k]);
    }
    return out;
}

}  // namespace groomlab
