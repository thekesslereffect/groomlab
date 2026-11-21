// SPDX-License-Identifier: Apache-2.0
#include "incremental.h"

#include <algorithm>
#include <atomic>
#include <string>

namespace groomlab {

namespace {

// Strands a block of relayUnlisted and sameCountsButListed takes.
constexpr std::size_t kRelayBlock = 2048;

template <class T>
void relay(T* to, const StrandSet& out, const T* from, const StrandSet& last, const std::vector<std::uint32_t>& listed) {
    const std::size_t count = out.strandCount();
    const std::vector<std::size_t>& now = out.offsets();
    const std::vector<std::size_t>& was = last.offsets();
    parallelFor(0, (count + kRelayBlock - 1) / kRelayBlock, [&](std::size_t b) {
        const std::size_t end = std::min(count, (b + 1) * kRelayBlock);
        std::size_t s = b * kRelayBlock;
        auto next = std::lower_bound(listed.begin(), listed.end(), static_cast<std::uint32_t>(s));
        while (s < end) {
            // The strands up to the next listed one (or the block's end), as one copy.
            const bool hit = next != listed.end() && *next < end;
            const std::size_t stop = hit ? *next : end;
            if (stop > s && now[stop] > now[s]) std::memcpy(to + now[s], from + was[s], (now[stop] - now[s]) * sizeof(T));
            if (hit) ++next;
            s = stop + 1;
        }
    }, 1);
}

}  // namespace

bool sameCountsButListed(const StrandSet& out, const StrandSet& last, const std::vector<std::uint32_t>& listed) {
    const std::size_t count = out.strandCount();
    if (last.strandCount() != count) return false;
    const std::vector<std::size_t>& now = out.offsets();
    const std::vector<std::size_t>& was = last.offsets();
    std::atomic<bool> same{true};
    parallelFor(0, (count + kRelayBlock - 1) / kRelayBlock, [&](std::size_t b) {
        const std::size_t end = std::min(count, (b + 1) * kRelayBlock);
        auto next = std::lower_bound(listed.begin(), listed.end(), static_cast<std::uint32_t>(b * kRelayBlock));
        for (std::size_t s = b * kRelayBlock; s < end; ++s) {
            if (next != listed.end() && *next == s) {
                ++next;
            } else if (now[s + 1] - now[s] != was[s + 1] - was[s]) {
                same.store(false, std::memory_order_relaxed);
                return;
            }
        }
    }, 1);
    return same.load();
}

void relayUnlisted(Vec3f* to, const StrandSet& out, const Vec3f* from, const StrandSet& last, const std::vector<std::uint32_t>& listed) {
    relay(to, out, from, last, listed);
}
void relayUnlisted(float* to, const StrandSet& out, const float* from, const StrandSet& last, const std::vector<std::uint32_t>& listed) {
    relay(to, out, from, last, listed);
}

bool pointsOfLast(StrandSet& out, const StrandsPtr& last, const IncrementalInputs& in, const StrandChanges& changes) {
    if (!changes.recounted) {
        positionsOfLast(out, last, in);
        return true;
    }
    if (last->positions().size() != last->offsets().back() || !sameCountsButListed(out, *last, changes.strands)) return false;
    relayUnlisted(out.overwritePositionsWithRoom().data(), out, last->positions().data(), *last, changes.strands);
    return true;
}

bool widthsOfLast(StrandSet& out, const StrandsPtr& last, const IncrementalInputs& in, const StrandChanges& changes) {
    if (!changes.recounted) {
        widthsOfLast(out, last, in);
        return true;
    }
    if (last->widths().size() != last->offsets().back() || out.widths().size() != out.pointCount() ||
        !sameCountsButListed(out, *last, changes.strands)) {
        return false;
    }
    relayUnlisted(out.overwriteWidthsWithRoom().data(), out, last->widths().data(), *last, changes.strands);
    return true;
}

std::vector<float>* pointChannelOfLast(StrandSet& out, const std::string& name, const StrandSet& last, const StrandChanges& changes) {
    if (!changes.recounted) return out.copyPointChannelFrom(name, last);
    const std::vector<float>* from = last.pointChannel(name);
    if (!from || from->size() != last.offsets().back() || !sameCountsButListed(out, last, changes.strands)) return nullptr;
    std::vector<float>& to = out.addPointChannel(name, last.pointChannelDefault(name));
    relayUnlisted(to.data(), out, from->data(), last, changes.strands);
    return &to;
}

StrandsPtr EngineStage::run(const StrandsPtr& in, const std::vector<std::uint32_t>* which, StrandSet* out) const {
    gpu::StageParamReader reader(params, "");
    gpu::StageInputs inputs;
    if (direction) {
        reader.directions = true;
        auto ways = std::make_shared<std::vector<Vec3f>>(which ? which->size() : in->strandCount());
        parallelFor(0, ways->size(), [&](std::size_t k) { (*ways)[k] = direction(which ? (*which)[k] : k); }, 4096);
        inputs.directions = std::move(ways);
    }
    if (!which) return gpu::runStage(kind, reader, in, scalp, std::move(inputs));
    gpu::runStageOnStrands(kind, reader, in, *which, scalp, *out, std::move(inputs));
    return nullptr;
}

bool rerunChanged(const StrandsPtr& in, bool local, const IncrementalInputs* incremental, Value* result, StrandChanges* changed,
                  const EngineStage& engine) {
    if (!incremental) {
        *result = engine.run(in, nullptr, nullptr);
        return true;
    }
    // (Some strands recounted before it: the last result's points laid out as the input is now.)
    const StrandChanges* changes = someStrandsChangedOrRecounted(*incremental, 0);
    const StrandsPtr last = changes ? previousStrandsFor(*incremental, *in, *changes) : nullptr;
    if (!last || !local) return false;
    if (changes->none()) {  // the same strands in: the same result
        *result = last;
        *changed = *changes;
        return true;
    }
    auto out = std::make_shared<StrandSet>(*in);
    if (!pointsOfLast(*out, last, *incremental, *changes)) return false;  // the strands left alone, as they were made
    engine.run(in, &changes->strands, out.get());
    *changed = *changes;
    *result = StrandsPtr(out);
    return true;
}

bool paramsStrandLocal(const ParamSet& params, std::initializer_list<const char*> names, const ScalpMesh* scalp, const StrandSet& in) {
    bool local = true;
    for (const char* name : names) {
        const ScalarField field(params, name, scalp);
        field.validate(in);
        local = local && field.strandLocal();
    }
    return local;
}

bool reshapeEach(const StrandsPtr& in, bool identity, bool local, const IncrementalInputs* incremental, bool otherInputsSame,
                 Value* result, StrandChanges* changed, const EngineStage& engine) {
    if (identity || in->empty()) {
        if (incremental) return false;  // the input handed on: the graph passes its changes on
        *result = in;
        return true;
    }
    return rerunChanged(in, local && otherInputsSame, incremental, result, changed, engine);
}

void writeStrands(StrandSet& out, const StrandSet& from, const std::vector<std::uint32_t>& at) {
    if (at.empty()) return;
    std::vector<Vec3f>& positions = out.mutablePositions();
    std::vector<float>& widths = out.mutableWidths();
    const bool hasWidths = widths.size() == out.pointCount() && from.widths().size() == from.pointCount();
    parallelFor(0, at.size(), [&](std::size_t k) {
        const std::size_t to = out.strandBegin(at[k]), src = from.strandBegin(k), n = from.strandPointCount(k);
        for (std::size_t i = 0; i < n; ++i) positions[to + i] = from.positions()[src + i];
        if (hasWidths) {
            for (std::size_t i = 0; i < n; ++i) widths[to + i] = from.widths()[src + i];
        }
    }, 128);
    // Each channel taken for writing only where some strand's values differ.
    for (const std::string& name : from.strandChannelNames()) {
        const std::vector<float>& values = *from.strandChannel(name);
        const std::vector<float>* have = out.strandChannel(name);
        if (!have) continue;
        bool differs = false;
        for (std::size_t k = 0; k < at.size() && !differs; ++k) {
            differs = std::memcmp(&values[k], &(*have)[at[k]], sizeof(float)) != 0;
        }
        if (!differs) continue;
        std::vector<float>& to = *out.mutableStrandChannel(name);
        for (std::size_t k = 0; k < at.size(); ++k) to[at[k]] = values[k];
    }
    for (const std::string& name : from.pointChannelNames()) {
        const std::vector<float>& values = *from.pointChannel(name);
        const std::vector<float>* have = out.pointChannel(name);
        if (!have) continue;
        std::atomic<bool> differs{false};
        parallelFor(0, at.size(), [&](std::size_t k) {
            const std::size_t n = from.strandPointCount(k);
            if (n && std::memcmp(&values[from.strandBegin(k)], &(*have)[out.strandBegin(at[k])], n * sizeof(float)) != 0) {
                differs.store(true, std::memory_order_relaxed);
            }
        }, 128);
        if (!differs.load()) continue;
        std::vector<float>& to = *out.mutablePointChannel(name);
        parallelFor(0, at.size(), [&](std::size_t k) {
            const std::size_t dst = out.strandBegin(at[k]), src = from.strandBegin(k), n = from.strandPointCount(k);
            for (std::size_t i = 0; i < n; ++i) to[dst + i] = values[src + i];
        }, 128);
    }
}

}  // namespace groomlab
