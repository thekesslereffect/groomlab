// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "groomlab/preview_core.h"
#include "groomlab/random.h"
#include "groomlab/look/look_engine_internal.h"
#include "groomlab/look/look_stage_params.h"

namespace groomlab::gpu {

const char* apiName(GpuApi api) { return api == GpuApi::D3D11 ? "DirectX11" : api == GpuApi::Cpu ? "CPU" : "OpenGL"; }

LookEngine::~LookEngine() = default;

const char* kernelName(LookEngine::Kernel kernel) {
    static const char* const names[] = {
#define GROOMLAB_KERNEL(Name) #Name,
#include "groomlab/look/look_kernels.inc"
#undef GROOMLAB_KERNEL
    };
    static_assert(sizeof names / sizeof names[0] == static_cast<std::size_t>(LookEngine::Kernel::Count), "a name a kernel");
    const auto k = static_cast<std::size_t>(kernel);
    return k < static_cast<std::size_t>(LookEngine::Kernel::Count) ? names[k] : "?";
}

std::string kernelNames(KernelSet kernels) {
    std::string names;
    for (int k = 0; k < static_cast<int>(LookEngine::Kernel::Count); ++k) {
        if (kernels.test(static_cast<std::size_t>(k))) names += std::string(names.empty() ? "" : ", ") + kernelName(static_cast<LookEngine::Kernel>(k));
    }
    return names;
}

KernelSet kernelsOf(const LookRun& run) {
    using K = LookEngine::Kernel;
    auto bit = [](K k) { return KernelSet().set(static_cast<std::size_t>(k)); };
    KernelSet set;
    for (K k : {K::Lines, K::LinesPoints, K::Gather, K::UvWidths, K::PointStrands, K::PointsTexture, K::PointsTexturePoints, K::Field,
                K::Longest, K::Width, K::WidthPoints}) {
        set |= bit(k);
    }
    const std::vector<StageParams>& stages = run.params.stages;
    for (std::size_t k = 0; k < stages.size(); ++k) {
        const StageInputs* in = k < run.inputs.size() ? &run.inputs[k] : nullptr;
        switch (stages[k].stage.kind) {
            case StageKind::Interpolate:
                set |= bit(K::Interpolate);
                if (in && in->interpolate && in->interpolate->clearance) set |= bit(K::Clearance);
                break;
            case StageKind::Length: set |= bit(K::Length); break;
            case StageKind::Clump:
                set |= bit(K::Clump) | bit(K::Centers) | bit(K::Share);
                if (in && in->clump && in->clump->centersRun) {
                    const LookCentersRun& centers = *in->clump->centersRun;
                    LookRun branch;
                    branch.params = centers.params;
                    branch.inputs = centers.inputs;
                    set |= kernelsOf(branch);
                }
                break;
            case StageKind::Noise: set |= bit(K::Noise) | bit(K::NoisePoints); break;
            case StageKind::Curl: set |= bit(K::Curl) | bit(K::CurlAxes); break;
            case StageKind::Width: break;  // (always)
            case StageKind::Part: set |= bit(K::Part); break;
            case StageKind::Gravity: set |= bit(K::Gravity); break;
            case StageKind::Flyaway: set |= bit(K::Flyaway); break;
            case StageKind::Collide: set |= bit(K::Collide); break;
            case StageKind::Wind: set |= bit(K::Wind); break;
            case StageKind::Blend: set |= bit(K::Blend) | bit(K::BlendPoints); break;
            case StageKind::SelfCollide:
            case StageKind::Static:
                for (K grid : {K::GridClear, K::GridBounds, K::GridLay, K::GridMark, K::GridDilate, K::GridCompact, K::GridZero, K::GridSplat,
                               K::GridUnknowns, K::GridSor, K::GridWalk}) {
                    set |= bit(grid);
                }
                if (stages[k].stage.kind == StageKind::SelfCollide) {
                    set |= bit(K::SelfCollideNode) | bit(K::SelfCollideGradient) | bit(K::SelfCollidePush);
                } else {
                    set |= bit(K::StaticReach) | bit(K::StaticMoments) | bit(K::StaticNode) | bit(K::StaticField) | bit(K::StaticPush);
                }
                break;
            case StageKind::Tie: set |= bit(K::TieFrame) | bit(K::TieRoute) | bit(K::TieLay); break;
            case StageKind::Bend: set |= bit(K::Bend); break;
            case StageKind::Direction: set |= bit(K::Direction); break;
            case StageKind::Transform: set |= bit(K::Transform); break;
            case StageKind::Displace: set |= bit(K::Displace); break;
            case StageKind::Twist: set |= bit(K::Twist) | bit(K::TwistAxes); break;
            case StageKind::Smooth: set |= bit(K::Smooth); break;
            case StageKind::RestoreLength: set |= bit(K::RestoreLength); break;
            case StageKind::BlendNeighbors: set |= bit(K::BlendNeighbors); break;
            case StageKind::Tangle: set |= bit(K::Tangle); break;
            case StageKind::Flow: set |= bit(K::Flow); break;
            case StageKind::Jiggle: set |= bit(K::Jiggle); break;
            case StageKind::MeshCut: set |= bit(K::MeshCut); break;
            case StageKind::Cull:
            case StageKind::Split:
            case StageKind::Mirror: set |= bit(K::Layout); [[fallthrough]];  // (and the run up its input's)
            case StageKind::Merge:  // (copies; b's own run's kernels where the drag moves b)
                if (in && in->merged) {
                    LookRun branch;
                    branch.params = in->merged->params;
                    branch.inputs = in->merged->inputs;
                    set |= kernelsOf(branch);
                }
                break;
            default: set.set(); break;  // (a stage this does not know: every kernel)
        }
        if (stages[k].masked) set |= bit(K::Affect);  // (affect masks)
    }
    return set;
}

bool LookEngine::kernelsReady(const LookRun& run, std::string* why) {
    const KernelSet need = kernelsOf(run);
    KernelSet compiling;
    for (int k = 0; k < static_cast<int>(Kernel::Count); ++k) {
        if (need.test(static_cast<std::size_t>(k)) && !kernelReady(static_cast<Kernel>(k), false)) compiling.set(static_cast<std::size_t>(k));
    }
    if (compiling.any() && why) *why = "the GPU's kernels are compiling (" + kernelNames(compiling) + ")";
    return compiling.none();
}

namespace {

double since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// The strands whose points differ between two sets of the same layout (strand and point counts,
// offsets, IDs and roots: what the per-strand data held still fits), else all. In parallel; a
// strand's points compared bit for bit.
StrandChanges diffStrands(const StrandSet& before, const StrandSet& after) {
    StrandChanges all;
    const std::size_t n = after.strandCount();
    if (before.strandCount() != n || before.pointCount() != after.pointCount()) return all;
    if (&before.offsets() != &after.offsets() && before.offsets() != after.offsets()) return all;
    if (&before.ids() != &after.ids() && before.ids() != after.ids()) return all;
    for (std::size_t s = 0; s < n; ++s) {
        const StrandRoot& a = before.roots()[s];
        const StrandRoot& b = after.roots()[s];
        if (a.face != b.face || std::memcmp(&a, &b, sizeof a) != 0) return all;
    }
    if (before.positions().data() == after.positions().data()) return StrandChanges::listed({});
    std::vector<std::uint8_t> moved(n, 0);
    parallelFor(0, n, [&](std::size_t s) {
        const std::size_t begin = after.strandBegin(s), count = after.strandPointCount(s);
        moved[s] = std::memcmp(before.positions().data() + begin, after.positions().data() + begin, count * sizeof(Vec3f)) != 0;
    }, 4096);
    std::vector<std::uint32_t> list;
    for (std::size_t s = 0; s < n; ++s) {
        if (moved[s]) list.push_back(static_cast<std::uint32_t>(s));
    }
    return StrandChanges::listed(std::move(list));
}

// internal.h paramAt.
float paramAt(std::size_t i, std::size_t n) { return n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f; }

// The blocks of numbers per strand a stage has in kStrandData (every one has one; Flyaway two:
// its strays' noise offset and draw, then its lengthening's draw; Transform two: its seven draws).
std::size_t strandDataBlocks(StageKind kind) { return kind == StageKind::Flyaway || kind == StageKind::Transform ? 2 : 1; }

constexpr std::size_t kCenterFloats = kClumpCenterSamples * 3;   // a center's curve
constexpr std::size_t kFrameFloats = kClumpCenterSamples * 9;    // its frames
constexpr std::size_t kNormalFloats = 4;                         // its root normal (xyz, a pad)
constexpr int kClearanceStamp = -5;  // a run's span kind: Interpolate's walk keeping clearance
constexpr int kPointStrandsStamp = -6;  // ... each point's strand made (counted in the total alone)
constexpr std::size_t kSameOffset = static_cast<std::size_t>(-1);  // a copy's source offset: its target's

// ---- Tie -----------------------------------------------------------------------------------------

constexpr std::size_t kTieSlots = 34;       // a strand's route's float4 slots (look.slang TIE_SLOTS)
constexpr std::size_t kTieFrameWords = 96;  // the frame and the ring's bins (look.slang TIE_FRAME_*)
// Tie's switches (look.slang TIE_*): the tail channel read, written, its affect a mask, a scalp
// (routes over it), its field (the path pushed out of it).
constexpr std::uint32_t kTieChannelIn = 1u, kTieWritesTail = 2u, kTieMasked = 4u, kTieScalp = 8u, kTiePush = 16u;

// Tie's dispatches' constants (look.slang's Tie section): gValue radius, tightness, spread and
// beside (clearancePush's: 4 clearances), gValue2 ease, twist, clearance and depth, gFlag its
// switches, its affect's number's bits and the search radius's, gFlag2 center and the scalp's
// size, gOctaveSeeds' first six words the direction and the scalp's center (bits).
void tieConstants(const StageParams& p, const TieInputs* in, StageConstants* c) {
    auto bits = [](float v) {
        std::uint32_t b;
        std::memcpy(&b, &v, 4);
        return b;
    };
    const bool push = in && in->tables;
    c->value[0] = p.value[0];
    c->value[1] = p.value[1];
    c->value[2] = p.value[2];
    c->value[3] = push ? 4.0f * in->clearance : 0.0f;
    c->value2[0] = p.value2[0];
    c->value2[1] = p.value[3];
    c->value2[2] = in ? in->clearance : 0.0f;
    c->value2[3] = in ? in->depth : 0.0f;
    std::uint32_t flags = 0;
    if (in && (in->channelFrom != kNoStage || in->channel)) flags |= kTieChannelIn;
    if (p.flag[0] != 0) flags |= kTieWritesTail;
    if (p.flag[2] != 0) flags |= kTieMasked;
    if (in && in->hasScalp) flags |= kTieScalp;
    if (push) flags |= kTiePush;
    c->flag[0] = flags;
    c->flag[1] = static_cast<std::uint32_t>(p.flag[1]);
    c->flag[2] = bits(in ? in->searchRadius : 0.0f);
    c->flag[3] = 0;
    for (int i = 0; i < 3; ++i) c->flag2[i] = static_cast<std::uint32_t>(p.flag2[i]);
    c->flag2[3] = bits(in ? in->scalpSize : 0.0f);
    for (int i = 0; i < 3; ++i) c->octaveSeeds[i] = bits(p.value2[1 + i]);
    const Vec3f center = in ? in->scalpCenter : Vec3f{};
    c->octaveSeeds[3] = bits(center.x);
    c->octaveSeeds[4] = bits(center.y);
    c->octaveSeeds[5] = bits(center.z);
    for (int i = 6; i < 16; ++i) c->octaveSeeds[i] = 0;
    // The CPU engine's: its scalp's queries (look.slang tieHost, the address in two words).
    const auto host = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(in ? in->host.get() : nullptr));
    c->octaveSeeds[6] = static_cast<std::uint32_t>(host);
    c->octaveSeeds[7] = static_cast<std::uint32_t>(host >> 32);
}

}  // namespace

void LookEngine::setBaseView(const StrandsPtr& base, const ScalpMesh* scalp, std::shared_ptr<const std::vector<std::uint32_t>> which) {
    if (!sharesBase()) throw std::runtime_error("look engine: a view of the strands is a sharing engine's");
    const StrandSet& s = *base;
    const std::vector<std::uint32_t>& list = *which;
    auto* offsets = static_cast<std::uint32_t*>(writable(kOffsets, std::max<std::size_t>(list.size(), 1) * 8));
    for (std::size_t k = 0; k < list.size(); ++k) {
        offsets[2 * k] = static_cast<std::uint32_t>(s.strandBegin(list[k]));
        offsets[2 * k + 1] = static_cast<std::uint32_t>(s.offsets()[list[k] + 1]);
    }
    normalsScalp_ = scalp;
    rootNormals_.clear();
    rootsOnScalp_.clear();
    strandCount_ = list.size();
    pointCount_ = s.pointCount();
    uniformCount_ = 0;  // (each strand's points by its two words)
    share(kBasePoints, s.positions().data(), pointCount_ * 12);
    if (s.widths().size() == pointCount_) {
        share(kBaseWidths, s.widths().data(), pointCount_ * 4);
    } else if (float* to = static_cast<float*>(writable(kBaseWidths, std::max<std::size_t>(pointCount_, 1) * 4))) {
        std::fill(to, to + pointCount_, 0.01f);
    }
    for (const int b : {kPointsA, kPointsB, kPointsC, kWidths, kOut0, kOut1, kOut2, kOut3, kFull0, kFull1, kFull2, kFull3}) release(b);
    ++baseSerial_;
    dataKey_.clear();
    fieldSerial_ = 0;
    resultBuffer_ = -1;
    base_ = base;
    view_ = std::move(which);
    shownPoints_.clear();
    shownKey_ = 0;
}

void LookEngine::letGoOfBase() {
    if (!sharesBase() || !base_) return;
    heldBase_ = base_;
    base_.reset();
}

bool LookEngine::setBase(const StrandsPtr& base, const ScalpMesh* scalp, const StrandChanges* changed) {
    if (!base) return false;
    if (view_) {  // (leaving a view: all of it again)
        view_.reset();
        base_.reset();
        heldBase_.reset();
    } else if (!base_) {
        base_ = heldBase_.lock();  // (alive: its arrays too, so what is compared by address is its)
    }
    heldBase_.reset();
    if (base == base_ && (!sharesBase() || scalp == normalsScalp_)) return false;
    const auto start = std::chrono::steady_clock::now();
    const StrandSet& s = *base;
    // The graph cannot say how the base changed (results came in between: a stroke's previews, a
    // new document): when its layout, IDs and roots are the ones held, compare the points.
    StrandChanges diffed;
    if ((!changed || changed->all) && base_ && !sharesBase()) {
        diffed = diffStrands(*base_, s);
        if (!diffed.all && diffed.strands.size() * 2 < s.strandCount()) changed = &diffed;
    }
    // Only some strands' points moved (StrandChanges: the same IDs, roots and point counts, so the
    // per-strand numbers and offsets held still hold): those points go up, in runs.
    if (changed && !changed->all && base_ && !sharesBase() && s.strandCount() == strandCount_ && s.pointCount() == pointCount_) {
        const std::vector<std::uint32_t>& list = changed->strands;
        std::size_t points = 0;
        for (std::size_t k = 0; k < list.size();) {
            // A run of changed strands, gaps of a few strands bridged (fewer, larger copies).
            std::size_t end = k + 1;
            while (end < list.size() && list[end] - list[end - 1] <= 8) ++end;
            const std::size_t first = s.strandBegin(list[k]), last = s.offsets()[list[end - 1] + 1];
            writeRange(kBasePoints, first * 12, s.positions().data() + first, (last - first) * 12);
            if (s.widths().size() == s.pointCount()) writeRange(kBaseWidths, first * 4, s.widths().data() + first, (last - first) * 4);
            points += last - first;
            k = end;
        }
        base_ = base;
        findShownPoints();
        fieldSerial_ = 0;  // the mapped values are read again for these strands
        uploadMs_ = since(start);
        uploadMegabytes_ = static_cast<double>(points * 12) / 1e6;
        uploadPartial_ = true;
        uploadStrands_ = list.size();
        ++uploads_;
        return true;
    }
    const std::size_t strands = s.strandCount(), points = s.pointCount();
    // The strands held, their points alone new (their layout, IDs and roots the very arrays: as the
    // operators hand them on), on the same scalp: what is made of those holds (the offsets, the
    // per-strand numbers and root normals, the root frames a sharing engine makes; baseSerial_).
    const bool sameStrands = base_ && strands == strandCount_ && points == pointCount_ && scalp == normalsScalp_ &&
                             base_->offsets().data() == s.offsets().data() && base_->ids().data() == s.ids().data() &&
                             base_->roots().data() == s.roots().data();
    std::vector<std::uint32_t> staged;
    if (!sameStrands) {
        // (In place where the engine holds it in memory: the CPU's.)
        auto* offsets = static_cast<std::uint32_t*>(writable(kOffsets, (strands + 1) * 4));
        if (!offsets) {
            staged.resize(strands + 1);
            offsets = staged.data();
        }
        parallelFor(0, strands, [&](std::size_t i) { offsets[i] = static_cast<std::uint32_t>(s.offsets()[i]); }, 16384);
        offsets[strands] = static_cast<std::uint32_t>(points);
        // A sharing engine (the CPU's: the caller's strands and scalp, run while it holds them)
        // makes its root normals when a run reads them (putStrandData); the GPU's here, with the upload.
        normalsScalp_ = scalp;
        rootNormals_.clear();
        rootsOnScalp_.clear();
        if (!sharesBase()) makeRootNormals(s, scalp);
    }
    strandCount_ = strands;
    pointCount_ = points;
    uniformCount_ = s.uniformPointCount();
    if (!staged.empty()) write(kOffsets, staged.data(), staged.size() * 4, false);
    share(kBasePoints, s.positions().data(), points * 12);
    // The base's widths, for the point texture where no Width runs (a set without widths: the
    // default the app draws them at, as buildHairBuffers does).
    if (s.widths().size() == points) {
        share(kBaseWidths, s.widths().data(), points * 4);
    } else if (float* to = static_cast<float*>(writable(kBaseWidths, std::max<std::size_t>(points, 1) * 4))) {
        parallelFor(0, (points + 65535) / 65536, [&](std::size_t c) {
            std::fill(to + c * 65536, to + std::min(points, (c + 1) * 65536), 0.01f);
        }, 1);
    } else {
        const std::vector<float> defaults(std::max<std::size_t>(points, 1), 0.01f);
        write(kBaseWidths, defaults.data(), defaults.size() * 4, false);
    }
    // The outputs of another size go; what the run made of the old strands is made again.
    for (const int b : {kPointsA, kPointsB, kPointsC, kWidths, kOut0, kOut1, kOut2, kOut3, kFull0, kFull1, kFull2, kFull3}) release(b);
    if (!sameStrands) {
        ++baseSerial_;
        dataKey_.clear();
    }
    fieldSerial_ = 0;
    resultBuffer_ = -1;
    base_ = base;
    findShownPoints();
    uploadMs_ = since(start);
    uploadMegabytes_ = static_cast<double>((strands + 1) * 4 + points * 12) / 1e6;
    uploadPartial_ = false;
    uploadStrands_ = strands;
    ++uploads_;
    return true;
}

