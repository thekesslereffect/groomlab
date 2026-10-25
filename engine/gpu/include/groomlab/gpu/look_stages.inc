// SPDX-License-Identifier: Apache-2.0
// What the GPU preview (look_chain.h) knows of each node type, a row each. An X-macro list (no
// include guard: included once per use; a macro left undefined expands to nothing):
//
//   GROOMLAB_STAGE(Kind, name, mapped0, mapped1, ramps, vectors, numbers)
//     A stage the GPU runs, for the node type of the same name: StageKind::Kind (look_chain.h, in
//     this order; kStageKinds counts them), stageName's `name`, the params its two mapped fields
//     are read for (StageFields::field[0], [1]; "" none), and the params a drag may set while it
//     previews (previewableParam): `ramps` take a ramp, `vectors` a vector, `numbers` a number (a
//     seed, a count or a choice's index too). Space-separated names; "affect" where the stage
//     mixes its result back by its affect (StageParams::masked). A param the kernel does not match
//     the CPU on is left out: the CPU evaluates it (docs/DEVELOPING.md section 6 Rules).
//   GROOMLAB_STAGE_NODE(Type, Kind, ramps, vectors, numbers)
//     Another node type that runs as Kind's stages (a Wet: a Clump stage a level).
//   GROOMLAB_GPU_PARAMS(Type, ramps, vectors, numbers)
//     A node type with no stage whose params a drag may still set (Sculpt: the guides it hands on
//     made again on the CPU at the dragged strength).
//   GROOMLAB_PASS_THROUGH(Type)
//     A node type that adds channels and leaves the points as they came (its strands share the
//     points it takes): the GPU's points go through it (look_chain.cpp channelsOnly).
//
// A new stage is a row here, its kernels (look_kernels.inc) and its code in look_chain.cpp
// (readLookParams, the inputs it uploads) and look_engine.cpp (kernelsOf, its dispatch):
// docs/EXTENDING.md.

#ifndef GROOMLAB_STAGE
#define GROOMLAB_STAGE(Kind, name, mapped0, mapped1, ramps, vectors, numbers)
#define GROOMLAB_STAGE_DEFAULTED
#endif
#ifndef GROOMLAB_STAGE_NODE
#define GROOMLAB_STAGE_NODE(Type, Kind, ramps, vectors, numbers)
#define GROOMLAB_STAGE_NODE_DEFAULTED
#endif
#ifndef GROOMLAB_GPU_PARAMS
#define GROOMLAB_GPU_PARAMS(Type, ramps, vectors, numbers)
#define GROOMLAB_GPU_PARAMS_DEFAULTED
#endif
#ifndef GROOMLAB_PASS_THROUGH
#define GROOMLAB_PASS_THROUGH(Type)
#define GROOMLAB_PASS_THROUGH_DEFAULTED
#endif

// (Interpolate's other params pick each strand's guides and weights: the CPU's binding.)
GROOMLAB_STAGE(Interpolate, "interpolate", "", "", "falloff", "", "limitDistance")
GROOMLAB_STAGE(Length, "length", "keep", "fraction", "", "", "affect keep fraction trimMin trimMax seed")
GROOMLAB_STAGE(Clump, "clump", "strength", "volumize", "volumizeProfile profile", "",
               "affect strength lockRadius repelRadius volumize flatness strengthRandom lockRadiusRandom seed")
GROOMLAB_STAGE(Noise, "noise", "amplitude", "fraction", "profile", "", "affect amplitude frequency fraction octaves seed")
GROOMLAB_STAGE(Curl, "curl", "radius", "", "frequencyProfile profile", "weightDirection",
               "affect radius frequency randomness phase phaseRandom coilWidths spacingRandom weight seed")
GROOMLAB_STAGE(Width, "width", "width", "", "profile", "", "affect width randomness seed")
// (An auto Part's splitAngle and minLength pick which lines it finds: the CPU's.)
GROOMLAB_STAGE(Part, "part", "strength", "", "profile", "", "affect strength width")
GROOMLAB_STAGE(Gravity, "gravity", "strength", "", "stiffnessProfile", "", "affect strength stiffness")
// (Its strays are drawn on the GPU by ID: a fraction or seed dragged picks others.)
GROOMLAB_STAGE(Flyaway, "flyaway", "stray", "fraction", "", "", "affect stray fraction frequency start standOut lengthen seed")
// (Its offset a number of the kernel's, its fields' tables made for every offset sharing the CPU's
// field: LookInputs::read. Its depth and softness make the tables: the CPU's.)
GROOMLAB_STAGE(Collide, "collide", "", "", "", "", "affect offset")
GROOMLAB_STAGE(Wind, "wind", "strength", "", "profile", "", "affect strength gust frequency speed turbulence seed")
GROOMLAB_STAGE(Blend, "blend", "mix", "", "profile", "", "mix")
// (The hair grid's layout is the core strands' and the scalp's: none of these change it.)
GROOMLAB_STAGE(SelfCollide, "selfCollide", "strength", "", "profile", "", "affect strength packing")
GROOMLAB_STAGE(Static, "static", "strength", "", "profile", "", "affect strength randomness")
// (Its ring's bins are made on the GPU from the core strands: a center dragged too.)
GROOMLAB_STAGE(Tie, "tie", "affect", "", "", "center direction", "affect radius tightness spread twist ease")
// The shapers working on each strand alone (transform_ops.cpp, deform_ops.cpp DisplaceOp,
// extra_ops.cpp TwistOp). Direction's mapped fields are the ones it maps (directionMapped).
GROOMLAB_STAGE(Bend, "bend", "angle", "", "", "direction", "affect angle start end randomness seed")
GROOMLAB_STAGE(Direction, "direction", "", "", "", "direction", "affect inclination roll strength inclinationRandom rollRandom seed")
GROOMLAB_STAGE(Transform, "transform", "scale", "", "", "translate rotate pivot translateRandom rotateRandom",
               "affect scale scaleRandom seed")
