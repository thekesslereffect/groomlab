// SPDX-License-Identifier: Apache-2.0
// GroomDocument's tube edits (groom_document.h, "Tubes"): Groom Tools' Tubes section over the
// core's tubes (tube.h) and the hierarchy's shape library (guide_hierarchy.h), as Maya's
// groomLabGuides -setTube / -tubeTargets / -editTubeShape and Blender's helper (tube_*) do them.

#include <algorithm>
#include <map>
#include <stdexcept>

#include "groom_document.h"
#include "groomlab/tube.h"

namespace groomlab::app {

using nlohmann::json;

namespace {

constexpr const char* kFreeze = "freeze";  // the freeze mask the brushes' solo sets (brush.h)

}  // namespace

GroomDocument::TubeTargets GroomDocument::tubeTargets() const {
    TubeTargets out;
    const std::shared_ptr<const GuideHierarchy> h = session().storedHierarchy();
    if (!h) return out;
    out.scale = h->scale();
    const groomlab::TubeTargets targets = tubeTargetsOf(*h, std::vector<GuideId>(selection_.begin(), selection_.end()), kFreeze);
    out.source = targets.source;
    out.guides = targets.guides;
    out.shapes = targets.shapes;
    out.midShapes = targets.midShapes;
    out.tipShapes = targets.tipShapes;
    out.inUse = targets.inUse;
    if (!out.guides.empty()) out.tube = h->guide(out.guides.front())->tube;
    return out;
}

std::vector<GroomDocument::TubeShapeInfo> GroomDocument::tubeShapes() const {
    std::vector<TubeShapeInfo> out;
    for (const std::string& name : tubeShapeNames()) {
        TubeShape shape = TubeShape::Round;
        parseTubeShape(name, &shape);
        TubeShapeInfo info;
        info.name = name;
        info.points = builtinTubeShapePoints(shape, &info.smooth);
        out.push_back(std::move(info));
    }
    if (const std::shared_ptr<const GuideHierarchy> h = session().storedHierarchy()) {
        for (const auto& [name, shape] : h->tubeShapes()) {
            TubeShapeInfo info;
            info.name = name;
            info.builtin = false;
            info.points = shape->points;
            info.smooth = shape->smooth;
            info.users = h->tubeShapeUsers(name);
            out.push_back(std::move(info));
        }
    }
    return out;
}

std::string GroomDocument::editTubes(const std::vector<GuideId>& guides, const json& edit, const std::string& label) {
    if (guides.empty()) return "no guides selected or soloed to edit";
    endTubePreview();
    const std::string text = edit.dump();
    std::size_t changed = 0;
    const std::string error = editGuides(label, [&](GuideHierarchy& h) {
        for (const GuideId id : guides) {
            const HierarchyGuide* g = h.guide(id);
            if (!g) continue;
            h.setTube(id, editedTube(g->tube, text, h.scale(), g->points.size(), &h.tubeShapes()));
            ++changed;
        }
        if (changed == 0) throw std::invalid_argument("none of those guides is in the groom");
    });
    return error;
}

std::string GroomDocument::scaleTubes(const std::vector<GuideId>& guides, float factor) {
    if (!(factor >= 0.0f)) return "the scale is 0 or more";
    return editTubes(guides, json{{"scaleWidth", factor}}, "scale tube");
}

std::string GroomDocument::smoothTubes(const std::vector<GuideId>& guides, const std::string& channel, int passes) {
    // The Shape brush's channel names, and what the tube calls each list.
    static const std::map<std::string, std::string> lists = {
        {"width", "width"}, {"flatness", "aspect"}, {"aspect", "aspect"}, {"twist", "twist"}, {"all", "all"}};
    const auto list = lists.find(channel);
    if (list == lists.end()) return "the channel is width, flatness, twist or all";
    return editTubes(guides, json{{"smooth", list->second}, {"passes", std::clamp(passes, 1, 100)}}, "smooth tube");
}

std::string GroomDocument::addTubeShape(const std::string& name, const std::vector<SectionPoint>& points, bool smooth,
                                        const std::vector<GuideId>& give, const std::string& profile) {
    endTubePreview();
    return editGuides(give.empty() ? "add tube shape" : "tube shape", [&](GuideHierarchy& h) {
        if (h.tubeShape(name)) throw std::invalid_argument("a tube shape named '" + name + "' exists");
        h.setTubeShape(name, points, smooth);
        const std::string edit = json{{profile, name}}.dump();
        for (const GuideId id : give) {
            if (const HierarchyGuide* g = h.guide(id)) h.setTube(id, editedTube(g->tube, edit, h.scale(), g->points.size(), &h.tubeShapes()));
        }
    });
}

std::string GroomDocument::setTubeShape(const std::string& name, const std::vector<SectionPoint>* points, std::optional<bool> smooth) {
    endTubePreview();
    return editGuides("edit tube shape", [&](GuideHierarchy& h) {
        const CustomTubeShapePtr old = h.tubeShape(name);
        if (!old) throw std::invalid_argument("no tube shape named '" + name + "'");
        h.setTubeShape(name, points ? *points : old->points, smooth ? *smooth : old->smooth);
    });
}

std::string GroomDocument::renameTubeShape(const std::string& from, const std::string& to) {
    endTubePreview();
    return editGuides("rename tube shape", [&](GuideHierarchy& h) {
        if (!h.tubeShape(from)) throw std::invalid_argument("no tube shape named '" + from + "'");
        h.renameTubeShape(from, to);
    });
}

std::string GroomDocument::removeTubeShape(const std::string& name, std::size_t* users) {
    endTubePreview();
    std::size_t count = 0;
    const std::string error = editGuides("remove tube shape", [&](GuideHierarchy& h) {
        if (!h.tubeShape(name)) throw std::invalid_argument("no tube shape named '" + name + "'");
        count = h.removeTubeShape(name);
    });
    if (users) *users = error.empty() ? count : 0;
    return error;
}

std::string GroomDocument::previewTubeShape(const std::string& name, const std::vector<SectionPoint>& points, bool smooth) {
    const HierarchyPtr authored = session().authoredHierarchy();
    if (!authored) return "the groom has no guides";
    auto preview = std::make_shared<GuideHierarchy>(*authored);
    try {
        preview->setTubeShape(name, points, smooth);
        preview->evaluate(session().scalp().get());
    } catch (const std::exception& e) {
        return e.what();
    }
    session().setPreviewHierarchy(preview);
    tubePreview_ = true;
    return {};
}

void GroomDocument::endTubePreview() {
    if (!tubePreview_) return;
    tubePreview_ = false;
    session().setPreviewHierarchy(nullptr);
}

std::optional<GroomDocument::FillTubes> GroomDocument::fillTubes() const {
    for (const NodeInfo& n : nodes()) {
        if (n.type != "Interpolate") continue;
        FillTubes out;
        const json fill = paramOrDefault(n.name, "fillTubes"), blend = paramOrDefault(n.name, "tubeBlend");
        out.fill = fill.is_boolean() && fill.get<bool>();
        if (blend.is_number()) out.blend = blend.get<float>();
        return out;
    }
    return std::nullopt;
}

std::string GroomDocument::setFillTubes(std::optional<bool> fill, std::optional<float> blend) {
    if (!fillTubes()) return "the groom's graph has no Interpolate node: nothing to fill the tubes";
    if (!fill && !blend) return {};
    return editGraph(fill ? "fill tubes" : "tube blend", [&](json& graph) {
        for (json& node : graph["nodes"]) {
            if (node.value("type", std::string()) != "Interpolate") continue;
            json& params = node["params"];
            if (fill) params["fillTubes"] = *fill;
            if (blend) params["tubeBlend"] = std::clamp(*blend, 0.0f, 1.0f);
        }
    });
}

}  // namespace groomlab::app