void LookEngine::makeRootNormals(const StrandSet& s, const ScalpMesh* scalp) {
    const std::size_t strands = view_ ? view_->size() : s.strandCount();
    rootNormals_.resize(strands);
    rootsOnScalp_.resize(strands);
    // The root normal each strand's lit lines lean by (display.cpp rootNormal: up when unrooted).
    parallelFor(0, strands, [&](std::size_t i) {
        const StrandRoot& root = s.roots()[baseStrand(i)];
        const bool rooted = scalp && root.face >= 0 && static_cast<std::size_t>(root.face) < scalp->triangleCount();
        rootNormals_[i] = rooted ? scalp->normal(root) : Vec3f{0, 1, 0};
        rootsOnScalp_[i] = rooted ? 1 : 0;
    }, 4096);
}

void LookEngine::findShownPoints() {
    const StrandSet& s = *base_;
    const std::vector<std::uint32_t> at = shownIndices(s);  // empty: none hidden
    std::vector<std::uint32_t> shown;
    if (!at.empty()) {
        shown.reserve(s.pointCount());
        for (std::size_t i = 0; i < at.size(); ++i) {
            if (at[i] == UINT32_MAX) continue;
            for (std::size_t p = s.strandBegin(i); p < s.offsets()[i + 1]; ++p) shown.push_back(static_cast<std::uint32_t>(p));
        }
    }
    if (shown != shownPoints_) {
        shownPoints_ = std::move(shown);
        shownKey_ = 0;  // uploaded again at the next run
    }
}

void LookEngine::walkOnlyChanged(std::size_t k, const StageParams& params, const InterpolateInputs& data) {
    StageHeld& held = held_[k];
    const InterpolateGuideRows& rows = *data.rows;
    const std::size_t guides = rows.roots.size();
    // What a strand's walk is made from besides its guides: the base (every upload, whole or of
    // some strands), the binding, the strands' normals, the scalp's field and the walk's numbers,
    // the node's params and falloff.
    std::uint64_t key = hashCombine(hashCombine(hash64(0x3A1C), uploads_), strandCount_);
    key = hashCombine(hashCombine(key, pointCount_), reinterpret_cast<std::uintptr_t>(data.binding.get()));
    key = hashCombine(hashCombine(key, reinterpret_cast<std::uintptr_t>(data.strandNormals.get())),
                      reinterpret_cast<std::uintptr_t>(data.clearance->tables.get()));
    key = hashCombine(hashCombine(key, reinterpret_cast<std::uintptr_t>(data.clearance->walk.get())),
                      reinterpret_cast<std::uintptr_t>(data.cursors.get()));
    key = hashFloat(hashFloat(hashFloat(hashFloat(hashFloat(hashFloat(key, data.clearance->floor), data.clearance->ease), data.clearance->span),
                                                  data.clearance->depth),
                              data.clearance->searchRadius),
                    data.clearance->beside);
    key = hashCombine(key, rows.samples);
    for (int i = 0; i < 4; ++i) key = hashCombine(hashFloat(key, params.value[i]), static_cast<std::uint32_t>(params.flag[i]));
    key = hashCombine(key, params.ramp.straight ? 1u : 0u);
    for (const auto& [x, y] : params.ramp.keys) key = hashFloat(hashFloat(key, x), y);
    // (GROOMLAB_GPU_WALK_ALL=1: every run walks every strand, for comparisons.)
    static const bool walkAll = [] {
        const char* v = std::getenv("GROOMLAB_GPU_WALK_ALL");
        return v && v[0] == '1';
    }();
    const bool heldWalk = !walkAll && held.walked && held.walkRows && held.walkKey == key && held.walkPoints == pointCount_;
    held.walked = false;  // (again once the run's dispatches are made: run)
    held.walkPartial = false;
    lastWalk_ = WalkStats{false, guides, guides};
    if (heldWalk) {
        const InterpolateGuideRows& before = *held.walkRows;
        if (before.samples == rows.samples && before.roots.size() == guides && before.offsets.size() == rows.offsets.size() &&
            before.normals.size() == rows.normals.size() && before.lengths.size() == rows.lengths.size() &&
            rows.offsets.size() == guides * rows.samples) {
            // Each guide's 8 words (look.slang guideNormal...), word 5 whether it changed since the
            // walk kept (the same to the bit: its strands' walks are the same).
            std::vector<std::uint32_t> words(guides * 8, 0u);
            std::size_t changed = 0;
            const std::size_t n = rows.samples;
            for (std::size_t g = 0; g < guides; ++g) {
                const bool same = std::memcmp(&rows.offsets[g * n], &before.offsets[g * n], n * sizeof(Vec3f)) == 0 &&
                                  std::memcmp(&rows.normals[g], &before.normals[g], sizeof(Vec3f)) == 0 &&
                                  std::memcmp(&rows.roots[g], &before.roots[g], sizeof(Vec3f)) == 0 &&
                                  std::memcmp(&rows.lengths[g], &before.lengths[g], sizeof(float)) == 0;
                std::uint32_t* h = words.data() + g * 8;
                std::memcpy(&h[0], &rows.normals[g], 12);
                h[3] = rows.normals[g].length() == 0.0f ? 0u : 1u;
                std::memcpy(&h[4], &rows.lengths[g], 4);
                h[5] = same ? 0u : 1u;
                changed += same ? 0 : 1;
            }
            writeRange(auxBuffer(k, 1), 0, words.data(), words.size() * 4);
            held.walkPartial = true;
            lastWalk_ = WalkStats{true, changed, guides};
        }
    }
    held.walkRows = data.rows;
    held.walkKey = key;
    held.walkPoints = pointCount_;
}

void LookEngine::putStrandData(const LookRun& run, bool frames) {
    const std::vector<StageParams>& stages = run.params.stages;
    bool normals = frames;
    for (const StageParams& p : stages) {
        normals = normals || p.stage.kind == StageKind::Clump || p.stage.kind == StageKind::Curl || p.stage.kind == StageKind::BlendNeighbors;
    }
    if (normals && rootNormals_.size() != strandCount_) makeRootNormals(*base_, normalsScalp_);
    normals = normals || !rootNormals_.empty();
    // What the numbers come from: the strands (each full upload), the root normals, each stage's
    // kind and seed, and Interpolate's normals.
    std::vector<std::uint64_t> key{baseSerial_, normals ? 1u : 0u};
    for (std::size_t k = 0; k < stages.size(); ++k) {
        key.push_back(static_cast<std::uint64_t>(stages[k].stage.kind));
        key.push_back(stages[k].seed);
        const InterpolateInputs* in = k < run.inputs.size() ? run.inputs[k].interpolate.get() : nullptr;
        key.push_back(reinterpret_cast<std::uintptr_t>(in ? in->strandNormals.get() : nullptr));
        key.push_back(reinterpret_cast<std::uintptr_t>(in ? in->cursors.get() : nullptr));
    }
    if (key == dataKey_) return;
    const StrandSet& s = *base_;
    const std::size_t n = strandCount_;
    dataBlocks_.assign(stages.size(), 0);
    std::size_t blocks = 1;  // block 0: the root normals
    for (std::size_t k = 0; k < stages.size(); ++k) {
        dataBlocks_[k] = blocks;
        blocks += strandDataBlocks(stages[k].stage.kind);
    }
    // Written in place where the engine holds it in memory (the CPU's), else up from here; each
    // strand's numbers zero but what its stages set.
    const std::size_t words = blocks * n * 4;
    std::unique_ptr<float[]> staged;
    float* data = static_cast<float*>(writable(kStrandData, words * 4));
    if (!data) {
        staged.reset(new float[std::max<std::size_t>(words, 1)]);
        data = staged.get();
    }
    parallelFor(0, n, [&](std::size_t i) {
        for (std::size_t b = 0; b < blocks; ++b) std::fill_n(data + (b * n + i) * 4, 4, 0.0f);
        if (normals) {
            float* up = data + i * 4;
            up[0] = rootNormals_[i].x;
            up[1] = rootNormals_[i].y;
            up[2] = rootNormals_[i].z;
            up[3] = rootsOnScalp_[i] ? 1.0f : 0.0f;  // the skin's normal (Clump's repel keeps off the skin)
        }
        const StrandId id = s.ids()[baseStrand(i)];
        for (std::size_t k = 0; k < stages.size(); ++k) {
            if (!dataBlocks_[k]) continue;
            float* d = data + (dataBlocks_[k] * n + i) * 4;
            const std::uint64_t seed = stages[k].seed;
            switch (stages[k].stage.kind) {
                case StageKind::Interpolate: {
                    const InterpolateInputs* in = k < run.inputs.size() ? run.inputs[k].interpolate.get() : nullptr;
                    const Vec3f nrm = in && in->strandNormals && i < in->strandNormals->size() ? (*in->strandNormals)[i] : Vec3f{};
                    d[0] = nrm.x;
                    d[1] = nrm.y;
                    d[2] = nrm.z;
                    // Over a deformed scalp: the vertex its clearance walk starts at (a word's bits).
                    const std::uint32_t cursor = in && in->cursors && i < in->cursors->size() ? (*in->cursors)[i] : 0xFFFFFFFFu;
                    std::memcpy(&d[3], &cursor, 4);
                    break;
                }
                case StageKind::Length: {
                    // LengthOp's draws: the trim's, and the pick's (under the fraction: trimmed).
                    d[0] = unitFloat(hashCombine(id, seed ^ 0x7A11));
                    d[3] = unitFloat(hashCombine(id, seed ^ 0x1E46));
                    break;
                }
                case StageKind::Clump: {
                    // Its spot in the lock: the angle's fraction and the radius' (sqrt of a
                    // uniform: uniform over the disk's area), as ClumpOp draws them.
                    const std::uint64_t h = hashCombine(id, seed ^ 0x10C4);
                    d[1] = unitFloat(h);
                    d[2] = std::sqrt(unitFloat(hash64(h)));
                    break;
                }
                case StageKind::Noise: {
                    // NoiseOp's strand offset (Rng(hash(id, seed)), three uniform(0, 1000)) and its
                    // fraction gate.
                    Rng rng(hashCombine(id, seed));
                    const Vec3f offset = randomOffsetZYX(rng);
                    d[0] = offset.x;
                    d[1] = offset.y;
                    d[2] = offset.z;
                    d[3] = unitFloat(hashCombine(id, seed ^ 0xF822));
                    break;
                }
                case StageKind::Curl: {
                    // CurlOp's four draws: phase, radius and frequency variation, the plane of a
                    // wave or zigzag (drawn by the CPU for those alone, the same number).
                    Rng rng(hashCombine(id, seed ^ 0xC041));
                    d[0] = rng.nextFloat();
                    d[1] = rng.nextFloat();
                    d[2] = rng.nextFloat();
                    d[3] = rng.nextFloat();
                    break;
                }
                case StageKind::Width: {
                    // WidthOp's variation draw.
                    Rng rng(hashCombine(id, seed ^ 0x3D17ull));
                    d[0] = rng.uniform(-1.0f, 1.0f);
                    break;
                }
                case StageKind::Part: break;  // (its numbers: PartInputs)
                case StageKind::Flyaway: {
                    // FlyawayOp's draws: the gate (unitFloat of the hash), then from Rng(hash) the
                    // noise offset and the lengthening's uniform(0.5, 1).
                    const std::uint64_t h = hashCombine(id, seed ^ 0xF1A7);
                    Rng rng(h);
                    const Vec3f offset = randomOffsetZYX(rng);
                    d[0] = offset.x;
                    d[1] = offset.y;
                    d[2] = offset.z;
                    d[3] = unitFloat(h);
                    float* more = data + ((dataBlocks_[k] + 1) * n + i) * 4;
                    more[0] = rng.uniform(0.5f, 1.0f);
                    break;
                }
                case StageKind::Wind: {
                    // WindOp's flutter phase along the strand (unitFloat of the ID's hash, in radians).
                    const std::uint64_t flutterSeed = hashCombine(seed, 0xF1E7);
                    d[0] = unitFloat(hash64(id ^ flutterSeed)) * static_cast<float>(6.283185307179586);
                    break;
                }
                case StageKind::SelfCollide: {
                    // SelfCollideOp's rankOf: the strand's own offset across a slab, in cells.
                    d[0] = 0.15f * (unitFloat(hash64(static_cast<std::uint64_t>(id) ^ 0x51AB0FF5E7ull)) - 0.5f);
                    break;
                }
                case StageKind::Static: {
                    // StaticOp's splayOf (its own direction) and its charge's draw (chargeOf's u).
                    const std::uint64_t h = hash64(static_cast<std::uint64_t>(id) ^ 0x5B1A7F0E5Dull);
                    const float z = 2.0f * unitFloat(h) - 1.0f;
                    const float a = 6.28318531f * unitFloat(hash64(h));
                    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
                    d[0] = r * pm::cos(a);
                    d[1] = r * pm::sin(a);
                    d[2] = z;
                    d[3] = unitFloat(hash64(static_cast<std::uint64_t>(id) ^ 0xC4A26E5A17ull));
                    break;
                }
                // The shapers' draws (transform_ops.cpp signedRandom, by the node's salt).
                case StageKind::Bend: d[0] = unitFloat(hashCombine(id, seed ^ 0xBE4Dull)); break;
                case StageKind::Direction: {
                    constexpr std::uint64_t kSalt = 0xD12EC7ull;
                    d[0] = 2.0f * unitFloat(hashCombine(hashCombine(id, seed ^ kSalt), 0)) - 1.0f;
                    d[1] = 2.0f * unitFloat(hashCombine(hashCombine(id, seed ^ kSalt), 1)) - 1.0f;
                    break;
                }
                case StageKind::Transform: {
                    // Its seven: the move's three, the turn's three, the scale's (the next block from the fifth).
                    constexpr std::uint64_t kSalt = 0x7A45F0ull;
                    float* more = data + ((dataBlocks_[k] + 1) * n + i) * 4;
                    for (std::uint64_t r = 0; r < 7; ++r) {
                        const float v = 2.0f * unitFloat(hashCombine(hashCombine(id, seed ^ kSalt), r)) - 1.0f;
                        (r < 4 ? d[r] : more[r - 4]) = v;
                    }
                    break;
                }
                case StageKind::Gravity:
                case StageKind::Collide:
                case StageKind::Blend:
                case StageKind::Tie: break;
                case StageKind::Displace:
                case StageKind::Twist: break;
                // (The shapers' numbers: their inputs, StageInputs.)
                case StageKind::Smooth:
                case StageKind::RestoreLength:
                case StageKind::BlendNeighbors:
                case StageKind::Tangle: break;
                case StageKind::Flow:
                    // FlowOp's turn of the strand off the flow: its draw (times rollRandom, -1 to 1).
                    d[0] = 2.0f * unitFloat(hashCombine(id, seed ^ 0xF70)) - 1.0f;
                    break;
                case StageKind::Jiggle:
                case StageKind::MeshCut:
                case StageKind::Merge:
                case StageKind::Cull:
                case StageKind::Split:
                case StageKind::Mirror: break;
            }
        }
    }, 4096);
    if (staged) write(kStrandData, data, std::max<std::size_t>(words, 1) * 4, false);
    runUploadMegabytes_ += static_cast<double>(words * 4) / 1e6;
    dataKey_ = std::move(key);
}

void LookEngine::putFields(const LookFields& fields, const std::vector<StageParams>& stages) {
    if (fields.serial && fields.serial == fieldSerial_ && fieldLayout_.size() == fields.stages.size() * kFieldSlots * 2) return;
    std::vector<std::uint32_t> layout(fields.stages.size() * kFieldSlots * 2, 0);
    std::vector<float> values;
    // The fields the GPU makes per strand (FieldCS): their programs, per-strand values and core
    // strands, one buffer of words.
    std::vector<std::uint32_t> programs;
    programs_.assign(fields.stages.size() * kFieldSlots, ProgramAt{});
    for (std::size_t k = 0; k < fields.stages.size(); ++k) {
        for (std::size_t i = 0; i < kFieldSlots; ++i) {
            const FieldValues& f = i < 2 ? fields.stages[k].field[i] : fields.stages[k].affect;  // (the affect: a masked node's)
            // (Up a Merge's input a: read on a's strands, the base's first.)
            const std::size_t strands = k < stages.size() ? stageStrands(stages[k]) : strandCount_;
            const std::size_t points = k < stages.size() ? stagePoints(stages[k]) : pointCount_;
            const std::size_t want = f.mode == FieldValues::kPerPoint ? points : f.mode == FieldValues::kPerStrand ? strands : 0;
            if (f.values.size() != want) throw std::runtime_error("GPU preview: mapped values read from other strands than the base");
            layout[(k * kFieldSlots + i) * 2] = static_cast<std::uint32_t>(f.mode);
            layout[(k * kFieldSlots + i) * 2 + 1] = static_cast<std::uint32_t>(values.size());
            // (A Tie's tail: read where the Tie writes it, AffectCS.)
            if (f.channelFrom == kNoStage) values.insert(values.end(), f.values.begin(), f.values.end());
            if (const FieldProgram* p = f.program.get()) {
                if (f.mode != FieldValues::kPerStrand || p->statics.size() != strands * p->slotCount) {
                    throw std::runtime_error("GPU preview: a mapped param's program read from other strands than the base");
                }
                ProgramAt& at = programs_[k * kFieldSlots + i];
                at.on = true;
                at.ops = static_cast<std::uint32_t>(programs.size());
                at.opCount = static_cast<std::uint32_t>(p->ops.size() / 4);
                programs.insert(programs.end(), p->ops.begin(), p->ops.end());
                at.statics = static_cast<std::uint32_t>(programs.size());
                at.slotCount = p->slotCount;
                const std::size_t was = programs.size();
                programs.resize(was + p->statics.size());
                if (!p->statics.empty()) std::memcpy(programs.data() + was, p->statics.data(), p->statics.size() * 4);
                at.core = static_cast<std::uint32_t>(programs.size());
                at.coreCount = static_cast<std::uint32_t>(p->core.size());
                programs.insert(programs.end(), p->core.begin(), p->core.end());
                at.depth = p->depth;
                at.shape = p->shape;
                at.longest = p->longest;
                at.influence = p->influence;
                at.low = p->low;
                at.high = p->high;
                // Lift reads the root's normal from the first three slots (fieldProgram).
                at.normal = false;
                for (std::size_t o = 0; o + 3 < p->ops.size(); o += 4) {
                    if (p->ops[o] == 1u && p->ops[o + 1] == 4u) at.normal = true;
                }
            }
        }
    }
    if (!programs.empty()) {
        write(kFieldPrograms, programs.data(), programs.size() * 4, false);
        runUploadMegabytes_ += static_cast<double>(programs.size() * 4) / 1e6;
    }
    // None mapped, as the buffer holds: nothing to write (a write may wait on what the GPU still
    // has to do with the buffer).
    if (values.empty() && layout == fieldLayout_ && fieldsEmpty_ && has(kFields)) {
        fieldSerial_ = fields.serial;
        return;
    }
    fieldsEmpty_ = values.empty();
    if (values.empty()) values.push_back(0.0f);
    write(kFields, values.data(), values.size() * 4, true);  // (FieldCS writes the values it makes)
    fieldLayout_ = std::move(layout);
    fieldSerial_ = fields.serial;
    fieldMegabytes_ = static_cast<double>(values.size() * 4) / 1e6;
    runUploadMegabytes_ += fieldMegabytes_;
}

