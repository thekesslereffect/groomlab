// SPDX-License-Identifier: Apache-2.0
#include "groomlab/gpu/look_preview.h"

#include <algorithm>
#include <chrono>
#include <set>

#include "groomlab/operators.h"
#include "groomlab/presets.h"

namespace groomlab::gpu {

namespace {

bool sameChain(const LookChain& a, const LookChain& b) {
    if (a.stages.size() != b.stages.size() || a.base != b.base) return false;
    for (std::size_t k = 0; k < a.stages.size(); ++k) {
        if (a.stages[k].node != b.stages[k].node || a.stages[k].level != b.stages[k].level) return false;
    }
    return true;
}

// A drag's values are the stored ones (what the editors show). On a groom scaled with its scalp
// (GroomSession::groomScale) the document's params are at its scale (scaleGraphJson): the drag's
// numbers and vectors of a unit that scales are scaled alike (unitScaleFactor), a param's unit as
// its spec says while its unitWhen holds.
void scaleOverrides(const GraphDocument& doc, double k, Overrides& own) {
    if (k == 1.0) return;
    for (auto& [node, params] : own) {
        const auto it = doc.nodesByName.find(node);
        if (it == doc.nodesByName.end()) continue;
        const Operator& op = doc.graph.node(it->second);
        const std::vector<ParamSpec> specs = op.paramSpecs();
        for (auto& [name, value] : params) {
            const auto spec = std::find_if(specs.begin(), specs.end(), [&](const ParamSpec& s) { return s.name == name; });
            if (spec == specs.end()) continue;
            if (!spec->unitWhen.empty()) {
                const std::string text = op.params().getString(spec->unitWhen.param);  // (the conditions are on text params)
                if (std::find(spec->unitWhen.in.begin(), spec->unitWhen.in.end(), text) == spec->unitWhen.in.end()) continue;
            }
            const std::optional<double> f = unitScaleFactor(spec->unit, k);
            if (!f) continue;
            if (auto* d = std::get_if<double>(&value)) *d *= *f;
            if (auto* v = std::get_if<Vec3f>(&value)) *v = *v * static_cast<float>(*f);
        }
    }
}

}  // namespace

bool strokeShapesGuides(const GroomSession& session) {
    return session.previewHierarchy() != nullptr || (session.hasPreviewSculpt() && !session.hasPreviewOverlays());
}

bool LookPreview::plan(const GroomSession& session, const Shown& shown, const Overrides& asked, const LookStart& start,
                       LookPlan* out, std::string* why) {
    const bool stroke = start.from == LookStart::From::Interpolate;
    // A compound's exposed params dragged: the inner nodes' params they set (the session holds a
    // compound as its nodes inline, GraphEdit::flattenCompounds).
    const GraphEdit::FlatCompounds& flat = session.flatCompounds();
    Overrides overrides;
    for (const auto& [node, params] : asked) {
        for (const auto& [param, value] : params) {
            const auto [inner, innerParam] = flat.param(node, param);
            overrides[inner][innerParam] = value;
        }
    }
    if (!stroke && session.hasPreview()) {
        *why = "a brush stroke is live";
        return false;
    }
    const StrandsPtr strands = shown();  // (before the document: the host may evaluate to answer)
    const GraphDocument* doc = session.viewDocument();
    if (!doc) {
        *why = "no graph";
        return false;
    }
    // A drag's params: those the kernels read (a param they do not would show its old value).
    if (!stroke && !previewableOverrides(*doc, overrides, why)) return false;
    // Sculpt nodes' strengths dragged: the guides each hands on made here, at the dragged strength
    // (guides are few), standing in for its cached result; the rest of the drag the stages' own.
    Overrides own = stroke ? Overrides{} : overrides;
    scaleOverrides(*doc, session.groomScale(), own);
    std::map<NodeId, GuidesPtr> made;
    std::set<NodeId> sculpts;
    for (auto at = own.begin(); at != own.end();) {
        const NodeId id = doc->nodesByName.at(at->first);  // (previewableOverrides: each is a node)
        if (doc->graph.node(id).typeName() != op::kSculpt) {
            ++at;
            continue;
        }
        const auto strength = at->second.find("strength");  // (previewableOverrides: its one previewable param, a number)
        GuidesPtr guides;
        if (strength != at->second.end() &&
            !sculptedGuides(*doc, id, session.sculpts().node(at->first), std::get<double>(strength->second), &guides, why)) {
            return false;
        }
        if (guides) {
            made[id] = std::move(guides);
            sculpts.insert(id);
        }
        at = own.erase(at);
    }
    LookChain full;
    if (!findLookChain(*doc, &full, why)) return false;
    std::size_t first = kNoStage;
    if (stroke) {
        const NodeId interpolate = full.node(StageKind::Interpolate);
        first = interpolate ? full.indexOf(interpolate) : kNoStage;
        if (first == kNoStage) {
            *why = "the guides do not shape the strands through an Interpolate the GPU runs";
            return false;
        }
    } else if (start.from == LookStart::From::Node) {
        if (start.node == "*") {
            first = 0;
        } else {
            const auto it = doc->nodesByName.find(start.node);
            first = it == doc->nodesByName.end() ? kNoStage : full.indexOf(it->second);
            if (first == kNoStage && flat.output.count(start.node)) {  // a compound: from its first node the GPU runs
                const std::string prefix = start.node + "/";
                for (std::size_t k = 0; k < full.stages.size() && first == kNoStage; ++k) {
                    if (doc->nameOf(full.stages[k].node).rfind(prefix, 0) == 0) first = k;
                }
            }
        }
    } else {
        first = firstOverridden(*doc, full, own);
        if (!sculpts.empty()) first = std::min(first, firstMadeFrom(*doc, full, sculpts));
    }
    if (first == kNoStage) {
        *why = "no node of the drag is one the GPU runs";
        return false;
    }
    LookPlan p;
    p.doc = doc;
    p.chain = chainFrom(*doc, full, first);
    if (!sculpts.empty() && madeFrom(doc->graph, p.chain.base, sculpts)) {  // (made from them before the GPU's run: not made anew)
        *why = "the strands the GPU starts from (" + doc->nameOf(p.chain.base) + ") are made from the dragged Sculpt node's guides";
        return false;
    }
    if (!readLookParams(*doc, p.chain, own, &p.run.params, why) ||
        !lookStrands(*doc, p.chain, &p.base, &p.output, why)) {
        return false;
    }
    if (strands != p.output) {  // carried to the scalp, instances pinned, a node viewed
        *why = "the viewport shows other strands than the graph's output";
        return false;
    }
    // Mapped params (maps, plugged fields): their values on the strands each node takes, read once
    // per evaluation (and chain), and whether they hold for this run.
    if (!sameChain(fieldsChain_, p.chain) || !sameLookFields(*doc, p.chain, fields_)) {
        const auto started = std::chrono::steady_clock::now();
        LookFields read;
        if (!readLookFields(*doc, p.chain, session.last().scalp.get(), &read, why)) {
            fields_ = LookFields{};
            fieldsChain_ = LookChain{};
            return false;
        }
        fields_ = std::move(read);
        fieldsChain_ = p.chain;
        fieldsMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
    if (!lookFieldsHold(*doc, p.chain, fields_, why)) return false;
    p.run.fields = fields_;
    // A stroke's guides are new on every dab: what Interpolate takes them from changes (and a
    // sculpt stroke's Sculpt node), and with it any clump centers made from them. So do a dragged
    // Sculpt node's.
    std::set<NodeId> changed = sculpts;
    NodeId guides = 0;
    if (stroke && doc->graph.inputSource(p.chain.stages.front().node, 1, &guides)) changed.insert(guides);
    if (stroke && session.hasPreviewSculpt()) {
        const auto found = doc->nodesByName.find(session.previewSculptNode());
        if (found != doc->nodesByName.end()) changed.insert(found->second);
    }
    if (!inputs_.read(*doc, p.chain, p.run.params, session.last().parts.get(), &p.run.inputs, why, changed,
                      session.last().scalp.get(), &p.run.fields, session.last().scalp, made)) {
        return false;
    }
    *out = std::move(p);
    return true;
}

// ---- Strokes ------------------------------------------------------------------------------------

bool LookPreview::armStroke(const GroomSession& session, const Shown& shown, std::string* why) {
    if (strokeArmed_) return true;
    if (strokeRefused_) return false;
    if (!strokeShapesGuides(session)) {  // a map or a stroke of parts: not the guides alone
        *why = "the stroke does not shape guides";
        strokeRefused_ = true;
        return false;
    }
    auto p = std::make_unique<LookPlan>();
    if (!plan(session, shown, {}, {LookStart::From::Interpolate, {}}, p.get(), why)) {
        strokeRefused_ = true;
        return false;
    }
    strokeArmed_ = true;
    strokeParts_ = session.last().parts.get();
    stopAt_ = p->doc->nameOf(p->chain.stages.front().node);
    strokePlan_ = std::move(p);
    return true;
}

bool LookPreview::strokeHolds(const GroomSession& session, const Shown& shown, std::string* why) {
    if (!strokeArmed_) return false;
    auto p = std::make_unique<LookPlan>();
    // The parts as at the press (Interpolate's binding and Clump's choice respect them).
    const bool sameParts = session.last().parts.get() == strokeParts_;
    if (!sameParts) *why = "the parts changed";
    if (!sameParts || !plan(session, shown, {}, {LookStart::From::Interpolate, {}}, p.get(), why)) {
        refuseStroke();
        return false;
    }
    strokePlan_ = std::move(p);
    return true;
}

bool LookPreview::replanStroke(const GroomSession& session, const Shown& shown, std::string* why) {
    auto p = std::make_unique<LookPlan>();
    if (!plan(session, shown, {}, {LookStart::From::Interpolate, {}}, p.get(), why)) return false;
    strokePlan_ = std::move(p);
    return true;
}

void LookPreview::refuseStroke() {
    strokeArmed_ = false;
    strokeRefused_ = true;
    strokePlan_.reset();
    stopAt_.clear();
}

void LookPreview::endStroke() {
    strokeArmed_ = strokeRefused_ = false;
    strokePlan_.reset();
    stopAt_.clear();
}

// ---- The base -----------------------------------------------------------------------------------

ChangesPtr LookPreview::upload(LookEngine& engine, const LookPlan& plan, const ScalpMesh* scalp, bool incremental) {
    const Graph& graph = plan.doc->graph;
    const std::uint64_t serial = graph.resultSerial(plan.chain.base);
    const ChangesPtr changes = incremental && engine.base() && baseSerial_ && baseNode_ == plan.chain.base
                                   ? graph.changesSince(plan.chain.base, baseSerial_, true)  // of the base as made
                                   : nullptr;
    engine.setBase(plan.base, scalp, changes.get());
    baseSerial_ = serial;
    baseNode_ = plan.chain.base;
    return changes;
}

void LookPreview::forgetBase() {
    baseSerial_ = 0;
    baseNode_ = 0;
}

void LookPreview::reset() {
    endStroke();
    forgetBase();
    inputs_ = LookInputs{};
    fields_ = LookFields{};
    fieldsChain_ = LookChain{};
    fieldsMs_ = 0.0;
}

}  // namespace groomlab::gpu
