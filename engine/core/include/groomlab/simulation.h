// SPDX-License-Identifier: Apache-2.0
#pragma once

// Guide simulation: hair dynamics every host shares (docs/SESSION.md, Simulation).
//
// A Simulate node (category motion, between Guides and Interpolate) gives its guides as a solver
// moved them over the frames since its start frame; the strands Interpolate grows from them
// follow. The graph stays pure: the node reads its frame from the session (the external
// "sim:<node>", simulationExternal) and the session steps the solver frame by frame, keeping
// every frame in a SimCache per node (GroomSession: scrubbing back reads them, a jump forward
// steps on from the last one, a change of the node's settings or of what it is fed drops them,
// a bake reads them from a file instead).
//
// The solver is position based (Mueller et al. 2007, "Position Based Dynamics"; Macklin et al.
// 2016, "XPBD"). Each guide is a rod of points with an orientation on every segment, after
// Kugelstadt and Schoemer 2016, "Position and Orientation Based Cosserat Rods": a stretch-shear
// constraint ties a segment's orientation to its two points (rigid across the segment, compliant
// along it by `stretchStiffness`), a bend-twist constraint (compliance from `bend` and `twist`)
// keeps the turn between neighbouring segments at the groomed shape's (its rest Darboux vector),
// the first segment's against its root's frame on the scalp: its error is the exact turn from the
// groomed one (a rotation vector), so the multipliers stay true for a guide pushed far from its
// shape. Each iteration solves all of a guide's constraints together, exactly (Deul, Kugelstadt,
// Weiler and Bender 2018, "Direct Position-Based Solver for Stiff Rods"): segment j's constraints
// touch only segments j - 1 and j, so the guide's system is block tridiagonal (6x6 blocks), solved
// in one pass down the guide and one back up, every point and orientation moved by its own inverse
// mass. A stiff guide is held whole within one iteration (it keeps its shape under gravity), and
// nothing is held still while its neighbour moves, so the solve adds no energy to the motion. (The
// constraints were once projected one at a time from the root, each turning only the segment after
// it and, above bend 0.5, moving the point before less and less: a moving root pumped energy into
// the chain, the tip's speed doubling every substep, and the ends of a bob flipped about in a head
// shake, more so at more substeps.) The system changes with the orientations alone, little over a
// substep: it is factored once a substep and solved again each iteration. Bend's compliance is
// 0.3 r^2.4 (r = 1/bend - 1) per unit of the orientation's inverse mass: 0.7 is clearly loose,
// 0.9 nearly rigid. The orientations keep no spin from one substep to the next (a hair's
// cross-section has next to no inertia; carried over, their spin fed the pull toward the groomed
// shape back into the motion and the hair never came to rest): each substep starts them where the
// last left them, each weighing as its tip point at the segment's length, so what one resists of
// a substep's turn damps the swing. After the iterations each point is put back at its segment's length from the one
// before (follow the leader with its velocity correction, Mueller et al. 2012, "Fast Simulation
// of Inextensible Hair and Fur"; the points pushed out of a surface keep it too, or hair resting
// on the scalp bounces), then pushed out of the scalp and the colliders through their
// distance fields (distance_field.h), with friction, and from the first point pushed on put back at
// its segment's length again and pushed out once more (a point as deep as half its guide's length
// under a surface is still pushed out; the fields reach as far as the longest guide asks, rounded
// up to a power of two). Roots are pinned to the scalp and move with
// it; `rootStiffnessProfile` pulls each point toward the groomed shape (moving with its root) by a
// profile along the length. Gravity, air drag toward a wind (the Wind node's gust field),
// damping of the motion relative to the root, and `inertia`: the share of the head's motion the
// hair feels (0: carried rigidly by its root, 1: all of it). With `groomedUnderGravity` the groomed
// shape is how the hair rests under gravity at the start: each guide feels only gravity's change
// in its root's frame since then. Guides are independent (TBB over
// guides, their rods solved four at a time side by side, each one's arithmetic as alone, to the
// bit); the guide-guide repulsion, when on, reads the points as they were before its pass,
// so results never depend on how threads met them, and moves a point at most half its radius a pass.
// Every share is per a reference time step, not per step (bend, twist and stretch's compliance,
// the length kept after the iterations, friction and repulsion per 1/96 s, rootStiffnessProfile
// per 24 fps frame): the look holds at any frame rate and substeps (finer steps only follow the
// motion more closely).
//
// Water (`water` on): points below the surface (the plane y = waterLevel, or the water mesh on
// the node's `water` input: below its surface along the closest point's normal) feel buoyancy
// (an upward share of gravity), water drag toward the current (a Wind-style flow of its own, much
// stronger than the air's) instead of the air's, all faded in over a band about the surface (5%
// of the point's guide's length) so a point crossing it is never kicked. Each point has a wetness, 0-1:
// it soaks up within a quarter second under water (or at the `rain` rate), dries over `dryTime`
// out of it, and while wet above the water the point is heavier and bends more easily
// (`wetWeight`). The frames carry it as the guides' "wetness" point layer, which Interpolate
// blends into the strands' wetness point channel for the look (Clump strength, HairColor, the
// shaders). With water off and no rain, none of this runs: the frames are the dry solver's to the
// bit, and the settings hash is the one made before water.
//
// Per guide and per point (Yeti's sim attributes): `stretchStiffness`, `bend`, `twist`, `damping`,
// `drag`, `rootStiffness`, `mass` and `strength` are mappable (a painted guide layer, a
// texture, a color set, an expression, a Field node plugged in): SimMaps holds their values per
// point, read on the guides the node is fed each frame (simulationMaps). `mass` weighs a point:
// the air (drag, wind) and other hair (repulsion, volume) move a heavier one less, and along a
// guide the constraints move its heavier points less; gravity pulls all alike. `strength` is how
// much a point is simulated: 0 holds it to its input shape (moving with its root, kinematic: the
// points after it hang from it), 1 is fully simulated, in between the frame shows that share of
// its simulated offset from the input shape. All at their defaults (1, no maps): the solver as it
// was, to the bit.
//
// Hair volume (`volume` above 0, Yeti's density gradient): every substep the guides' points are
// counted into a grid of `volumeCellSize` cells and each point is pushed down the density the
// others make there, so long hair keeps its body instead of collapsing into itself. The grid is
// the world's (cells from the origin, whatever the guides' bounds), so a guide's push depends only
// on the hair within a cell of it. The counts are summed in point order (one thread), the pushes
// read them on any number of threads: the same result on every run.
//
// Locality: what moves a guide in a substep is its own settings and inputs, the surfaces near it
// and the other guides within the repulsion radius or two volume cells of it; nothing of the guide
// set as a whole (its bounds, its longest guide) reaches it but the collision fields' reach (a
// power of two). An edit of the guides or the scalp in one place leaves the guides out of reach of what it
// changed (a reach that grows by at most that much a substep) as they were, to the bit.
//
// Friction per surface: the scalp's is `friction`, each collider's its ColliderMesh node's
// `friction` (SimInputs::colliderFriction; negative: `friction`); a point pushed out of several
// takes the largest.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/export_view.h"
#include "groomlab/graph.h"
#include "groomlab/ramp.h"