LookEngine& LookEngine::runSibling(std::size_t k, const LookCentersRun& branch) {
    if (!branch.base) throw std::runtime_error("GPU preview: a branch run on a sibling has no strands");
    std::unique_ptr<LookEngine>& engine = centers_[k];
    if (!engine) engine = sibling();
    engine->setPointThreads(pointThreads_);  // (as this engine's now: they may have changed since)
    engine->setDispatchLimit(dispatchLimit_);
    // Its strands (fixed through a stroke or a drag: uploaded once), then its run, which the GPU does
    // before this engine's (the same queue).
    engine->setBase(branch.base, branch.scalp);
    LookRun run;
    run.params = branch.params;
    run.fields = branch.fields;
    run.inputs = branch.inputs;
    engine->run(run, false);
    return *engine;
}

void LookEngine::runCenters(std::size_t k, const LookCentersRun& centers) {
    // Its points and offsets read by the Clump's centers (CentersCS).
    LookEngine& engine = runSibling(k, centers);
    borrow(auxBuffer(k, 6), engine, engine.allPointsBuffer());
    borrow(auxBuffer(k, 7), engine, kOffsets);
}

void LookEngine::runMerged(std::size_t k, std::size_t strandsA, const LookCentersRun& merged) {
    LookEngine& engine = runSibling(k, merged);
    // b's strands after a's (StageParams flag[0] of them), point for point.
    if (strandsA > strandCount_ || engine.strandCount_ != strandCount_ - strandsA ||
        engine.pointCount_ != pointCount_ - base_->strandBegin(strandsA)) {
        throw std::runtime_error("GPU preview: a Merge's input b's run is for other strands");
    }
    borrow(auxBuffer(k, 0), engine, engine.allPointsBuffer());
    if (engine.widthsRun_) borrow(auxBuffer(k, 1), engine, kWidths);
}

void LookEngine::runGathered(std::size_t k, const LookCentersRun& run, const GatherInputs& layout) {
    LookEngine& engine = runSibling(k, run);
    // Each strand of the base (the node's result) from one of the run's, point for point.
    if (!layout.strands || layout.strands->size() != strandCount_ * 8) {
        throw std::runtime_error("GPU preview: a Cull's, Split's or Mirror's layout is for other strands");
    }
    borrow(auxBuffer(k, 0), engine, engine.allPointsBuffer());
    if (engine.widthsRun_) borrow(auxBuffer(k, 1), engine, kWidths);
    StageHeld& held = held_[k];
    if (held.a != layout.strands.get() || !has(auxBuffer(k, 2))) {
        write(auxBuffer(k, 2), layout.strands->data(), layout.strands->size() * 4, false);
        held.keep = layout.strands;
        held.a = layout.strands.get();
        runUploadMegabytes_ += static_cast<double>(layout.strands->size() * 4) / 1e6;
    }
}