GROOMLAB_STAGE(Displace, "displace", "distance", "", "profile", "direction", "affect distance")
GROOMLAB_STAGE(Twist, "twist", "turns", "", "profile", "", "affect turns")
// The shapers reading their neighbours or a reference (look_chain.cpp's section of them).
// (BlendNeighbors' neighbors, Tangle's fraction, radius and seed pick whom a strand reads: the CPU's.)
GROOMLAB_STAGE(Smooth, "smooth", "strength", "", "profile", "", "affect strength scale")
GROOMLAB_STAGE(RestoreLength, "restoreLength", "", "", "", "", "affect")
GROOMLAB_STAGE(BlendNeighbors, "blendNeighbors", "strength", "", "profile", "", "affect strength")
GROOMLAB_STAGE(Tangle, "tangle", "strength", "", "", "", "affect strength start span reach")
GROOMLAB_STAGE(Flow, "flow", "lift", "", "", "", "affect lift bend rollRandom seed")
// Jiggle with motion (motion_ops.cpp JiggleOp; its history: the frames the host sampled for it
// read again, more refused: readLookParams); MeshCut (mesh_cut_op.cpp).
GROOMLAB_STAGE(Jiggle, "jiggle", "strength", "", "profile", "", "affect strength frequency damping history")
GROOMLAB_STAGE(MeshCut, "meshCut", "", "", "", "", "affect offset")
// A Merge without a seam to blend (look_ops.cpp MergeOp): the run goes up its input a; b's
// strands, as cached, follow a's in the base and are handed on (LookStage::prefix).
GROOMLAB_STAGE(Merge, "merge", "", "", "", "", "")
// The nodes keeping or adding strands (extra_ops.cpp CullOp, feather_ops.cpp SplitOp's outputs,
// mirror_op.cpp MirrorOp): a run through one starts with it, the nodes before it a run of their own
// (LookBranch), their strands copied into its result as its last evaluation laid that out (look_chain.cpp
// gathers). Their own params pick the strands: the CPU's.
GROOMLAB_STAGE(Cull, "cull", "", "", "", "", "")
GROOMLAB_STAGE(Split, "split", "", "", "", "", "")
GROOMLAB_STAGE(Mirror, "mirror", "", "", "", "", "")

// (Wet's other params pick its levels' centers: the CPU's.)
GROOMLAB_STAGE_NODE(Wet, Clump, "", "", "affect wetness")
GROOMLAB_GPU_PARAMS(Sculpt, "", "", "strength")

// RenderData (the shader's channels) and Cards (its card layer) too: their channels and cards stay
// as the last evaluation made them while the GPU previews, and the commit makes them again from
// the committed strands. StrandMesh hands its strands on as they are (hosts make its mesh).
// Instance's instances stay as the last evaluation placed them, and the commit places them again.
GROOMLAB_PASS_THROUGH(HairColor)
GROOMLAB_PASS_THROUGH(Bands)
GROOMLAB_PASS_THROUGH(FeatherColor)
GROOMLAB_PASS_THROUGH(Attribute)
GROOMLAB_PASS_THROUGH(RenderData)
GROOMLAB_PASS_THROUGH(Cards)
GROOMLAB_PASS_THROUGH(StrandMesh)
GROOMLAB_PASS_THROUGH(Instance)

#ifdef GROOMLAB_STAGE_DEFAULTED
#undef GROOMLAB_STAGE
#undef GROOMLAB_STAGE_DEFAULTED
#endif
#ifdef GROOMLAB_STAGE_NODE_DEFAULTED
#undef GROOMLAB_STAGE_NODE
#undef GROOMLAB_STAGE_NODE_DEFAULTED
#endif
#ifdef GROOMLAB_GPU_PARAMS_DEFAULTED
#undef GROOMLAB_GPU_PARAMS
#undef GROOMLAB_GPU_PARAMS_DEFAULTED
#endif
#ifdef GROOMLAB_PASS_THROUGH_DEFAULTED
#undef GROOMLAB_PASS_THROUGH
#undef GROOMLAB_PASS_THROUGH_DEFAULTED
#endif