namespace groomlab {

// A Simulate node's settings: its params read once (SimSettings::fromParams).
struct SimSettings {
    float stiffness = 1.0f;  // segment lengths kept, 0-1 (1: inextensible), per 1/96 s
    float bend = 0.5f;       // the groomed curvature kept, 0-1 (0: a rope)
    float twist = 0.5f;      // the groomed twist kept, 0-1
    float damping = 0.1f;    // motion relative to the root dies out, 0-1
    float drag = 0.05f;      // air drag toward the wind's air, 0-1
    float gravity = 980.0f;  // scene units per second squared (cm: 980)
    Vec3f gravityDirection{0.0f, -1.0f, 0.0f};
    // The groomed shape is how the hair rests under gravity at the start frame: a still groom
    // keeps it (preload says how). Off: gravity pulls on the groomed shape.
    bool groomedUnderGravity = true;
    // How groomedUnderGravity holds it (the param: "preload", "weightless", "off"): true preloads
    // the rest shape (gravity preloading, Liu 2022): each joint's rest turn takes the groomed
    // shape's weight beyond it, so the hair keeps its weight and rests in the groomed shape (soft
    // guides that could not hold all of it take a share, the rest as weightless); false cancels
    // the start's gravity in each root's frame (the hair weighs nothing while still).
    bool preload = true;
    float windSpeed = 0.0f;  // the air's speed, scene units per second (0: still air)
    Vec3f windDirection{1.0f, 0.0f, 0.0f};
    float windGust = 0.5f;        // how much the wind varies, 0-1 (as Wind's gust)
    float windFrequency = 0.1f;   // gust features per scene unit
    Ramp rootStiffnessProfile{{0.0f, 1.0f}, {0.2f, 0.0f}};  // pull toward the groomed shape, root to tip, per 24 fps frame
    float inertia = 1.0f;         // the share of the head's motion the hair feels, 0-1
    bool collideScalp = true;
    float collisionOffset = 0.05f;  // distance kept from the scalp and colliders
    float friction = 0.3f;          // 0-1 of the sliding along a surface taken away (per 1/96 s)
    float repulsion = 0.0f;         // guide-guide repulsion strength, 0-1 (0: off), per 1/96 s
    float repulsionRadius = 0.2f;   // distance guides keep from each other
    int substeps = 4;
    int iterations = 6;
    int startFrame = 1;
    std::uint64_t seed = 0;  // the wind's gusts (and the current's)
    bool live = true;        // cacheMode "live": evaluation steps frames not cached; "cached": only Simulate Range does
    // Water (see the top of this file).
    bool water = false;
    float waterLevel = 0.0f;   // the surface's height (world y) without a water mesh
    float buoyancy = 0.9f;     // the upward pull under water, a share of gravity (above 1: hair floats up)
    float waterDrag = 0.5f;    // drag toward the water's flow, 0-1 (much stronger than the air's at the same value)
    float currentSpeed = 0.0f; // the water's flow, scene units per second
    Vec3f currentDirection{1.0f, 0.0f, 0.0f};
    float dryTime = 10.0f;     // seconds a wet point takes to dry most of the way (to 1/e)
    float wetWeight = 0.5f;    // how much heavier and limper wet hair is above the water, 0-1
    float rain = 0.0f;         // rain wetting the hair, 0-1 (1: soaked in about a second), no forces
    // Per guide and point (see the top of this file): these are the numbers where no map is set.
    float rootStiffness = 1.0f;       // rootStiffnessProfile scaled, 0 and up
    float mass = 1.0f;                // a point's weight against the air and other hair (1: as before mass)
    float strength = 1.0f;            // how much is simulated: 0 held to the input shape, 1 simulated
    // Hair volume (Yeti's density gradient), 0-1 (0: off), per 1/96 s; the grid's cell size.
    float volume = 0.0f;
    float volumeCellSize = 1.0f;
    // The maps set on the mappable params (MapRefs, by their text): 0 for none. What a texture
    // reads is in the inputs' hash (SimMaps::hash), not here.
    std::uint64_t mapsKey = 0;