void LookEngine::putStageInputs(const LookRun& run) {
    const std::vector<StageParams>& stages = run.params.stages;
    lastWalk_ = WalkStats{};
    held_.resize(stages.size());
    centers_.resize(stages.size());
    for (std::size_t k = 0; k < stages.size(); ++k) {
        StageHeld& held = held_[k];
        const std::size_t stageN = stageStrands(stages[k]), stageP = stagePoints(stages[k]);  // (up a Merge's input a: its strands)
        const int kindNow = static_cast<int>(stages[k].stage.kind);
        if (held.kind != kindNow && sharesBase()) {
            // (Parked for its kind's next turn; the new kind's taken back where it was parked.)
            if (held.kind >= 0) {
                Parked& parked = parked_[{k, held.kind}];
                parked.held = std::move(held);
                for (int b = 0; b < kAuxPerStage; ++b) parked.buffers[b] = parkBuffer(auxBuffer(k, b));
            }
            held = StageHeld{};
            held.kind = kindNow;
            const auto back = parked_.find({k, kindNow});
            if (back != parked_.end()) {
                held = std::move(back->second.held);
                for (int b = 0; b < kAuxPerStage; ++b) unparkBuffer(auxBuffer(k, b), std::move(back->second.buffers[b]));
                parked_.erase(back);
            }
        } else if (held.kind != kindNow) {
            held = StageHeld{};
            held.kind = kindNow;
            for (int b = 0; b < kAuxPerStage; ++b) release(auxBuffer(k, b));
        }
        const ClumpInputs* clump = stages[k].stage.kind == StageKind::Clump && k < run.inputs.size() ? run.inputs[k].clump.get() : nullptr;
        const bool gathering = gathers(stages[k].stage.kind);
        const LookCentersRun* merged =
            (stages[k].stage.kind == StageKind::Merge || gathering) && k < run.inputs.size() ? run.inputs[k].merged.get() : nullptr;
        if ((!clump || !clump->centersRun) && !merged) centers_[k].reset();
        const StageInputs* in = k < run.inputs.size() ? &run.inputs[k] : nullptr;
        if (merged && gathering) {
            if (!in->gather) throw std::runtime_error("GPU preview: a Cull's, Split's or Mirror's layout missing");
            runGathered(k, *merged, *in->gather);
        } else if (merged) {
            runMerged(k, static_cast<std::size_t>(std::max(stages[k].flag[0], 0)), *merged);
        }
        if (stages[k].stage.kind == StageKind::Interpolate) {
            const InterpolateInputs* data = in ? in->interpolate.get() : nullptr;
            if (!data || !data->binding || !data->rows) throw std::runtime_error("GPU preview: Interpolate's binding and guides missing");
            const InterpolateState& binding = *data->binding;
            if (binding.strandCount != stageN) throw std::runtime_error("GPU preview: Interpolate's binding is for other strands");
            // The binding (look.slang InterpolateCS): per strand two words, its header (where its
            // records start, in words, times 64; 32 when it has twists; how many) and its nearest
            // guide's distance squared; then the records, a word each (the guide's index, the
            // weight's code times 65536; past 65535 guides two words: index, code), a strand's
            // twists after its records when it has any. The records go chunk by chunk; only the
            // chunks the buffer does not hold are written (a stroke binding again near the roots it
            // moved), where the chunk was if it fits, else after the others, else all of them again.
            const std::vector<const void*> chunks = interpolateChunks(binding);
            const std::size_t per = interpolateChunkStrands();
            const std::uint32_t recordWords = binding.roots.size() > 65535 ? 2u : 1u;
            const std::size_t headerWords = 2 * stageN;
            bool whole = !has(auxBuffer(k, 0)) || held.chunks.size() != chunks.size() || held.aux[0] != recordWords;
            if (!whole && sharesBase() && held.a != data->binding.get()) {
                // On the CPU, another binding goes up by chunks only when it is the held one bound
                // again (sharing a chunk with it: the held chunks are alive, held.keep, so an address
                // is theirs); another operator's of as many strands goes up whole.
                bool shares = false;
                for (std::size_t c = 0; c < chunks.size() && !shares; ++c) shares = held.chunks[c] == chunks[c];
                whole = !shares;
            }
            std::vector<std::size_t> changed;
            for (std::size_t c = 0; c < chunks.size(); ++c) {
                if (whole || held.chunks[c] != chunks[c]) changed.push_back(c);
            }
            // A chunk's headers (their starts from its first record) and records.
            struct Packed {
                std::vector<std::uint32_t> headers, records;
            };
            auto pack = [&](const std::vector<std::size_t>& which) {
                std::vector<Packed> packed(which.size());
                std::atomic<bool> overflow{false};
                parallelFor(0, which.size(), [&](std::size_t i) {
                    InterpolateRecords r;
                    interpolateRecords(binding, which[i] * per, per, &r);
                    Packed& p = packed[i];
                    const std::size_t n = r.offsets.size() - 1;
                    p.headers.assign(2 * n, 0u);
                    p.records.reserve(r.guides.size() * recordWords);
                    for (std::size_t s = 0; s < n; ++s) {
                        const std::uint32_t first = r.offsets[s], count = r.offsets[s + 1] - first;
                        std::memcpy(&p.headers[2 * s + 1], &r.nearest2[s], 4);
                        if (count > kInterpolateSlots) {
                            overflow = true;
                            continue;
                        }
                        bool twisted = false;
                        for (std::uint32_t g = 0; g < count; ++g) twisted = twisted || r.twists[first + g] != 0.0f;
                        p.headers[2 * s] = static_cast<std::uint32_t>(p.records.size() << 6) | (twisted ? 32u : 0u) | count;
                        for (std::uint32_t g = 0; g < count; ++g) {
                            if (recordWords == 1) {
                                p.records.push_back(r.guides[first + g] | (static_cast<std::uint32_t>(r.weights[first + g]) << 16));
                            } else {
                                p.records.push_back(r.guides[first + g]);
                                p.records.push_back(r.weights[first + g]);
                            }
                        }
                        if (twisted) {
                            for (std::uint32_t g = 0; g < count; ++g) {
                                std::uint32_t bits;
                                std::memcpy(&bits, &r.twists[first + g], 4);
                                p.records.push_back(bits);
                            }
                        }
                    }
                }, 1);
                if (overflow) throw std::runtime_error("GPU preview: a strand blends more guides than the GPU takes");
                return packed;
            };
            // Its headers with its records at word `at`.
            auto placed = [](Packed& p, std::uint32_t at) {
                for (std::size_t s = 0; s < p.headers.size(); s += 2) p.headers[s] += at << 6;
            };
            if (!changed.empty() && !whole) {
                std::vector<Packed> packed = pack(changed);
                // Each where it was if it fits, else after the records (else all again, below).
                std::size_t end = held.recordsEnd;
                std::vector<std::uint32_t> at(changed.size());
                for (std::size_t i = 0; i < changed.size() && !whole; ++i) {
                    const std::size_t c = changed[i], words = packed[i].records.size();
                    if (words <= held.chunkRoom[c]) {
                        at[i] = held.chunkAt[c];
                    } else if (end + words <= held.recordsRoom) {
                        at[i] = static_cast<std::uint32_t>(end);
                        end += words;
                    } else {
                        whole = true;
                    }
                }
                if (!whole) {
                    double bytes = 0.0;
                    for (std::size_t i = 0; i < changed.size(); ++i) {
                        const std::size_t c = changed[i];
                        Packed& p = packed[i];
                        if (at[i] != held.chunkAt[c]) {
                            held.chunkAt[c] = at[i];
                            held.chunkRoom[c] = static_cast<std::uint32_t>(p.records.size());
                        }
                        placed(p, at[i]);
                        writeRange(auxBuffer(k, 0), c * per * 8, p.headers.data(), p.headers.size() * 4);
                        writeRange(auxBuffer(k, 0), std::size_t{at[i]} * 4, p.records.data(), p.records.size() * 4);
                        bytes += static_cast<double>((p.headers.size() + p.records.size()) * 4);
                    }
                    held.recordsEnd = end;
                    runUploadMegabytes_ += bytes / 1e6;
                    held.chunks = chunks;
                }
            }
            if (whole) {
                std::vector<std::size_t> all(chunks.size());
                for (std::size_t c = 0; c < chunks.size(); ++c) all[c] = c;
                std::vector<Packed> packed = pack(all);
                std::size_t total = 0;
                for (const Packed& p : packed) total += p.records.size();
                // Room for chunks bound again that outgrow theirs (a stroke's).
                const std::size_t room = headerWords + total + total / 16 + 1024;
                if (room >= (std::size_t{1} << 26)) throw std::runtime_error("GPU preview: Interpolate's binding is too large for the GPU");
                std::vector<std::uint32_t> words(headerWords + total);
                held.chunkAt.assign(chunks.size(), 0);
                held.chunkRoom.assign(chunks.size(), 0);
                std::size_t at = headerWords;
                for (std::size_t c = 0; c < chunks.size(); ++c) {
                    held.chunkAt[c] = static_cast<std::uint32_t>(at);
                    held.chunkRoom[c] = static_cast<std::uint32_t>(packed[c].records.size());
                    at += packed[c].records.size();
                }
                parallelFor(0, chunks.size(), [&](std::size_t c) {
                    Packed& p = packed[c];
                    placed(p, held.chunkAt[c]);
                    std::copy(p.headers.begin(), p.headers.end(), words.begin() + static_cast<std::ptrdiff_t>(c * per * 2));
                    std::copy(p.records.begin(), p.records.end(), words.begin() + held.chunkAt[c]);
                }, 16);
                allocate(auxBuffer(k, 0), room * 4);
                writeRange(auxBuffer(k, 0), 0, words.data(), words.size() * 4);
                held.recordsEnd = words.size();
                held.recordsRoom = room;
                runUploadMegabytes_ += static_cast<double>(words.size() * 4) / 1e6;
                held.chunks = chunks;
            }
            held.aux[0] = recordWords;
            held.a = data->binding.get();
            held.keep = data->binding;
            if (data->tubes && !sharesBase()) throw std::runtime_error("GPU preview: Interpolate's tubes are the CPU engine's");
            if (held.b != data->rows.get() || held.alive[2].get() != data->tubes.get()) {
                // The guides: 8 words each (normal xyz, turns, length), then their offsets.
                const InterpolateGuideRows& rows = *data->rows;
                const std::size_t count = rows.roots.size();
                std::vector<float> words(count * 8 + rows.offsets.size() * 3, 0.0f);
                for (std::size_t gi = 0; gi < count; ++gi) {
                    float* h = words.data() + gi * 8;
                    h[0] = rows.normals[gi].x;
                    h[1] = rows.normals[gi].y;
                    h[2] = rows.normals[gi].z;
                    const std::uint32_t turns = rows.normals[gi].length() == 0.0f ? 0u : 1u;
                    std::memcpy(&h[3], &turns, 4);
                    h[4] = rows.lengths[gi];
                }
                if (sharesBase()) {
                    // On the CPU each guide's offsets by fours, x y z rows of four samples each (the
                    // last standing in past the end): look.slang addTurnedLanes.
                    const std::size_t n = rows.samples, quads = (n + 3) / 4;
                    words.resize(count * 8 + count * quads * 12, 0.0f);  // (the 8 words a guide kept)
                    for (std::size_t gi = 0; gi < count && n > 0; ++gi) {
                        for (std::size_t q = 0; q < quads; ++q) {
                            float* to = words.data() + count * 8 + (gi * quads + q) * 12;
                            for (std::size_t l = 0; l < 4; ++l) {
                                const Vec3f& o = rows.offsets[gi * n + std::min(4 * q + l, n - 1)];
                                to[l] = o.x;
                                to[4 + l] = o.y;
                                to[8 + l] = o.z;
                            }
                        }
                    }
                } else if (!rows.offsets.empty()) {
                    std::memcpy(words.data() + count * 8, rows.offsets.data(), rows.offsets.size() * 12);
                }
                // fillTubes: the tubes and each strand's (look.slang tubeOf) after them, at word aux[2].
                held.aux[2] = 0;
                if (data->tubes && !data->tubes->empty()) {
                    held.aux[2] = static_cast<std::uint32_t>(words.size());
                    words.resize(words.size() + data->tubes->size());
                    std::memcpy(words.data() + held.aux[2], data->tubes->data(), data->tubes->size() * 4);
                }
                write(auxBuffer(k, 1), words.data(), words.size() * 4, false);
                held.b = data->rows.get();
                held.alive[1] = data->rows;
                held.alive[2] = data->tubes;
                held.aux[3] = static_cast<std::uint32_t>(count * 8);
                runUploadMegabytes_ += static_cast<double>(words.size() * 4) / 1e6;
            }
            // keepClearance: the scalp's field as tables, once per scalp.
            if (data->clearance) {
                const MeshDistanceField::Tables& tables = data->clearance->tables;
                if (!tables) throw std::runtime_error("GPU preview: Interpolate's clearance field missing");
                // Over a deformed scalp the walk's words after the rest field's tables (held.aux[1]:
                // where they start, 0: a still scalp).
                const std::shared_ptr<const std::vector<std::uint32_t>>& walk = data->clearance->walk;
                if (walk && !data->cursors) throw std::runtime_error("GPU preview: Interpolate's walk over a deformed scalp has no cursors");
                if (held.tables != tables || held.walk != walk || !has(auxBuffer(k, 2))) {
                    if (walk) {
                        std::vector<std::uint32_t> both(*tables);
                        both.insert(both.end(), walk->begin(), walk->end());
                        write(auxBuffer(k, 2), both.data(), both.size() * 4, false);
                    } else {
                        write(auxBuffer(k, 2), tables->data(), tables->size() * 4, false);
                    }
                    held.tables = tables;
                    held.walk = walk;
                    held.aux[1] = walk ? static_cast<std::uint32_t>(tables->size()) : 0u;
                    runUploadMegabytes_ += static_cast<double>((tables->size() + (walk ? walk->size() : 0)) * 4) / 1e6;
                }
                walkOnlyChanged(k, stages[k], *data);
            }
        } else if (stages[k].stage.kind == StageKind::Clump) {
            const ClumpInputs* data = in ? in->clump.get() : nullptr;
            if (!data || !data->assigned || data->assigned->size() != stageN) {
                throw std::runtime_error("GPU preview: Clump's centers are for other strands");
            }
            if (held.a != data->assigned.get()) {
                // (On the CPU the very memory: read alone, kept alive while held.)
                if (data->assigned->empty()) {
                    const std::uint32_t none = 0xffffffffu;
                    write(auxBuffer(k, 0), &none, 4, false);
                } else {
                    share(auxBuffer(k, 0), data->assigned->data(), data->assigned->size() * 4);
                }
                held.a = data->assigned.get();
                held.keep = data->assigned;
                runUploadMegabytes_ += static_cast<double>(data->assigned->size() * 4) / 1e6;
            }
            // The centers' curves, then their frames (kernels write the frames, and the curves
            // where they make them), then their root normals (a float4 each: what the frames
            // start from), then their variation (a float4 each).
            // Then each center's cumulative arc lengths at its samples (CentersCS writes them).
            const std::size_t variationAt = data->centers * (kCenterFloats + kFrameFloats + kNormalFloats);
            const std::size_t arcsAt = variationAt + data->centers * 4;
            const std::size_t bytes = (arcsAt + data->centers * kClumpCenterSamples) * 4;
            held.aux[1] = static_cast<std::uint32_t>(data->centers * kCenterFloats);
            held.aux[2] = static_cast<std::uint32_t>(variationAt);
            held.aux[3] = static_cast<std::uint32_t>(arcsAt);
            if (!data->normals || data->normals->size() != data->centers) {
                throw std::runtime_error("GPU preview: Clump's centers' normals missing");
            }
            if (!data->variation || data->variation->size() != data->centers * 4) {
                throw std::runtime_error("GPU preview: Clump's variation missing");
            }
            auto withNormals = [&](std::vector<float>& words) {
                float* at = words.data() + data->centers * (kCenterFloats + kFrameFloats);
                for (std::size_t c = 0; c < data->centers; ++c, at += kNormalFloats) {
                    const Vec3f& n = (*data->normals)[c];
                    at[0] = n.x;
                    at[1] = n.y;
                    at[2] = n.z;
                    at[3] = 0.0f;
                }
                if (!data->variation->empty()) std::memcpy(words.data() + variationAt, data->variation->data(), data->variation->size() * 4);
                held.c = data->variation.get();
                held.alive[2] = data->variation;
            };
            if (data->curves) {
                if (held.b != data->curves.get() || !has(auxBuffer(k, 1))) {
                    if (data->curves->size() != data->centers * kClumpCenterSamples) {
                        throw std::runtime_error("GPU preview: Clump's centers do not match its curves");
                    }
                    std::vector<float> words(bytes / 4, 0.0f);
                    if (!data->curves->empty()) std::memcpy(words.data(), data->curves->data(), data->curves->size() * 12);
                    withNormals(words);
                    write(auxBuffer(k, 1), words.data(), bytes, true);
                    held.b = data->curves.get();
                    held.alive[1] = data->curves;
                    held.framesMade = false;
                    runUploadMegabytes_ += static_cast<double>(bytes) / 1e6;
                }
            } else {
                if (!data->chosen || data->chosen->size() != data->centers) throw std::runtime_error("GPU preview: Clump's centers chosen missing");
                if (held.b != data->chosen.get()) {
                    std::vector<float> words(bytes / 4, 0.0f);  // the curves and frames the kernels write
                    withNormals(words);
                    write(auxBuffer(k, 1), words.data(), bytes, true);
                    write(auxBuffer(k, 2), data->chosen->data(), data->chosen->size() * 4, false);
                    held.b = data->chosen.get();
                    held.alive[1] = data->chosen;
                    held.framesMade = false;
                    runUploadMegabytes_ += static_cast<double>(bytes) / 1e6;
                }
                // Centers made by a branch of their own: that branch run first, on its strands.
                if (data->centersRun) runCenters(k, *data->centersRun);
                // copyAmount: each center's nearest centers, and the curves as resampled before
                // they share (ShareCS reads them).
                if (data->neighbors && held.d != data->neighbors.get()) {
                    write(auxBuffer(k, 4), data->neighbors->data(), data->neighbors->size() * 4, false);
                    allocate(auxBuffer(k, 5), std::max<std::size_t>(data->centers * kCenterFloats * 4, 4));
                    held.d = data->neighbors.get();
                    held.alive[3] = data->neighbors;
                }
            }
            // A seed dragged: the variation drawn again (the curves and frames as they are).
            if (held.c != data->variation.get()) {
                if (!data->variation->empty()) writeRange(auxBuffer(k, 1), variationAt * 4, data->variation->data(), data->variation->size() * 4);
                held.c = data->variation.get();
                held.alive[2] = data->variation;
            }
        } else if (stages[k].stage.kind == StageKind::Curl && stages[k].flag[2] != 0) {
            const CurlInputs* data = in ? in->curl.get() : nullptr;
            if (!data || !data->clumpOf || data->clumpOf->size() != stageN || !data->members || !data->clumps) {
                throw std::runtime_error("GPU preview: Curl's clumps are for other strands");
            }
            if (held.a != data->clumpOf.get()) {
                write(auxBuffer(k, 0), data->clumpOf->data(), data->clumpOf->size() * 4, false);
                held.a = data->clumpOf.get();
                held.alive[0] = data->clumpOf;
                runUploadMegabytes_ += static_cast<double>(data->clumpOf->size() * 4) / 1e6;
            }
            if (held.b != data->members.get()) {
                write(auxBuffer(k, 1), data->members->data(), data->members->size() * 4, false);
                allocate(auxBuffer(k, 3), std::max<std::size_t>(data->count * kClumpCenterSamples * 18 * 4, 4));  // (look.slang CLUMP_AXIS_FLOATS)
                held.b = data->members.get();
                held.alive[1] = data->members;
            }
            if (held.c != data->clumps.get()) {
                std::vector<float> clumps = *data->clumps;
                if (clumps.empty()) clumps.push_back(0.0f);
                write(auxBuffer(k, 2), clumps.data(), clumps.size() * 4, false);
                held.c = data->clumps.get();
                held.alive[2] = data->clumps;
            }
        } else if (stages[k].stage.kind == StageKind::Part) {
            const PartInputs* data = in ? in->part.get() : nullptr;
            if (!data || !data->sides || data->sides->size() != stageN * 4) {
                throw std::runtime_error("GPU preview: Part's sides are for other strands");
            }
            if (held.a != data->sides.get()) {
                write(auxBuffer(k, 0), data->sides->data(), data->sides->size() * 4, false);
                held.a = data->sides.get();
                held.alive[0] = data->sides;
                runUploadMegabytes_ += static_cast<double>(data->sides->size() * 4) / 1e6;
            }
        } else if (stages[k].stage.kind == StageKind::Collide) {
            // The surfaces' fields as tables, one buffer: the scalp's, then the collider's from word
            // aux[0] (look.slang CollideCS), once per surface.
            const CollideInputs* data = in ? in->collide.get() : nullptr;
            if (!data || data->tables.size() > 2) throw std::runtime_error("GPU preview: Collide's fields missing");
            for (const MeshDistanceField::Tables& t : data->tables) {
                if (!t || t->empty()) throw std::runtime_error("GPU preview: Collide's fields missing");
            }
            const void* first = data->tables.size() > 0 ? data->tables[0].get() : nullptr;
            const void* second = data->tables.size() > 1 ? data->tables[1].get() : nullptr;
            if (held.a != first || held.b != second || !has(auxBuffer(k, 0))) {
                held.aux[0] = 0;
                held.aux[1] = static_cast<std::uint32_t>(data->tables.size());
                if (data->tables.empty()) {
                    const std::uint32_t none = 0;
                    write(auxBuffer(k, 0), &none, 4, false);
                } else if (data->tables.size() == 1) {
                    share(auxBuffer(k, 0), data->tables[0]->data(), data->tables[0]->size() * 4);  // (read alone, kept alive)
                    runUploadMegabytes_ += static_cast<double>(data->tables[0]->size() * 4) / 1e6;
                } else {
                    std::vector<std::uint32_t> both(*data->tables[0]);
                    held.aux[0] = static_cast<std::uint32_t>(both.size());
                    both.insert(both.end(), data->tables[1]->begin(), data->tables[1]->end());
                    write(auxBuffer(k, 0), both.data(), both.size() * 4, false);
                    runUploadMegabytes_ += static_cast<double>(both.size() * 4) / 1e6;
                }
                held.a = first;
                held.b = second;
                held.tables = data->tables.empty() ? nullptr : data->tables[0];  // (kept alive: compared by address)
                held.keep = data->tables.size() > 1 ? std::shared_ptr<const void>(data->tables[1]) : nullptr;
            }
        } else if (stages[k].stage.kind == StageKind::SelfCollide || stages[k].stage.kind == StageKind::Static) {
            const HairGridInputs* data = in ? in->grid.get() : nullptr;
            if (!data || !data->layout) throw std::runtime_error("GPU preview: the hair grid's layout missing");
            const HairGridLayout& l = *data->layout;
            // The splatted strands' segments (look.slang gridSegment), once per layout.
            if (held.a != data->layout.get() || !has(auxBuffer(k, 0))) {
                for (std::size_t w = 0; w + 1 < l.segments.size(); w += 2) {
                    if (l.segments[w + 1] >= stageN || l.segments[w] + 1 >= stageP) {
                        throw std::runtime_error("GPU preview: the hair grid's strands are other strands");
                    }
                }
                const std::uint32_t none[2] = {0u, 0u};
                write(auxBuffer(k, 0), l.segments.empty() ? none : l.segments.data(), std::max<std::size_t>(l.segments.size(), 2) * 4, false);
                held.a = data->layout.get();
                held.keep = data->layout;
                runUploadMegabytes_ += static_cast<double>(l.segments.size() * 4) / 1e6;
            }
            // The floor's tables, then the scalp's walls (look.slang clearancePush, gridWall), once
            // per scalp and cell.
            if (held.b != data->walls.get() || held.tables != l.tables || !has(auxBuffer(k, 4))) {
                std::vector<std::uint32_t> words;
                if (l.tables) words = *l.tables;
                held.wallsAt = data->walls ? static_cast<std::uint32_t>(words.size()) : 0xFFFFFFFFu;
                if (data->walls) words.insert(words.end(), data->walls->begin(), data->walls->end());
                if (words.empty()) words.push_back(0u);
                write(auxBuffer(k, 4), words.data(), words.size() * 4, false);
                held.b = data->walls.get();
                held.tables = l.tables;
                held.walk = data->walls;  // (kept alive: compared by address)
                runUploadMegabytes_ += static_cast<double>(words.size() * 4) / 1e6;
            }
            // The grid's room: the CPU's grid on the strands the node took, half as wide again and
            // a few blocks more each way (a drag or a dab moves the hair it takes), its blocks twice
            // over. A grid wider (or of more blocks) loses its far side (look.slang GRID_OVERFLOW).
            std::uint64_t cells = 1;
            for (int a = 0; a < 3; ++a) {
                const std::int64_t b = std::max<std::int64_t>(l.blockCells[a], 1);
                held.room[a] = static_cast<std::uint32_t>(b + b / 2 + 4);
                cells *= held.room[a];
            }
            held.room[3] = static_cast<std::uint32_t>(std::min<std::uint64_t>(cells, 2 * static_cast<std::uint64_t>(l.blocks) + 256));
            const std::uint64_t sumBytes = std::uint64_t{held.room[3]} * 64 * (stages[k].stage.kind == StageKind::Static ? 10 : 24) * 4;
            if (cells > (std::uint64_t{1} << 26) || sumBytes > (std::uint64_t{1} << 30)) {
                throw std::runtime_error("GPU preview: the hair grid is too large for the GPU");
            }
        } else if (stages[k].stage.kind == StageKind::Tie) {
            // Tie (look.slang TieFrameCS, TieRouteCS, TieLayCS): aux 0 the scalp's field's tables;
            // 1 each strand's seed face, then the core strands (from word aux[1], aux[2] of them);
            // 5 the tail channel as the strands it takes carry it (when no Tie of the run writes it).
            const TieInputs* data = in ? in->tie.get() : nullptr;
            if (!data || !data->seeds || data->seeds->size() != stageN || !data->core) {
                throw std::runtime_error("GPU preview: Tie's inputs are for other strands");
            }
            if (data->channel && data->channel->size() != stageP) throw std::runtime_error("GPU preview: Tie's channel is for other strands");
            if (data->channelFrom != kNoStage && (data->channelFrom >= k || stages[data->channelFrom].stage.kind != StageKind::Tie)) {
                throw std::runtime_error("GPU preview: Tie's channel from a node that is no Tie before it");
            }
            if (held.a != data->tables.get() || !has(auxBuffer(k, 0))) {
                const std::uint32_t none = 0;
                if (data->tables) {
                    write(auxBuffer(k, 0), data->tables->data(), data->tables->size() * 4, false);
                    runUploadMegabytes_ += static_cast<double>(data->tables->size() * 4) / 1e6;
                } else {
                    write(auxBuffer(k, 0), &none, 4, false);
                }
                held.a = data->tables.get();
            }
            if (held.b != data->seeds.get() || held.c != data->core.get()) {
                std::vector<std::uint32_t> words(*data->seeds);
                words.insert(words.end(), data->core->begin(), data->core->end());
                if (words.empty()) words.push_back(0u);
                write(auxBuffer(k, 1), words.data(), words.size() * 4, false);
                held.b = data->seeds.get();
                held.c = data->core.get();
                held.aux[1] = static_cast<std::uint32_t>(stageN);
                held.aux[2] = static_cast<std::uint32_t>(data->core->size());
                runUploadMegabytes_ += static_cast<double>(words.size() * 4) / 1e6;
            }
            if (data->channelFrom == kNoStage && data->channel && held.d != data->channel.get()) {
                write(auxBuffer(k, 5), data->channel->data(), data->channel->size() * 4, true);  // (read as a writable view)
                held.d = data->channel.get();
                runUploadMegabytes_ += static_cast<double>(data->channel->size() * 4) / 1e6;
            }
            held.keep = in->tie;  // (what the pointers compared above are of, alive till the next run compares them)
        } else {
            putShaperInputs(k, stages[k], in);
        }
        // ---- The shapers working on each strand alone ----
        const StageKind kind = stages[k].stage.kind;
        if (kind == StageKind::Bend || kind == StageKind::Direction || kind == StageKind::Transform || kind == StageKind::Displace) {
            // Each strand's root on the scalp (look.slang rootFrameOf), once per roots and scalp. A
            // sharing engine given none makes them from its base and scalp, in its own memory.
            const RootInputs* data = in ? in->roots.get() : nullptr;
            if (!data && sharesBase() && stageN == strandCount_) {
                if (held.madeFrames != baseSerial_ || held.a || !has(auxBuffer(k, 0))) {
                    writeRootFrames(*base_, normalsScalp_, static_cast<float*>(writable(auxBuffer(k, 0), std::max<std::size_t>(stageN, 1) * 32)),
                                    view_.get());
                    held.madeFrames = baseSerial_;
                    held.a = nullptr;
                    held.keep.reset();
                }
            } else if (!data || !data->frames || data->frames->size() != stageN * 8) {
                throw std::runtime_error("GPU preview: a shaper's roots are for other strands");
            } else if (held.a != data->frames.get() || !has(auxBuffer(k, 0))) {
                // (On the CPU the very memory: never written, kept alive while held.)
                if (data->frames->empty()) {
                    const float none = 0.0f;
                    write(auxBuffer(k, 0), &none, 4, false);
                } else {
                    share(auxBuffer(k, 0), data->frames->data(), data->frames->size() * 4);
                }
                held.a = data->frames.get();
                held.keep = data->frames;  // (kept alive: compared by address)
                held.madeFrames = 0;
                runUploadMegabytes_ += static_cast<double>(data->frames->size() * 4) / 1e6;
            }
        } else if (kind == StageKind::Twist) {
            // Each strand's axis (and the root it was found for), the axes as the CPU made them (around
            // axes) or room for the clumps' middles and the clumps' core strands (look.slang TwistCS).
            const TwistInputs* data = in ? in->twist.get() : nullptr;
            if (!data || !data->strands || data->strands->size() != stageN * 4 || (!data->axes && !data->members)) {
                throw std::runtime_error("GPU preview: Twist's axes are for other strands");
            }
            if (held.a != data->strands.get() || !has(auxBuffer(k, 0))) {
                // (On the CPU the very memory, as the inputs below: read alone, kept alive while held.)
                const std::uint32_t none[4] = {0xffffffffu, 0u, 0u, 0u};
                if (data->strands->empty()) {
                    write(auxBuffer(k, 0), none, sizeof(none), false);
                } else {
                    share(auxBuffer(k, 0), data->strands->data(), data->strands->size() * 4);
                }
                held.a = data->strands.get();
                held.keep = data->strands;
                runUploadMegabytes_ += static_cast<double>(data->strands->size() * 4) / 1e6;
            }
            const void* axes = data->axes ? static_cast<const void*>(data->axes.get()) : static_cast<const void*>(data->members.get());
            if (held.b != axes || !has(auxBuffer(k, 1))) {
                if (data->axes) {
                    share(auxBuffer(k, 1), data->axes->data(), data->axes->size() * 4);
                    release(auxBuffer(k, 2));
                    held.tables = data->axes;
                } else {
                    // (look.slang TWIST_AXIS_FLOATS a clump, then its length.)
                    allocate(auxBuffer(k, 1), std::max<std::size_t>(data->count * (kTwistAxisSamples * 3 + 1) * 4, 4));
                    share(auxBuffer(k, 2), data->members->data(), data->members->size() * 4);
                    held.tables = data->members;
                }
                held.b = axes;
            }
        }
        // ---- Jiggle and MeshCut ----
        if (kind == StageKind::Jiggle) {
            // Each strand's root lag and face, then each face's turn (look.slang JiggleCS), once per lags.
            const JiggleInputs* data = in ? in->jiggle.get() : nullptr;
            if (!data || !data->strands || data->strands->size() != stageN * 4 || !data->faces) {
                throw std::runtime_error("GPU preview: Jiggle's lags are for other strands");
            }
            if (held.a != data || !has(auxBuffer(k, 0)) || !has(auxBuffer(k, 1))) {
                write(auxBuffer(k, 0), data->strands->data(), std::max<std::size_t>(data->strands->size(), 4) * 4, false);
                write(auxBuffer(k, 1), data->faces->data(), data->faces->size() * 4, false);
                // (The turns in double the CPU's alone: JiggleCS drags by them there.)
                if (sharesBase() && data->faceTurns) share(auxBuffer(k, 2), data->faceTurns->data(), data->faceTurns->size() * 8);
                held.a = data;
                held.keep = in->jiggle;  // (kept alive: compared by address)
                runUploadMegabytes_ += static_cast<double>((data->strands->size() + data->faces->size()) * 4) / 1e6;
            }
        } else if (kind == StageKind::MeshCut) {
            // Its mesh's tree, points, normals and triangles (look.slang MeshCutCS), once per mesh.
            const MeshCutInputs* data = in ? in->meshCut.get() : nullptr;
            if (!data) throw std::runtime_error("GPU preview: MeshCut's mesh missing");
            if (held.a != data->tree.get() || !has(auxBuffer(k, 0))) {
                const std::uint32_t none = 0;
                if (data->tree) {
                    write(auxBuffer(k, 0), data->tree->data(), data->tree->size() * 4, false);
                    runUploadMegabytes_ += static_cast<double>(data->tree->size() * 4) / 1e6;
                } else {
                    write(auxBuffer(k, 0), &none, 4, false);
                }
                held.a = data->tree.get();
                held.keep = data->tree;
            }
        }
    }
}

