// SPDX-License-Identifier: Apache-2.0
// Every built-in node type, one line each, in the add-node menu's order (by category, the
// sections below in nodeCategories()' order; within one, the order the menu lists them).
// An X-macro list (no include guard: included once per use):
//
//   GROOMLAB_OPERATOR(Type): a type the core registers. operators.h makes it op::kType = "Type"
//     (what graph JSON stores: never renamed), operators/internal.h declares
//     registerTypeOperator(OperatorRegistry&), defined in the type's source file next to its
//     operator with the node's menu info (NodeInfo: category, description, search words), and
//     registerBuiltinOperators calls it.
//   GROOMLAB_OPERATOR_ELSEWHERE(Type): a type registered outside the core (engine/io's
//     ImportCurves: the hosts register it), listed for its place in the menu only.
//
// A new node is a line here in its category's section (tools/dev/new_node.py adds it; the
// comment says what it takes and gives) and its source file: docs/EXTENDING.md.

#ifndef GROOMLAB_OPERATOR
#define GROOMLAB_OPERATOR(Type)
#define GROOMLAB_OPERATOR_DEFAULTED
#endif
#ifndef GROOMLAB_OPERATOR_ELSEWHERE
#define GROOMLAB_OPERATOR_ELSEWHERE(Type)
#define GROOMLAB_OPERATOR_ELSEWHERE_DEFAULTED
#endif