    // Whether points get wet (water or rain): the solver keeps a wetness then.
    bool wetting() const { return water || rain > 0.0f; }
    // Whether any of the per point settings is off its default (maps aside): mass, strength,
    // rootStiffness.
    bool perPointSettings() const { return rootStiffness != 1.0f || mass != 1.0f || strength != 1.0f; }

    static SimSettings fromParams(const ParamSet& params);
    // What the frames depend on (not `live`): stable across sessions and hosts (a bake's stamp).
    std::uint64_t hash() const;
};

// The external a Simulate node reads its frame from: "sim:<node>".
std::string simulationExternal(const std::string& node);
// A settings stamp as stored in groom files and Maya's simCaches (16 hex digits), and back (0 for
// text that is not one).
std::string formatSimulationStamp(std::uint64_t stamp);
std::uint64_t parseSimulationStamp(const std::string& text);

// The world one frame of the solver moves toward: the guides as the Simulate node's input gives
// them then (each guide's groomed shape, moving with its root: what roots are pinned to and points
// pulled toward), the scalp then (the roots' frames; collisions, through the distance field of its
// rest pose, or of the scalp at the start frame without one, each point carried there by its
// root's motion: exact for a head moving rigidly), the colliders then (each through its field, or,
// deformed away from a rest pose, its rest pose's: CollisionSurface, distance_field.h; so is the
// scalp for guides not rooted on it; a collider that moved since the last inputs, the same mesh
// with other points, is met by each substep on its way from there, so one sweeping through the
// hair does not jump past it) and the water's surface (null: the plane at waterLevel).
// Simulate's mappable params per point of the guides (every guide's points in order), each empty
// while that param is a number (SimSettings holds it then).
struct SimMaps {
    std::vector<float> stiffness, bend, twist, damping, drag, rootStiffness, mass, strength;
    bool empty() const {
        return stiffness.empty() && bend.empty() && twist.empty() && damping.empty() && drag.empty() &&
               rootStiffness.empty() && mass.empty() && strength.empty();
    }
    std::uint64_t hash() const;
};
using SimMapsPtr = std::shared_ptr<const SimMaps>;

// The mappable params of a Simulate node read on `guides` (on `scalp`: textures, color sets):
// null when every one is a number. `params` with its plugged fields set (ParamSet::setField).
// Throws GraphError when a map cannot be read (a missing layer or file).
SimMapsPtr simulationMaps(const ParamSet& params, const GuideSet& guides, const MeshPtr& scalp);
// The mappable params of the Simulate node (in SimMaps' order).
const std::vector<std::string>& simulationMapParams();

struct SimInputs {
    GuidesPtr guides;
    MeshPtr scalp;
    std::vector<MeshPtr> colliders;
    MeshPtr water;
    // Each collider's friction, 0-1, in `colliders` order (negative or missing: the settings').
    std::vector<float> colliderFriction;
    // The mappable params read on `guides` (null: all numbers).
    SimMapsPtr maps;
};

// The point layer of a Simulate node's guides holding each point's wetness, 0-1 (with water or
// rain): Interpolate blends it into the strands' point channel of the same name
// (export_view.h kWetnessChannel, which the hair shading reads as Wet's).

// The position-based rod solver over a set of guides (see the top of this file).
class GuideSolver {
public:
    GuideSolver();
    ~GuideSolver();
    GuideSolver(const GuideSolver&) = delete;
    GuideSolver& operator=(const GuideSolver&) = delete;