// ---- The shapers reading their neighbours or a reference ----------------------------------------

void LookEngine::putShaperInputs(std::size_t k, const StageParams& stage, const StageInputs* in) {
    const std::size_t stageN = stageStrands(stage), stageP = stagePoints(stage);
    StageHeld& held = held_[k];
    if (stage.stage.kind == StageKind::Smooth) {
        // On the CPU its boxes' window ends, worked out once (look_stage_params.h smoothEnds:
        // look.slang SmoothCS's cpu form reads them from aux 0).
        if (!sharesBase()) return;
        const std::vector<SmoothEnd> ends = smoothEnds(stage.value[1], static_cast<std::size_t>(std::max(stage.flag[0], 0)));
        void* to = writable(auxBuffer(k, 0), std::max<std::size_t>(ends.size(), 1) * sizeof(SmoothEnd));
        if (!ends.empty()) std::memcpy(to, ends.data(), ends.size() * sizeof(SmoothEnd));
        return;
    }
    if (stage.stage.kind == StageKind::RestoreLength) {
        // A reference the run does not make (look.slang RestoreLengthCS): aux 0 its segments' lengths
        // per point, aux 1 its first segments per strand. (One the run makes: that stage's points.)
        const RestoreInputs* data = in ? in->restore.get() : nullptr;
        if (stage.flag[0] == 2 || stage.stage.from2 != kNoStage) return;
        if (!data || !data->lengths || data->lengths->size() != stageP || !data->firsts || data->firsts->size() != stageN * 4) {
            throw std::runtime_error("GPU preview: RestoreLength's reference is for other strands");
        }
        if (held.a != data->lengths.get() || !has(auxBuffer(k, 0))) {
            // (On the CPU the very memory, kept alive while held.)
            if (data->lengths->empty()) {
                write(auxBuffer(k, 0), nullptr, 4, false);
            } else {
                share(auxBuffer(k, 0), data->lengths->data(), data->lengths->size() * 4);
            }
            if (data->firsts->empty()) {
                write(auxBuffer(k, 1), nullptr, 16, false);
            } else {
                share(auxBuffer(k, 1), data->firsts->data(), data->firsts->size() * 4);
            }
            held.a = data->lengths.get();
            held.keep = in->restore;
            runUploadMegabytes_ += static_cast<double>((data->lengths->size() + data->firsts->size()) * 4) / 1e6;
        }
    } else if (stage.stage.kind == StageKind::BlendNeighbors) {
        // Its neighbours as its operator keeps them (look.slang BlendNeighborsCS): aux 0 the offsets,
        // aux 1 the neighbours, aux 2 their shares (on the CPU the very arrays, kept alive while held);
        // its root normals the root normals' (putStrandData).
        const BlendNeighborsState* data = in ? in->neighbors.get() : nullptr;
        if (!data || data->offsets.size() != stageN + 1 || data->normals.size() != stageN ||
            data->neighbors.size() != data->weights.size() || data->offsets.back() != data->neighbors.size()) {
            throw std::runtime_error("GPU preview: BlendNeighbors' neighbours are for other strands");
        }
        if (held.a != data || !has(auxBuffer(k, 0))) {
            const std::size_t n = stageN;
            if (std::any_of(data->neighbors.begin(), data->neighbors.end(), [&](std::uint32_t g) { return g >= n; })) {
                throw std::runtime_error("GPU preview: BlendNeighbors' neighbours are other strands");
            }
            auto put = [&](int buffer, const void* from, std::size_t bytes) {
                if (bytes) {
                    share(buffer, from, bytes);
                } else {
                    const std::uint32_t none = 0;
                    write(buffer, &none, 4, false);
                }
            };
            put(auxBuffer(k, 0), data->offsets.data(), data->offsets.size() * 4);
            put(auxBuffer(k, 1), data->neighbors.data(), data->neighbors.size() * 4);
            put(auxBuffer(k, 2), data->weights.data(), data->weights.size() * 4);
            held.a = data;
            held.keep = in->neighbors;
            runUploadMegabytes_ += static_cast<double>((data->offsets.size() + 2 * data->neighbors.size()) * 4) / 1e6;
        }
    } else if (stage.stage.kind == StageKind::Tangle) {
        // Its partners (look.slang TangleCS): aux 0, 4 words a strand (TangleInputs).
        const TangleInputs* data = in ? in->tangle.get() : nullptr;
        if (!data || !data->strands || data->strands->size() != stageN * 4) {
            throw std::runtime_error("GPU preview: Tangle's partners are for other strands");
        }
        if (held.a != data->strands.get() || !has(auxBuffer(k, 0))) {
            const std::uint32_t none[4] = {kNoTanglePartner, 0u, 0u, 0u};
            write(auxBuffer(k, 0), data->strands->empty() ? none : data->strands->data(), std::max<std::size_t>(data->strands->size(), 4) * 4, false);
            held.a = data->strands.get();
            held.keep = in->tangle;
            runUploadMegabytes_ += static_cast<double>(data->strands->size() * 4) / 1e6;
        }
    } else if (stage.stage.kind == StageKind::Flow) {
        // Its field and scalp (look.slang FlowCS): aux 0, 12 floats a strand; aux 1 the field; aux 2
        // the scalp's tables.
        const FlowInputs* data = in ? in->flow.get() : nullptr;
        if (!data || !data->strands || data->strands->size() != stageN * 12 || !data->field) {
            throw std::runtime_error("GPU preview: Flow's field is for other strands");
        }
        if (held.a != data->strands.get() || !has(auxBuffer(k, 0))) {
            const float none[12] = {};
            write(auxBuffer(k, 0), data->strands->empty() ? none : data->strands->data(), std::max<std::size_t>(data->strands->size(), 12) * 4, false);
            held.a = data->strands.get();
            runUploadMegabytes_ += static_cast<double>(data->strands->size() * 4) / 1e6;
        }
        if (held.b != data->field.get() || !has(auxBuffer(k, 1))) {
            write(auxBuffer(k, 1), data->field->data(), data->field->size() * 4, false);
            held.b = data->field.get();
            runUploadMegabytes_ += static_cast<double>(data->field->size() * 4) / 1e6;
        }
        if (held.c != data->tables.get() || !has(auxBuffer(k, 2))) {
            const std::uint32_t empty = 0;
            write(auxBuffer(k, 2), data->tables ? data->tables->data() : &empty, data->tables ? data->tables->size() * 4 : 4, false);
            held.c = data->tables.get();
            held.tables = data->tables;  // (kept alive: compared by address)
            if (data->tables) runUploadMegabytes_ += static_cast<double>(data->tables->size() * 4) / 1e6;
        }
        held.keep = in->flow;
    }
}

// The shapers' dispatches (stage k from `input` into `to`), into the run's steps.
LookEngine::Kernel LookEngine::shaperKernel(std::size_t k, const StageParams& stage, const StageInputs* in, int input2, Binding* b,
                                            std::size_t* groups, StageConstants* c) const {
    const std::size_t stageN = stageStrands(stage);
    switch (stage.stage.kind) {
        case StageKind::Smooth:
            // A group a strand (look.slang SmoothCS); on the CPU a thread a strand, SMOOTH_GROUP (64)
            // strands a group, reading its window ends.
            *groups = sharesBase() ? (stageN + 63) / 64 : stageN;
            if (sharesBase()) b->aux[0] = auxBuffer(k, 0);
            return Kernel::Smooth;
        case StageKind::RestoreLength:
            if (stage.flag[0] != 2) {
                // The reference's points (a stage of the run), else its lengths and first segments.
                b->aux[0] = stage.stage.from2 != kNoStage ? input2 : auxBuffer(k, 0);
                b->aux[1] = stage.stage.from2 != kNoStage ? -1 : auxBuffer(k, 1);
            }
            return Kernel::RestoreLength;
        case StageKind::BlendNeighbors:
            b->aux[0] = auxBuffer(k, 0);
            b->aux[1] = auxBuffer(k, 1);
            b->aux[2] = auxBuffer(k, 2);
            return Kernel::BlendNeighbors;
        case StageKind::Tangle: b->aux[0] = auxBuffer(k, 0); return Kernel::Tangle;
        case StageKind::Flow: {
            // gFlag: arrows, the field's vertices, its pins; gFlag2: its sinks, a scalp (look.slang FlowCS).
            const FlowInputs* flow = in ? in->flow.get() : nullptr;
            if (!flow) throw std::runtime_error("GPU preview: Flow's field missing");
            c->flag[0] = flow->arrows ? 1u : 0u;
            c->flag[1] = flow->vertices;
            c->flag[2] = flow->pins;
            c->flag2[0] = flow->sinks;
            c->flag2[1] = flow->tables ? 1u : 0u;
            if (sharesBase()) {
                // (On the CPU the field and scalp themselves, asked by address: look.slang hostFlow*.)
                const auto host = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&flow->host));
                c->flag2[1] = flow->host.scalp ? 1u : 0u;
                c->flag2[2] = static_cast<std::uint32_t>(host);
                c->flag2[3] = static_cast<std::uint32_t>(host >> 32);
            }
            b->aux[0] = auxBuffer(k, 0);
            b->aux[1] = auxBuffer(k, 1);
            b->aux[2] = auxBuffer(k, 2);
            return Kernel::Flow;
        }
        default: break;
    }
    throw std::runtime_error("GPU preview: a node this engine does not run");
}

void LookEngine::prepare(const LookRun& run, bool frames) {
    const auto start = std::chrono::steady_clock::now();
    runUploadMegabytes_ = 0;
    if (run.params.stages.size() > kMaxStages) throw std::runtime_error("GPU preview: more nodes than it runs");
    if (!run.fields.stages.empty() && run.fields.stages.size() != run.params.stages.size()) {
        throw std::runtime_error("GPU preview: mapped values read for another chain");
    }
    putStrandData(run, frames);
    prepareParts_[0] = since(start);
    if (run.fields.stages.empty()) {
        if (fieldLayout_.size() != run.params.stages.size() * kFieldSlots * 2 || fieldSerial_ != 0) {
            fieldLayout_.assign(run.params.stages.size() * kFieldSlots * 2, 0);
            const float none = 0.0f;
            write(kFields, &none, 4, false);
            fieldSerial_ = 0;
            fieldsEmpty_ = true;
        }
        programs_.clear();
    } else {
        putFields(run.fields, run.params.stages);
    }
    prepareParts_[1] = since(start);
    putStageInputs(run);
    runUploadMs_ = since(start);
    prepareParts_[2] = runUploadMs_;
}

