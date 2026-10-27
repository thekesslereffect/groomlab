// SPDX-License-Identifier: Apache-2.0
// The look engine on the CPU: the kernels of engine/gpu/shaders/look.slang compiled to C++ by slangc
// (generated/look_kernels_cpu.cpp, kept in the repository: the very source the GPU runs, the core's
// operators' bits), its buffers in memory, a dispatch its groups across the core's threads.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

#include "groomlab/flow.h"
#include "groomlab/operator_state.h"
#include "groomlab/parallel.h"
#include "groomlab/look/look_engine_internal.h"
#include "look_kernels_cpu_support.h"
#include "slang-cpp-prelude.h"

#ifdef SLANG_PRELUDE_NAMESPACE
using namespace SLANG_PRELUDE_NAMESPACE;
#endif

using CpuKernelFn = void (*)(ComputeVaryingInput*, void*, void*);
#define GROOMLAB_KERNEL(Name) extern "C" void Name##CS(ComputeVaryingInput*, void*, void*);
#include "groomlab/look/look_kernels.inc"
#undef GROOMLAB_KERNEL

namespace groomlab::gpu {

namespace cpu_host {

namespace {
const FlowHost& flowHost(std::uint64_t host) { return *reinterpret_cast<const FlowHost*>(static_cast<std::uintptr_t>(host)); }
const CollideHost& collideHost(std::uint64_t host) { return *reinterpret_cast<const CollideHost*>(static_cast<std::uintptr_t>(host)); }
// A strand's walk state as the kernel holds it (9 words: the cursor's vertex, 0xffffffff none, its
// anchor xyz and clear; the bound's anchor xyz and the bound) and as collideRestPush takes it.
CollideRestState restState(const float w[9]) {
    CollideRestState st;
    std::uint32_t vertex = 0;
    std::memcpy(&vertex, &w[0], 4);
    st.cursor.vertex = vertex == 0xFFFFFFFFu ? RestPoseCollision::Cursor::kNone : static_cast<std::size_t>(vertex);
    st.cursor.anchor = Vec3f(w[1], w[2], w[3]);
    st.cursor.clear = w[4];
    st.anchor = Vec3f(w[5], w[6], w[7]);
    st.bound = w[8];
    return st;
}
void putState(const CollideRestState& st, float w[9]) {
    const std::uint32_t vertex = st.cursor.vertex == RestPoseCollision::Cursor::kNone ? 0xFFFFFFFFu : static_cast<std::uint32_t>(st.cursor.vertex);
    std::memcpy(&w[0], &vertex, 4);
    w[1] = st.cursor.anchor.x;
    w[2] = st.cursor.anchor.y;
    w[3] = st.cursor.anchor.z;
    w[4] = st.cursor.clear;
    w[5] = st.anchor.x;
    w[6] = st.anchor.y;
    w[7] = st.anchor.z;
    w[8] = st.bound;
}
void put(const Vec3f& v, float out[3]) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}
}  // namespace

void flowAt(std::uint64_t host, const float p[3], const float normal[3], float out[3]) {
    put(flowHost(host).flow->at(Vec3f(p[0], p[1], p[2]), Vec3f(normal[0], normal[1], normal[2])), out);
}

void flowAtRoot(std::uint64_t host, std::uint32_t face, float u, float v, const float normal[3], float out[3]) {
    const FlowHost& h = flowHost(host);
    put(h.flow->atRoot(*h.scalp, StrandRoot{static_cast<std::int32_t>(face), u, v}, Vec3f(normal[0], normal[1], normal[2])), out);
}

void collideCursor(std::uint64_t host, std::uint32_t mesh, std::uint32_t strand, const float p[3], float state[9]) {
    const CollideHost& h = collideHost(host);
    if (!h.rests[mesh]) return;  // (method exact: no rest pose to walk)
    CollideRestState st = restState(state);
    const StrandRoot root = h.roots && strand < h.roots->size() ? (*h.roots)[strand] : StrandRoot{-1, 0.0f, 0.0f};
    st.cursor = h.rests[mesh]->cursor(root, Vec3f(p[0], p[1], p[2]));
    putState(st, state);
}

bool collidePush(std::uint64_t host, std::uint32_t mesh, float p[3], float to, bool soft, float state[9]) {
    const CollideHost& h = collideHost(host);
    CollideRestState st = restState(state);
    Vec3f q(p[0], p[1], p[2]);
    const bool moved = collideRestPush(h.rests[mesh].get(), *h.meshes[mesh], q, to, soft, h.softness, h.maxDepth, h.searchRadius, st);
    put(q, p);
    putState(st, state);
    return moved;
}

void gridFloorCursor(std::uint64_t host, std::uint32_t strand, const float p[3], float state[9]) {
    const HairGridInputs& h = *reinterpret_cast<const HairGridInputs*>(static_cast<std::uintptr_t>(host));
    CollideRestState st = restState(state);
    const StrandRoot root = h.roots && strand < h.roots->size() ? (*h.roots)[strand] : StrandRoot{-1, 0.0f, 0.0f};
    st.cursor = hairGridFloorCursor(*h.layout, root, Vec3f(p[0], p[1], p[2]));
    putState(st, state);
}

bool gridFloorPush(std::uint64_t host, float p[3], float state[9]) {
    const HairGridInputs& h = *reinterpret_cast<const HairGridInputs*>(static_cast<std::uintptr_t>(host));
    CollideRestState st = restState(state);
    Vec3f q(p[0], p[1], p[2]);
    const bool moved = hairGridFloorPush(*h.layout, q, st.cursor);
    put(q, p);
    putState(st, state);
    return moved;
}

bool clearanceMeshPush(std::uint64_t host, const float p[3], float offset, float maxDepth, float searchRadius, float probe[2], float delta[3]) {
    const InterpolateClearance& c = *reinterpret_cast<const InterpolateClearance*>(static_cast<std::uintptr_t>(host));
    MeshDistanceField::Probe pr;
    pr.beside = c.beside;
    pr.learnt(probe[0], probe[1]);
    Vec3f d;
    const bool moved = meshPushOf(*c.mesh, Vec3f(p[0], p[1], p[2]), offset, maxDepth, searchRadius, &d, &pr);
    probe[0] = pr.heightLow;
    probe[1] = pr.distanceLow;
    put(d, delta);
    return moved;
}

bool flowClosest(std::uint64_t host, const float p[3], float reach, std::uint32_t* face, float uv[2], float at[3], float normal[3]) {
    const SurfacePoint hit = flowHost(host).scalp->closestPoint(Vec3f(p[0], p[1], p[2]), reach);
    if (hit.root.face < 0) return false;
    *face = static_cast<std::uint32_t>(hit.root.face);
    uv[0] = hit.root.u;
    uv[1] = hit.root.v;
    put(hit.position, at);
    put(hit.normal, normal);
    return true;
}

}  // namespace cpu_host