    // At rest on the inputs of `frame`: every guide as its input gives it, still.
    void start(const SimSettings& settings, double framesPerSecond, int frame, const SimInputs& inputs);
    bool started() const;
    int frame() const;  // the frame the state is at
    // Whether `guides` are the guides being simulated: the same IDs with the same point counts, in order.
    bool sameLayout(const GuideSet& guides) const;
    // `frames` frames on, toward `inputs` at the last of them: the roots and the groomed shapes
    // move from the last inputs to these along the way (each root by its frame's rigid motion),
    // the colliders are these. `frameDone(k)` after each frame (k = 1..frames). Throws
    // std::invalid_argument for guides other than the ones simulated (sameLayout).
    void step(const SimInputs& inputs, int frames = 1, const std::function<void(int)>& frameDone = {});
    // The guides now: the last inputs' guides (IDs, roots) with the simulated points (and their
    // wetness layer, kWetnessChannel, while the settings wet them).
    GuidesPtr guides() const;
    // The points of every guide, in order (as shown: `strength` between 0 and 1 blended toward the
    // input shape), and their wetness (empty while nothing wets them) (tests).
    const std::vector<Vec3f>& points() const;
    const std::vector<float>& wetness() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A Simulate node's frames over time: the solver at its last frame, and every frame from the
// start frame to that one kept (scrubbing back reads them). Frames are at `framesPerSecond`:
// frame f at f / fps seconds.
class SimCache {
public:
    SimCache();
    ~SimCache();
    SimCache(const SimCache&) = delete;
    SimCache& operator=(const SimCache&) = delete;

    // The node's settings and the fps: other ones (SimSettings::hash) drop every frame.
    void configure(const SimSettings& settings, double framesPerSecond);
    const SimSettings& settings() const { return settings_; }
    double framesPerSecond() const { return fps_; }
    int startFrame() const { return settings_.startFrame; }

    bool started() const { return !frames_.empty(); }
    int lastFrame() const;  // the last frame simulated (startFrame() - 1: none)
    bool has(int frame) const { return frames_.count(frame) != 0; }
    GuidesPtr frame(int frame) const;  // the simulated guides (IDs, roots, points); null when not cached
    std::uint64_t frameHash(int frame) const;  // the frame's key, new for every frame made (0 when not cached)
    std::uint64_t inputHash(int frame) const;  // what the frame was stepped toward (0 when not cached)
    std::size_t frameCount() const { return frames_.size(); }
    // The frames cached, as runs of first..last (inclusive).
    std::vector<std::pair<int, int>> ranges() const;

