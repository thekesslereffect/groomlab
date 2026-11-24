// SPDX-License-Identifier: Apache-2.0
// Sculpt nodes' edits (sculpt.h).

#include "groomlab/portable_math.h"
#include "groomlab/sculpt.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "groomlab/curve.h"
#include "groomlab/frames.h"
#include "groomlab/random.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

using json = nlohmann::json;

namespace {

constexpr float kMoved = 1e-4f;      // a move that counts as sculpted, as a share of the guide's length
constexpr float kIdentity = 1e-6f;   // a turn (radians) or length factor this close to none is none
constexpr float kMinStrength = 1e-3f;

// The segment frames of a polyline (tangent along each segment), rotation-minimizing from a root
// frame (its unit normal, and an edge across it). Zero-length segments keep the frame before them.
std::vector<Frame> segmentFramesFrom(const std::vector<Vec3f>& pts, const Vec3f& normal, Vec3f edge) {
    const std::size_t segments = pts.size() < 2 ? 0 : pts.size() - 1;
    std::vector<Frame> frames(segments);
    if (segments == 0) return frames;
    edge = edge - normal * edge.dot(normal);
    edge = edge.length() > 1e-12f ? edge.normalized() : anyPerpendicular(normal);
    Vec3f tangent = normal;  // the direction before the first segment: the normal
    Vec3f n = edge;
    for (std::size_t j = 0; j < segments; ++j) {
        const Vec3f d = pts[j + 1] - pts[j];
        const float len = d.length();
        const Vec3f t = len > 0.0f ? d / len : tangent;
        n = Rotation(tangent, t)(n);
        n = n - t * n.dot(t);
        const float nl = n.length();
        n = nl > 1e-12f ? n / nl : anyPerpendicular(t);
        frames[j].tangent = t;
        frames[j].normal = n;
        frames[j].binormal = t.cross(n);
        tangent = t;
    }
    return frames;
}

// The root's surface frame (sculpt.h): the scalp's normal there and its triangle's first edge;
// world +Y and +X off the scalp.
std::vector<Frame> segmentFrames(const std::vector<Vec3f>& pts, const StrandRoot& root, const ScalpMesh* scalp) {
    Vec3f normal(0.0f, 1.0f, 0.0f), edge(1.0f, 0.0f, 0.0f);
    if (scalp && scalp->contains(root)) {
        normal = scalp->normal(root);
        const auto& tri = scalp->triangles()[static_cast<std::size_t>(root.face)];
        edge = scalp->points()[tri[1]] - scalp->points()[tri[0]];
    }
    return segmentFramesFrom(pts, normal, edge);
}

// The normalized arc length of each segment's middle; false for a guide of no length.
bool segmentMiddles(const std::vector<Vec3f>& pts, std::vector<float>& t, std::vector<float>& lengths) {
    const std::size_t segments = pts.size() < 2 ? 0 : pts.size() - 1;
    t.assign(segments, 0.0f);
    lengths.assign(segments, 0.0f);
    double total = 0.0;
    for (std::size_t j = 0; j < segments; ++j) {
        lengths[j] = (pts[j + 1] - pts[j]).length();
        total += lengths[j];
    }
    if (!(total > 0.0)) return false;
    double along = 0.0;
    for (std::size_t j = 0; j < segments; ++j) {
        t[j] = static_cast<float>((along + 0.5 * lengths[j]) / total);
        along += lengths[j];
    }
    return true;
}

// z turned by the axis-angle (ax, ay, 0).
Vec3f turnedZ(float ax, float ay) {
    const float angle = std::sqrt(ax * ax + ay * ay);
    if (angle < 1e-12f) return {0.0f, 0.0f, 1.0f};
    const float s = pm::sin(angle) / angle;
    return {ay * s, -ax * s, pm::cos(angle)};
}

json guideJson(const GuideSculpt& g) {
    json flat = json::array();
    for (const SculptSample& s : g.samples) flat.insert(flat.end(), {s.t, s.ax, s.ay, s.scale});
    return flat;
}

}  // namespace

