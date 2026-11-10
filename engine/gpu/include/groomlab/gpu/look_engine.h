// SPDX-License-Identifier: Apache-2.0
#pragma once

// GPU look preview: the look engine (groomlab/look/look_engine.h) on a DirectX 11 device or an
// OpenGL context (see look_chain.h for what it runs). Each new value of a drag (or brush dab) runs
// the chain's nodes and the lit lines' frames as compute kernels (shaders/look.slang, compiled by
// Slang when the module builds: DXBC for DirectX 11, GLSL for OpenGL's driver), from the strands
// entering the chain, uploaded once; the results go straight into the host's vertex buffers where
// the kernels can write them (LookTargets).

#include <cstddef>
#include <memory>
#include <string>

#include "groomlab/gpu/look_chain.h"
#include "groomlab/look/look_engine.h"

struct ID3D11Device;
struct ID3D11Buffer;

namespace groomlab::gpu {

// The engine on a DirectX 11 device (Maya's, or a test's): the kernels from their bytecode, run on
// the device's immediate context. Throws std::runtime_error.
std::unique_ptr<LookEngine> makeD3D11LookEngine(ID3D11Device* device);
// A vertex buffer the kernels write in place (vertex buffer, shader resource and unordered access,
// raw views), `bytes` long, for a host to draw from. The caller owns the reference (Release()).
// Throws std::runtime_error.
ID3D11Buffer* createD3D11DisplayBuffer(ID3D11Device* device, std::size_t bytes);

// The engine on the OpenGL context current on the calling thread: OpenGL 4.5 (the kernels are
// GLSL 4.50 compute, compiled by the driver: see prepareGLLookKernels). Its targets are buffer
// names, written in place. Throws std::runtime_error (no context, an older one).
std::unique_ptr<LookEngine> makeGLLookEngine();
// Starts the kernels' programs for the OpenGL context current on the calling thread, without
// waiting: each from the program cache when it holds it, else compiled by the driver, in the
// background where it can (KHR_parallel_shader_compile; a cold compile takes seconds). A host calls
// it early and as it draws (a groom drawn) so the first GPU preview finds them ready; the engines of
// the context share them. Cheap once they are all ready; returns whether they are. Never throws
// (false: no OpenGL 4.5 context current).
bool prepareGLLookKernels();
// Whether the kernels a run of `run` dispatches are ready on OpenGL, as the last look at them found
// them (prepareGLLookKernels, an engine's kernelsReady): asks no context, so a host arming a brush
// stroke on the GPU can ask it anywhere. `why`: which are compiling, or which did not compile.
bool glLookKernelsReady(const LookRun& run, std::string* why = nullptr);
// Whether every kernel is, so.
bool glLookKernelsAllReady();
// Forgets the kernels' programs of the context current (tests: the next engine starts them anew).
void forgetGLLookKernels();

#ifdef __APPLE__
// The engine on a Metal device (macOS: id<MTLDevice>, as the app's QRhi hands it; null: the
// system's default device). The kernels are Metal Shading Language from Slang, compiled by the
// system's Metal compiler in the background (prepareMetalLookKernels; the system caches them, so
// only a first run ever waits). Its targets are MTLBuffers (id<MTLBuffer>), written in place, and
// the point texture an RGBA32Float MTLTexture made for shader writes. Throws std::runtime_error.
std::unique_ptr<LookEngine> makeMetalLookEngine(void* device);
// The system's default Metal device (id<MTLDevice>, not retained: it lives as long as the process),
// null where there is none.
void* metalSystemDevice();
// Starts compiling the kernels for `device` (null: the default) without waiting; true once every
// one is ready. Never throws.
bool prepareMetalLookKernels(void* device);
// A buffer of `bytes` on `device` (null: the default) in memory the CPU shares (Apple silicon's
// unified memory): a target the kernels write that a host can read without a copy. Retained for
// the caller (releaseMetalBuffer). metalBufferContents: its memory (the GPU's writes visible once
// the engine's run is finished: LookEngine::finish).
void* createMetalBuffer(void* device, std::size_t bytes);
// Waits for every run committed so far, on any engine (a host about to read a target's memory).
void finishMetalRuns();
void releaseMetalBuffer(void* buffer);
void* metalBufferContents(void* buffer);
#endif

}  // namespace groomlab::gpu
