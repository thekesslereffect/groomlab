// SPDX-License-Identifier: Apache-2.0
// ImportCurves: guides from an Alembic file (GH-8).
//
// Reads every curve in the file (or under one object) at the evaluation time,
// resamples each by arc length, and, when a scalp is connected, flips curves
// drawn tip-first and roots them on the scalp. The cache key includes the
// file's size and modification time so rewriting the file re-reads it.

#include <algorithm>

#include "groomlab/alembic_io.h"
#include "groomlab/alembic_operators.h"
#include "groomlab/curve.h"
#include "groomlab/graph.h"
#include "groomlab/paths.h"
#include "groomlab/random.h"
#include "groomlab/scalp_pieces.h"
#include "groomlab/texture.h"

namespace groomlab {

namespace {

class ImportCurvesOp : public Operator {
public:
    std::string typeName() const override { return op::kImportCurves; }
    std::string category() const override { return "source"; }
    std::vector<PortSpec> inputs() const override { return {{"scalp", PortType::Mesh, true}}; }
    PortSpec output() const override { return {"guides", PortType::Guides}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"path", std::string(), "Alembic file with guide curves"},
            {"object", std::string(), "Only curves under this object name (empty: all)"},
            notAnimatable({"samples", std::int64_t{24}, "Points per guide after resampling (0: keep as read)"})
                .range(0, 1024)
                .soft(0, 64),
        };
    }

    // The curves are read at the evaluation time: the one context field this operator depends on.
    std::uint64_t contextSalt(const EvalContext& ctx) const override { return hashFloat(hash64(0x71), static_cast<float>(ctx.time)); }
    std::uint64_t cacheSalt() const override {
        const std::string path = resolvePath(params().getString("path"));
        return path.empty() ? 0 : fileStamp(path);
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const std::string path = resolvePath(params().getString("path"));
        if (path.empty()) return GuidesPtr{};  // unset: behaves like no guides

        StrandSet curves;
        try {
            curves = readAlembicCurvesAt(path, ctx.time, params().getString("object"));
        } catch (const AlembicError& e) {
            throw GraphError(std::string("ImportCurves: ") + e.what());
        }

        const auto samples = static_cast<std::size_t>(params().getInt("samples"));
        auto guides = std::make_shared<GuideSet>();
        for (std::size_t s = 0; s < curves.strandCount(); ++s) {
            const std::size_t n = curves.strandPointCount(s);
            if (n < 2) continue;
            const Vec3f* pts = curves.strandPoints(s);
            guides->add(curves.ids()[s], samples >= 2 ? resampleByArcLength(pts, n, samples)
                                                      : std::vector<Vec3f>(pts, pts + n));
        }

        if (const MeshPtr scalp = valueAs<MeshPtr>(inputs.at(0))) {
            guides->orientAndRoot(*scalp);
            // Rooted on one of several scalps (scalp_pieces.h): roots on the whole mesh, as every root.
            for (Guide& g : guides->guides()) g.root = toWholeRoot(*scalp, g.root);
        }
        return GuidesPtr(guides);
    }
};

}  // namespace

void registerAlembicOperators(OperatorRegistry& registry) {
    registry.registerType(op::kImportCurves,
                          {"source", "Curves from an Alembic file, as strands or guides.", "alembic abc curves file"},
                          [] { return std::make_unique<ImportCurvesOp>(); });
    registerGuideCacheOperator(registry);
}

}  // namespace groomlab