bool GuideSculpt::identity() const {
    return std::all_of(samples.begin(), samples.end(), [](const SculptSample& s) {
        return std::abs(s.ax) <= kIdentity && std::abs(s.ay) <= kIdentity && std::abs(s.scale - 1.0f) <= kIdentity;
    });
}

SculptSample GuideSculpt::at(float t) const {
    if (samples.empty()) return {t, 0.0f, 0.0f, 1.0f};
    if (t <= samples.front().t) return samples.front();
    if (t >= samples.back().t) return samples.back();
    const auto next = std::upper_bound(samples.begin(), samples.end(), t,
                                       [](float x, const SculptSample& s) { return x < s.t; });
    const SculptSample& b = *next;
    const SculptSample& a = *(next - 1);
    const float span = b.t - a.t;
    const float w = span > 0.0f ? (t - a.t) / span : 0.0f;
    if (w == 0.0f) return a;
    return {t, a.ax + (b.ax - a.ax) * w, a.ay + (b.ay - a.ay) * w, a.scale + (b.scale - a.scale) * w};
}

std::uint64_t SculptEdits::contentHash() const {
    std::uint64_t h = hash64(0x5C0197);
    for (const auto& [id, g] : guides) {
        h = hashCombine(h, id);
        h = hashCombine(h, g.samples.size());
        for (const SculptSample& s : g.samples) h = hashFloat(hashFloat(hashFloat(hashFloat(h, s.t), s.ax), s.ay), s.scale);
    }
    for (const auto& [id, v] : frozen) h = hashFloat(hashCombine(hashCombine(h, 0xF0F0), id), v);
    for (const auto& [id, s] : strands) {
        h = hashCombine(hashCombine(h, 0x57A4D), id);
        h = hashFloat(hashFloat(hashFloat(hashCombine(h, static_cast<std::uint64_t>(s.root.face)), s.root.u), s.root.v), s.spacing);
        h = hashCombine(h, s.edit.samples.size());
        for (const SculptSample& x : s.edit.samples) h = hashFloat(hashFloat(hashFloat(hashFloat(h, x.t), x.ax), x.ay), x.scale);
    }
    return h;
}

bool SculptSet::empty() const {
    return std::all_of(nodes.begin(), nodes.end(), [](const auto& n) { return n.second.empty(); });
}

const SculptEdits* SculptSet::node(const std::string& name) const {
    const auto it = nodes.find(name);
    return it == nodes.end() ? nullptr : &it->second;
}

std::string SculptSet::toJson() const {
    json out = json::object();
    for (const auto& [name, edits] : nodes) {
        if (edits.empty()) continue;
        json guides = json::object();
        for (const auto& [id, g] : edits.guides) guides[std::to_string(id)] = guideJson(g);
        json node = {{"guides", std::move(guides)}};
        if (!edits.frozen.empty()) {
            json frozen = json::object();
            for (const auto& [id, v] : edits.frozen) frozen[std::to_string(id)] = v;
            node["frozen"] = std::move(frozen);
        }
        if (!edits.strands.empty()) {
            json strands = json::object();
            for (const auto& [id, s] : edits.strands) {
                json flat = {s.root.face, s.root.u, s.root.v, s.spacing};
                for (const SculptSample& x : s.edit.samples) flat.insert(flat.end(), {x.t, x.ax, x.ay, x.scale});
                strands[std::to_string(id)] = std::move(flat);
            }
            node["strands"] = std::move(strands);
        }
        out[name] = std::move(node);
    }
    return out.dump();
}