namespace {

// look.slang's resources as slangc lays them out for C++ (its GlobalParams: the cbuffer, then the
// t-registers, the u-registers, the texture), in register order.
struct CpuGlobals {
    const StageConstants* look = nullptr;
    ByteAddressBuffer offsets{}, strandData{}, in{}, ramps{}, fields{}, aux0{}, aux1{}, aux2{}, pointStrands{};
    RWByteAddressBuffer out{}, out2{}, out3{};
    RWTexture2D<Vector<float, 4>> texture{};
};

const CpuKernelFn* cpuKernels() {
    static const auto table = [] {
        std::vector<CpuKernelFn> t(static_cast<std::size_t>(LookEngine::Kernel::Count), nullptr);
#define GROOMLAB_KERNEL(Name) t[static_cast<std::size_t>(LookEngine::Kernel::Name)] = &Name##CS;
#include "groomlab/look/look_kernels.inc"
#undef GROOMLAB_KERNEL
        return t;
    }();
    return table.data();
}

// A buffer: words of its own, or a host's memory it is written in place (a target); shared so a
// sibling's can be borrowed.
struct Buffer {
    std::unique_ptr<std::uint32_t[]> own;  // (uninitialized: a buffer the kernels write is undefined till then)
    std::size_t ownWords = 0;
    std::uint32_t* host = nullptr;
    std::size_t hostWords = 0;
    std::uint32_t* data() { return host ? host : own.get(); }
    std::size_t size() const { return host ? hostWords : ownWords; }
};
using Words = std::shared_ptr<Buffer>;
Words ownWords(std::size_t bytes) {
    auto w = std::make_shared<Buffer>();
    w->ownWords = std::max<std::size_t>((bytes + 3) / 4, 1);
    w->own.reset(new std::uint32_t[w->ownWords]);
    return w;
}
Words hostWords(void* memory, std::size_t bytes) {
    auto w = std::make_shared<Buffer>();
    w->host = static_cast<std::uint32_t*>(memory);
    w->hostWords = bytes / 4;
    return w;
}

class CpuLookEngine final : public LookEngine {
public:
    // A thread a strand (the per-point kernels walk each strand's arc to every point: the GPU's
    // width is the CPU's waste).
    CpuLookEngine() { setPointThreads(false); }

