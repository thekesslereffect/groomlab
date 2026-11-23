// SPDX-License-Identifier: Apache-2.0
// FollowGuides: strands follow guides from how they were (`rest`) to how they are now (`guides`), as a
// wire deformer does. A Braid's sections, brushed in a Sculpt node, move the hair woven in them:
//
//     Braid --strands--------------------------> FollowGuides
//       \--sections--> rest ------------------->   |
//        \--sections--> Sculpt (brushed) -> guides-/
//
// Each strand is bound to the rest guides whose `match` attributes equal its own channels of those
// names (braidId and braidSection: a section's hair to its own section's curve), point by point: its
// closest place on the guide and the offset from it in the guide's rotation-minimizing frame there
// (drive_guides.h bindToDrivers). It is carried along the guide as it is now (matched by ID, the same
// point count: a Sculpt node keeps both), every point but the root, times `strength` and the `weight`
// point channel (braidWoven: hair on its way in from the scalp follows as it comes into the braid, so
// the roots and the gather stay as the Braid laid them). Strands matching no guide that changed stay as they came. The guides'
// edits are correctives: the Sculpt keeps them as bends of what it is fed, so when the braid above
// changes (its path drawn again, its width, the weave) the sections come back sculpted on top.
//
// The binding is kept between evaluations while the strands and the rest guides stay the same, so
// brushing the guides costs the carry alone. CPU only (the GPU preview reads its result).

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "groomlab/drive_guides.h"
#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "internal.h"

namespace groomlab {

namespace {

std::vector<std::string> wordsOf(const std::string& text) {
    std::vector<std::string> words;
    std::string word;
    for (const char c : text) {
        if (c == ' ' || c == ',' || c == '\t') {
            if (!word.empty()) words.push_back(word);
            word.clear();
        } else {
            word += c;
        }
    }
    if (!word.empty()) words.push_back(word);
    return words;
}

class FollowGuidesOp : public Operator {
public:
    std::string typeName() const override { return op::kFollowGuides; }
    std::string category() const override { return "shape"; }
    std::vector<PortSpec> inputs() const override {
        return {{"strands", PortType::Strands}, {"rest", PortType::Guides, true}, {"guides", PortType::Guides, true}};
    }
    PortSpec output() const override { return {"out", PortType::Strands}; }
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }  // (strength's maps)

    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"strength", 1.0, "How much the strands follow their guides' change (mappable); 0: as they came"}
                .atLeast(0)
                .soft(0, 1),
            notAnimatable({"match", std::string("braidId braidSection"),
                           "The strand channels a strand's guides match by attribute (a Braid's braidId and "
                           "braidSection: each section's hair follows its own section curve)"}),
            notAnimatable({"weight", std::string("braidWoven"),
                           "A point channel scaling how much each point follows (braidWoven: hair follows as it comes "
                           "into the braid, its way in from the scalp stays); empty: every point fully"}),
        };
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, 0, op::kFollowGuides);
        const GuidesPtr rest = inputs.size() > 1 ? valueAs<GuidesPtr>(inputs[1]) : nullptr;
        const GuidesPtr now = inputs.size() > 2 ? valueAs<GuidesPtr>(inputs[2]) : nullptr;
        const ScalarField strength(params(), "strength", scalpOf(ctx));
        strength.validate(*in);
        const std::vector<std::string> match = wordsOf(params().getString("match"));
        const std::string weightName = params().getString("weight");
        if (!rest || !now || rest->empty() || in->strandCount() == 0) return in;
        if (strength.isConstant() && strength.maximum() <= 0.0f) return in;

        // Which guides each strand follows: those of its match channels' values.
        if (match.empty()) throw GraphError("FollowGuides: no match channels (braidId braidSection, say)");
        std::vector<const std::vector<float>*> channels;
        for (const std::string& name : match) {
            const std::vector<float>* c = in->strandChannel(name);
            if (!c) throw GraphError("FollowGuides: the strands have no '" + name + "' channel to match their guides by");
            channels.push_back(c);
        }
        // Only guides that changed move anything (the others' hair stays to the bit, and is not bound).
        std::map<std::uint64_t, const Guide*> nowById;
        for (const Guide& g : now->guides()) nowById[g.id] = &g;
        std::map<std::vector<float>, std::vector<std::uint64_t>> byKey;
        for (const Guide& g : rest->guides()) {
            const auto there = nowById.find(g.id);
            if (there == nowById.end() || there->second->points == g.points) continue;
            std::vector<float> key;
            key.reserve(match.size());
            for (const std::string& name : match) {
                const auto it = g.attributes.find(name);
                if (it == g.attributes.end()) break;
                key.push_back(it->second);
            }
            if (key.size() == match.size()) byKey[key].push_back(g.id);
        }
        if (byKey.empty()) return in;
        DriverAttachments attached;
        std::uint64_t hash = 0x6F11;
        std::vector<float> key(match.size());
        for (std::size_t s = 0; s < in->strandCount(); ++s) {
            for (std::size_t k = 0; k < match.size(); ++k) key[k] = (*channels[k])[s];
            const auto it = byKey.find(key);
            if (it == byKey.end()) continue;
            attached.byGuide[in->ids()[s]] = it->second;
            hash = hashCombine(hashCombine(hash, in->ids()[s]), it->second.front());
        }
        if (attached.byGuide.empty()) return in;
        attached.hash = hash;

        std::shared_ptr<const DriveBinding> binding;
        {
            const std::uint64_t bindKey = driveBindingKey(*in, *rest, attached);
            std::lock_guard<std::mutex> lock(mutex_);
            if (!binding_ || binding_->key != bindKey) binding_ = std::make_shared<const DriveBinding>(bindToDrivers(*in, *rest, attached));
            binding = binding_;
        }
        if (binding->perPoint == 0) return in;

        std::vector<float> amount(in->strandCount());
        for (std::size_t s = 0; s < amount.size(); ++s) amount[s] = strength.at(*in, s);
        auto out = std::make_shared<StrandSet>(*in);
        driveCurves(*out, *binding, *rest, *now, amount, Ramp::constant(1.0f));
        // Each point that far of the way (the weight channel), from where it came.
        if (!weightName.empty()) {
            const std::vector<float>* weight = in->pointChannel(weightName);
            if (weight) {
                std::vector<Vec3f>& to = out->mutablePositions();
                const std::vector<Vec3f>& from = in->positions();
                parallelFor(0, to.size(), [&](std::size_t p) {
                    const float w = std::clamp((*weight)[p], 0.0f, 1.0f);
                    if (w < 1.0f) to[p] = from[p] + (to[p] - from[p]) * w;
                }, 4096);
            }
        }
        return StrandsPtr(out);
    }

private:
    mutable std::mutex mutex_;
    mutable std::shared_ptr<const DriveBinding> binding_;  // the last binding made (its key says for what)
};

}  // namespace

void registerFollowGuidesOperator(OperatorRegistry& registry) {
    registry.registerType(op::kFollowGuides,
                          {"deform",
                           "Strands follow guides from how they were (rest) to how they are now: a Braid's sections "
                           "brushed in a Sculpt node move their hair (Sculpt Sections on the Braid node).",
                           "wire deformer follow curves braid sections sculpt shape art direct"},
                          [] { return std::make_unique<FollowGuidesOp>(); });
}

}  // namespace groomlab