SculptSet SculptSet::fromJson(const std::string& text) {
    SculptSet set;
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return set;
    try {
        const json root = json::parse(text);
        if (!root.is_object()) throw std::invalid_argument("not an object");
        for (auto node = root.begin(); node != root.end(); ++node) {
            SculptEdits edits;
            const json guides = node.value().value("guides", json::object());
            for (auto it = guides.begin(); it != guides.end(); ++it) {
                const json& flat = it.value();
                GuideSculpt g;
                for (std::size_t i = 0; i + 3 < flat.size(); i += 4) {
                    g.samples.push_back({flat[i].get<float>(), flat[i + 1].get<float>(), flat[i + 2].get<float>(),
                                         flat[i + 3].get<float>()});
                }
                std::sort(g.samples.begin(), g.samples.end(),
                          [](const SculptSample& a, const SculptSample& b) { return a.t < b.t; });
                if (!g.samples.empty()) edits.guides[std::stoull(it.key())] = std::move(g);
            }
            const json frozen = node.value().value("frozen", json::object());
            for (auto it = frozen.begin(); it != frozen.end(); ++it) {
                const float v = it.value().get<float>();
                if (v > 0.0f) edits.frozen[std::stoull(it.key())] = v;
            }
            const json strands = node.value().value("strands", json::object());
            for (auto it = strands.begin(); it != strands.end(); ++it) {
                const json& flat = it.value();
                if (!flat.is_array() || flat.size() < 4) throw std::invalid_argument("strand " + it.key() + " has no root");
                StrandSculpt s;
                s.root = {flat[0].get<std::int32_t>(), flat[1].get<float>(), flat[2].get<float>()};
                s.spacing = flat[3].get<float>();
                for (std::size_t i = 4; i + 3 < flat.size(); i += 4) {
                    s.edit.samples.push_back({flat[i].get<float>(), flat[i + 1].get<float>(), flat[i + 2].get<float>(),
                                              flat[i + 3].get<float>()});
                }
                std::sort(s.edit.samples.begin(), s.edit.samples.end(),
                          [](const SculptSample& a, const SculptSample& b) { return a.t < b.t; });
                edits.strands[std::stoull(it.key())] = std::move(s);  // (an empty edit too: brushed, left as it was)
            }
            if (!edits.empty()) set.nodes[node.key()] = std::move(edits);
        }
    } catch (const std::exception& e) {
        throw std::invalid_argument(std::string("sculptData: ") + e.what());
    }
    return set;
}

namespace {

// The node path `path` takes when node `from` becomes `to`: `to` for `from` itself, "to/x" for
// "from/x" (a node inside it); "" for any other.
std::string movedPath(const std::string& path, const std::string& from, const std::string& to) {
    if (path == from) return to;
    if (path.size() > from.size() && path.compare(0, from.size(), from) == 0 && path[from.size()] == '/') {
        return to + path.substr(from.size());
    }
    return {};
}

// The edits of `from` and the nodes inside it copied (`keep`) or moved to `to`; false when none.
bool carry(SculptSet& set, const std::string& from, const std::string& to, bool keep) {
    if (from.empty() || from == to) return false;
    std::vector<std::pair<std::string, SculptEdits>> carried;
    for (auto it = set.nodes.begin(); it != set.nodes.end();) {
        const std::string path = movedPath(it->first, from, to);
        if (path.empty()) {
            ++it;
            continue;
        }
        if (keep) {
            carried.emplace_back(path, it->second);
            ++it;
        } else {
            carried.emplace_back(path, std::move(it->second));
            it = set.nodes.erase(it);
        }
    }
    for (auto& [path, edits] : carried) set.nodes[path] = std::move(edits);
    return !carried.empty();
}

}  // namespace

std::string renameSculptNode(const std::string& text, const std::string& from, const std::string& to) {
    SculptSet set = SculptSet::fromJson(text);
    return carry(set, from, to, false) ? set.toJson() : text;
}

std::string copySculptNode(const std::string& text, const std::string& from, const std::string& to) {
    SculptSet set = SculptSet::fromJson(text);
    return carry(set, from, to, true) ? set.toJson() : text;
}

