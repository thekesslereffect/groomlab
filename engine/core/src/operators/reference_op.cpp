// SPDX-License-Identifier: Apache-2.0
// Reference: another groom file's result brought into this groom (groom libraries): a look made
// once ("eyebrows.groom", "fur_patch.groom") reused, or instanced several times, in other grooms.
//
// The file is evaluated through a GroomSession of its own (every host evaluates grooms through
// the core, so Maya, the app, Blender, the CLI and the Arnold procedural agree) at this
// evaluation's preview density, point budget, time and viewport flag, and read again when it
// changes on disk (its file stamp salts the node's key). A relative path resolves as a map's
// file does (EvalContext::paths: the scene's or groom file's folder, then its search paths); the
// referenced groom's own paths resolve against its file (resolveGroomPath). Sessions are kept per
// file, for every graph and document that references it.
//
// Outputs: "strands" (the referenced groom's output) and "guides" (its guide hierarchy, posed and
// corrected, flattened to its deepest level, and its flat guides). `bind` places them in this
// groom: "world" keeps them where the file has them, unrooted (nothing binds them to this scalp;
// they do not follow it); "closestPoint" moves each strand's root to the closest point of this
// groom's scalp (or the mesh on the node's scalp input: one of several scalps), turned by the
// least rotation from the normal under it in the file to the one there; "uv" moves it to the
// point of this scalp with the UV it had on the file's scalp, turned from its frame there (the
// normal and the UV's U direction) to the same frame here: a patch on one head lands on another
// head's same spot when both share a UV layout. A root with no such UV on this scalp drops its
// strand. Rebound strands are rooted on this scalp, so they follow it like its own.

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "groomlab/frames.h"
#include "groomlab/graph.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_session.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/paths.h"
#include "groomlab/random.h"
#include "groomlab/process_caches.h"
#include "groomlab/scalp_pieces.h"
#include "groomlab/texture.h"
#include "internal.h"

namespace groomlab {

namespace {

namespace fs = std::filesystem;

float component(const Vec3f& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }

// A referenced groom file, loaded: its session (used by one evaluation at a time) and the last
// result it gave, with what it was asked for.
struct LoadedGroom {
    std::mutex mutex;
    std::string path;
    std::uint64_t stamp = 0;
    std::unique_ptr<GroomSession> session;
    bool readsTime = false;
    bool evaluated = false;
    std::uint64_t key = 0;
    StrandsPtr strands;
    GuidesPtr guides;
    MeshPtr scalp;  // what the roots of both index
};

std::string fileKey(const std::string& path);

// The files being loaded on this thread (by fileKey): loading one asks its graph whether it reads
// the time, which asks its Reference nodes, which would load it again.
thread_local std::vector<std::string> tLoading;

// The loaded groom of `path` at `stamp`: kept for the last few files, loaded again when its
// stamp changed. Throws GraphError for a file that does not read as a groom, or one being loaded
// on this thread (a file referencing itself).
std::shared_ptr<LoadedGroom> loadedGroom(const std::string& path, std::uint64_t stamp, const std::string& stored) {
    constexpr std::size_t kKept = 8;
    static std::mutex mutex;
    static std::list<std::shared_ptr<LoadedGroom>> kept;
    static const bool registered = (registerProcessCache("referenced grooms", [] {
                                        std::list<std::shared_ptr<LoadedGroom>> drop;
                                        const std::lock_guard<std::mutex> lock(mutex);
                                        drop.swap(kept);
                                    }),
                                    true);
    (void)registered;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (auto it = kept.begin(); it != kept.end(); ++it) {
            if ((*it)->path != path || (*it)->stamp != stamp) continue;
            kept.splice(kept.begin(), kept, it);
            return kept.front();
        }
    }
    const std::string key = fileKey(path);
    if (std::find(tLoading.begin(), tLoading.end(), key) != tLoading.end()) {
        throw GraphError("Reference: '" + stored + "' references itself");
    }
    tLoading.push_back(key);
    struct Loading {
        ~Loading() { tLoading.pop_back(); }
    } loading;
    auto loaded = std::make_shared<LoadedGroom>();
    loaded->path = path;
    loaded->stamp = stamp;
    try {
        const GroomFile file = readGroomFile(path);
        loaded->session = std::make_unique<GroomSession>(file.data);
        loaded->session->setFramesPerSecond(file.framesPerSecond);
        loaded->session->setPathResolver([file](const std::string& relative) { return resolveGroomPath(file, relative); });
    } catch (const std::exception& e) {
        throw GraphError("Reference: '" + stored + "' does not read as a groom file (" + e.what() + ")");
    }
    loaded->readsTime = loaded->session->readsTime();
    const std::lock_guard<std::mutex> lock(mutex);
    kept.remove_if([&](const std::shared_ptr<LoadedGroom>& g) { return g->path == path; });
    kept.push_front(loaded);
    while (kept.size() > kKept) kept.pop_back();
    return loaded;
}

// The files being evaluated on this thread, outermost first (by fileKey): a file referencing
// itself (or one that references it back) is an error, not an endless evaluation; so is a chain of
// references deeper than kMaxDepth.
thread_local std::vector<std::string> tEvaluating;
constexpr std::size_t kMaxDepth = 16;

// A file's path as one key however it is spelled (slashes, "..", case on Windows).
std::string fileKey(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::u8path(path), ec);
    if (ec) p = fs::u8path(path).lexically_normal();
    std::string key = p.generic_u8string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return key;
}