void LookEngine::run(const LookRun& r, bool frames, const LookTargets& targets) {
    if (!base_) throw std::runtime_error("GPU preview: no strands uploaded");
    // Hidden core strands (preview_core.h): every strand runs into kFull0-3, the shown ones are
    // gathered into the outputs after.
    // (An operator's own run, baseAsOutput: every strand its strand, none hidden.)
    const bool gather = !shownPoints_.empty() && !targets.baseAsOutput;
    if (gather && targets.pointTexture) throw std::runtime_error("GPU preview: a point texture of strands with hidden ones");
    // The kernels it may dispatch, waited for where they compile (a host asks kernelsReady first).
    const KernelSet need = kernelsOf(r);
    for (int k = 0; k < static_cast<int>(Kernel::Count); ++k) {
        if (need.test(static_cast<std::size_t>(k))) kernelReady(static_cast<Kernel>(k), true);
    }
    prepare(r, frames);
    const std::vector<StageParams>& stages = r.params.stages;
    const std::size_t bytes = pointCount_ * 12;
    outPoints_ = gather ? shownPoints_.size() : pointCount_;
    const std::size_t outBytes = outPoints_ * 12;
    const std::size_t n = strandCount_;
    if (gather && shownKey_ != baseSerial_ + 1) {
        write(kShown, shownPoints_.data(), shownPoints_.size() * 4, false);
        shownKey_ = baseSerial_ + 1;
    }
    const int finalOut = gather ? static_cast<int>(kFull0) : static_cast<int>(kOut0);

    // The profiles: each stage's keys (position, value, tangent each: Ramp's own tangents) and,
    // where it reads them by point index and every strand has the same count, the CPU's RampAlong
    // table.
    ramps_.clear();
    struct RampAt {
        std::uint32_t count = 0, offset = 0, straight = 0, tableCount = 0xffffffffu, tableOffset = 0;
    };
    auto putRamp = [&](const Ramp& ramp, bool tabled) {
        RampAt at;
        at.count = static_cast<std::uint32_t>(ramp.keys.size());
        at.offset = static_cast<std::uint32_t>(ramps_.size());
        at.straight = ramp.straight ? 1u : 0u;
        for (std::size_t i = 0; i < ramp.keys.size(); ++i) {
            ramps_.push_back(ramp.keys[i].first);
            ramps_.push_back(ramp.keys[i].second);
            ramps_.push_back(ramp.tangent(i));
        }
        if (tabled && uniformCount_ > 0) {
            at.tableCount = static_cast<std::uint32_t>(uniformCount_);
            at.tableOffset = static_cast<std::uint32_t>(ramps_.size());
            for (std::size_t i = 0; i < uniformCount_; ++i) ramps_.push_back(ramp(paramAt(i, uniformCount_)));
        }
        return at;
    };
    std::vector<RampAt> rampAt(stages.size()), rampAt2(stages.size());
    for (std::size_t k = 0; k < stages.size(); ++k) {
        rampAt[k] = putRamp(stages[k].ramp, stages[k].tabled);
        if (stages[k].hasRamp2) rampAt2[k] = putRamp(stages[k].ramp2, true);  // (RampAlong: by point index)
    }
    if (ramps_.empty()) ramps_.push_back(0.0f);
    write(kRamps, ramps_.data(), ramps_.size() * 4, false);

    // The dispatches: each stage reads the points the one before wrote (the base first) and writes
    // the next of two buffers, the last stage that moves points the output; Width writes widths.
    struct Step {
        Kernel kernel;
        Binding binding;
        std::size_t groups;
        bool copy;  // the base copied to the output (no stage moved the points)
        int stampKind;  // after it, a stamp closing a span of this kind (StageKind; -1 lines); -3: none
        std::size_t repeat = 0;  // > 0: that many dispatches, its slot and the next in turn (dispatchAlternating)
        int copyFrom = kBasePoints, copyTo = kOut0;  // `copy`: what into what (copyBytes; 0: the points)
        std::size_t copyBytes = 0, copyOffset = 0;    // (from that byte of both)
        std::size_t copyFromOffset = kSameOffset;     // (from that byte of copyFrom instead)
        int onlyIf = -1;                 // >= 0: dispatched only where that buffer's first word is onlyIfValue or more (runsIf)
        std::uint32_t onlyIfValue = 0;
        // >= 0: its groups at most that buffer's word countWord (+ 1 every other repeat) over
        // countPer, where the engine can tell (countedGroups); the kernel leaves past it either way.
        int countBuffer = -1;
        std::uint32_t countWord = 0, countPer = 1;
    };
    std::vector<Step> steps;
    // Room for the widths a Width makes: on the CPU, writing them into the run's target, that very
    // memory (bindTargets), none of its own.
    auto roomForWidths = [&] {
        if (!(sharesBase() && targets.widths)) allocate(kWidths, pointCount_ * 4);
    };
    std::vector<StageConstants> slots;
    std::size_t lastMover = kNoStage;
    for (std::size_t k = 0; k < stages.size(); ++k) {
        if (stages[k].stage.kind != StageKind::Width) lastMover = k;
    }
    // A field made on the GPU reads the guides' influence: the run's Interpolate writes it.
    bool needInfluence = false;
    for (const ProgramAt& p : programs_) needInfluence = needInfluence || (p.on && p.influence);
    if (needInfluence) allocate(kInfluence, std::max<std::size_t>(n * 4, 4));
    auto constantsOf = [&](std::size_t k) {
        StageConstants c{};
        const StageParams& p = stages[k];
        // (Up a Merge's input a: a's strands alone, the base's first; the dispatches as many as for all.)
        c.strandCount = static_cast<std::uint32_t>(stageStrands(p));
        c.uniformCount = static_cast<std::uint32_t>(uniformCount_);
        c.pointCount = static_cast<std::uint32_t>(stagePoints(p));
        c.dataBlock = static_cast<std::uint32_t>(dataBlocks_[k] * n);
        for (int i = 0; i < 4; ++i) {
            c.value[i] = p.value[i];
            c.flag[i] = static_cast<std::uint32_t>(p.flag[i]);
            c.field[i] = k * kFieldSlots * 2 + i < fieldLayout_.size() ? fieldLayout_[k * kFieldSlots * 2 + i] : 0u;
            c.aux[i] = held_[k].aux[i];
        }
        if (p.stage.kind == StageKind::Interpolate) {
            // keepClearance (flag 3): the clearance, the depth, beside; flag 2 the search radius's
            // float bits; value2 0 how far an offset eases back over, value2 1 how far apart two
            // asked points walk the points between one by one. (Value 0 is limitDistance.)
            const InterpolateInputs* in = k < r.inputs.size() ? r.inputs[k].interpolate.get() : nullptr;
            const InterpolateClearance* clearance = in ? in->clearance.get() : nullptr;
            c.flag[3] = clearance ? 1u : 0u;
            if (clearance) {
                c.value[1] = clearance->floor;
                c.value[2] = clearance->depth;
                c.value[3] = clearance->beside;
                std::memcpy(&c.flag[2], &clearance->searchRadius, sizeof(float));
            }
        }
        if (p.stage.kind == StageKind::Noise || p.stage.kind == StageKind::Flyaway) {
            for (std::uint64_t o = 0; o < 8; ++o) {  // VectorNoise's octave seeds
                const std::uint64_t seed = hashCombine(p.seed, o);
                c.octaveSeeds[o * 2] = static_cast<std::uint32_t>(seed);
                c.octaveSeeds[o * 2 + 1] = static_cast<std::uint32_t>(seed >> 32);
            }
        }
        if (p.stage.kind == StageKind::Wind) {
            // WindOp's noises' octave seeds: the gusts' two octaves, then the sway's and the
            // flutter's one each.
            const std::uint64_t gust = hashCombine(p.seed, 0x6057), sway = hashCombine(p.seed, 0x5A7), flutter = hashCombine(p.seed, 0xF1E7);
            const std::uint64_t seeds[4] = {hashCombine(gust, 0), hashCombine(gust, 1), hashCombine(sway, 0), hashCombine(flutter, 0)};
            for (std::size_t o = 0; o < 4; ++o) {
                c.octaveSeeds[o * 2] = static_cast<std::uint32_t>(seeds[o]);
                c.octaveSeeds[o * 2 + 1] = static_cast<std::uint32_t>(seeds[o] >> 32);
            }
        }
        c.ramp[0] = rampAt[k].count;
        c.ramp[1] = rampAt[k].offset;
        c.ramp[2] = rampAt[k].straight;
        c.ramp[3] = rampAt[k].tableCount;
        c.table[0] = rampAt[k].tableOffset;
        c.table[1] = 0;  // the root normals: block 0
        for (int i = 0; i < 4; ++i) {
            c.value2[i] = p.value2[i];
            c.flag2[i] = static_cast<std::uint32_t>(p.flag2[i]);
        }
        c.ramp2[0] = rampAt2[k].count;
        c.ramp2[1] = rampAt2[k].offset;
        c.ramp2[2] = rampAt2[k].straight;
        c.ramp2[3] = rampAt2[k].tableCount;
        c.table2[0] = rampAt2[k].tableOffset;
        if (p.stage.kind == StageKind::Interpolate) {
            c.flag2[0] = needInfluence ? 1u : 0u;  // the influence for FieldCS
            c.flag2[2] = held_[k].aux[1];          // keeping clearance over a deformed scalp: its walk words (0: none)
            const InterpolateInputs* in = k < r.inputs.size() ? r.inputs[k].interpolate.get() : nullptr;
            if (in && in->clearance) {  // keeping clearance: how far an offset eases back over, the span walked one by one
                c.value2[0] = in->clearance->ease;
                c.value2[1] = in->clearance->span;
            }
        }
        if (p.stage.kind == StageKind::Clump) {
            const ClumpInputs* in = k < r.inputs.size() ? r.inputs[k].clump.get() : nullptr;
            c.flag[3] = in ? static_cast<std::uint32_t>(in->centers) : 0u;  // (its root normals follow its frames)
        }
        if (p.stage.kind == StageKind::Tie) tieConstants(p, k < r.inputs.size() ? r.inputs[k].tie.get() : nullptr, &c);
        if (p.stage.kind == StageKind::Transform) std::memcpy(c.octaveSeeds, p.more, sizeof c.octaveSeeds);  // (look.slang gMore)
        return c;
    };
    // FieldCS (and LongestCS before it) making a field's values from the points the stage takes.
    auto fieldSteps = [&](std::size_t k, int in) {
        for (std::size_t i = 0; i < kFieldSlots; ++i) {  // (2: a masked node's affect, on what it takes)
            const std::size_t at = k * kFieldSlots + i;
            if (at >= programs_.size() || !programs_[at].on) continue;
            const ProgramAt& p = programs_[at];
            if (p.longest) {
                allocate(kLongest, 16);
                StageConstants c{};
                c.strandCount = static_cast<std::uint32_t>(stageStrands(stages[k]));
                c.aux[0] = p.core;
                c.aux[1] = p.coreCount;
                Binding b;
                b.in = in;
                b.aux[0] = kFieldPrograms;
                b.out[0] = kLongest;
                slots.push_back(c);
                steps.push_back({Kernel::Longest, b, 1, false, -3});
            }
            StageConstants c{};
            c.strandCount = static_cast<std::uint32_t>(stageStrands(stages[k]));
            c.aux[0] = p.ops;
            c.aux[1] = p.opCount;
            c.aux[2] = p.statics;
            c.aux[3] = p.slotCount;
            c.flag[0] = (p.shape ? 1u : 0u) | (p.longest ? 2u : 0u) | (p.influence ? 4u : 0u) | (p.normal ? 8u : 0u);
            c.flag[1] = (at * 2 + 1) < fieldLayout_.size() ? fieldLayout_[at * 2 + 1] : 0u;
            c.value2[0] = p.low;
            c.value2[1] = p.high;
            Binding b;
            b.in = in;
            b.aux[0] = kFieldPrograms;
            b.aux[1] = p.influence ? static_cast<int>(kInfluence) : -1;
            b.aux[2] = p.longest ? static_cast<int>(kLongest) : -1;
            b.out[0] = kFields;
            slots.push_back(c);
            steps.push_back({Kernel::Field, b, (n + 63) / 64, false, -3});
        }
    };
    const std::size_t strandGroups = (n + 63) / 64, pointGroups = (pointCount_ + 255) / 256;
    // SelfCollide and Static (stage k, its points from `input` into `to`; `widths` a Width before it
    // in the run made kWidths): the hair grid laid, splatted, solved and each strand pushed
    // (look.slang's hair grid), SelfCollide's three passes of it. Aux 0: the segments; 1: the grid
    // (header, block table, blocks' cells); 2: the sums; 3: the nodes' own; 4: the floor's tables and
    // the scalp's walls; 5: the solve's unknowns; 6: each point's move for the walk.
    auto gridSteps = [&](std::size_t k, int input, int to, bool widths) {
        const StageParams& p = stages[k];
        const HairGridLayout& l = *r.inputs[k].grid->layout;
        const bool isStatic = p.stage.kind == StageKind::Static;
        const StageHeld& held = held_[k];
        const std::size_t cells = std::size_t{held.room[0]} * held.room[1] * held.room[2], blocks = held.room[3];
        const std::uint32_t sumWords = isStatic ? 10u : 24u;  // (a node's mass, then its cell's 4 sums or 11, two words each)
        const int segments = auxBuffer(k, 0), grid = auxBuffer(k, 1), sums = auxBuffer(k, 2), nodes = auxBuffer(k, 3), floor = auxBuffer(k, 4);
        const int unknowns = auxBuffer(k, 5);  // the solve's unknowns, a list a colour (look.slang GridUnknownsCS)
        const int moves = auxBuffer(k, 6);     // each point's move, for the walk (look.slang GridWalkCS)
        const int moved = auxBuffer(k, 7);     // SelfCollide: the last pass that moved hair, + 1 (SelfCollidePushCS)
        allocate(grid, (32 + cells + blocks) * 4);
        if (!isStatic) allocate(moved, 4);
        allocate(moves, bytes);
        allocate(unknowns, std::max<std::size_t>(blocks * 64 * 10 * 4, 4));  // (look.slang UNKNOWN_WORDS)
        allocate(sums, std::max<std::size_t>(blocks * 64 * sumWords * 4, 4));
        allocate(nodes, std::max<std::size_t>(blocks * 64 * 12 * 4, 4));
        const double h = l.h;
        StageConstants g = constantsOf(k);  // (its per-strand numbers, mapped strength, profile)
        g.value[0] = p.value[0];
        g.value[1] = static_cast<float>(h);
        g.value[2] = static_cast<float>(0.5 * h);
        g.value[3] = l.beside;
        g.value2[0] = static_cast<float>(4294967296.0 / (h * h * h));
        g.value2[1] = static_cast<float>(isStatic ? l.section : l.nominal);
        g.value2[2] = static_cast<float>(l.weight);
        g.value2[3] = p.value[1];
        const std::size_t segmentCount = l.segments.size() / 2;
        g.flag[0] = (isStatic ? 1u : 0u) | (!isStatic && l.widths ? 2u : 0u) | (l.tables ? 4u : 0u);
        g.flag[1] = static_cast<std::uint32_t>(segmentCount);
        g.flag[2] = static_cast<std::uint32_t>(l.margin);
        g.flag[3] = 0;
        for (int a = 0; a < 4; ++a) g.flag2[a] = held.room[a];
        auto bits = [](float v) {
            std::uint32_t b;
            std::memcpy(&b, &v, 4);
            return b;
        };
        std::uint32_t* grid4 = g.octaveSeeds;  // (look.slang gGrid0-3)
        grid4[0] = isStatic ? 0u : static_cast<std::uint32_t>(p.flag2[0]);
        grid4[1] = isStatic ? 0u : static_cast<std::uint32_t>(p.flag2[1]);
        grid4[2] = bits(isStatic ? 1.8f : 1.7f);  // (static_op.cpp, self_collide_op.cpp kOmega)
        grid4[3] = held.wallsAt;
        grid4[4] = bits(l.floor);
        grid4[5] = bits(l.depth);
        grid4[6] = bits(l.searchRadius);
        grid4[7] = bits(static_cast<float>(std::sqrt(4.0 * l.nominal / 3.14159265358979323846)));
        grid4[8] = bits(static_cast<float>(0.5 / h));
        grid4[9] = bits(static_cast<float>(h * h));
        grid4[10] = bits(static_cast<float>(2.0 * 2.0 * h * h));  // (static_op.cpp kSoften cells, squared)
        grid4[11] = bits(static_cast<float>(l.wallReach * h));
        grid4[12] = sumWords;
        for (int w = 13; w < 16; ++w) grid4[w] = 0;
        // The ops' numbers in double (the CPU's kernels: look.slang gridWeightD, gridNominalD, and
        // SelfCollide's gridPackingD, as readStageParams put it in `more`).
        const double section = isStatic ? l.section : l.nominal;
        std::memcpy(&g.ramp2[0], &l.weight, sizeof(double));
        std::memcpy(&g.ramp2[2], &section, sizeof(double));
        if (!isStatic) std::memcpy(&g.table2[0], &p.more[0], sizeof(double));
        const std::size_t segmentGroups = std::max<std::size_t>((segmentCount + 63) / 64, 1), cellGroups = (32 + cells + 63) / 64;
        // A pass after one moving no hair would find the same (its grid, so no push): skipped where
        // the engine can tell (runsIf; self_collide_op.cpp stops there).
        int onlyIf = -1;
        std::uint32_t onlyIfValue = 0;
        auto add = [&](Kernel kernel, const Binding& b, std::size_t groups, std::uint32_t which, bool last = false) {
            StageConstants c = g;
            c.flag[3] = which;
            slots.push_back(c);
            steps.push_back({kernel, b, groups, false, last ? static_cast<int>(p.stage.kind) : -3});
            steps.back().onlyIf = onlyIf;
            steps.back().onlyIfValue = onlyIfValue;
        };
        // A kernel a thread a node of the blocks laid: its groups (a block each) as many as laid
        // (look.slang GRID_COUNT), where the engine can tell.
        auto blocksCounted = [&] {
            steps.back().countBuffer = grid;
            steps.back().countWord = 15;
            steps.back().countPer = 1;
        };
        auto solve = [&](const Binding& b, int sweeps) {  // red-black: colour 0, then 1, each sweep
            StageConstants c = g;
            c.flag[3] = 0;
            slots.push_back(c);
            c.flag[3] = 1;
            slots.push_back(c);
            Step s{Kernel::GridSor, b, blocks, false, -3};
            s.repeat = static_cast<std::size_t>(2 * sweeps);
            s.onlyIf = onlyIf;
            s.onlyIfValue = onlyIfValue;
            s.countBuffer = grid;  // (each colour's unknowns, look.slang GRID_UNKNOWNS: a thread each)
            s.countWord = 26;
            s.countPer = 64;
            steps.push_back(s);
        };
        const int passes = isStatic ? 1 : 3;  // (self_collide_op.cpp kPasses)
        for (int pass = 0; pass < passes; ++pass) {
            const int points = pass == 0 ? input : to;  // (later passes splat the strands the one before moved)
            if (pass > 0) {
                onlyIf = moved;
                onlyIfValue = static_cast<std::uint32_t>(pass);
            }
            Binding b;
            b.out[0] = grid;
            Binding cleared = b;
            if (!isStatic) cleared.out[1] = moved;
            add(Kernel::GridClear, cleared, cellGroups, static_cast<std::uint32_t>(pass));
            Binding sampled;
            sampled.in = points;
            sampled.aux[0] = segments;
            sampled.out[0] = grid;
            add(Kernel::GridBounds, sampled, segmentGroups, 0);
            add(Kernel::GridLay, b, 1, 0);
            add(Kernel::GridMark, sampled, segmentGroups, 0);
            for (int ring = 0; ring < l.dilate; ++ring) add(Kernel::GridDilate, b, cellGroups, 0);
            add(Kernel::GridCompact, b, cellGroups, 0);
            Binding zero;
            zero.aux[1] = grid;
            zero.out[0] = sums;
            zero.out[1] = nodes;
            add(Kernel::GridZero, zero, blocks, 0);
            blocksCounted();
            Binding splat;
            splat.in = points;
            splat.aux[0] = segments;
            splat.aux[1] = grid;
            splat.aux[2] = widths ? static_cast<int>(kWidths) : static_cast<int>(kBaseWidths);
            splat.out[0] = sums;
            add(Kernel::GridSplat, splat, segmentGroups, 0);
            Binding node;
            node.aux[0] = sums;
            node.aux[1] = grid;
            node.aux[2] = floor;
            node.out[0] = nodes;
            Binding solved;
            solved.aux[0] = sums;
            solved.aux[1] = grid;
            solved.out[0] = nodes;
            Binding listed;  // (the unknowns listed, from the nodes' words)
            listed.aux[0] = sums;
            listed.aux[1] = nodes;
            listed.out[0] = grid;
            listed.out[1] = unknowns;
            Binding sor;
            sor.aux[0] = unknowns;
            sor.aux[1] = grid;
            sor.out[0] = nodes;
            Binding push;
            push.in = input;  // (the lengths as they came)
            push.aux[0] = nodes;
            push.aux[1] = grid;
            push.aux[2] = floor;
            push.out[0] = to;
            push.out[1] = moves;
            if (!isStatic) push.out[2] = moved;
            Binding walk;  // (the strand from its first point moved, kept above the floor)
            walk.in = input;
            walk.aux[0] = moves;
            walk.aux[1] = grid;
            walk.aux[2] = floor;
            walk.out[0] = to;
            if (!isStatic) {
                add(Kernel::SelfCollideNode, node, blocks, 0);
                blocksCounted();
                add(Kernel::GridUnknowns, listed, blocks, 0);
                blocksCounted();
                solve(sor, 48);  // (self_collide_op.cpp kSweeps)
                Binding gradient;
                gradient.aux[1] = grid;
                gradient.out[0] = nodes;
                add(Kernel::SelfCollideGradient, gradient, blocks, 0);
                blocksCounted();
                add(Kernel::SelfCollidePush, push, strandGroups, static_cast<std::uint32_t>(pass));
                add(Kernel::GridWalk, walk, strandGroups, 0, pass + 1 == passes);
            } else {
                for (std::uint32_t axis = 0; axis < 3; ++axis) {
                    add(Kernel::StaticReach, solved, blocks, axis);
                    blocksCounted();
                }
                Binding moments;
                moments.aux[0] = sums;
                moments.out[0] = grid;
                add(Kernel::StaticMoments, moments, blocks, 0);
                blocksCounted();
                add(Kernel::StaticNode, node, blocks, 0);
                blocksCounted();
                add(Kernel::GridUnknowns, listed, blocks, 0);
                blocksCounted();
                solve(sor, 64);  // (static_op.cpp kSweeps)
                add(Kernel::StaticField, solved, blocks, 0);
                blocksCounted();
                add(Kernel::StaticPush, push, strandGroups, 0);
                add(Kernel::GridWalk, walk, strandGroups, 0, true);
            }
        }
    };
    // Strands of different point counts run a thread a point by each point's strand: made once
    // per layout of the base (a full upload), on the GPU from the offsets.
    if (perPoint() && uniformCount_ == 0 && pointCount_ > 0 && pointStrandsKey_ != baseSerial_) {
        allocate(kPointStrands, pointCount_ * 4);
        StageConstants c{};
        c.strandCount = static_cast<std::uint32_t>(n);
        c.pointCount = static_cast<std::uint32_t>(pointCount_);
        Binding b;
        b.out[0] = kPointStrands;
        slots.push_back(c);
        steps.push_back({Kernel::PointStrands, b, strandGroups, false, kPointStrandsStamp});
    }
    // Each stage's result: the buffer holding it (a Width's: the points it took), and the last
    // stage taking it (LookStage::from; a Blend takes two). A stage writes a buffer holding no
    // result a stage after it takes: two take turns along a chain, a third holds where a Blend's
    // looks part while look a runs, then look a's result while look b runs.
    std::vector<int> resultOf(stages.size(), -1);
    std::vector<std::size_t> lastTaker(stages.size(), 0);  // (0: none, every taker comes after)
    for (std::size_t k = 0; k < stages.size(); ++k) {
        const LookStage& at = stages[k].stage;
        for (const std::size_t from : {at.from, at.from2}) {  // (from2: a Blend's look b, a RestoreLength's reference)
            if (from == kNoStage) continue;
            if (from >= k) throw std::runtime_error("GPU preview: a node takes the result of one after it");
            lastTaker[from] = k;
        }
    }
    // ---- Affect masks ----
    // Each stage's node's first stage (a Wet's largest level), and whether it is its node's last: a
    // masked node's result mixed back toward what its first stage took after its last (AffectCS),
    // which holds that till then.
    std::vector<std::size_t> firstOfNode(stages.size(), 0);
    auto lastOfNode = [&](std::size_t k) { return k + 1 == stages.size() || stages[k + 1].stage.node != stages[k].stage.node; };
    for (std::size_t k = 0; k < stages.size(); ++k) {
        firstOfNode[k] = k > 0 && stages[k - 1].stage.node == stages[k].stage.node ? firstOfNode[k - 1] : k;
        const std::size_t from = stages[firstOfNode[k]].stage.from;
        if (stages[k].masked && stages[k].stage.kind != StageKind::Tie && lastOfNode(k) && from != kNoStage) {
            lastTaker[from] = std::max(lastTaker[from], k);
        }
    }
    auto taken = [&](std::size_t from) { return from == kNoStage ? static_cast<int>(kBasePoints) : resultOf[from]; };
    auto freeBuffer = [&](std::size_t k, int in, int in2) {
        for (const int buffer : {static_cast<int>(kPointsA), static_cast<int>(kPointsB), static_cast<int>(kPointsC)}) {
            bool held = buffer == in || buffer == in2;
            for (std::size_t j = 0; j < k && !held; ++j) held = resultOf[j] == buffer && lastTaker[j] >= k;
            if (!held) return buffer;
        }
        throw std::runtime_error("GPU preview: no buffer free for a node's result");
    };
    int current = kBasePoints;  // the last stage's result
    bool widthsMade = false;  // a Width ran before the stage (kWidths: its widths)
    // ---- Affect masks ----
    // Stage k's node's result (its points in `to`; a Width's widths in kWidths, `widthsIn` those it
    // took) mixed back toward what its first stage took by its affect (look.slang AffectCS): the
    // field the node's first stage holds (or a Tie's tail where the run writes it), times its number.
    // Its time the node's: the stamp closing it moves after the mix.
    auto mixAffect = [&](std::size_t k, int to, int widthsIn) {
        const StageParams& p = stages[k];
        if (!p.masked || p.stage.kind == StageKind::Tie || !lastOfNode(k)) return;
        const bool width = p.stage.kind == StageKind::Width;
        if (width && !p.mixWidths) return;  // (strands without widths: the CPU keeps the Width's)
        const std::size_t first = firstOfNode[k];
        const FieldValues* affect = first < r.fields.stages.size() ? &r.fields.stages[first].affect : nullptr;
        StageConstants c{};
        c.strandCount = static_cast<std::uint32_t>(stageStrands(p));
        c.uniformCount = static_cast<std::uint32_t>(uniformCount_);
        c.pointCount = static_cast<std::uint32_t>(stagePoints(p));
        c.value[0] = p.affect;
        const std::size_t at = (first * kFieldSlots + 2) * 2;
        c.field[0] = at < fieldLayout_.size() ? fieldLayout_[at] : 0u;
        c.field[1] = at + 1 < fieldLayout_.size() ? fieldLayout_[at + 1] : 0u;
        Binding b;
        b.in = taken(stages[first].stage.from);
        if (affect && affect->channelFrom != kNoStage) {
            if (affect->channelFrom >= k || stages[affect->channelFrom].stage.kind != StageKind::Tie) {
                throw std::runtime_error("GPU preview: an affect read from a node that is no Tie before it");
            }
            c.flag[0] = 1u;
            c.value[1] = affect->low;
            c.value[2] = affect->high;
            b.aux[0] = auxBuffer(affect->channelFrom, 4);
        }
        if (width) {
            c.flag[1] = 1u;
            b.aux[1] = widthsIn;
            b.out[1] = kWidths;
        } else {
            c.flag[2] = 1u;
            b.out[0] = to;
        }
        int stamp = -3;
        if (!steps.empty() && steps.back().stampKind == static_cast<int>(p.stage.kind)) std::swap(stamp, steps.back().stampKind);
        slots.push_back(c);
        steps.push_back({Kernel::Affect, b, strandGroups, false, stamp});
    };
    std::vector<std::size_t> walkedStages;  // Interpolates whose walk the run keeps (StageHeld::walked)
    for (std::size_t k = 0; k < stages.size(); ++k) {
        const StageParams& p = stages[k];
        StageConstants c = constantsOf(k);
        const int input = taken(p.stage.from);
        fieldSteps(k, input);
        if (p.stage.kind == StageKind::Width) {
            // Masked: the widths it takes kept for the mix (a Width's before it, else the base's).
            int widthsIn = kBaseWidths;
            if (p.masked && p.mixWidths && widthsMade) {
                widthsIn = auxBuffer(k, 0);
                allocate(widthsIn, pointCount_ * 4);
                Step keep{Kernel::Count, Binding{}, 0, true, -3};
                keep.copyFrom = kWidths;
                keep.copyTo = widthsIn;
                keep.copyBytes = pointCount_ * 4;
                steps.push_back(keep);
            }
            widthsMade = true;
            roomForWidths();
            Binding b;
            b.in = input;
            b.out[0] = kWidths;
            slots.push_back(c);
            steps.push_back({perPoint() ? Kernel::WidthPoints : Kernel::Width, b, perPoint() ? pointGroups : strandGroups, false,
                             static_cast<int>(p.stage.kind)});
            mixAffect(k, -1, widthsIn);
            resultOf[k] = current = input;
            continue;
        }
        if (gathers(p.stage.kind)) {
            // The run up its input (on the sibling: its points aux 0, its widths aux 1 where it made
            // them) laid out as the node's result (aux 2: LayoutCS), into a buffer of the run's (the
            // output when nothing after it moves points). Without that run: its result as cached.
            const bool branch = k < r.inputs.size() && r.inputs[k].merged;
            int out = branch || k == lastMover ? (k == lastMover ? finalOut : freeBuffer(k, input, -1)) : static_cast<int>(kBasePoints);
            if (out != kOut0 && out != kBasePoints) allocate(out, bytes);
            if (!branch) {
                if (out != kBasePoints) {
                    Step copy{Kernel::Count, Binding{}, 0, true, -3};
                    copy.copyFrom = kBasePoints;
                    copy.copyTo = out;
                    copy.copyBytes = bytes;
                    steps.push_back(copy);
                }
                resultOf[k] = current = out;
                continue;
            }
            Binding b;
            b.in = auxBuffer(k, 0);
            b.out[0] = out;
            b.aux[0] = auxBuffer(k, 2);
            if (runsWidth(r.inputs[k].merged->params)) {
                roomForWidths();
                widthsMade = true;
                b.aux[1] = auxBuffer(k, 1);
                b.out[1] = kWidths;
                c.flag[1] = 1u;
            }
            slots.push_back(c);
            steps.push_back({Kernel::Layout, b, strandGroups, false, static_cast<int>(p.stage.kind)});
            resultOf[k] = current = out;
            continue;
        }
        if (p.stage.kind == StageKind::Merge) {
            // b's strands follow a's: their points (and their widths, where a Width up a made a's in
            // kWidths) from the base, after a's result, in place (into the output when nothing after
            // the Merge moves points). Moved by the drag, from b's own run on the sibling (aux 0, its
            // widths aux 1 where it made them: a's then the base's where no Width up a made them),
            // into a buffer of the run's (never the base).
            const std::size_t strandsA = std::min<std::size_t>(static_cast<std::size_t>(std::max(p.flag[0], 0)), n);
            const std::size_t pointsA = base_->strandBegin(strandsA);
            const bool branch = k < r.inputs.size() && r.inputs[k].merged;
            const bool branchWidths = branch && runsWidth(r.inputs[k].merged->params);
            int out = input;
            if ((k == lastMover && input != finalOut) || (branch && input == kBasePoints)) {
                out = k == lastMover ? finalOut : freeBuffer(k, input, -1);
                if (out != kOut0) allocate(out, bytes);
                Step a{Kernel::Count, Binding{}, 0, true, -3};
                a.copyFrom = input;
                a.copyTo = out;
                a.copyBytes = pointsA * 12;
                if (pointsA) steps.push_back(a);
            }
            if (out != kBasePoints && pointCount_ > pointsA) {
                Step b{Kernel::Count, Binding{}, 0, true, -3};
                b.copyFrom = branch ? auxBuffer(k, 0) : static_cast<int>(kBasePoints);
                b.copyTo = out;
                b.copyOffset = pointsA * 12;
                if (branch) b.copyFromOffset = 0;
                b.copyBytes = (pointCount_ - pointsA) * 12;
                steps.push_back(b);
            }
            if (branchWidths && !widthsMade) {
                roomForWidths();
                Step w{Kernel::Count, Binding{}, 0, true, -3};
                w.copyFrom = kBaseWidths;
                w.copyTo = kWidths;
                w.copyBytes = pointsA * 4;
                if (pointsA) steps.push_back(w);
                widthsMade = true;
            }
            if (widthsMade && pointCount_ > pointsA) {
                Step w{Kernel::Count, Binding{}, 0, true, -3};
                w.copyFrom = branchWidths ? auxBuffer(k, 1) : static_cast<int>(kBaseWidths);
                w.copyTo = kWidths;
                w.copyOffset = pointsA * 4;
                if (branchWidths) w.copyFromOffset = 0;
                w.copyBytes = (pointCount_ - pointsA) * 4;
                steps.push_back(w);
            }
            resultOf[k] = current = out;
            continue;
        }        const int input2 = p.stage.kind == StageKind::Blend || p.stage.from2 != kNoStage ? taken(p.stage.from2) : -1;
        const int to = k == lastMover ? finalOut : freeBuffer(k, input, input2);
        if (to != kOut0) allocate(to, bytes);
        if (p.stage.kind == StageKind::SelfCollide || p.stage.kind == StageKind::Static) {
            if (k >= r.inputs.size() || !r.inputs[k].grid || !r.inputs[k].grid->layout) {
                throw std::runtime_error("GPU preview: the hair grid's layout missing");
            }
            gridSteps(k, input, to, widthsMade);
            mixAffect(k, to, -1);
            resultOf[k] = current = to;
            continue;
        }
        if (p.stage.kind == StageKind::Tie) {
            // Its frame and ring's bins (one group), each strand's route over the scalp, then each
            // strand laid along its path (look.slang TieFrameCS, TieRouteCS, TieLayCS). Aux 2: the
            // frame; 3: the routes; 4: the tail it writes (a Tie after it reads it there).
            if (k >= r.inputs.size() || !r.inputs[k].tie) throw std::runtime_error("GPU preview: Tie's inputs missing");
            const TieInputs& tie = *r.inputs[k].tie;
            const int channelIn = tie.channelFrom != kNoStage ? auxBuffer(tie.channelFrom, 4) : tie.channel ? auxBuffer(k, 5) : -1;
            // Its tail written where a Tie after it in the run reads it (nothing else here does:
            // the drawing reads none; a stage mapping a param to it is the CPU's).
            bool read = false;
            for (std::size_t j = k + 1; j < stages.size(); ++j) {
                read = read || (stages[j].stage.kind == StageKind::Tie && j < r.inputs.size() && r.inputs[j].tie &&
                                r.inputs[j].tie->channelFrom == k);
                read = read || (j < r.fields.stages.size() && r.fields.stages[j].affect.channelFrom == k);  // (an affect mapped to it)
            }
            const bool writes = p.flag[0] != 0 && (read || r.tailsOut);
            if (!writes) c.flag[0] &= ~kTieWritesTail;
            allocate(auxBuffer(k, 2), kTieFrameWords * 4);
            allocate(auxBuffer(k, 3), std::max<std::size_t>(n * kTieSlots * 16, 16));
            if (writes) allocate(auxBuffer(k, 4), std::max<std::size_t>(pointCount_ * 4, 4));
            Binding frame;
            frame.in = input;
            frame.aux[0] = auxBuffer(k, 1);
            frame.aux[1] = channelIn;
            frame.out[0] = auxBuffer(k, 2);
            slots.push_back(c);
            steps.push_back({Kernel::TieFrame, frame, 1, false, -3});
            Binding route;
            route.in = input;
            route.aux[0] = auxBuffer(k, 1);
            route.aux[1] = auxBuffer(k, 2);
            route.aux[2] = auxBuffer(k, 0);
            route.out[0] = auxBuffer(k, 3);
            route.out[1] = channelIn;  // (read alone)
            slots.push_back(c);
            steps.push_back({Kernel::TieRoute, route, strandGroups, false, -3});
            Binding lay;
            lay.in = input;
            lay.aux[0] = auxBuffer(k, 3);
            lay.aux[1] = auxBuffer(k, 2);
            lay.aux[2] = auxBuffer(k, 0);
            lay.out[0] = to;
            lay.out[1] = writes ? auxBuffer(k, 4) : -1;
            lay.out[2] = channelIn;  // (read alone)
            slots.push_back(c);
            steps.push_back({Kernel::TieLay, lay, strandGroups, false, static_cast<int>(StageKind::Tie)});
            resultOf[k] = current = to;
            continue;
        }
        Binding b;
        b.in = input;
        b.out[0] = to;
        Kernel kernel = Kernel::Noise;
        std::size_t groups = strandGroups;
        switch (p.stage.kind) {
            case StageKind::Interpolate:
                kernel = Kernel::Interpolate;
                // Four strands a group, two where the guides have more than 32 samples (look.slang
                // InterpolateCS); on the CPU a thread a strand, INTERP_GROUP (64) strands a group.
                groups = sharesBase() ? (n + 63) / 64 : p.flag[0] > 32 ? (n + 1) / 2 : (n + 3) / 4;
                b.aux[0] = auxBuffer(k, 0);
                b.aux[1] = auxBuffer(k, 1);
                b.aux[2] = r.inputs[k].interpolate && r.inputs[k].interpolate->clearance ? auxBuffer(k, 2) : -1;
                if (needInfluence) b.out[2] = kInfluence;
                break;
            case StageKind::Length: kernel = Kernel::Length; break;
            case StageKind::Clump: {
                kernel = Kernel::Clump;
                const ClumpInputs& in = *r.inputs[k].clump;
                const bool made = !in.curves;
                // Frames for a lock radius or a flatness.
                const bool framed = p.value[3] > 0.0f || p.value[2] > 0.0f;
                // copyAmount on centers made here: resampled into a buffer of their own, then shared.
                const bool share = made && in.neighbors && p.value2[2] > 0.0f && in.centers > 1;
                const std::size_t centerGroups = (in.centers + 63) / 64;
                if (in.centers > 0 && (made || !held_[k].framesMade)) {
                    // The centers' curves made from the points so far (the Clump's own strands), and
                    // with a lock radius or a flatness their frames; their arc lengths where the
                    // curves are final (not shared after).
                    StageConstants centers = c;
                    centers.flag[0] = static_cast<std::uint32_t>(in.centers);
                    centers.flag[1] = framed && !share ? 1u : 0u;
                    centers.flag[2] = made ? 0u : 1u;  // 1: the curves are there, the frames alone
                    centers.flag[3] = share ? 0u : 1u;  // the arcs
                    Binding cb;
                    cb.in = input;
                    if (made && in.centersRun) {  // the curves of the centers' branch, as its run left them
                        cb.in = auxBuffer(k, 6);
                        cb.offsets = auxBuffer(k, 7);
                    }
                    cb.out[0] = share ? auxBuffer(k, 5) : auxBuffer(k, 1);
                    cb.aux[0] = made ? auxBuffer(k, 2) : -1;
                    slots.push_back(centers);
                    steps.push_back({Kernel::Centers, cb, centerGroups, false, -3});
                    if (!made) held_[k].framesMade = true;
                }
                if (share) {
                    StageConstants shared = c;
                    shared.flag[0] = static_cast<std::uint32_t>(in.centers);
                    Binding sb;
                    sb.in = auxBuffer(k, 5);
                    sb.aux[0] = auxBuffer(k, 4);
                    sb.out[0] = auxBuffer(k, 1);
                    slots.push_back(shared);
                    steps.push_back({Kernel::Share, sb, centerGroups, false, -3});
                    {
                        StageConstants framesOnly = c;  // (and the arcs, on the shared curves)
                        framesOnly.flag[0] = static_cast<std::uint32_t>(in.centers);
                        framesOnly.flag[1] = framed ? 1u : 0u;
                        framesOnly.flag[2] = 1u;
                        framesOnly.flag[3] = 1u;
                        Binding fb;
                        fb.out[0] = auxBuffer(k, 1);
                        slots.push_back(framesOnly);
                        steps.push_back({Kernel::Centers, fb, centerGroups, false, -3});
                    }
                }
                b.aux[0] = auxBuffer(k, 0);
                b.aux[1] = auxBuffer(k, 1);
                break;
            }
            case StageKind::Part:
                kernel = Kernel::Part;
                b.aux[0] = auxBuffer(k, 0);
                break;
            case StageKind::Gravity: kernel = Kernel::Gravity; break;
            case StageKind::Flyaway: kernel = Kernel::Flyaway; break;
            case StageKind::Wind: kernel = Kernel::Wind; break;
            case StageKind::Blend:
                kernel = perPoint() ? Kernel::BlendPoints : Kernel::Blend;
                groups = perPoint() ? pointGroups : strandGroups;
                b.aux[0] = input2;  // look b's points (look a's: its input)
                break;
            case StageKind::Collide:
                kernel = Kernel::Collide;
                c.flag[2] = r.inputs[k].collide ? r.inputs[k].collide->colliders : 0u;  // (look.slang collideWalk)
                if (p.value2[1] > 0.0f) {  // smoothing: whether the collider moved each strand, for the second dispatch
                    b.out[1] = auxBuffer(k, 1);
                    allocate(b.out[1], std::max<std::size_t>(n * 4, 4));
                }
                b.aux[2] = auxBuffer(k, 0);  // the fields' tables, read as the clearance walk reads its
                break;
            case StageKind::Noise: {
                // Kinky in world space walks each strand's arc length: a thread a strand.
                const bool byPoint = perPoint() && !(p.flag[1] != 0 && p.flag[2] != 0);
                kernel = byPoint ? Kernel::NoisePoints : Kernel::Noise;
                groups = byPoint ? pointGroups : strandGroups;
                break;
            }
            case StageKind::Curl: {
                kernel = Kernel::Curl;
                if (sharesBase()) groups = (n + 255) / 256;  // (the CPU's: four strands a thread, look.slang curlFour)
                const CurlInputs* in = p.flag[2] != 0 && k < r.inputs.size() ? r.inputs[k].curl.get() : nullptr;
                if (p.flag[2] != 0 && !in) throw std::runtime_error("GPU preview: Curl's clumps missing");
                if (p.flag[0] == 3 && widthsMade) {
                    // A coil by the widths a Width before it made in this run (look.slang CurlCS).
                    if (in) throw std::runtime_error("GPU preview: a coil by clump on widths the run makes");
                    c.flag[3] = 1u;
                    b.aux[0] = kWidths;
                }
                if (in) {
                    // By clump: each clump's middle from its core strands, then each strand curled with it.
                    if (in->count > 0) {
                        StageConstants axes = c;
                        axes.flag[0] = static_cast<std::uint32_t>(in->count);
                        Binding ab;
                        ab.in = input;
                        ab.aux[0] = auxBuffer(k, 1);
                        ab.aux[1] = auxBuffer(k, 2);
                        ab.out[0] = auxBuffer(k, 3);
                        slots.push_back(axes);
                        steps.push_back({Kernel::CurlAxes, ab, (in->count + 1) / 2, false, -3});  // two clumps a group (look.slang CurlAxesCS)
                    }
                    b.aux[0] = auxBuffer(k, 0);
                    b.aux[1] = auxBuffer(k, 3);
                    b.aux[2] = auxBuffer(k, 2);
                }
                break;
            }
            // ---- The shapers working on each strand alone ----
            case StageKind::Bend: kernel = Kernel::Bend; b.aux[0] = auxBuffer(k, 0); break;
            case StageKind::Direction: kernel = Kernel::Direction; b.aux[0] = auxBuffer(k, 0); break;
            case StageKind::Transform: kernel = Kernel::Transform; b.aux[0] = auxBuffer(k, 0); break;
            case StageKind::Displace: kernel = Kernel::Displace; b.aux[0] = auxBuffer(k, 0); break;
            case StageKind::Twist: {
                kernel = Kernel::Twist;
                const TwistInputs& in = *r.inputs[k].twist;
                c.flag[0] = in.axes ? 1u : 0u;
                c.flag[1] = static_cast<std::uint32_t>(in.count);
                c.value2[0] = 1.0f / static_cast<float>(kTwistAxisSamples - 1);  // (TwistOp's dt)
                if (!in.axes && in.count > 0) {
                    // Each clump's middle from its core strands (look.slang TwistAxesCS), two a group.
                    Binding ab;
                    ab.in = input;
                    ab.aux[0] = auxBuffer(k, 2);
                    ab.out[0] = auxBuffer(k, 1);
                    slots.push_back(c);
                    steps.push_back({Kernel::TwistAxes, ab, (in.count + 1) / 2, false, -3});
                }
                b.aux[0] = auxBuffer(k, 0);
                b.aux[1] = auxBuffer(k, 1);
                break;
            }
            case StageKind::Width:
            case StageKind::SelfCollide:
            case StageKind::Static:
            case StageKind::Tie: break;  // (made above)
            case StageKind::Smooth:
            case StageKind::RestoreLength:
            case StageKind::BlendNeighbors:
            case StageKind::Tangle:
            case StageKind::Flow: kernel = shaperKernel(k, p, k < r.inputs.size() ? &r.inputs[k] : nullptr, input2, &b, &groups, &c); break;
            // ---- Jiggle and MeshCut ----
            case StageKind::Jiggle:
                // Aux 0: each strand's root lag and face; 1: each face's turn (look.slang JiggleCS).
                kernel = Kernel::Jiggle;
                b.aux[0] = auxBuffer(k, 0);
                b.aux[1] = auxBuffer(k, 1);
                if (sharesBase()) b.aux[2] = auxBuffer(k, 2);  // (the turns in double)
                break;
            case StageKind::MeshCut:
                // Aux 0: its mesh's tree (ScalpMesh::rayTree; none: the strands pass through).
                kernel = Kernel::MeshCut;
                b.aux[0] = auxBuffer(k, 0);
                c.flag[1] = k < r.inputs.size() && r.inputs[k].meshCut && r.inputs[k].meshCut->tree ? 1u : 0u;
                break;
        }
        slots.push_back(c);
        // Collide smoothing (value2 1): a second dispatch on the first's points (look.slang CollideCS,
        // flag 1), the stage's time stamped after it.
        const bool collideTwice = kernel == Kernel::Collide && p.value2[1] > 0.0f;
        steps.push_back({kernel, b, groups, false, collideTwice ? -3 : static_cast<int>(p.stage.kind)});
        if (collideTwice) {
            StageConstants second = c;
            second.flag[1] = 1u;
            Binding again = b;
            again.out[1] = -1;
            again.aux[0] = auxBuffer(k, 1);  // the first's word a strand
            slots.push_back(second);
            steps.push_back({kernel, again, groups, false, static_cast<int>(p.stage.kind)});
        }
        if (kernel == Kernel::Interpolate && c.flag[3] != 0) {
            // Keeping clearance: the walk, on the points Interpolate wrote (in place), with its
            // constants.
            // Every strand's walked points kept (aux 4) for the next run, which walks only the
            // strands bound to the guides it changed (flags beside the guides' rows, aux 1). (Not
            // for an operator's own run, each walking afresh: flag2.w 1, no room.)
            const bool keeps = !targets.baseAsOutput;
            Binding walk;
            walk.out[0] = to;
            walk.out[2] = auxBuffer(k, 4);
            allocate(walk.out[2], keeps ? bytes : 4);
            walk.aux[0] = b.aux[0];
            walk.aux[1] = b.aux[1];
            walk.aux[2] = b.aux[2];
            c.flag2[1] = keeps && held_[k].walkPartial ? 1u : 0u;
            c.flag2[3] = keeps ? 0u : 1u;
            if (keeps) walkedStages.push_back(k);
            slots.push_back(c);
            steps.push_back({Kernel::Clearance, walk, strandGroups, false, kClearanceStamp});
        }
        mixAffect(k, to, -1);
        resultOf[k] = current = to;
    }
    inPlace_ = bindTargets(targets, frames, outBytes, outPoints_ * 8);
    if (current == kBasePoints && !gather && !targets.baseAsOutput) {  // no stage moves the points: the output is the base
        Step copy{Kernel::Count, Binding{}, 0, true, -3};
        steps.push_back(copy);
        current = kOut0;
    }
    allPoints_ = current;  // every strand's points (before the shown ones are gathered)
    if (frames) {
        StageConstants c{};
        c.strandCount = static_cast<std::uint32_t>(n);
        c.uniformCount = static_cast<std::uint32_t>(uniformCount_);
        c.pointCount = static_cast<std::uint32_t>(pointCount_);
        c.table[1] = 0;
        Binding b;
        b.in = current;
        for (int i = 0; i < 3; ++i) {
            b.out[i] = gather ? kFull1 + i : kOut1 + i;
            if (gather) allocate(b.out[i], bytes);
        }
        slots.push_back(c);
        steps.push_back({perPoint() ? Kernel::LinesPoints : Kernel::Lines, b, perPoint() ? pointGroups : strandGroups, false, -1});
    }
    if (gather) {
        // The shown strands' points (and frames) into the outputs: three streams a pass.
        StageConstants c{};
        c.strandCount = static_cast<std::uint32_t>(n);
        c.pointCount = static_cast<std::uint32_t>(outPoints_);
        c.flag[0] = frames ? 1u : 0u;
        Binding b;
        b.in = current;
        b.aux[0] = kShown;
        if (frames) {
            b.aux[1] = kFull1;
            b.aux[2] = kFull2;
            b.out[1] = kOut1;
            b.out[2] = kOut2;
        }
        b.out[0] = kOut0;
        const std::size_t groups = (outPoints_ + 255) / 256;
        slots.push_back(c);
        steps.push_back({Kernel::Gather, b, groups, false, -2});
        if (frames) {
            c.flag[0] = 0;
            Binding last;
            last.in = kFull3;
            last.aux[0] = kShown;
            last.out[0] = kOut3;
            slots.push_back(c);
            steps.push_back({Kernel::Gather, last, groups, false, -2});
        }
        current = kOut0;
    }
    const bool widthsRun = runsWidth(r.params);  // (a Width's, or a Merge's input b's run's)
    // The lines' uvs: Width's widths into each shown point's second float (the shown strands'
    // places among the run's points where it holds hidden ones).
    uvsWritten_ = widthsRun && targets.uvs && has(kOutUvs);
    if (uvsWritten_) {
        StageConstants c{};
        c.strandCount = static_cast<std::uint32_t>(n);
        c.pointCount = static_cast<std::uint32_t>(outPoints_);
        c.flag[0] = gather ? 1u : 0u;
        Binding b;
        b.in = kWidths;
        b.aux[0] = gather ? static_cast<int>(kShown) : -1;
        b.out[0] = kOutUvs;
        slots.push_back(c);
        steps.push_back({Kernel::UvWidths, b, (outPoints_ + 255) / 256, false, -2});
    }
    if (targets.pointTexture) {
        // The app's point texture from the final points: the widths Width's where it ran, else
        // the base's.
        if (targets.textureWidth == 0) throw std::runtime_error("GPU preview: the point texture's width is 0");
        StageConstants c{};
        c.strandCount = static_cast<std::uint32_t>(n);
        c.uniformCount = static_cast<std::uint32_t>(uniformCount_);
        c.pointCount = static_cast<std::uint32_t>(pointCount_);
        c.flag[0] = targets.textureWidth;
        Binding b;
        b.in = current;
        b.aux[0] = widthsRun ? static_cast<int>(kWidths) : static_cast<int>(kBaseWidths);
        b.texture = true;
        slots.push_back(c);
        steps.push_back({perPoint() ? Kernel::PointsTexturePoints : Kernel::PointsTexture, b, perPoint() ? pointGroups : strandGroups, false, -4});
    }

    // Each dispatch's shape: rows of at most dispatchLimit_ groups (the kernels' threadIndex).
    struct Shape {
        std::size_t x = 0, y = 0;
    };
    std::vector<Shape> shapes;
    std::vector<std::size_t> slotOf;  // per step: its constants' slot (a repeated one's first of two)
    std::size_t nextSlot = 0;
    for (const Step& s : steps) {
        Shape shape;
        slotOf.push_back(nextSlot);
        if (s.copy) {
            shapes.push_back(shape);
            continue;
        }
        if (s.groups) {
            shape.x = std::min(s.groups, dispatchLimit_);
            shape.y = (s.groups + shape.x - 1) / shape.x;
            if (shape.y > kMaxGroups) throw std::runtime_error("GPU preview: more points than a dispatch takes");
        }
        const std::size_t used = s.repeat ? 2 : 1;
        for (std::size_t u = 0; u < used; ++u) slots[nextSlot + u].dispatch[0] = static_cast<std::uint32_t>(shape.x);
        nextSlot += used;
        shapes.push_back(shape);
    }
    if (nextSlot != slots.size()) throw std::runtime_error("GPU preview: the dispatches' constants are out of step");

    if (view_) {
        for (StageConstants& c : slots) c.table2[3] = 1u;  // (look.slang strandBegin: a view's two words a strand)
    }
    beginRun();
    setConstants(slots.data(), slots.size(), sizeof(StageConstants));
    stamped_.clear();
    std::size_t stamp = 0;
    this->stamp(stamp++);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const Step& s = steps[i];
        if (s.copy) {
            copy(s.copyFrom, s.copyTo, s.copyBytes ? s.copyBytes : bytes, s.copyFromOffset == kSameOffset ? s.copyOffset : s.copyFromOffset,
                 s.copyOffset);
            continue;
        }
        if (s.onlyIf >= 0 && !runsIf(s.onlyIf, s.onlyIfValue)) {
            // (skipped)
        } else if (s.countBuffer >= 0) {
            // As few groups as the count asks (rows of the dispatch's own width, its gDispatch.x).
            for (std::size_t r = 0; r < std::max<std::size_t>(s.repeat, 1); ++r) {
                const std::size_t odd = s.repeat ? (r & 1) : 0;
                const std::size_t groups = countedGroups(s.countBuffer, s.countWord + static_cast<std::uint32_t>(odd), s.countPer, s.groups);
                if (!groups) continue;
                const std::size_t x = std::min(groups, shapes[i].x);
                dispatch(s.kernel, slotOf[i] + odd, s.binding, x, (groups + x - 1) / x);
            }
        } else if (s.repeat) {
            dispatchAlternating(s.kernel, slotOf[i], s.binding, shapes[i].x, shapes[i].y, s.repeat);
        } else {
            dispatch(s.kernel, slotOf[i], s.binding, shapes[i].x, shapes[i].y);
        }
        if (s.stampKind != -3) {
            stamped_.push_back({s.stampKind, stamp - 1, stamp});
            this->stamp(stamp++);
        }
    }
    if (!inPlace_) copyToTargets(frames, outBytes);
    if (!steps.empty() && steps.front().kernel == Kernel::PointStrands) pointStrandsKey_ = baseSerial_;
    stamped_.push_back({-2, stamp - 1, stamp});
    this->stamp(stamp++);
    endRun(stamp);
    for (const std::size_t k : walkedStages) held_[k].walked = true;
    stampCount_ = stamp;
    stampsPending_ = true;
    times_ = GpuTimes{};
    resultBuffer_ = kOut0;
    framesRun_ = frames;
    widthsRun_ = widthsRun;
}