std::string moveSculptNodes(const std::string& text, const std::vector<std::pair<std::string, std::string>>& moves) {
    SculptSet set = SculptSet::fromJson(text);
    bool changed = false;
    for (const auto& [from, to] : moves) changed = carry(set, from, to, false) || changed;
    return changed ? set.toJson() : text;
}

std::string clearSculptNode(const std::string& text, const std::string& name) {
    SculptSet set = SculptSet::fromJson(text);
    if (!set.nodes.erase(name)) return text;
    return set.toJson();
}

std::vector<std::string> sculptedNodes(const std::string& text) {
    std::vector<std::string> names;
    for (const auto& [name, edits] : SculptSet::fromJson(text).nodes) {
        if (!edits.empty()) names.push_back(name);
    }
    return names;
}

std::string sculptExternal(const std::string& nodePath) { return "sculpt:" + nodePath; }

const char* const kPathFrameLayers[10] = {"braidPathAt",  "braidPathX",    "braidPathY",    "braidPathZ",    "braidNormalX",
                                          "braidNormalY", "braidNormalZ", "braidTangentX", "braidTangentY", "braidTangentZ"};

namespace {

// A guide's path frame per point (kPathFrameLayers): the arc length, the point and the frame.
struct PathFrames {
    std::vector<float> at;
    std::vector<Vec3f> center;
    std::vector<Frame> frames;