struct Evaluating {
    explicit Evaluating(const std::string& path) { tEvaluating.push_back(path); }
    ~Evaluating() { tEvaluating.pop_back(); }
};

// A root's frame: x the U direction laid on the surface (any perpendicular where the UVs give
// none), y the normal, z their cross product.
struct UvFrame {
    Vec3f x, y, z;
};
UvFrame uvFrame(const ScalpMesh& scalp, const StrandRoot& root) {
    UvFrame f;
    f.y = scalp.normal(root);
    Vec3f t = scalp.uvTangent(root.face);
    t = t - f.y * t.dot(f.y);
    f.x = t.length() > 1e-9f ? t.normalized() : anyPerpendicular(f.y);
    f.z = f.x.cross(f.y);
    return f;
}

// Where each UV is on a scalp: its triangles bucketed on a grid over the UV square they span.
class UvLocator {
public:
    explicit UvLocator(const ScalpMesh& scalp) : scalp_(scalp) {
        const auto& uvs = scalp.cornerUVs();
        const std::size_t tris = scalp.triangleCount();
        if (uvs.size() < tris * 3 || tris == 0) return;
        lo_[0] = lo_[1] = 1e30f;
        hi_[0] = hi_[1] = -1e30f;
        for (const auto& uv : uvs) {
            for (int k = 0; k < 2; ++k) {
                lo_[k] = std::min(lo_[k], uv[k]);
                hi_[k] = std::max(hi_[k], uv[k]);
            }
        }
        n_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::sqrt(static_cast<double>(tris))));
        cells_.assign(n_ * n_, {});
        for (std::size_t f = 0; f < tris; ++f) {
            float a[2] = {1e30f, 1e30f}, b[2] = {-1e30f, -1e30f};
            for (int c = 0; c < 3; ++c) {
                for (int k = 0; k < 2; ++k) {
                    a[k] = std::min(a[k], uvs[f * 3 + c][k]);
                    b[k] = std::max(b[k], uvs[f * 3 + c][k]);
                }
            }
            const std::size_t x0 = cell(a[0], 0), x1 = cell(b[0], 0), y0 = cell(a[1], 1), y1 = cell(b[1], 1);
            for (std::size_t y = y0; y <= y1; ++y) {
                for (std::size_t x = x0; x <= x1; ++x) cells_[y * n_ + x].push_back(static_cast<std::int32_t>(f));
            }
        }
    }
    // The root at `uv` (face -1 when no triangle holds it).
    StrandRoot find(const ScalpMesh::UV& uv) const {
        if (cells_.empty() || uv[0] < lo_[0] || uv[0] > hi_[0] || uv[1] < lo_[1] || uv[1] > hi_[1]) return {};
        const auto& uvs = scalp_.cornerUVs();
        for (const std::int32_t f : cells_[cell(uv[1], 1) * n_ + cell(uv[0], 0)]) {
            const auto& a = uvs[static_cast<std::size_t>(f) * 3];
            const auto& b = uvs[static_cast<std::size_t>(f) * 3 + 1];
            const auto& c = uvs[static_cast<std::size_t>(f) * 3 + 2];
            const float e0u = b[0] - a[0], e0v = b[1] - a[1], e1u = c[0] - a[0], e1v = c[1] - a[1];
            const float det = e0u * e1v - e1u * e0v;
            if (!(std::fabs(det) > 1e-14f)) continue;
            const float pu = uv[0] - a[0], pv = uv[1] - a[1];
            const float u = (pu * e1v - e1u * pv) / det, v = (e0u * pv - pu * e0v) / det;
            constexpr float kEdge = 1e-5f;
            if (u < -kEdge || v < -kEdge || u + v > 1.0f + kEdge) continue;
            const float cu = std::clamp(u, 0.0f, 1.0f);
            return {f, cu, std::clamp(v, 0.0f, 1.0f - cu)};
        }
        return {};
    }