GpuTimes LookEngine::times(bool wait) {
    if (!stampsPending_) return times_;
    std::vector<std::uint64_t> ticks;
    double frequency = 0;
    if (!stampTicks(wait, &ticks, &frequency)) return GpuTimes{};
    stampsPending_ = false;
    if (frequency <= 0 || ticks.size() < stampCount_ || stampCount_ == 0) return times_ = GpuTimes{};
    GpuTimes g;
    g.valid = true;
    auto ms = [&](std::size_t a, std::size_t b) { return 1000.0 * static_cast<double>(ticks[b] - ticks[a]) / frequency; };
    for (const Stamped& s : stamped_) {
        const double t = ms(s.from, s.to);
        switch (s.kind) {
            case static_cast<int>(StageKind::Interpolate): g.interpolateMs += t; break;
            case static_cast<int>(StageKind::Length): g.lengthMs += t; break;
            case static_cast<int>(StageKind::Clump): g.clumpMs += t; break;
            case static_cast<int>(StageKind::Noise): g.noiseMs += t; break;
            case static_cast<int>(StageKind::Curl): g.curlMs += t; break;
            case static_cast<int>(StageKind::Width): g.widthMs += t; break;
            case static_cast<int>(StageKind::Part): g.partMs += t; break;
            case static_cast<int>(StageKind::Gravity): g.gravityMs += t; break;
            case static_cast<int>(StageKind::Flyaway): g.flyawayMs += t; break;
            case static_cast<int>(StageKind::Collide): g.collideMs += t; break;
            case static_cast<int>(StageKind::Wind): g.windMs += t; break;
            case static_cast<int>(StageKind::Blend): g.blendMs += t; break;
            case static_cast<int>(StageKind::SelfCollide): g.selfCollideMs += t; break;
            case static_cast<int>(StageKind::Static): g.staticMs += t; break;
            case static_cast<int>(StageKind::Tie): g.tieMs += t; break;
            case static_cast<int>(StageKind::Bend): g.bendMs += t; break;
            case static_cast<int>(StageKind::Direction): g.directionMs += t; break;
            case static_cast<int>(StageKind::Transform): g.transformMs += t; break;
            case static_cast<int>(StageKind::Displace): g.displaceMs += t; break;
            case static_cast<int>(StageKind::Twist): g.twistMs += t; break;
            case static_cast<int>(StageKind::Smooth): g.smoothMs += t; break;
            case static_cast<int>(StageKind::RestoreLength): g.restoreLengthMs += t; break;
            case static_cast<int>(StageKind::BlendNeighbors): g.blendNeighborsMs += t; break;
            case static_cast<int>(StageKind::Tangle): g.tangleMs += t; break;
            case static_cast<int>(StageKind::Flow): g.flowMs += t; break;
            case static_cast<int>(StageKind::Jiggle): g.jiggleMs += t; break;
            case static_cast<int>(StageKind::MeshCut): g.meshCutMs += t; break;
            case static_cast<int>(StageKind::Cull):
            case static_cast<int>(StageKind::Split):
            case static_cast<int>(StageKind::Mirror): g.layoutMs += t; break;
            case -1: g.linesMs += t; break;
            case -2: g.copyMs += t; break;
            case -4: g.textureMs += t; break;
            case kClearanceStamp: g.clearanceMs += t; break;
            default: break;
        }
    }
    g.totalMs = ms(0, stampCount_ - 1);
    // The Clumps' centers' branches, run before (their nodes' times with this run's own kinds).
    for (const std::unique_ptr<LookEngine>& centers : centers_) {
        if (!centers) continue;
        const GpuTimes c = centers->times(wait);
        if (!c.valid) continue;
        g.interpolateMs += c.interpolateMs;
        g.clearanceMs += c.clearanceMs;
        g.lengthMs += c.lengthMs;
        g.clumpMs += c.clumpMs;
        g.noiseMs += c.noiseMs;
        g.curlMs += c.curlMs;
        g.widthMs += c.widthMs;
        g.partMs += c.partMs;
        g.gravityMs += c.gravityMs;
        g.flyawayMs += c.flyawayMs;
        g.collideMs += c.collideMs;
        g.windMs += c.windMs;
        g.blendMs += c.blendMs;
        g.selfCollideMs += c.selfCollideMs;
        g.staticMs += c.staticMs;
        g.tieMs += c.tieMs;
        g.bendMs += c.bendMs;
        g.directionMs += c.directionMs;
        g.transformMs += c.transformMs;
        g.displaceMs += c.displaceMs;
        g.twistMs += c.twistMs;
        g.smoothMs += c.smoothMs;
        g.restoreLengthMs += c.restoreLengthMs;
        g.blendNeighborsMs += c.blendNeighborsMs;
        g.tangleMs += c.tangleMs;
        g.flowMs += c.flowMs;
        g.jiggleMs += c.jiggleMs;
        g.meshCutMs += c.meshCutMs;
        g.layoutMs += c.layoutMs;
        g.copyMs += c.copyMs;
        g.totalMs += c.totalMs;
    }
    return times_ = g;
}