    // A point (of index j) in the path's straightened space, and back.
    Vec3f straight(std::size_t j, const Vec3f& p) const {
        const Vec3f l = frames[j].toLocal(p - center[j]);
        return {l.x, l.y, at[j] + l.z};
    }
    Vec3f onPath(std::size_t j, const Vec3f& q) const { return center[j] + frames[j].toWorld({q.x, q.y, q.z - at[j]}); }
};

bool pathFramesOf(const Guide& g, PathFrames& out) {
    const std::size_t n = g.points.size();
    const std::vector<float>* layers[10];
    for (std::size_t k = 0; k < 10; ++k) {
        const auto it = g.pointAttributes.find(kPathFrameLayers[k]);
        if (it == g.pointAttributes.end() || it->second.size() != n) return false;
        layers[k] = &it->second;
    }
    out.at = *layers[0];
    out.center.resize(n);
    out.frames.resize(n);
    for (std::size_t j = 0; j < n; ++j) {
        const auto v = [&](std::size_t k) { return Vec3f{(*layers[k])[j], (*layers[k + 1])[j], (*layers[k + 2])[j]}; };
        out.center[j] = v(1);
        Frame& f = out.frames[j];
        f.tangent = v(7).normalized();
        Vec3f normal = v(4);
        normal = normal - f.tangent * normal.dot(f.tangent);
        if (!(f.tangent.length() > 0.5f) || !(normal.length() > 1e-6f)) return false;
        f.normal = normal.normalized();
        f.binormal = f.tangent.cross(f.normal);
    }
    return true;
}

// The relative form laid on `pts` in its segments' frames (`framesOf(pts)`, made only when needed).
template <class FramesOf>
std::vector<Vec3f> applySculptPts(const std::vector<Vec3f>& pts, const GuideSculpt& edit, float strength, FramesOf framesOf) {
    std::vector<float> t, lengths;
    if (pts.size() < 2 || edit.samples.empty() || strength == 0.0f || !segmentMiddles(pts, t, lengths)) return pts;
    const std::vector<Frame> frames = framesOf(pts);
    std::vector<Vec3f> out(pts.size());
    out[0] = pts[0];
    for (std::size_t j = 0; j + 1 < pts.size(); ++j) {
        if (lengths[j] <= 0.0f) {
            out[j + 1] = out[j];
            continue;
        }
        const SculptSample s = edit.at(t[j]);
        const Vec3f local = turnedZ(s.ax * strength, s.ay * strength);
        const float scale = std::max(0.0f, 1.0f + (s.scale - 1.0f) * strength);
        out[j + 1] = out[j] + frames[j].toWorld(local) * (lengths[j] * scale);
    }
    return out;
}

std::vector<Vec3f> applySculptIn(const Guide& input, const GuideSculpt& edit, float strength, const ScalpMesh* scalp) {
    return applySculptPts(input.points, edit, strength,
                          [&](const std::vector<Vec3f>& pts) { return segmentFrames(pts, input.root, scalp); });
}

// The relative form taking `pts` to `sculpted` in `pts`' segments' frames (`framesOf(pts)`).
template <class FramesOf>
GuideSculpt recordSculptPts(const std::vector<Vec3f>& pts, const std::vector<Vec3f>& sculpted, float strength, FramesOf framesOf) {
    if (std::abs(strength) < kMinStrength) {
        throw std::invalid_argument("the Sculpt node's strength is 0: raise it to sculpt");
    }
    if (sculpted.size() != pts.size()) throw std::invalid_argument("a sculpted guide has another point count than its input");
    GuideSculpt edit;
    std::vector<float> t, lengths;
    if (pts.size() < 2 || !segmentMiddles(pts, t, lengths)) return edit;
    const std::vector<Frame> frames = framesOf(pts);
    edit.samples.reserve(pts.size() - 1);
    for (std::size_t j = 0; j + 1 < pts.size(); ++j) {
        SculptSample s;
        s.t = t[j];
        const Vec3f e = sculpted[j + 1] - sculpted[j];
        const float len = e.length();
        if (lengths[j] > 0.0f && len > 0.0f) {
            const Vec3f l = frames[j].toLocal(e / len);
            const float across = std::sqrt(l.x * l.x + l.y * l.y);
            const float angle = pm::atan2(across, l.z);
            // The axis z x l = (-l.y, l.x, 0), its length `across`: the angle along it.
            if (across > 1e-12f) {
                s.ax = -l.y / across * angle;
                s.ay = l.x / across * angle;
            }
            s.scale = len / lengths[j];
        } else if (lengths[j] > 0.0f) {
            s.scale = 0.0f;
        }
        s.ax /= strength;
        s.ay /= strength;
        s.scale = 1.0f + (s.scale - 1.0f) / strength;
        edit.samples.push_back(s);
    }
    // Two samples at the same t (zero-length segments): only the first is read.
    return edit;
}

GuideSculpt recordSculptIn(const Guide& input, const std::vector<Vec3f>& sculpted, float strength, const ScalpMesh* scalp) {
    return recordSculptPts(input.points, sculpted, strength,
                           [&](const std::vector<Vec3f>& pts) { return segmentFrames(pts, input.root, scalp); });
}

// The guide in its path's straightened space (its root for no scalp: world axes there).
Guide straightened(const Guide& input, const PathFrames& path, const std::vector<Vec3f>& points) {
    Guide flat;
    flat.id = input.id;
    flat.points.resize(points.size());
    for (std::size_t j = 0; j < points.size(); ++j) flat.points[j] = path.straight(j, points[j]);
    return flat;
}

}  // namespace

std::vector<Vec3f> applySculpt(const Guide& input, const GuideSculpt& edit, float strength, const ScalpMesh* scalp) {
    PathFrames path;
    if (edit.samples.empty() || strength == 0.0f || !pathFramesOf(input, path)) return applySculptIn(input, edit, strength, scalp);
    std::vector<Vec3f> out = applySculptIn(straightened(input, path, input.points), edit, strength, nullptr);
    for (std::size_t j = 1; j < out.size(); ++j) out[j] = path.onPath(j, out[j]);
    if (!out.empty()) out[0] = input.points[0];  // (the root to the bit)
    return out;
}

GuideSculpt recordSculpt(const Guide& input, const std::vector<Vec3f>& sculpted, float strength, const ScalpMesh* scalp) {
    PathFrames path;
    if (sculpted.size() != input.points.size() || !pathFramesOf(input, path)) return recordSculptIn(input, sculpted, strength, scalp);
    return recordSculptIn(straightened(input, path, input.points), straightened(input, path, sculpted).points, strength, nullptr);
}