    // At the start frame: the solver at rest on these inputs, the frame kept.
    void start(const SimInputs& inputs, std::uint64_t inputHash);
    // `frames` frames on from lastFrame() toward these inputs (GuideSolver::step), each kept.
    // False when the guides are not the ones simulated (another count, IDs, point counts): the
    // cache is cleared then, to start again.
    bool step(const SimInputs& inputs, std::uint64_t inputHash, int frames = 1);
    void clear();
    double lastStepMs() const { return lastStepMs_; }  // the last frame's step (per frame of a multi-frame step)
    std::size_t bytes() const;  // the frames' points (stats)

private:
    void keep(std::uint64_t inputHash);
    SimSettings settings_;
    std::uint64_t settingsHash_ = 0;
    double fps_ = 24.0;
    GuideSolver solver_;
    struct Frame {
        GuidesPtr guides;
        std::uint64_t hash = 0, input = 0;
    };
    std::map<int, Frame> frames_;
    double lastStepMs_ = 0.0;
};

// A simulation baked to a file (an Alembic guide cache, engine/io sim_cache.h): the guides at a
// time (by ID), read on demand. `stamp` is the settings hash it was made with (SimSettings::hash):
// a node whose settings changed since ignores it and simulates (0: always taken).
struct BakedSimulation {
    std::function<GuidesPtr(double seconds)> read;
    double firstSeconds = 0.0, lastSeconds = 0.0;
    std::uint64_t stamp = 0;
    std::string file;  // where it was read from (status)
};
using BakedSimulationPtr = std::shared_ptr<const BakedSimulation>;

// What a host shows of a Simulate node (the timeline's strip, stats).
struct SimStatus {
    std::string node;
    int startFrame = 1;
    std::vector<std::pair<int, int>> ranges;  // frames cached in memory, runs of first..last
    std::size_t frames = 0;
    bool live = true;          // its cacheMode
    bool baked = false;        // a bake is attached and matches its settings
    bool bakeStale = false;    // a bake is attached but was made with other settings (ignored)
    int bakedFirst = 0, bakedLast = 0;  // the bake's frames
    std::string bakedFile;
    double stepMs = 0.0;       // the last frame's step
    std::size_t bytes = 0;
    std::uint64_t stamp = 0;   // SimSettings::hash of the node as set
};

// After each frame a Simulate Range stepped: frames done of `count`; false stops (the frames
// made stay cached).
using SimProgress = std::function<bool(int done, int count)>;

// How the host's inputs at another time reach the session: it sets them through the session's
// setters (the scalp, guide poses, colliders, controls at `seconds`), as it does before showing a
// frame, and returns true; false when it cannot read that time now (Maya while it plays), having
// set nothing (the frame is stepped toward the inputs as they are: GroomSession::setSimulationFeed).
// The session puts back what it had after stepping.
using SimFeed = std::function<bool(double seconds)>;

struct SimRun {
    int frames = 0;          // frames stepped
    bool cancelled = false;  // the progress callback stopped it
    double milliseconds = 0.0;
    std::vector<std::string> errors;
};

// One-click settings for a Simulate node every host offers (Maya's Graph Editor, the app's
// parameter panel, groomlab.sim.simulate_preset): "default" (the node's defaults) and "fur"
// (springy fur that holds its groom: a firm hold toward the groomed shape along the whole length,
// stiff bend, less of the head's motion, more damping). Each sets the same params (the motion's
// stiffnesses, damping, hold and solver passes), so switching between them leaves no mix behind;
// gravity, wind, collisions, water and maps are left as they are.
struct SimPreset {
    std::string name;   // "default", "fur"
    std::string title;  // "Default", "Fur"
    std::string description;
};
const std::vector<SimPreset>& simulatePresets();
// The params a preset sets, as a node's "params" JSON object (graph_io.h's form: a ramp as its
// keys). Throws std::invalid_argument for a name that is not a preset.
std::string simulatePresetParamsJson(const std::string& name);

}  // namespace groomlab