// [source]
GROOMLAB_OPERATOR(Scalp)                 // external "scalp" -> Mesh
GROOMLAB_OPERATOR(Guides)                // external "guides" -> Guides
GROOMLAB_OPERATOR(ColliderMesh)          // external "mesh:<name>" -> Mesh (a collider, say)
GROOMLAB_OPERATOR_ELSEWHERE(ImportCurves)  // [Mesh] -> Guides, from an Alembic file (alembic_operators.h)
GROOMLAB_OPERATOR(Reference)             // -> Strands, Guides (another groom file's result: a library look)
GROOMLAB_OPERATOR(Input)                 // a compound input inside its subgraph
// [grow]
GROOMLAB_OPERATOR(Scatter)               // Mesh -> Strands
GROOMLAB_OPERATOR(Interpolate)           // Strands + Guides -> Strands
GROOMLAB_OPERATOR(Cull)                  // Strands -> Strands (thinned by a map)
// [shape]
GROOMLAB_OPERATOR(Length)                // Strands -> Strands
GROOMLAB_OPERATOR(Width)                 // Strands -> Strands
GROOMLAB_OPERATOR(Resample)              // Curves -> Curves (segments per strand)
GROOMLAB_OPERATOR(Direction)             // Curves -> Curves (turned about the root to an inclination and roll)
GROOMLAB_OPERATOR(Flow)                  // Strands -> Strands (laid along the flow arrows on the scalp)
GROOMLAB_OPERATOR(Part)                  // Strands -> Strands (opens the parts)
GROOMLAB_OPERATOR(Smooth)                // Curves -> Curves (bumps up to a size relaxed, at 1 straight; lengths kept)
GROOMLAB_OPERATOR(Mirror)                // Curves -> Curves (mirrored across a plane, rooted again on the scalp)
GROOMLAB_OPERATOR(RestoreLength)         // Curves (+ Curves reference) -> Curves (segment lengths restored)
GROOMLAB_OPERATOR(Sculpt)                // Guides (+ Mesh scalp) -> Guides (brush strokes kept in the node: sculpt.h)
GROOMLAB_OPERATOR(Subdivide)             // Curves -> Curves (segments doubled per level along a smooth curve; viewport/render levels)
// [deform]
GROOMLAB_OPERATOR(Bend)                  // Curves -> Curves (bent toward a direction between a start and an end)
GROOMLAB_OPERATOR(Transform)             // Curves -> Curves (moved, turned, scaled about each root or a pivot)
GROOMLAB_OPERATOR(BlendNeighbors)        // Curves -> Curves (each strand toward its neighbours' mean shape)
GROOMLAB_OPERATOR(Gravity)               // Curves -> Curves (bent from the root under their weight)
GROOMLAB_OPERATOR(Displace)              // Curves -> Curves (offset along the scalp's normal or a direction)
// [collide]
GROOMLAB_OPERATOR(Collide)               // Strands + Mesh (+ Mesh collider) -> Strands (push out of the meshes)
GROOMLAB_OPERATOR(SelfCollide)           // Strands -> Strands (overpacked hair pushed apart: no strand through another)
GROOMLAB_OPERATOR(MeshCut)               // Curves (+ Mesh) -> Curves (cut where they cross the mesh)
// [look]
GROOMLAB_OPERATOR(Clump)                 // Strands + Guides (centers) -> Strands
GROOMLAB_OPERATOR(Twist)                 // Strands (+ Guides axes) -> Strands (turned around an axis)
GROOMLAB_OPERATOR(Braid)                 // Strands + Guides (braid paths) -> Strands
GROOMLAB_OPERATOR(Noise)                 // Strands -> Strands
GROOMLAB_OPERATOR(Curl)                  // Strands -> Strands
GROOMLAB_OPERATOR(Flyaway)               // Strands -> Strands (stray hairs)
GROOMLAB_OPERATOR(Static)                // Strands -> Strands (charged hair repels itself: puffed out, tips lifted, locks frayed)
GROOMLAB_OPERATOR(Tangle)                // Strands -> Strands (mats, wet strands in pairs)
GROOMLAB_OPERATOR(Wet)                   // Strands -> Strands (small clumps near the root merging into larger ones toward the tip)
GROOMLAB_OPERATOR(Instance)              // Strands -> Strands (geometry instanced on them)
GROOMLAB_OPERATOR(Scales)                // Strands -> Strands (a scale at each root, on the skin)
// [motion]
GROOMLAB_OPERATOR(Wind)                  // Curves -> Curves (bent by a wind moving through space and time)
GROOMLAB_OPERATOR(Jiggle)                // Curves -> Curves (lag and overshoot from the scalp's recent motion)
GROOMLAB_OPERATOR(Simulate)              // Guides (+ Mesh collider) -> Guides (the hair solver's frame: simulation.h)
// [feathers]
GROOMLAB_OPERATOR(Propagate)             // Curves (shafts) -> Strands (barbs grown off each shaft: feathers)
GROOMLAB_OPERATOR(Plume)                 // Strands (feathers) (+ Mesh collider) -> Strands (laid over one another and off the body)
// [color]
GROOMLAB_OPERATOR(HairColor)             // Strands -> Strands (melanin, linear RGB and highlight channels)
GROOMLAB_OPERATOR(Bands)                 // Strands -> Strands (bands of another pigment along each hair: fur)
GROOMLAB_OPERATOR(FeatherColor)          // Strands -> Strands (feather patterns, down color, iridescence)
GROOMLAB_OPERATOR(RenderData)            // Strands -> Strands (the opacity channel for the shader)
// [convert]
GROOMLAB_OPERATOR(ToGuides)              // Strands -> Guides (procedural guide curves)
GROOMLAB_OPERATOR(ToStrands)             // Guides -> Strands (each guide a strand)
GROOMLAB_OPERATOR(StrandMesh)            // Strands -> Strands (as they came; hosts make a tube mesh of them)
// [field]
GROOMLAB_OPERATOR(Map)                   // -> Field (a map painted on the scalp: guide layer, vertex map or texture; or an image)
GROOMLAB_OPERATOR(Expression)            // -> Field (a per-strand expression)
GROOMLAB_OPERATOR(Control)               // -> Field (a host value by name: a keyable slider in Maya)
GROOMLAB_OPERATOR(FieldNoise)            // -> Field (3D noise over the scalp)
GROOMLAB_OPERATOR(FieldRemap)            // Field -> Field (range and curve)
GROOMLAB_OPERATOR(FieldCombine)          // Field + Field -> Field
GROOMLAB_OPERATOR(Curve)                 // -> Curve (a reusable ramp for ramp params)
// [utility]
GROOMLAB_OPERATOR(Value)                 // -> Value (one value for a whole node: number, integer, switch, vector, text)
GROOMLAB_OPERATOR(Group)                 // Curves -> Curves (a named 0-1 strand channel: a region, map or guide groups)
GROOMLAB_OPERATOR(Blend)                 // Strands + Strands -> Strands (two looks of the same strands)
GROOMLAB_OPERATOR(Attribute)             // Strands -> Strands (adds a channel)
GROOMLAB_OPERATOR(Split)                 // Curves -> Curves a, b, c, d (shares of the strands, by ID or a map)
GROOMLAB_OPERATOR(Merge)                 // Strands + Strands -> Strands
GROOMLAB_OPERATOR(Switch)                // Strands | Strands -> Strands
GROOMLAB_OPERATOR(Output)                // Strands -> Strands (graph result)
GROOMLAB_OPERATOR(Compound)              // a subgraph as one node (PG-6)
GROOMLAB_OPERATOR(Script)                // Strands -> Strands (a script node: script.h, from a .glnode file)
// [games]
GROOMLAB_OPERATOR(Cards)                 // Strands -> Strands (+ game cards from these strands)
// [end]

#ifdef GROOMLAB_OPERATOR_DEFAULTED
#undef GROOMLAB_OPERATOR
#undef GROOMLAB_OPERATOR_DEFAULTED
#endif
#ifdef GROOMLAB_OPERATOR_ELSEWHERE_DEFAULTED
#undef GROOMLAB_OPERATOR_ELSEWHERE
#undef GROOMLAB_OPERATOR_ELSEWHERE_DEFAULTED
#endif