std::vector<Vec3f> applySculptFramed(const std::vector<Vec3f>& points, const GuideSculpt& edit, float strength, const Vec3f& normal,
                                     const Vec3f& across) {
    return applySculptPts(points, edit, strength, [&](const std::vector<Vec3f>& pts) { return segmentFramesFrom(pts, normal, across); });
}

GuideSculpt recordSculptFramed(const std::vector<Vec3f>& points, const std::vector<Vec3f>& sculpted, float strength, const Vec3f& normal,
                               const Vec3f& across) {
    return recordSculptPts(points, sculpted, strength, [&](const std::vector<Vec3f>& pts) { return segmentFramesFrom(pts, normal, across); });
}

std::shared_ptr<const GuideSet> applySculpts(const std::shared_ptr<const GuideSet>& input, const SculptEdits& edits,
                                             float strength, const ScalpMesh* scalp) {
    if (!input || edits.empty() || strength == 0.0f) return input;
    std::shared_ptr<GuideSet> out;
    for (std::size_t i = 0; i < input->size(); ++i) {
        const Guide& g = input->guides()[i];
        const auto it = edits.guides.find(g.id);
        if (it == edits.guides.end()) continue;
        if (!out) out = std::make_shared<GuideSet>(*input);
        out->guides()[i].points = applySculpt(g, it->second, strength, scalp);
    }
    return out ? std::shared_ptr<const GuideSet>(out) : input;
}

namespace {

// One guide of a stroke's result recorded into `edits` (recordSculptStroke); whether it was.
bool recordStrokeGuide(SculptEdits& edits, const Guide& g, const HierarchyGuide* s, float strength, const ScalpMesh* scalp) {
    if (!s || s->points.size() != g.points.size() || g.points.size() < 2) return false;
    const auto it = edits.guides.find(g.id);
    const std::vector<Vec3f> now = it == edits.guides.end() ? g.points : applySculpt(g, it->second, strength, scalp);
    // Compared segment by segment (the root is the input's: a stroke never moves it).
    const float moved = kMoved * arcLength(now);
    bool changed = false;
    for (std::size_t k = 1; k < now.size() && !changed; ++k) {
        const Vec3f d = (s->points[k] - s->points[0]) - (now[k] - now[0]);
        changed = d.dot(d) > moved * moved;
    }
    if (!changed) return false;
    GuideSculpt edit = recordSculpt(g, s->points, strength, scalp);
    if (edit.identity()) {
        edits.guides.erase(g.id);
    } else {
        edits.guides[g.id] = std::move(edit);
    }
    return true;
}

// Whether two guides' points are the same bits (a -0 for a 0 records another edit).
bool samePoints(const std::vector<Vec3f>& a, const std::vector<Vec3f>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Vec3f)) == 0);
}

}  // namespace

std::vector<GuideId> recordSculptStroke(SculptEdits& edits, const GuideSet& input, const GuideHierarchy& sculpted,
                                        float strength, const ScalpMesh* scalp) {
    if (std::abs(strength) < kMinStrength) {
        throw std::invalid_argument("the Sculpt node's strength is 0: raise it to sculpt");
    }
    std::vector<GuideId> recorded;
    for (const Guide& g : input.guides()) {
        if (recordStrokeGuide(edits, g, sculpted.guide(g.id), strength, scalp)) recorded.push_back(g.id);
    }
    return recorded;
}

void SculptStrokeRecord::clear() {
    seen_.clear();
    started_ = false;
}

