// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shot sculpting: corrective layers on top of an animated (simulated or
// cached) groom. Each layer holds per-guide point offsets at a time, fading
// in before it and out after it (or holding). Offsets live in the guide's
// rotation-minimizing frames, so a correction sculpted on one frame rides
// along with the motion on the frames around it.
//
// Layers apply level by level: primaries are corrected, their children
// re-follow them and then take their own corrections, and so on. Recording a
// sculpt inverts that exactly, so a child that only followed its corrected
// parent gets no offset of its own.
//
// A layer's weight over time comes from one of: its `weightCurve` (sampled
// keys, e.g. from Maya's keyable weight, for renders and groom files), or its
// own envelope (`time`, fades, holds) times `weight`. In Maya a layer with a
// `slot` reads its weight from the groom's keyable correctiveWeight[slot], as
// Yeti's layer weights: fades are keys the artist edits like any animation.

#include <map>
#include <set>
#include <string>
#include <vector>

#include "groomlab/guide_hierarchy.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

struct CorrectiveLayer {
    std::string name;
    double time = 0.0;         // seconds: full weight here
    double fadeIn = 0.5;       // seconds before `time` over which the layer fades in
    double fadeOut = 0.5;      // seconds after `time` over which it fades out
    bool holdBefore = false;   // full weight at every earlier time
    bool holdAfter = false;    // full weight at every later time
    float weight = 1.0f;       // envelope
    bool enabled = true;
    int slot = -1;             // Maya: the groom's correctiveWeight element it reads (-1: its own envelope)
    std::vector<std::pair<double, float>> weightCurve;  // (seconds, weight), sorted; replaces the envelope
    std::map<GuideId, std::vector<Vec3f>> offsets;  // per point, frame-local (x normal, y binormal, z tangent)

    // Weight at a time: the weight curve (linear between samples, held past
    // its ends), else weight x a smoothstep fade in and out.
    float weightAt(double seconds) const;
    // Full weight on every frame: no keys, held before and after (a sculpt; else a shot fix).
    bool allFrames() const { return weightCurve.empty() && holdBefore && holdAfter; }
    // The weight curve's value at `seconds` set (a key added, or the one there changed), kept sorted.
    void keyAt(double seconds, float w);
};

class Correctives {
public:
    std::vector<CorrectiveLayer> layers;

    bool empty() const;  // no layer with offsets
    CorrectiveLayer* layer(const std::string& name);
    const CorrectiveLayer* layer(const std::string& name) const;

    // A layer's name as every host makes it from `wanted` (Maya's shots.py, the Blender helper, the
    // app): letters, digits and _ (a host names an attribute or property after it), "c" before a
    // leading digit, "corrective" for nothing; then _2, _3... until no other layer has it, nor any
    // of `taken` (names the host holds otherwise: Maya's attributes). `current`: the layer being
    // renamed, which keeps its own name.
    std::string freeLayerName(const std::string& wanted, const std::string& current = {},
                              const std::vector<std::string>& taken = {}) const;
    // A new layer's name before freeLayerName: sculpt<N> (every frame; N the stack's size + 1) or
    // shot<F> (a fix at frame F, rounded).
    std::string newLayerName(bool allFrames, double frame) const;

    // {"layers": [{"name", "time", "fadeIn", "fadeOut", "holdBefore", "holdAfter",
    //   "weight", "enabled", "slot"?, "weightCurve"?: [[seconds, w], ...],
    //   "offsets": {"<guide id>": [x, y, z, ...]}}]}.
    // Empty text is no layers. fromJson throws std::invalid_argument.
    std::string toJson() const;
    static Correctives fromJson(const std::string& text);

    // Applies every enabled layer at `seconds` to an evaluated (and posed)
    // hierarchy. Guides in `skipOwn` get no correction of their own (their
    // parents' still moves them): used by record.
    void apply(GuideHierarchy& h, double seconds, const ScalpMesh* scalp,
               const std::set<GuideId>* skipOwn = nullptr) const;

    // Adds a sculpt to layer `name` (created at `seconds` if missing).
    // `posed` is the hierarchy before correctives at `seconds`; `sculpted` is
    // what the artist sees after the stroke (with every layer applied, then
    // sculpted). Throws std::invalid_argument when the layer has no weight at
    // `seconds` (nothing could be recorded). Returns the guides that got offsets.
    std::vector<GuideId> record(const std::string& name, const GuideHierarchy& posed, const GuideHierarchy& sculpted,
                                double seconds, const ScalpMesh* scalp);
};

}  // namespace groomlab