    GpuApi api() const override { return GpuApi::Cpu; }
    void* device() const override { return nullptr; }
    std::unique_ptr<LookEngine> sibling() override { return std::make_unique<CpuLookEngine>(); }
    void borrow(int id, LookEngine& from, int fromId) override {
        auto& other = dynamic_cast<CpuLookEngine&>(from);
        if (!other.has(fromId)) throw std::runtime_error("CPU look engine: a sibling's buffer to borrow is missing");
        buffers_[id] = other.words(fromId);
    }
    std::size_t videoBytes() const override { return 0; }
    void dropTargets() override {
        for (int i = 0; i < 4; ++i) {
            if (buffers_.count(kOut0 + i) && buffers_[kOut0 + i]->host) buffers_.erase(kOut0 + i);
        }
        buffers_.erase(kOutUvs);
    }
    void finish() override {}

protected:
    void write(int id, const void* data, std::size_t bytes, bool) override {
        Words& w = buffers_[id];
        w = ownWords(bytes);
        if (bytes) std::memcpy(w->data(), data, bytes);
    }
    void writeRange(int id, std::size_t offset, const void* data, std::size_t bytes) override {
        const auto it = buffers_.find(id);
        if (it == buffers_.end() || !bytes) return;
        std::memcpy(reinterpret_cast<std::uint8_t*>(it->second->data()) + offset, data, bytes);
    }
    void allocate(int id, std::size_t bytes) override {
        Words& w = buffers_[id];
        if (!w || w->size() * 4 < bytes) w = ownWords(bytes);
    }
    // The base's points and widths: the strands' own arrays (the engine holds them; never written).
    void share(int id, const void* data, std::size_t bytes) override { buffers_[id] = hostWords(const_cast<void*>(data), bytes); }
    bool sharesBase() const override { return true; }
    void* writable(int id, std::size_t bytes) override {
        Words& w = buffers_[id];
        if (!w || w->host || w->ownWords < (bytes + 3) / 4) w = ownWords(bytes);
        return w->data();
    }
    bool has(int id) const override { return buffers_.count(id) != 0; }
    void release(int id) override { buffers_.erase(id); }
    std::shared_ptr<void> parkBuffer(int id) override {
        const auto it = buffers_.find(id);
        if (it == buffers_.end()) return nullptr;
        std::shared_ptr<void> parked = it->second;
        buffers_.erase(it);
        return parked;
    }
    void unparkBuffer(int id, std::shared_ptr<void> parked) override {
        if (parked) {
            buffers_[id] = std::static_pointer_cast<Words::element_type>(std::move(parked));
        } else {
            buffers_.erase(id);
        }
    }