void SculptStrokeRecord::record(SculptEdits& edits, const SculptEdits& stored, const GuideSet& input, const GuideHierarchy& sculpted,
                                float strength, const ScalpMesh* scalp) {
    if (std::abs(strength) < kMinStrength) {
        throw std::invalid_argument("the Sculpt node's strength is 0: raise it to sculpt");
    }
    const std::vector<Guide>& guides = input.guides();
    static const std::vector<Vec3f> none;
    const auto shown = [&](const Guide& g) -> const std::vector<Vec3f>& {
        const HierarchyGuide* s = sculpted.guide(g.id);
        return s ? s->points : none;
    };
    if (!started_ || seen_.size() != guides.size()) {
        // The first dab: every guide, from the stored edits.
        edits = stored;
        seen_.assign(guides.size(), {});
        started_ = true;
        try {
            for (std::size_t i = 0; i < guides.size(); ++i) {
                recordStrokeGuide(edits, guides[i], sculpted.guide(guides[i].id), strength, scalp);
                seen_[i] = shown(guides[i]);
            }
        } catch (...) {
            clear();
            throw;
        }
        return;
    }
    // A guide's edit follows from its input, its stored edit and its sculpted points alone: only
    // the guides whose points changed since the last dab are recorded again, from their stored
    // edits and in the input's order (guides sharing an ID change together, and record in turn).
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < guides.size(); ++i) {
        if (!samePoints(seen_[i], shown(guides[i]))) changed.push_back(i);
    }
    try {
        for (const std::size_t i : changed) {
            const auto it = stored.guides.find(guides[i].id);
            if (it == stored.guides.end()) {
                edits.guides.erase(guides[i].id);
            } else {
                edits.guides[guides[i].id] = it->second;
            }
        }
        for (const std::size_t i : changed) {
            recordStrokeGuide(edits, guides[i], sculpted.guide(guides[i].id), strength, scalp);
            seen_[i] = shown(guides[i]);
        }
    } catch (...) {
        clear();
        throw;
    }
}

void layFrozen(GuideHierarchy& h, const SculptEdits& edits, const std::string& freeze) {
    for (const auto& [id, v] : edits.frozen) {
        if (h.guide(id)) h.setAttribute(id, freeze, v);
    }
}

std::map<std::uint64_t, float> frozenOf(const GuideHierarchy& h, const std::string& freeze) {
    std::map<std::uint64_t, float> out;
    for (const HierarchyGuide& g : h.guides()) {
        const auto it = g.attributes.find(freeze);
        if (it != g.attributes.end() && it->second > 0.0f) out[g.id] = std::min(it->second, 1.0f);
    }
    return out;
}

std::shared_ptr<GuideHierarchy> sculptHierarchy(const GuideSet& guides, const std::string& group, const ScalpMesh* scalp) {
    auto h = std::make_shared<GuideHierarchy>();
    const GroupId root = h->addGroup(group.empty() ? std::string("sculpt") : group);
    if (guides.hasDisplayColor()) {
        const Vec3f& c = guides.displayColor();
        h->setGroupColor(root, {c.x, c.y, c.z});
    }
    // Each where the node has it: a guide fed off the scalp (a Braid's sections start where its
    // weave does) is shown and brushed there, not moved onto the scalp (the node never moves a root).
    // (The path frames a Braid's sections carry are the edits' frames, not layers to brush or show.)
    const bool framed = std::any_of(guides.guides().begin(), guides.guides().end(), [](const Guide& g) {
        return g.pointAttributes.count(kPathFrameLayers[0]) > 0;
    });
    std::vector<GuideId> added;
    if (framed) {
        GuideSet shown = guides;
        for (Guide& g : shown.guides()) {
            for (const char* layer : kPathFrameLayers) g.pointAttributes.erase(layer);
        }
        added = h->addGuides(root, shown);
    } else {
        added = h->addGuides(root, guides);  // (none to leave out: no copy of a large set)
    }
    for (const GuideId id : added) h->setKeepsPlace(id);
    if (scalp && scalp->triangleCount() > 0) h->evaluate(scalp);
    return h;
}

}  // namespace groomlab
