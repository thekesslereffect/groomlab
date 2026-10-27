// SPDX-License-Identifier: Apache-2.0
#pragma once

// The host-free half of a viewport's GPU look preview, which every host shares: planning a run
// of the graph's last nodes (look_chain.h) on a groom's GroomSession for a param's drag or a
// brush stroke's dabs, with the reasons a run cannot happen (the host then previews on the CPU);
// a stroke armed after its press, each dab held or given back to the CPU; and the strands the
// engine starts from, uploaded when they changed (only the changed strands after an incremental
// evaluation). A host keeps one beside its engine (look_engine.h) and adds what is its own: the
// device, the buffers a run writes, when it runs. Maya's draw override and the standalone app's
// renderer are the hosts (their gpu_preview.cpp).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "groomlab/gpu/look_chain.h"
#include "groomlab/gpu/look_engine.h"
#include "groomlab/groom_session.h"

namespace groomlab::gpu {

// Whether the live stroke of `session` shapes guides alone, which a run from Interpolate draws
// (the guides made on the CPU, few: a dab's evaluation stops at Interpolate): a stroke of the hand
// guides (a guide preview), or of a Sculpt node's (a sculpt preview: GroomSession::setPreviewSculpt).
// A map, part or flow stroke does not.
bool strokeShapesGuides(const GroomSession& session);

// What a run takes: the chain from its first node, the params, mapped values and inputs, the
// strands it starts from (`base`) and the ones it stands for (`output`, the graph's).
struct LookPlan {
    LookChain chain;
    LookRun run;
    StrandsPtr base, output;
    const GraphDocument* doc = nullptr;
    // The node the run starts from: its first stage's, or where that is a Cull, Split or Mirror, the
    // first of the run up its input (StageParams::branch: a stroke's Interpolate).
    NodeId start = 0;
};

// Where a plan's run starts.
struct LookStart {
    enum class From {
        Overridden,   // the first node the overrides change (a param's drag)
        Interpolate,  // Interpolate (a stroke's dabs: the guides it takes are new on each)
        Node,         // the node named `node` ("*": the chain's first)
    };
    From from = From::Overridden;
    std::string node;
};

class LookPreview {
public:
    // The strands the viewport shows, asked for once a plan gets that far (a host may evaluate
    // the groom to answer): a run stands for the graph's output, so they must be it.
    using Shown = std::function<StrandsPtr()>;

    // A run of `session`'s view document with `overrides` from `start` (the overrides are not
    // read for a stroke). A Sculpt node's strength dragged: its guides made on the CPU at that
    // strength (sculptedGuides, its stored edits), the run from the first stage they make. False
    // with *why when the GPU cannot take it: a stroke live during a drag, no chain the GPU runs,
    // the viewport showing other strands, mapped params that do not hold, inputs it cannot keep...
    bool plan(const GroomSession& session, const Shown& shown, const Overrides& overrides, const LookStart& start,
              LookPlan* out, std::string* why);

    // ---- Strokes -------------------------------------------------------------------------------
    // After a stroke's press was evaluated and shown: whether the GPU takes the next dabs (a
    // stroke of the guides alone, strokeShapesGuides, planned from Interpolate: a sculpt stroke's
    // dabs lay their edits on the Sculpt node's cached input on the CPU, the strands after it run
    // on the GPU). A refused stroke stays the CPU's until endStroke.
    bool armStroke(const GroomSession& session, const Shown& shown, std::string* why);
    // A dab's evaluation stopped at the stroke's Interpolate: whether the GPU draws it (planned
    // again for the dab's guides, the parts as at the press). False: this dab and the rest of the
    // stroke are the CPU's.
    bool strokeHolds(const GroomSession& session, const Shown& shown, std::string* why);
    // The armed stroke's plan made again for the document as it is now (the stroke stays armed
    // whatever the answer).
    bool replanStroke(const GroomSession& session, const Shown& shown, std::string* why);
    // The rest of the stroke is the CPU's (a reason of the host's: no item to draw, a run that
    // failed...), until endStroke.
    void refuseStroke();
    // The stroke is over (released, or dropped): the next press may arm again.
    void endStroke();
    bool strokeArmed() const { return strokeArmed_; }
    bool strokeRefused() const { return strokeRefused_; }
    // The armed stroke's plan (its press's, or its last held dab's); null while none is armed.
    const LookPlan* strokePlan() const { return strokePlan_.get(); }
    // The node a dab's evaluation stops at while armed (the Interpolate the GPU runs from).
    const std::string& stopAt() const { return stopAt_; }

    // ---- The base --------------------------------------------------------------------------------
    // `plan`'s base strands onto `engine` before a run: with `incremental`, only those the graph
    // says changed since the base the engine holds (a brush stroke's incremental evaluation),
    // else all. Returns what changed (null: unknown, everything went up). Throws as
    // LookEngine::setBase does.
    ChangesPtr upload(LookEngine& engine, const LookPlan& plan, const ScalpMesh* scalp, bool incremental = true);
    // The engine is new (or gone): it holds no base.
    void forgetBase();

    // Another groom: nothing of the last one holds (the stroke, the values and inputs read, the base).
    void reset();

    // What reading the mapped params' values took the last time (milliseconds), and the inputs'
    // reader (its own stats).
    double fieldsReadMs() const { return fieldsMs_; }
    const LookInputs& inputs() const { return inputs_; }

private:
    LookInputs inputs_;      // what Interpolate and Clump take, kept per binding, guides, choice
    LookFields fields_;      // the mapped params' values the last plan read
    LookChain fieldsChain_;  // the chain they were read for
    double fieldsMs_ = 0.0;
    std::uint64_t baseSerial_ = 0;  // the base node's result serial the engine holds
    NodeId baseNode_ = 0;
    // A live stroke: armed after its press, the plan the last held dab made.
    bool strokeArmed_ = false, strokeRefused_ = false;
    const void* strokeParts_ = nullptr;  // the parts at the press
    std::unique_ptr<LookPlan> strokePlan_;
    std::string stopAt_;
};

}  // namespace groomlab::gpu