std::vector<float> LookEngine::readTail(std::size_t k) {
    std::vector<float> out;
    const int id = auxBuffer(k, 4);
    if (!has(id)) return out;
    const std::vector<std::uint8_t> bytes = readBack(id, pointCount_ * 4);
    out.resize(pointCount_);
    if (!out.empty() && bytes.size() == pointCount_ * 4) std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

std::vector<Vec3f> LookEngine::readPositions() {
    std::vector<Vec3f> out;
    if (resultBuffer_ < 0 || !has(resultBuffer_)) return out;
    const std::vector<std::uint8_t> bytes = readBack(resultBuffer_, outPoints_ * 12);
    out.resize(outPoints_);
    if (!out.empty() && bytes.size() == outPoints_ * 12) std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

std::vector<float> LookEngine::readWidths() {
    std::vector<float> out(pointCount_);
    if (!widthsRun_ || !has(kWidths)) return std::vector<float>(outPoints_);
    const std::vector<std::uint8_t> bytes = readBack(kWidths, pointCount_ * 4);
    if (!out.empty() && bytes.size() == pointCount_ * 4) std::memcpy(out.data(), bytes.data(), bytes.size());
    if (shownPoints_.empty()) return out;
    std::vector<float> shown(shownPoints_.size());  // the shown strands' (as the outputs hold them)
    for (std::size_t p = 0; p < shown.size(); ++p) shown[p] = out[shownPoints_[p]];
    return shown;
}

void LookEngine::readFrames(std::vector<Vec3f>* normals, std::vector<Vec3f>* tangents, std::vector<Vec3f>* bitangents) {
    std::vector<Vec3f>* to[3] = {normals, tangents, bitangents};
    for (int i = 0; i < 3; ++i) {
        if (!to[i]) continue;
        to[i]->clear();
        if (!framesRun_ || !has(kOut1 + i)) continue;
        const std::vector<std::uint8_t> bytes = readBack(kOut1 + i, outPoints_ * 12);
        to[i]->resize(outPoints_);
        if (bytes.size() == outPoints_ * 12 && outPoints_) std::memcpy(to[i]->data(), bytes.data(), bytes.size());
    }
}

}  // namespace groomlab::gpu