private:
    std::size_t cell(float x, int k) const {
        const float span = hi_[k] - lo_[k];
        const float t = span > 0.0f ? (x - lo_[k]) / span : 0.0f;
        return std::min(n_ - 1, static_cast<std::size_t>(std::max(0.0f, t) * static_cast<float>(n_)));
    }
    const ScalpMesh& scalp_;
    float lo_[2] = {0, 0}, hi_[2] = {0, 0};
    std::size_t n_ = 0;
    std::vector<std::vector<std::int32_t>> cells_;
};

// How a root and the points after it land on this groom's scalp.
struct Placement {
    bool kept = true;
    StrandRoot root;      // on `target`'s whole mesh (face -1: unrooted)
    Vec3f from, to;       // the root before and after
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};  // the turn (rows)
    Vec3f point(const Vec3f& p) const {
        const Vec3f d = p - from;
        return to + Vec3f(m[0][0] * d.x + m[0][1] * d.y + m[0][2] * d.z, m[1][0] * d.x + m[1][1] * d.y + m[1][2] * d.z,
                          m[2][0] * d.x + m[2][1] * d.y + m[2][2] * d.z);
    }
};

class ReferenceOp : public Operator {
public:
    std::string typeName() const override { return op::kReference; }
    std::string category() const override { return "source"; }
    std::vector<PortSpec> inputs() const override { return {{"scalp", PortType::Mesh, true}}; }
    PortSpec output() const override { return {"strands", PortType::Strands}; }
    std::vector<PortSpec> outputs() const override { return {output(), {"guides", PortType::Guides}}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"path", std::string(),
                      "The groom file (.groom) whose result this node brings in: a library look reused or instanced "
                      "(relative paths resolve like a map's file: the scene's or groom file's folder, then its "
                      "search paths). Read again when the file changes"},
            notAnimatable({"bind", std::string("world"),
                           "world: where the file has them, unrooted (they do not follow this scalp); closestPoint: "
                           "each root moved to the closest point of this scalp (or the mesh on the scalp input), "
                           "turned with the surface; uv: to the point of this scalp with the root's UV in the file, "
                           "turned with the surface and its UVs (both scalps sharing a UV layout)",
                           "", {"world", "closestPoint", "uv"}}),
        };
    }
    std::vector<std::string> externalInputs() const override {
        if (params().getString("bind") == "world") return {};
        return {"scalp"};
    }
    // The file's stamp: a rewritten file evaluates again.
    std::uint64_t cacheSalt() const override {
        const std::string stored = params().getString("path");
        if (stored.empty()) return 0;
        return hashCombine(hash64(0x5EFE), fileStamp(resolvePath(stored)));
    }
    // What the referenced groom is evaluated with beyond the context's hash: the viewport flag
    // (its nodes set to renderOnly or viewportOnly), and the time when it reads the time.
    std::uint64_t contextSalt(const EvalContext& ctx) const override {
        std::uint64_t salt = hashCombine(hash64(0x5EF1), ctx.viewport ? 1u : 0u);
        const std::string stored = params().getString("path");
        if (stored.empty()) return salt;
        const std::string path = resolvePath(stored);
        const std::uint64_t stamp = fileStamp(path);
        if (stamp == 0) return salt;
        try {
            if (loadedGroom(path, stamp, stored)->readsTime) salt = hashDouble(salt, ctx.time);
        } catch (const GraphError&) {
            salt = hashDouble(salt, ctx.time);  // said by the evaluation
        }
        return salt;
    }
    std::string note(const EvalContext&) const override {
        if (params().getString("path").empty()) return "No groom file set: nothing referenced";
        return {};
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        return evaluateOutputs(ctx, inputs).front();
    }

    std::vector<Value> evaluateOutputs(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const std::string stored = params().getString("path");
        if (stored.empty()) return {StrandsPtr(std::make_shared<StrandSet>()), GuidesPtr{}};  // nothing yet (the note says so)
        const std::string path = resolvePath(stored);
        const std::uint64_t stamp = fileStamp(path);
        if (stamp == 0) throw GraphError("Reference: no groom file at '" + stored + "'");
        const std::string key = fileKey(path);
        if (std::find(tEvaluating.begin(), tEvaluating.end(), key) != tEvaluating.end()) {
            throw GraphError("Reference: '" + stored + "' references itself");
        }
        if (tEvaluating.size() >= kMaxDepth) throw GraphError("Reference: '" + stored + "': references nested too deep");
        const std::shared_ptr<LoadedGroom> loaded = loadedGroom(path, stamp, stored);
        const Evaluating guard(key);
        StrandsPtr strands;
        GuidesPtr guides;
        MeshPtr from;
        {
            const std::lock_guard<std::mutex> lock(loaded->mutex);
            ViewRequest request;
            request.density = ctx.previewDensity;
            request.pointBudget = ctx.maxPoints;
            request.time = ctx.time;
            request.viewport = ctx.viewport;
            request.wantBounds = false;
            std::uint64_t asked = hashFloat(hash64(0x5EF2), request.density);
            asked = hashDouble(hashDouble(asked, request.pointBudget), loaded->readsTime ? request.time : 0.0);
            asked = hashCombine(asked, request.viewport ? 1u : 0u);
            if (!loaded->evaluated || asked != loaded->key) {
                const ViewResult& view = loaded->session->evaluate(request);
                if (!view.strands) {
                    throw GraphError("Reference '" + stored + "': " + (view.errors.empty() ? std::string("no strands") : view.errors.front()));
                }
                auto set = std::make_shared<GuideSet>();
                if (view.hierarchy) *set = view.hierarchy->flatten(-1);
                if (const GuidesPtr& flat = loaded->session->data().guides) {
                    for (const Guide& g : flat->guides()) {
                        Guide& added = set->add(g.id, g.points);
                        added.root = g.root;
                        added.attributes = g.attributes;
                        added.pointAttributes = g.pointAttributes;
                    }
                }
                loaded->strands = view.strands;
                loaded->guides = set->empty() ? nullptr : GuidesPtr(std::move(set));
                loaded->scalp = view.scalp;
                loaded->key = asked;
                loaded->evaluated = true;
            }
            strands = loaded->strands;
            guides = loaded->guides;
            from = loaded->scalp;
        }
        const std::string bind = params().getString("bind");
        if (bind == "world") return {unrooted(strands), unrootedGuides(guides)};

        // This groom's scalp: the mesh on the scalp input (one of several scalps), else the groom's.
        MeshPtr target = inputs.empty() ? nullptr : valueAs<MeshPtr>(inputs[0]);
        if (!target) target = valueAs<MeshPtr>(ctx.external("scalp"));
        if (!target || target->triangleCount() == 0) {
            throw GraphError("Reference: bind " + bind + " needs this groom's scalp");
        }
        const bool uv = bind == "uv";
        if (uv && (!target->hasUVs() || !from || !from->hasUVs())) {
            throw GraphError("Reference: bind uv needs UVs on both scalps (this groom's and the file's)");
        }
        std::unique_ptr<UvLocator> locator = uv ? std::make_unique<UvLocator>(*target) : nullptr;
        // Where a root at `root` (on `from`) with first point `p0` and first direction lands.
        auto place = [&](const StrandRoot& root, const Vec3f& p0, const Vec3f& second) {
            Placement out;
            out.from = p0;
            const bool onFrom = from && from->contains(root);
            if (uv) {
                if (!onFrom) {
                    out.kept = false;
                    return out;
                }
                const StrandRoot at = locator->find(from->uvAt(root));
                if (at.face < 0) {
                    out.kept = false;
                    return out;
                }
                const UvFrame a = uvFrame(*from, root), b = uvFrame(*target, at);
                // M = B * A^T: a's axes onto b's.
                const Vec3f ax[3] = {a.x, a.y, a.z}, bx[3] = {b.x, b.y, b.z};
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        float sum = 0.0f;
                        for (int k = 0; k < 3; ++k) sum += component(bx[k], r) * component(ax[k], c);
                        out.m[r][c] = sum;
                    }
                }
                out.root = toWholeRoot(*target, at);
                out.to = target->position(at);
                return out;
            }
            const SurfacePoint sp = target->closestPoint(p0);
            if (sp.root.face < 0) {
                out.kept = false;
                return out;
            }
            Vec3f was = onFrom ? from->normal(root) : second - p0;
            const Rotation turn(was.length() > 1e-9f ? was.normalized() : sp.normal, sp.normal);
            for (int c = 0; c < 3; ++c) {
                Vec3f e{0, 0, 0};
                (c == 0 ? e.x : c == 1 ? e.y : e.z) = 1.0f;
                const Vec3f t = turn(e);
                out.m[0][c] = t.x;
                out.m[1][c] = t.y;
                out.m[2][c] = t.z;
            }
            out.root = toWholeRoot(*target, sp.root);
            out.to = sp.position;
            return out;
        };
        return {placedStrands(strands, place), placedGuides(guides, place)};
    }

