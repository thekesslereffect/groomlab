// SPDX-License-Identifier: Apache-2.0
#include "groomlab/correctives.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "groomlab/curve.h"
#include "groomlab/frames.h"

namespace groomlab {

using json = nlohmann::json;

namespace {

constexpr float kMoved = 1e-4f;  // a move that counts as sculpted, as a share of the guide's length

// smoothstep from 0 to 1 of a share of a fade (clamped in double, then a float: the same).
float smoothstep01(double t) { return smoothstep(0.0f, 1.0f, static_cast<float>(t)); }

// Guides with offsets in any enabled layer weighted at `seconds`, by depth.
std::map<int, std::vector<GuideId>> correctedByDepth(const GuideHierarchy& h, const std::vector<CorrectiveLayer>& layers,
                                                     double seconds) {
    std::set<GuideId> ids;
    for (const CorrectiveLayer& l : layers) {
        if (!l.enabled || l.weightAt(seconds) <= 0.0f) continue;
        for (const auto& [id, offsets] : l.offsets) ids.insert(id);
    }
    std::map<int, std::vector<GuideId>> byDepth;
    for (const GuideId id : ids) {
        const HierarchyGuide* g = h.guide(id);
        if (g) byDepth[h.groupDepth(g->group)].push_back(id);
    }
    return byDepth;
}

}  // namespace

float CorrectiveLayer::weightAt(double seconds) const {
    if (!enabled) return 0.0f;
    if (!weightCurve.empty()) {
        if (seconds <= weightCurve.front().first) return weightCurve.front().second;
        if (seconds >= weightCurve.back().first) return weightCurve.back().second;
        const auto next = std::upper_bound(weightCurve.begin(), weightCurve.end(), seconds,
                                           [](double t, const std::pair<double, float>& k) { return t < k.first; });
        const auto prev = next - 1;
        const double span = next->first - prev->first;
        const float f = span > 0.0 ? static_cast<float>((seconds - prev->first) / span) : 1.0f;
        return prev->second + (next->second - prev->second) * f;
    }
    float w = 1.0f;
    if (seconds < time && !holdBefore) {
        w = fadeIn > 0.0 ? smoothstep01(1.0 - (time - seconds) / fadeIn) : (time - seconds < 1e-6 ? 1.0f : 0.0f);
    } else if (seconds > time && !holdAfter) {
        w = fadeOut > 0.0 ? smoothstep01(1.0 - (seconds - time) / fadeOut) : (seconds - time < 1e-6 ? 1.0f : 0.0f);
    }
    return w * weight;
}

bool Correctives::empty() const {
    return std::none_of(layers.begin(), layers.end(), [](const CorrectiveLayer& l) { return !l.offsets.empty(); });
}

CorrectiveLayer* Correctives::layer(const std::string& name) {
    for (CorrectiveLayer& l : layers) {
        if (l.name == name) return &l;
    }
    return nullptr;
}

const CorrectiveLayer* Correctives::layer(const std::string& name) const {
    return const_cast<Correctives*>(this)->layer(name);
}

std::string Correctives::toJson() const {
    json out = json::object();
    json list = json::array();
    for (const CorrectiveLayer& l : layers) {
        json offsets = json::object();
        for (const auto& [id, pts] : l.offsets) {
            json flat = json::array();
            for (const Vec3f& p : pts) flat.insert(flat.end(), {p.x, p.y, p.z});
            offsets[std::to_string(id)] = std::move(flat);
        }
        json entry = {{"name", l.name}, {"time", l.time}, {"fadeIn", l.fadeIn}, {"fadeOut", l.fadeOut},
                      {"holdBefore", l.holdBefore}, {"holdAfter", l.holdAfter}, {"weight", l.weight},
                      {"enabled", l.enabled}, {"offsets", std::move(offsets)}};
        if (l.slot >= 0) entry["slot"] = l.slot;
        if (!l.weightCurve.empty()) {
            json curve = json::array();
            for (const auto& [t, w] : l.weightCurve) curve.push_back({t, w});
            entry["weightCurve"] = std::move(curve);
        }
        list.push_back(std::move(entry));
    }
    out["layers"] = std::move(list);
    return out.dump();
}

Correctives Correctives::fromJson(const std::string& text) {
    Correctives c;
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return c;
    try {
        const json root = json::parse(text);
        for (const json& l : root.value("layers", json::array())) {
            CorrectiveLayer layer;
            layer.name = l.at("name").get<std::string>();
            layer.time = l.value("time", 0.0);
            layer.fadeIn = l.value("fadeIn", 0.5);
            layer.fadeOut = l.value("fadeOut", 0.5);
            layer.holdBefore = l.value("holdBefore", false);
            layer.holdAfter = l.value("holdAfter", false);
            layer.weight = l.value("weight", 1.0f);
            layer.enabled = l.value("enabled", true);
            layer.slot = l.value("slot", -1);
            for (const json& k : l.value("weightCurve", json::array())) {
                layer.weightCurve.emplace_back(k.at(0).get<double>(), k.at(1).get<float>());
            }
            std::sort(layer.weightCurve.begin(), layer.weightCurve.end());
            const json offsets = l.value("offsets", json::object());
            for (auto it = offsets.begin(); it != offsets.end(); ++it) {
                const std::string key = it.key();
                const json& flat = it.value();
                std::vector<Vec3f> pts;
                for (std::size_t i = 0; i + 2 < flat.size(); i += 3) {
                    pts.emplace_back(flat[i].get<float>(), flat[i + 1].get<float>(), flat[i + 2].get<float>());
                }
                layer.offsets[std::stoull(key)] = std::move(pts);
            }
            c.layers.push_back(std::move(layer));
        }
    } catch (const std::exception& e) {
        throw std::invalid_argument(std::string("correctives: ") + e.what());
    }
    return c;
}

void Correctives::apply(GuideHierarchy& h, double seconds, const ScalpMesh* scalp, const std::set<GuideId>* skipOwn) const {
    const float scale = h.scale();  // offsets are kept at the groom's own size (GuideHierarchy::setScale)
    for (const auto& [depth, ids] : correctedByDepth(h, layers, seconds)) {
        std::vector<GuideId> moved;
        for (const GuideId id : ids) {
            if (skipOwn && skipOwn->count(id)) continue;
            const HierarchyGuide* g = h.guide(id);
            const std::size_t n = g->points.size();
            if (n < 2) continue;
            const std::vector<Frame> frames = g->frames();
            std::vector<Vec3f> pts = g->points;
            bool any = false;
            for (const CorrectiveLayer& l : layers) {
                const float w = l.weightAt(seconds);
                auto it = l.offsets.find(id);
                if (w <= 0.0f || it == l.offsets.end()) continue;
                const std::size_t m = std::min(n, it->second.size());
                for (std::size_t k = 0; k < m; ++k) pts[k] += frames[k].toWorld(it->second[k] * scale) * w;
                any = true;
            }
            if (!any) continue;
            h.pose(id, std::move(pts));
            moved.push_back(id);
        }
        // Children follow their corrected parents before their own corrections (next depth).
        if (!moved.empty()) h.evaluateDescendants(scalp, moved);
    }
}

std::vector<GuideId> Correctives::record(const std::string& name, const GuideHierarchy& posed,
                                         const GuideHierarchy& sculpted, double seconds, const ScalpMesh* scalp) {
    if (!layer(name)) {
        CorrectiveLayer l;
        l.name = name;
        l.time = seconds;
        layers.push_back(std::move(l));
    }
    const float w = layer(name)->weightAt(seconds);
    if (w < 1e-3f) {
        throw std::invalid_argument("corrective layer '" + name + "' has no weight on this frame: raise its weight "
                                    "(and key it) to sculpt here");
    }
    std::vector<GuideId> recorded;
    const float scale = posed.scale();  // offsets are kept at the groom's own size (GuideHierarchy::setScale)
    const int deepest = posed.maxDepth();
    // Level by level: what a level's guides would be once the levels above are recorded.
    for (int depth = 0; depth <= deepest; ++depth) {
        GuideHierarchy current = posed;
        apply(current, seconds, scalp);
        std::set<GuideId> edited;
        for (const HierarchyGuide& g : sculpted.guides()) {
            if (sculpted.groupDepth(g.group) != depth) continue;
            const HierarchyGuide* c = current.guide(g.id);
            if (!c || c->points.size() != g.points.size()) continue;
            const float moved = kMoved * arcLength(c->points);  // a share: the same at any scene scale
            for (std::size_t k = 0; k < g.points.size(); ++k) {
                const Vec3f d = g.points[k] - c->points[k];
                if (d.dot(d) > moved * moved) {
                    edited.insert(g.id);
                    break;
                }
            }
        }
        if (edited.empty()) continue;
        GuideHierarchy base = posed;  // parents corrected, these guides' own corrections left out
        apply(base, seconds, scalp, &edited);
        CorrectiveLayer* target = layer(name);
        for (const GuideId id : edited) {
            const HierarchyGuide& b = *base.guide(id);
            const HierarchyGuide& s = *sculpted.guide(id);
            const std::vector<Frame> frames = b.frames();
            const std::size_t n = b.points.size();
            std::vector<Vec3f>& mine = target->offsets[id];
            mine.resize(n);
            for (std::size_t k = 0; k < n; ++k) {
                Vec3f local = frames[k].toLocal(s.points[k] - b.points[k]) * (1.0f / scale);  // every layer's share, at the groom's own size
                for (const CorrectiveLayer& other : layers) {
                    if (&other == target) continue;
                    auto it = other.offsets.find(id);
                    const float ow = other.weightAt(seconds);
                    if (ow > 0.0f && it != other.offsets.end() && k < it->second.size()) local -= it->second[k] * ow;
                }
                mine[k] = local / w;
            }
            recorded.push_back(id);
        }
    }
    return recorded;
}

void CorrectiveLayer::keyAt(double seconds, float w) {
    for (auto& [t, v] : weightCurve) {
        if (std::abs(t - seconds) < 1e-6) {
            v = w;
            return;
        }
    }
    weightCurve.emplace_back(seconds, w);
    std::sort(weightCurve.begin(), weightCurve.end());
}

std::string Correctives::freeLayerName(const std::string& wanted, const std::string& current,
                                       const std::vector<std::string>& taken) const {
    std::string trimmed = wanted;
    const auto first = trimmed.find_first_not_of(" \t");
    trimmed = first == std::string::npos ? std::string() : trimmed.substr(first, trimmed.find_last_not_of(" \t") - first + 1);
    std::string base;
    for (const char ch : trimmed) base += std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' ? ch : '_';
    if (base.empty()) base = "corrective";
    if (std::isdigit(static_cast<unsigned char>(base[0]))) base = "c" + base;
    auto isTaken = [&](const std::string& n) {
        if (n == current) return false;
        return layer(n) != nullptr || std::find(taken.begin(), taken.end(), n) != taken.end();
    };
    std::string name = base;
    for (int n = 2; isTaken(name); ++n) name = base + "_" + std::to_string(n);
    return name;
}

std::string Correctives::newLayerName(bool allFrames, double frame) const {
    return allFrames ? "sculpt" + std::to_string(layers.size() + 1) : "shot" + std::to_string(std::lround(frame));
}

}  // namespace groomlab