    // A host's targets (float3 per point, the uvs' float2, in memory) are written in place; the
    // outputs it gives none of are the engine's own.
    bool bindTargets(const LookTargets& targets, bool frames, std::size_t bytes, std::size_t uvBytes) override {
        if (targets.pointTexture) throw std::runtime_error("CPU look engine: no point texture");
        if (targets.widths) buffers_[kWidths] = hostWords(targets.widths, bytes / 3);
        void* const want[4] = {targets.positions, targets.normals, targets.tangents, targets.bitangents};
        for (int i = 0; i < 4; ++i) {
            if ((i > 0 && !frames) || (i == 0 && !want[0] && targets.baseAsOutput)) {
                buffers_.erase(kOut0 + i);
                continue;
            }
            if (want[i]) {
                buffers_[kOut0 + i] = hostWords(want[i], bytes);
            } else {
                if (buffers_.count(kOut0 + i) && buffers_[kOut0 + i]->host) buffers_.erase(kOut0 + i);
                allocate(kOut0 + i, bytes);
            }
        }
        if (targets.uvs) {
            buffers_[kOutUvs] = hostWords(targets.uvs, uvBytes);
        } else {
            buffers_.erase(kOutUvs);
        }
        return targets.positions != nullptr;
    }
    void copyToTargets(bool, std::size_t) override {}
    void copy(int from, int to, std::size_t bytes, std::size_t fromOffset, std::size_t toOffset) override {
        if (!bytes) return;
        const Words src = words(from), dst = words(to);
        std::memmove(reinterpret_cast<std::uint8_t*>(dst->data()) + toOffset, reinterpret_cast<const std::uint8_t*>(src->data()) + fromOffset,
                     bytes);
    }
    void setConstants(const void* slots, std::size_t count, std::size_t slotBytes) override {
        constants_.resize(count);
        for (std::size_t i = 0; i < count; ++i) std::memcpy(&constants_[i], static_cast<const char*>(slots) + i * slotBytes, sizeof(StageConstants));
    }
    void dispatch(Kernel kernel, std::size_t slot, const Binding& b, std::size_t groupsX, std::size_t groupsY) override {
        const CpuKernelFn fn = cpuKernels()[static_cast<std::size_t>(kernel)];
        if (!fn) throw std::runtime_error(std::string("CPU look engine: no CPU form of the ") + kernelName(kernel) + " kernel");
        if (b.texture) throw std::runtime_error("CPU look engine: no point texture");
        if (!groupsX || !groupsY) return;
        CpuGlobals g;
        g.look = &constants_.at(slot);
        auto ro = [&](int id) {
            ByteAddressBuffer r{};
            if (id >= 0 && has(id)) {
                const Words& w = buffers_.at(id);
                r.data = w->data();
                r.sizeInBytes = w->size() * 4;
            }
            return r;
        };
        auto rw = [&](int id) {
            RWByteAddressBuffer r{};
            if (id >= 0 && has(id)) {
                const Words& w = buffers_.at(id);
                r.data = w->data();
                r.sizeInBytes = w->size() * 4;
            }
            return r;
        };
        g.offsets = ro(b.offsets >= 0 ? b.offsets : static_cast<int>(kOffsets));
        g.strandData = ro(kStrandData);
        g.in = ro(b.in);
        g.ramps = ro(kRamps);
        g.fields = b.out[0] == kFields ? ByteAddressBuffer{} : ro(kFields);
        g.aux0 = ro(b.aux[0]);
        g.aux1 = ro(b.aux[1]);
        g.aux2 = ro(b.aux[2]);
        g.pointStrands = b.out[0] == kPointStrands ? ByteAddressBuffer{} : ro(kPointStrands);
        g.out = rw(b.out[0]);
        g.out2 = rw(b.out[1]);
        g.out3 = rw(b.out[2]);
        // Every group a thread's work (the kernels write apart, as on the GPU), a row's groups split
        // across the core's threads.
        const std::size_t groups = groupsX * groupsY;
        parallelFor(0, groups, [&](std::size_t i) {
            ComputeVaryingInput v{};
            v.startGroupID = {static_cast<uint32_t>(i % groupsX), static_cast<uint32_t>(i / groupsX), 0};
            v.endGroupID = {v.startGroupID.x + 1, v.startGroupID.y + 1, 1};
            fn(&v, nullptr, &g);
        }, 1);  // (a group is 64 or 256 threads' work: each its own task)
    }
    std::size_t countedGroups(int id, std::uint32_t word, std::uint32_t per, std::size_t groups) override {
        if (!has(id) || buffers_.at(id)->size() <= word) return groups;
        const std::size_t count = buffers_.at(id)->data()[word];
        return std::min(groups, (count + per - 1) / per);
    }
    bool runsIf(int id, std::uint32_t least) override {
        return has(id) && buffers_.at(id)->size() > 0 && buffers_.at(id)->data()[0] >= least;
    }
    void beginRun() override { stampTimes_.clear(); }
    void stamp(std::size_t index) override {
        if (stampTimes_.size() <= index) stampTimes_.resize(index + 1);
        stampTimes_[index] = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                            std::chrono::steady_clock::now().time_since_epoch())
                                                            .count());
    }
    void endRun(std::size_t stamps) override {
        stampTimes_.resize(std::min(stamps, stampTimes_.size()));
        ticksPending_ = true;
    }
    bool stampTicks(bool, std::vector<std::uint64_t>* ticks, double* frequency) override {
        if (!ticksPending_) return false;
        *ticks = stampTimes_;
        *frequency = 1e9;
        ticksPending_ = false;
        return true;
    }
    std::vector<std::uint8_t> readBack(int id, std::size_t size) override {
        std::vector<std::uint8_t> bytes(size);
        if (!has(id) || !size) return bytes;
        const Words& w = buffers_.at(id);
        std::memcpy(bytes.data(), w->data(), std::min(size, w->size() * 4));
        return bytes;
    }

private:
    const Words& words(int id) const {
        const auto it = buffers_.find(id);
        if (it == buffers_.end()) throw std::runtime_error("CPU look engine: a buffer not made");
        return it->second;
    }

    std::map<int, Words> buffers_;
    std::vector<StageConstants> constants_;
    std::vector<std::uint64_t> stampTimes_;
    bool ticksPending_ = false;
};

}  // namespace

std::unique_ptr<LookEngine> makeCpuLookEngine() { return std::make_unique<CpuLookEngine>(); }

}  // namespace groomlab::gpu