private:
    // The strands with their roots off any scalp (bind world).
    static StrandsPtr unrooted(const StrandsPtr& strands) {
        if (!strands || strands->strandCount() == 0) return strands;
        auto out = std::make_shared<StrandSet>(*strands);
        for (StrandRoot& r : out->mutableRoots()) r = StrandRoot{};
        return out;
    }
    static GuidesPtr unrootedGuides(const GuidesPtr& guides) {
        if (!guides) return guides;
        auto out = std::make_shared<GuideSet>(*guides);
        for (Guide& g : out->guides()) g.root = StrandRoot{};
        return out;
    }
    template <class Place>
    static StrandsPtr placedStrands(const StrandsPtr& strands, const Place& place) {
        if (!strands || strands->strandCount() == 0) return strands;
        const std::size_t n = strands->strandCount();
        std::vector<Placement> placements(n);
        parallelFor(0, n, [&](std::size_t s) {
            const std::size_t count = strands->strandPointCount(s);
            const Vec3f* pts = strands->strandPoints(s);
            if (count == 0) {
                placements[s].kept = false;
                return;
            }
            placements[s] = place(strands->roots()[s], pts[0], count > 1 ? pts[1] : pts[0]);
        });
        std::vector<std::size_t> kept;
        kept.reserve(n);
        for (std::size_t s = 0; s < n; ++s) {
            if (placements[s].kept) kept.push_back(s);
        }
        auto out = std::make_shared<StrandSet>(kept.size() == n ? StrandSet(*strands) : strands->subset(kept));
        std::vector<StrandRoot>& roots = out->mutableRoots();
        out->mutablePositions();  // cloned once: the strands are written side by side
        parallelFor(0, kept.size(), [&](std::size_t i) {
            const Placement& p = placements[kept[i]];
            roots[i] = p.root;
            Vec3f* pts = out->strandPoints(i);
            for (std::size_t k = 0; k < out->strandPointCount(i); ++k) pts[k] = p.point(pts[k]);
        });
        return out;
    }
    template <class Place>
    static GuidesPtr placedGuides(const GuidesPtr& guides, const Place& place) {
        if (!guides) return guides;
        auto out = std::make_shared<GuideSet>();
        for (const Guide& g : guides->guides()) {
            if (g.points.empty()) continue;
            const Placement p = place(g.root, g.points[0], g.points.size() > 1 ? g.points[1] : g.points[0]);
            if (!p.kept) continue;
            std::vector<Vec3f> pts(g.points.size());
            for (std::size_t k = 0; k < pts.size(); ++k) pts[k] = p.point(g.points[k]);
            Guide& added = out->add(g.id, std::move(pts));
            added.root = p.root;
            added.attributes = g.attributes;
            added.pointAttributes = g.pointAttributes;
        }
        return out->empty() ? nullptr : GuidesPtr(std::move(out));
    }
};

}  // namespace

void registerReferenceOperator(OperatorRegistry& registry) {
    registry.registerType(op::kReference,
                          {"source",
                           "Another groom file's result (its strands, or its guides): a library look reused or instanced, where the file has it or bound onto this scalp.",
                           "reference library instance groom file link",
                           "Linked Groom"},
                          [] { return std::make_unique<ReferenceOp>(); });
}

}  // namespace groomlab
