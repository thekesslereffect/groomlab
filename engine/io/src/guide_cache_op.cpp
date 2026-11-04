// SPDX-License-Identifier: Apache-2.0
// GuideCache: the guides posed from an Alembic cache of simulated guide curves (another package's
// simulation: Houdini, nHair, a Simulate bake).
//
// Export Guides for Simulation writes a guide group as Alembic curves, each with its guide's ID as
// a per-curve strandId; the curves are simulated elsewhere (keeping strandId) and this node lays
// each curve of the cache, at the evaluation time, on the guide of the same ID it is fed: the
// guides keep their roots' attachment, painted layers and groups, only their points move (as the
// Simulate node lays its frames). A curve of another point count is resampled along its length to
// the guide's; guides with no curve in the cache pass through. By order instead of ID for a cache
// whose curves lost strandId (the curves in the order the guides come). The cache key includes the
// file's size and modification time, so rewriting the file re-reads it.

#include <algorithm>
#include <unordered_map>

#include "groomlab/alembic_io.h"
#include "groomlab/alembic_operators.h"
#include "groomlab/curve.h"
#include "groomlab/graph.h"
#include "groomlab/paths.h"
#include "groomlab/random.h"
#include "groomlab/texture.h"

namespace groomlab {

namespace {

class GuideCacheOp : public Operator {
public:
    std::string typeName() const override { return op::kGuideCache; }
    std::string category() const override { return "motion"; }
    std::vector<PortSpec> inputs() const override { return {{"guides", PortType::Guides}}; }
    PortSpec output() const override { return {"guides", PortType::Guides}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"path", std::string(), "Alembic file of the simulated guide curves (Export Guides for Simulation writes the "
                                    "guides to simulate; keep each curve's strandId)"},
            {"object", std::string(), "Only curves under this object name (empty: all)"},
            notAnimatable({"matchBy", std::string("id"),
                           "id: each curve poses the guide its strandId names; order: the curves pose the guides in "
                           "the order they come (a cache that lost strandId)",
                           "", {"id", "order"}}),
            notAnimatable({"frameOffset", 0.0, "Frames added to the scene's frame before reading the cache (a cache "
                                               "that starts later or earlier than the shot)"})
                .soft(-100, 100),
            ParamSpec{"blend", 1.0, "How much of the cache's pose: 1 the cache, 0 the guides as they come in"}.range(0, 1),
        };
    }

    // The curves are read at the evaluation time (and its offset): the context field it depends on.
    std::uint64_t contextSalt(const EvalContext& ctx) const override { return hashFloat(hash64(0x6763), static_cast<float>(ctx.time)); }
    std::uint64_t cacheSalt() const override {
        const std::string path = resolvePath(params().getString("path"));
        return path.empty() ? 0 : fileStamp(path);
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const GuidesPtr in = inputs.empty() ? nullptr : valueAs<GuidesPtr>(inputs[0]);
        if (!in) return GuidesPtr{};  // nothing fed: no guides, as an empty Guides node gives
        const std::string path = resolvePath(params().getString("path"));
        if (path.empty()) {
            ctx.note("No cache file: the guides pass through");
            return in;
        }
        const double fps = ctx.framesPerSecond > 0.0 ? ctx.framesPerSecond : 24.0;
        const double seconds = ctx.time + params().getDouble("frameOffset") / fps;
        StrandSet curves;
        try {
            curves = readAlembicCurvesAt(path, seconds, params().getString("object"));
        } catch (const AlembicError& e) {
            throw GraphError(std::string("GuideCache: ") + e.what());
        }

        const bool byOrder = params().getString("matchBy") == "order";
        std::unordered_map<std::uint64_t, std::size_t> byId;
        if (!byOrder) {
            byId.reserve(curves.strandCount());
            for (std::size_t s = 0; s < curves.strandCount(); ++s) byId.emplace(curves.ids()[s], s);
        }
        const float blend = static_cast<float>(std::clamp(params().getDouble("blend"), 0.0, 1.0));
        auto out = std::make_shared<GuideSet>(*in);
        std::size_t posed = 0;
        for (std::size_t g = 0; g < out->guides().size(); ++g) {
            Guide& guide = out->guides()[g];
            std::size_t s = 0;
            if (byOrder) {
                if (g >= curves.strandCount()) break;
                s = g;
            } else {
                const auto it = byId.find(guide.id);
                if (it == byId.end()) continue;
                s = it->second;
            }
            const std::size_t n = curves.strandPointCount(s);
            const std::size_t want = guide.points.size();
            if (n < 2 || want < 2) continue;
            const Vec3f* pts = curves.strandPoints(s);
            std::vector<Vec3f> pose = n == want ? std::vector<Vec3f>(pts, pts + n) : resampleByArcLength(pts, n, want);
            if (blend < 1.0f) {
                for (std::size_t k = 0; k < want; ++k) pose[k] = guide.points[k] + (pose[k] - guide.points[k]) * blend;
            }
            guide.points = std::move(pose);
            ++posed;
        }
        if (posed == 0 && !out->empty()) {
            ctx.note(byOrder ? std::string("The cache has no curves: the guides pass through")
                             : std::string("No curve in the cache has a guide's ID (strandId): the guides pass through "
                                           "(matchBy order poses them by order)"));
        } else if (posed < out->size()) {
            ctx.note(std::to_string(posed) + " of " + std::to_string(out->size()) +
                     " guides posed by the cache; the others pass through");
        }
        return GuidesPtr(out);
    }
};

}  // namespace

void registerGuideCacheOperator(OperatorRegistry& registry) {
    registry.registerType(op::kGuideCache,
                          {"motion",
                           "The guides posed from an Alembic cache of simulated guide curves (Houdini, nHair...), matched "
                           "by guide ID: the groom stays, only the guides move.",
                           "cache simulation houdini alembic abc animated guides import sim"},
                          [] { return std::make_unique<GuideCacheOp>(); });
}

}  // namespace groomlab
