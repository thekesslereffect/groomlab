// SPDX-License-Identifier: Apache-2.0
// Every compute kernel of shaders/look.slang, one line each: GROOMLAB_KERNEL(Name) is its entry
// point NameCS. An X-macro list (no include guard: included once per use) that makes
// LookEngine::Kernel (look_engine.h, in this order), kernelName, the DirectX 11 and OpenGL
// backends' tables of compiled kernels (look_engine_d3d11.cpp, look_engine_gl.cpp), and the
// kernels engine/gpu/CMakeLists.txt compiles and embeds (it reads the lines starting
// "GROOMLAB_KERNEL(" with a regex: one a line, nothing before it).
//
// A new kernel: a line here and its [shader("compute")] NameCS in look.slang; what runs it
// (look_engine.cpp: kernelsOf and the stage's dispatch) is the stage's code (docs/EXTENDING.md).

#ifndef GROOMLAB_KERNEL
#define GROOMLAB_KERNEL(Name)
#define GROOMLAB_KERNEL_DEFAULTED
#endif

GROOMLAB_KERNEL(Interpolate)
GROOMLAB_KERNEL(Clearance)
GROOMLAB_KERNEL(Length)
GROOMLAB_KERNEL(Centers)
GROOMLAB_KERNEL(Clump)
GROOMLAB_KERNEL(Noise)
GROOMLAB_KERNEL(NoisePoints)
GROOMLAB_KERNEL(Curl)
GROOMLAB_KERNEL(Width)
GROOMLAB_KERNEL(Lines)
GROOMLAB_KERNEL(LinesPoints)
// The standalone app's point texture (look_engine.h LookTargets::pointTexture).
GROOMLAB_KERNEL(PointsTexture)
GROOMLAB_KERNEL(PointsTexturePoints)
GROOMLAB_KERNEL(Gather)
GROOMLAB_KERNEL(UvWidths)
GROOMLAB_KERNEL(PointStrands)
GROOMLAB_KERNEL(WidthPoints)
GROOMLAB_KERNEL(Share)
GROOMLAB_KERNEL(CurlAxes)
GROOMLAB_KERNEL(Part)
GROOMLAB_KERNEL(Longest)
GROOMLAB_KERNEL(Field)
GROOMLAB_KERNEL(Gravity)
GROOMLAB_KERNEL(Flyaway)
GROOMLAB_KERNEL(Collide)
GROOMLAB_KERNEL(Wind)
GROOMLAB_KERNEL(Blend)
GROOMLAB_KERNEL(BlendPoints)
// The hair grid (SelfCollide and Static), then each node's own.
GROOMLAB_KERNEL(GridClear)
GROOMLAB_KERNEL(GridBounds)
GROOMLAB_KERNEL(GridLay)
GROOMLAB_KERNEL(GridMark)
GROOMLAB_KERNEL(GridDilate)
GROOMLAB_KERNEL(GridCompact)
GROOMLAB_KERNEL(GridZero)
GROOMLAB_KERNEL(GridSplat)
GROOMLAB_KERNEL(GridUnknowns)
GROOMLAB_KERNEL(GridSor)
GROOMLAB_KERNEL(GridWalk)
GROOMLAB_KERNEL(SelfCollideNode)
GROOMLAB_KERNEL(SelfCollideGradient)
GROOMLAB_KERNEL(SelfCollidePush)
GROOMLAB_KERNEL(StaticReach)
GROOMLAB_KERNEL(StaticMoments)
GROOMLAB_KERNEL(StaticNode)
GROOMLAB_KERNEL(StaticField)
GROOMLAB_KERNEL(StaticPush)
// Tie: its frame and ring's bins, each strand's route, the strands laid.
GROOMLAB_KERNEL(TieFrame)
GROOMLAB_KERNEL(TieRoute)
GROOMLAB_KERNEL(TieLay)
// The shapers working on each strand alone (TwistAxes: a Twist's clumps' middles).
GROOMLAB_KERNEL(Bend)
GROOMLAB_KERNEL(Direction)
GROOMLAB_KERNEL(Transform)
GROOMLAB_KERNEL(Displace)
GROOMLAB_KERNEL(Twist)
GROOMLAB_KERNEL(TwistAxes)
// The shapers reading their neighbours or a reference.
GROOMLAB_KERNEL(Smooth)
GROOMLAB_KERNEL(RestoreLength)
GROOMLAB_KERNEL(BlendNeighbors)
GROOMLAB_KERNEL(Tangle)
GROOMLAB_KERNEL(Flow)
// A masked node's result mixed back by its affect; Jiggle with motion; MeshCut.
GROOMLAB_KERNEL(Affect)
GROOMLAB_KERNEL(Jiggle)
GROOMLAB_KERNEL(MeshCut)
// Cull, Split and Mirror: the strands of the run up their input copied into their layout.
GROOMLAB_KERNEL(Layout)

#ifdef GROOMLAB_KERNEL_DEFAULTED
#undef GROOMLAB_KERNEL
#undef GROOMLAB_KERNEL_DEFAULTED
#endif
