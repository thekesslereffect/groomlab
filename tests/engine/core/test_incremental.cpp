// SPDX-License-Identifier: Apache-2.0
// Incremental evaluation (graph.h: Operator::evaluateIncremental): a node whose inputs changed in
// some strands makes only those again, and its result is a full evaluation's, bit for bit.
// Each test checks results against a fresh graph's (no cache) by content hash, with the graph's
// own check on (Graph::setVerifyIncremental: every incremental result against a full evaluation,
// and the strands it says it left alone against its last result).

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "groomlab/display.h"
#include "groomlab/export_view.h"
#include "groomlab/graph.h"
#include "groomlab/groom_session.h"
#include "groomlab/flow.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/guide_set.h"
#include "groomlab/instances.h"
#include "groomlab/groom_file.h"
#include "groomlab/paths.h"
#include "groomlab/presets.h"
#include "groomlab/map_ref.h"
#include "groomlab/operators.h"
#include "groomlab/parts.h"
#include "groomlab/random.h"
#include "groomlab/strand_set.h"
#include "groomlab/stroke.h"
#include "groomlab/view_projection.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// Hands on the strands the host set as the external "strands" (every evaluation a new set).
class HeldStrandsOp : public Operator {
public:
    std::string typeName() const override { return "TestHeldStrands"; }
    std::vector<PortSpec> inputs() const override { return {}; }
    std::vector<std::string> externalInputs() const override { return {"strands"}; }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>&) const override { return ctx.external("strands"); }
};

// Hands on its input; incrementally it says which strands differ from its last result by
// comparing them, as Interpolate compares its guides: the source of changes for these tests.
class DiffOp : public Operator {
public:
    std::string typeName() const override { return "TestDiff"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override { return inputs.at(0); }
    bool evaluateIncremental(const EvalContext&, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        const StrandsPtr in = valueAs<StrandsPtr>(inputs.at(0)), last = valueAs<StrandsPtr>(incremental.previous);
        if (!in || !last || in->ids() != last->ids() || in->offsets() != last->offsets() ||
            in->strandChannelNames() != last->strandChannelNames() || in->pointChannelNames() != last->pointChannelNames()) {
            return false;
        }
        std::vector<std::uint32_t> list;
        for (std::size_t s = 0; s < in->strandCount(); ++s) {
            const std::size_t begin = in->strandBegin(s), n = in->strandPointCount(s);
            bool same = std::memcmp(in->positions().data() + begin, last->positions().data() + begin, n * sizeof(Vec3f)) == 0 &&
                        std::memcmp(in->widths().data() + begin, last->widths().data() + begin, n * sizeof(float)) == 0;
            for (const std::string& name : in->strandChannelNames()) {
                same = same && (*in->strandChannel(name))[s] == (*last->strandChannel(name))[s];
            }
            if (!same) list.push_back(static_cast<std::uint32_t>(s));
        }
        *out = list.empty() ? Value(last) : inputs.at(0);
        *changed = StrandChanges::listed(std::move(list));
        return true;
    }
};

// Lies: says nothing changed and hands on its last result (what verification must catch).
class StaleOp : public Operator {
public:
    std::string typeName() const override { return "TestStale"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override { return inputs.at(0); }
    bool evaluateIncremental(const EvalContext&, const std::vector<Value>&, const IncrementalInputs& incremental, Value* out,
                             StrandChanges* changed) const override {
        *out = incremental.previous;
        *changed = StrandChanges::listed({});
        return true;
    }
};

// Every incremental result checked against a full evaluation while a test runs.
struct Verified {
    Verified() { Graph::setVerifyIncremental(true); }
    ~Verified() { Graph::setVerifyIncremental(false); }
};

// `count` strands of 8 points with a per-strand "mask" channel.
std::shared_ptr<StrandSet> someStrands(std::size_t count, std::uint64_t seed) {
    auto out = std::make_shared<StrandSet>();
    Rng rng(seed);
    std::vector<Vec3f> points(8);
    out->addStrandChannel("mask", 1.0f);
    for (std::size_t i = 0; i < count; ++i) {
        const Vec3f root(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1));
        for (std::size_t k = 0; k < points.size(); ++k) {
            points[k] = root + Vec3f(0.01f * rng.uniform(-1, 1), 0.1f * static_cast<float>(k), 0.0f);
        }
        out->addStrand(static_cast<StrandId>(1000 + i), points);
    }
    std::vector<float>& m = *out->mutableStrandChannel("mask");
    for (std::size_t i = 0; i < count; ++i) m[i] = rng.uniform(0, 1);
    return out;
}

// A copy of `strands` with `dabs` strands moved (some points of each), as a brush dab moves some.
std::shared_ptr<StrandSet> dabbed(const StrandSet& strands, std::size_t dabs, Rng& rng) {
    auto out = std::make_shared<StrandSet>(strands);
    for (std::size_t d = 0; d < dabs; ++d) {
        const auto s = static_cast<std::size_t>(rng.nextFloat() * static_cast<float>(strands.strandCount())) % strands.strandCount();
        Vec3f* pts = out->strandPoints(s);
        for (std::size_t k = 1; k < out->strandPointCount(s); ++k) pts[k] += Vec3f(0.02f, 0.0f, 0.01f * static_cast<float>(k));
        if (d % 3 == 0) (*out->mutableStrandChannel("mask"))[s] = rng.nextFloat();
    }
    return out;
}

// The set's longest strand's length (each strand's segments summed in order, as an expression's
// `longest` reads it).
float longestOf(const StrandSet& strands) {
    float longest = 0.0f;
    for (std::size_t s = 0; s < strands.strandCount(); ++s) {
        const Vec3f* pts = strands.strandPoints(s);
        float length = 0.0f;
        for (std::size_t k = 1; k < strands.strandPointCount(s); ++k) length += (pts[k] - pts[k - 1]).length();
        longest = std::max(longest, length);
    }
    return longest;
}

// `strands` with its longest strand longer still (its points half as far again from its root):
// the set's longest changes.
std::shared_ptr<StrandSet> longestLengthened(const StrandSet& strands) {
    auto out = std::make_shared<StrandSet>(strands);
    std::size_t most = 0;
    float longest = -1.0f;
    for (std::size_t s = 0; s < strands.strandCount(); ++s) {
        const Vec3f* pts = strands.strandPoints(s);
        float length = 0.0f;
        for (std::size_t k = 1; k < strands.strandPointCount(s); ++k) length += (pts[k] - pts[k - 1]).length();
        if (length > longest) {
            longest = length;
            most = s;
        }
    }
    Vec3f* pts = out->strandPoints(most);
    for (std::size_t k = 1; k < out->strandPointCount(most); ++k) pts[k] = pts[0] + (pts[k] - pts[0]) * 1.5f;
    return out;
}

// held -> diff -> noise -> curl -> length -> width -> resample -> out, built in code: the test
// operators are not registered (every registered type must have a golden case).
struct Chain {
    Graph graph;
    std::map<std::string, NodeId> ids;
    ExternalInputs ext;
    EvalContext ctx;

    NodeId operator[](const std::string& name) const { return ids.at(name); }
    Operator& node(const std::string& name) { return graph.node(ids.at(name)); }
    StrandsPtr run(const StrandsPtr& strands) {
        ext.set("strands", strands, strands->contentHash());
        ctx.externals = &ext;
        return graph.evaluateStrands(ids.at("out"), ctx);
    }
};

using ChainEdit = std::function<void(Chain&)>;

std::unique_ptr<Chain> makeChain(const ChainEdit& edit = {}, bool stale = false) {
    auto chain = std::make_unique<Chain>();
    Graph& g = chain->graph;
    auto add = [&](const std::string& name, std::unique_ptr<Operator> op) {
        chain->ids[name] = g.addNode(std::move(op));
        g.setLabel(chain->ids[name], name);
    };
    auto& registry = OperatorRegistry::instance();
    add("held", std::make_unique<HeldStrandsOp>());
    add("diff", stale ? std::unique_ptr<Operator>(std::make_unique<StaleOp>()) : std::make_unique<DiffOp>());
    add("noise", registry.create(op::kNoise));
    add("curl", registry.create(op::kCurl));
    add("length", registry.create(op::kLength));
    add("width", registry.create(op::kWidth));
    add("resample", registry.create(op::kResample));
    add("out", registry.create(op::kOutput));
    chain->node("noise").params().set("amplitude", 0.05);
    chain->node("noise").params().set("frequency", 3.0);
    chain->node("curl").params().set("radius", 0.02);
    chain->node("length").params().set("keep", 0.8);
    chain->node("length").params().set("randomness", 0.3);
    chain->node("resample").params().set("segments", std::int64_t{7});
    chain->node("resample").params().set("mode", std::string("even"));
    const char* order[] = {"held", "diff", "noise", "curl", "length", "width", "resample", "out"};
    for (std::size_t k = 0; k + 1 < std::size(order); ++k) g.connect(chain->ids[order[k]], chain->ids[order[k + 1]], 0);
    if (edit) edit(*chain);
    return chain;
}

// The output of a fresh chain (nothing cached, nothing kept) on these strands.
std::uint64_t freshHash(const StrandsPtr& strands, const ChainEdit& edit = {}) {
    return makeChain(edit)->run(strands)->contentHash();
}

TEST(Incremental, PerStrandOperatorsMakeOnlyTheChangedStrandsAgain) {
    const Verified verified;
    auto chain = makeChain();
    StrandsPtr strands = someStrands(400, 1);
    ASSERT_EQ(chain->run(strands)->contentHash(), freshHash(strands));
    Rng rng(5);
    for (int dab = 0; dab < 6; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 1 + static_cast<std::size_t>(dab) * 7, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands)) << "dab " << dab;
        // TestDiff, Noise, Curl, Length and Width made only the changed strands again.
        EXPECT_EQ(chain->graph.incrementalCount() - before, 5u) << "dab " << dab;
    }
}

TEST(Incremental, ASerialFromAnotherDocumentNamesNothingHere) {
    // Two documents of the same graph (one parsed again): a serial held from the first is not a
    // result of the second, so its changes since that serial are unknown, never "none".
    auto first = makeChain();
    auto second = makeChain();
    StrandsPtr strands = someStrands(50, 2);
    first->run(strands);
    second->run(strands);
    const std::uint64_t held = first->graph.resultSerial((*first)["out"]);
    ASSERT_NE(held, 0u);
    EXPECT_NE(second->graph.resultSerial((*second)["out"]), held);
    EXPECT_FALSE(second->graph.changesSince((*second)["out"], held));
}

TEST(Incremental, TheOutputSaysWhichStrandsChanged) {
    const Verified verified;
    auto chain = makeChain();
    StrandsPtr strands = someStrands(300, 2);
    chain->run(strands);
    const std::uint64_t serial = chain->graph.resultSerial((*chain)["out"]);
    auto moved = std::make_shared<StrandSet>(*strands);
    for (const std::size_t s : {7u, 42u, 299u}) moved->strandPoints(s)[3] += Vec3f(0.1f, 0.0f, 0.0f);
    EXPECT_EQ(chain->run(moved)->contentHash(), freshHash(moved));
    // Handed on by Resample (nothing to do) and Output: Width's changes, which are TestDiff's.
    const ChangesPtr changes = chain->graph.changesSince((*chain)["out"], serial);
    ASSERT_TRUE(changes);
    EXPECT_FALSE(changes->all);
    EXPECT_EQ(changes->strands, (std::vector<std::uint32_t>{7, 42, 299}));
    EXPECT_FALSE(chain->graph.changesSince((*chain)["out"], serial - 1));  // not the result before: unknown
    // Moved back: another set in, the same strands as two evaluations ago; every node made again
    // only those three, and the output is that first result's content.
    const std::size_t before = chain->graph.incrementalCount();
    EXPECT_EQ(chain->run(std::make_shared<StrandSet>(*strands))->contentHash(), freshHash(strands));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 5u);
}

TEST(Incremental, BypassedNodesAndParamEditsAndLayoutChanges) {
    const Verified verified;
    auto chain = makeChain();
    StrandsPtr strands = someStrands(300, 3);
    chain->run(strands);
    Rng rng(9);
    // Curl bypassed: what follows evaluates in full once, then dabs pass through it.
    const ChainEdit bypassed = [](Chain& c) { c.graph.setBypass(c["curl"], true); };
    bypassed(*chain);
    strands = dabbed(*strands, 5, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, bypassed));
    std::size_t before = chain->graph.incrementalCount();
    strands = dabbed(*strands, 5, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, bypassed));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 4u);  // TestDiff, Noise, Length, Width
    chain->graph.setBypass((*chain)["curl"], false);
    // A param edit: that node and what follows evaluate in full, with the dab.
    const ChainEdit quieter = [](Chain& c) { c.node("noise").params().set("amplitude", 0.03); };
    quieter(*chain);
    before = chain->graph.incrementalCount();
    strands = dabbed(*strands, 5, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, quieter));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 1u);  // TestDiff alone
    // Fewer strands: everything in full.
    auto fewer = std::make_shared<StrandSet>(strands->subset({0, 1, 2, 5, 8, 13}));
    before = chain->graph.incrementalCount();
    EXPECT_EQ(chain->run(fewer)->contentHash(), freshHash(fewer, quieter));
    EXPECT_EQ(chain->graph.incrementalCount(), before);
    // Off: nothing incremental, the same results.
    Graph::setIncrementalEvaluation(false);
    strands = dabbed(*fewer, 2, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, quieter));
    EXPECT_EQ(chain->graph.incrementalCount(), before);
    Graph::setIncrementalEvaluation(true);
}

TEST(Incremental, AnotherSetOfInstancesIsSaidWithTheStrandsThatChanged) {
    // Two results of the same strands, one strand moved and the instances made again (an Instance
    // node's probability reading a length a dab changed): the strand that moved, and that the
    // instances are another set (the nodes after evaluate in full; a host draws them anew).
    const StrandsPtr before = someStrands(50, 3);
    auto withInstances = [](const StrandSet& strands, std::size_t count) {
        auto set = std::make_shared<InstanceSet>();
        set->instances.resize(count);
        for (std::size_t k = 0; k < count; ++k) set->instances[k].strand = strands.ids()[k];
        auto out = std::make_shared<StrandSet>(strands);
        out->setInstances(set);
        return out;
    };
    const StrandsPtr a = withInstances(*before, 4);
    auto moved = withInstances(*before, 5);
    moved->strandPoints(7)[1] += Vec3f(0.0f, 0.1f, 0.0f);
    const ChangesPtr changes = strandChangesBetween(a, moved);
    ASSERT_TRUE(changes);
    EXPECT_FALSE(changes->all);
    EXPECT_EQ(changes->strands, std::vector<std::uint32_t>{7});
    EXPECT_TRUE(changes->instancesChanged);
    // The same set of instances: not said.
    auto same = std::make_shared<StrandSet>(*a);
    same->strandPoints(9)[1] += Vec3f(0.0f, 0.1f, 0.0f);
    const ChangesPtr kept = strandChangesBetween(a, same);
    ASSERT_TRUE(kept);
    EXPECT_FALSE(kept->instancesChanged);
}

TEST(Incremental, AWholeSetValueReadFallsBackToAFullEvaluation) {
    // Noise's amplitude from an expression of `longest` (the set's longest strand): a dab changing
    // the longest changes every strand's value, so Noise evaluates in full; one leaving it, only
    // the strands it moved (LongestKept). Width's per-strand mask stays local.
    const Verified verified;
    const ChainEdit maps = [](Chain& c) {
        MapRef longest;
        longest.source = MapRef::Source::Expression;
        longest.name = "0.05 * longest";
        c.node("noise").params().set("amplitude", longest);
        MapRef mask;
        mask.source = MapRef::Source::Attribute;
        mask.name = "mask";
        mask.max = 0.02f;
        c.node("width").params().set("width", mask);
    };
    auto chain = makeChain(maps);
    StrandsPtr strands = someStrands(200, 4);
    ASSERT_EQ(chain->run(strands)->contentHash(), freshHash(strands, maps));
    Rng rng(2);
    // TestDiff on every dab; Noise, with the longest changed, in full, then says which strands its
    // result changed in (Graph::setDiffFullResults): Curl, Length and Width follow them.
    bool fell = false, kept = false;
    for (int dab = 0; dab < 4; ++dab) {
        const float was = longestOf(*strands);
        strands = dab == 3 ? longestLengthened(*strands) : dabbed(*strands, 4, rng);
        const std::size_t before = chain->graph.incrementalCount();
        EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, maps)) << "dab " << dab;
        const bool same = longestOf(*strands) == was;
        (same ? kept : fell) = true;
        EXPECT_EQ(chain->graph.incrementalCount() - before, same ? 5u : 4u) << "dab " << dab;
    }
    EXPECT_TRUE(fell && kept);
}

// held -> diff -> collide (the scalp and a collider) -> smooth -> gravity -> displace -> flow ->
// flyaway -> attribute (a map of the dabbed mask) -> attribute (random) -> twist (around guides) ->
// wind -> restore (its reference the diff's strands, which change with the input) -> instance -> out.
std::unique_ptr<Chain> makeDeformChain(const ChainEdit& edit = {}) {
    auto chain = std::make_unique<Chain>();
    Graph& g = chain->graph;
    auto add = [&](const std::string& name, std::unique_ptr<Operator> op) {
        chain->ids[name] = g.addNode(std::move(op));
        g.setLabel(chain->ids[name], name);
    };
    auto& registry = OperatorRegistry::instance();
    add("held", std::make_unique<HeldStrandsOp>());
    add("diff", std::make_unique<DiffOp>());
    for (const char* type : {op::kScalp, op::kColliderMesh, op::kGuides, op::kCollide, op::kSmooth, op::kGravity, op::kDisplace,
                             op::kFlow, op::kFlyaway, op::kTwist, op::kWind, op::kRestoreLength, op::kInstance, op::kOutput}) {
        add(type, registry.create(type));
    }
    add("mask", registry.create(op::kAttribute));
    add("random", registry.create(op::kAttribute));
    chain->node(op::kCollide).params().set("offset", 0.03);
    chain->node(op::kCollide).params().set("maxDepth", 0.3);
    chain->node(op::kSmooth).params().set("strength", 0.4);
    chain->node(op::kFlyaway).params().set("fraction", 0.3);
    MapRef mask;
    mask.source = MapRef::Source::Attribute;
    mask.name = "mask";
    chain->node("mask").params().set("name", std::string("painted"));
    chain->node("mask").params().set("value", mask);
    chain->node("random").params().set("name", std::string("r"));
    MapRef random;
    random.source = MapRef::Source::Expression;
    random.name = "rand";
    chain->node("random").params().set("value", random);
    chain->node(op::kTwist).params().set("turns", 0.5);
    chain->node(op::kWind).params().set("turbulence", 0.5);
    chain->node(op::kInstance).params().set("count", std::int64_t{2});
    chain->node(op::kInstance).params().set("probability", 0.4);
    const char* order[] = {"held", "diff", op::kCollide, op::kSmooth, op::kGravity, op::kDisplace, op::kFlow, op::kFlyaway,
                           "mask", "random", op::kTwist, op::kWind, op::kRestoreLength, op::kInstance, op::kOutput};
    for (std::size_t k = 0; k + 1 < std::size(order); ++k) g.connect(chain->ids[order[k]], chain->ids[order[k + 1]], 0);
    g.connect((*chain)[op::kScalp], (*chain)[op::kCollide], 1);
    g.connect((*chain)[op::kColliderMesh], (*chain)[op::kCollide], 2);
    g.connect((*chain)[op::kGuides], (*chain)[op::kTwist], 1);
    g.connect((*chain)["diff"], (*chain)[op::kRestoreLength], 1);
    chain->ids["out"] = chain->ids[op::kOutput];
    // The scalp a sphere the strands cross; the collider a smaller one above it; axes upright.
    const auto scalp = test::makeSphere(16, 32, 0.9f);
    chain->ext.set("scalp", MeshPtr(scalp), scalp->contentHash());
    std::vector<Vec3f> points = test::makeSphere(10, 20, 0.4f)->points();
    for (Vec3f& p : points) p += Vec3f(0.2f, 0.9f, 0.1f);
    const auto collider = std::make_shared<const ScalpMesh>(std::move(points), test::makeSphere(10, 20, 0.4f)->triangles());
    chain->ext.set("mesh:collider", MeshPtr(collider), collider->contentHash());
    auto axes = std::make_shared<GuideSet>();
    Rng rng(11);
    for (std::uint64_t i = 0; i < 6; ++i) {
        const Vec3f root(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1));
        axes->add(i + 1, test::straightCurve(root, Vec3f(0.1f, 0.8f, 0.0f), 5));
    }
    chain->ext.set("guides", GuidesPtr(axes), 0x6A11DE5ull);
    if (edit) edit(*chain);
    return chain;
}

std::uint64_t freshDeformHash(const StrandsPtr& strands, const ChainEdit& edit = {}) {
    return makeDeformChain(edit)->run(strands)->contentHash();
}

TEST(Incremental, CollideOnDeformedMeshesFollowsRandomDabs) {
    // The scalp and the collider deformed away from their rest poses: Collide asks the rest poses'
    // fields (RestPoseCollision) and still walks again only the strands a dab moved, every result
    // a fresh chain's.
    const ChainEdit deformed = [](Chain& c) {
        auto deform = [](const std::shared_ptr<const ScalpMesh>& rest, float bend) {
            auto rested = std::make_shared<ScalpMesh>(*rest);
            rested->setRestPoints(rest->points());
            std::vector<Vec3f> points = rest->points();
            for (Vec3f& p : points) p = Vec3f(p.x * 1.1f, p.y + bend * p.x * p.x, p.z - 0.05f);
            return MeshPtr(std::make_shared<const ScalpMesh>(rested->withPoints(std::move(points))));
        };
        const MeshPtr scalp = deform(test::makeSphere(16, 32, 0.9f), 0.1f);
        c.ext.set("scalp", scalp, scalp->contentHash());
        std::vector<Vec3f> points = test::makeSphere(10, 20, 0.4f)->points();
        for (Vec3f& p : points) p += Vec3f(0.2f, 0.9f, 0.1f);
        const MeshPtr collider =
            deform(std::make_shared<const ScalpMesh>(std::move(points), test::makeSphere(10, 20, 0.4f)->triangles()), -0.2f);
        c.ext.set("mesh:collider", collider, collider->contentHash());
    };
    const Verified verified;
    auto chain = makeDeformChain(deformed);
    StrandsPtr strands = someStrands(500, 7);
    ASSERT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands, deformed));
    EXPECT_NE(chain->run(strands)->contentHash(), freshDeformHash(strands));  // the deformed meshes' result
    Rng rng(17);
    for (int dab = 0; dab < 6; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 1 + static_cast<std::size_t>(rng.nextFloat() * 40.0f), rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands, deformed)) << "dab " << dab;
        EXPECT_EQ(chain->graph.incrementalCount() - before, 13u) << "dab " << dab;
    }
}

TEST(Incremental, CollideAndThePerStrandDeformersFollowRandomDabs) {
    // Collide walks again only the strands a dab moved (the scalp's and the collider's meshes the
    // same), and so do Smooth, Gravity, Displace, Flow, Flyaway, Attribute, Twist around guides,
    // Wind, RestoreLength (its reference changed in the same strands) and Instance: every result a
    // fresh chain's.
    const Verified verified;
    auto chain = makeDeformChain();
    StrandsPtr strands = someStrands(500, 7);
    ASSERT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands));
    Rng rng(13);
    for (int dab = 0; dab < 12; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 1 + static_cast<std::size_t>(rng.nextFloat() * 40.0f), rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands)) << "dab " << dab;
        // TestDiff and the 12 operators after it.
        EXPECT_EQ(chain->graph.incrementalCount() - before, 13u) << "dab " << dab;
    }
    // Another collider: Collide and what follows it evaluate in full, then dabs are incremental again.
    const ChainEdit moved = [](Chain& c) {
        std::vector<Vec3f> points = test::makeSphere(10, 20, 0.5f)->points();
        for (Vec3f& p : points) p += Vec3f(-0.3f, 0.7f, 0.0f);
        const auto collider = std::make_shared<const ScalpMesh>(std::move(points), test::makeSphere(10, 20, 0.5f)->triangles());
        c.ext.set("mesh:collider", MeshPtr(collider), collider->contentHash());
    };
    moved(*chain);
    std::size_t before = chain->graph.incrementalCount();
    strands = dabbed(*strands, 6, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands, moved));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 1u);  // TestDiff alone
    before = chain->graph.incrementalCount();
    strands = dabbed(*strands, 6, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands, moved));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 13u);
    // A Collide param edit, then a map reading the whole set on Smooth: those fall back.
    const ChainEdit deeper = [&](Chain& c) {
        moved(c);
        c.node(op::kCollide).params().set("maxDepth", 0.5);
    };
    deeper(*chain);
    before = chain->graph.incrementalCount();
    strands = dabbed(*strands, 6, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands, deeper));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 1u);
    const ChainEdit longest = [&](Chain& c) {
        deeper(c);
        MapRef map;
        map.source = MapRef::Source::Expression;
        map.name = "0.3 * longest";
        c.node(op::kSmooth).params().set("strength", map);
    };
    longest(*chain);
    for (int dab = 0; dab < 2; ++dab) {
        before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 6, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands, longest));
        // TestDiff and Collide; Smooth in full, the 10 after it on the strands it says changed.
        EXPECT_EQ(chain->graph.incrementalCount() - before, dab == 0 ? 2u : 12u);
    }
}

TEST(Incremental, InstancesOfAPerStrandChanceAreDrawnAgainOnlyWhereADabMoved) {
    // Instance's probability by each strand's length over the longest: a dab leaving the longest
    // draws again only the strands it moved, the others' instances spliced in from the last
    // result (the very set when none of theirs changed); one changing it, in full. Every result a
    // fresh chain's.
    const Verified verified;
    const ChainEdit byLength = [](Chain& c) {
        MapRef chance;
        chance.source = MapRef::Source::Expression;
        chance.name = "length / longest";
        c.node(op::kInstance).params().set("probability", chance);
        c.node(op::kInstance).params().set("count", std::int64_t{3});
    };
    auto chain = makeDeformChain(byLength);
    StrandsPtr strands = someStrands(500, 7);
    StrandsPtr last = chain->run(strands);
    ASSERT_EQ(last->contentHash(), freshDeformHash(strands, byLength));
    Rng rng(19);
    bool kept = false, fell = false;
    for (int dab = 0; dab < 8; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        const float was = longestOf(*last);
        strands = dab == 7 ? longestLengthened(*strands) : dabbed(*strands, 1 + static_cast<std::size_t>(rng.nextFloat() * 40.0f), rng);
        const StrandsPtr out = chain->run(strands);
        EXPECT_EQ(out->contentHash(), freshDeformHash(strands, byLength)) << "dab " << dab;
        const bool same = longestOf(*out) == was;
        (same ? kept : fell) = true;
        last = out;
        // TestDiff and the 12 operators after it; with the longest changed, Instance in full.
        EXPECT_EQ(chain->graph.incrementalCount() - before, same ? 13u : 12u) << "dab " << dab;
    }
    EXPECT_TRUE(kept && fell);
}

// Plugs `field` into param `param` of `node` (an exposed param), as a link in the editor does.
void plug(Chain& c, NodeId field, const std::string& node, const std::string& param) {
    const NodeId id = c[node];
    std::vector<std::string> exposed = c.graph.paramInputs(id);
    exposed.push_back(param);
    c.graph.setParamInputs(id, exposed);
    c.graph.connect(field, id, c.graph.node(id).inputs().size() + exposed.size() - 1);
}

NodeId addField(Chain& c, const std::string& name, const char* type) {
    c.ids[name] = c.graph.addNode(type);
    c.graph.setLabel(c.ids[name], name);
    return c.ids[name];
}

TEST(Incremental, FieldNodesPluggedIntoParamsKeepNodesIncremental) {
    // An Expression (the strands' mask), a FieldNoise, a FieldRemap of it and a FieldCombine of
    // the two read each strand alone (FieldValue::strandLocal): the nodes they feed stay incremental.
    const Verified verified;
    const ChainEdit fields = [](Chain& c) {
        const NodeId mask = addField(c, "mask", op::kExpression);
        c.graph.node(mask).params().set("expression", std::string("$mask"));
        const NodeId spots = addField(c, "spots", op::kFieldNoise);
        const NodeId remap = addField(c, "remap", op::kFieldRemap);
        c.graph.connect(spots, remap, 0);
        const NodeId both = addField(c, "both", op::kFieldCombine);
        c.graph.connect(mask, both, 0);
        c.graph.connect(remap, both, 1);
        plug(c, spots, "noise", "amplitude");
        plug(c, both, "curl", "radius");
        plug(c, mask, "width", "width");
    };
    auto chain = makeChain(fields);
    StrandsPtr strands = someStrands(300, 11);
    ASSERT_EQ(chain->run(strands)->contentHash(), freshHash(strands, fields));
    Rng rng(4);
    for (int dab = 0; dab < 4; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 3 + static_cast<std::size_t>(dab) * 5, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, fields)) << "dab " << dab;
        EXPECT_EQ(chain->graph.incrementalCount() - before, 5u) << "dab " << dab;  // TestDiff, Noise, Curl, Length, Width
    }
    // An Expression of `longest` plugged into Noise: with the longest changed Noise evaluates in
    // full, and tells the nodes after it which strands its result changed in
    // (Graph::setDiffFullResults), so they do not.
    const ChainEdit whole = [](Chain& c) {
        const NodeId expression = addField(c, "whole", op::kExpression);
        c.graph.node(expression).params().set("expression", std::string("0.05 * longest"));
        c.graph.disconnect(c["noise"], c.graph.node(c["noise"]).inputs().size());
        c.graph.connect(expression, c["noise"], c.graph.node(c["noise"]).inputs().size());
    };
    const ChainEdit longest = [&](Chain& c) {
        fields(c);
        whole(c);
    };
    whole(*chain);
    for (int dab = 0; dab < 3; ++dab) {
        const float was = longestOf(*strands);
        strands = dab == 2 ? longestLengthened(*strands) : dabbed(*strands, 4, rng);
        const std::size_t before = chain->graph.incrementalCount();
        EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, longest)) << "dab " << dab;
        const std::size_t made = longestOf(*strands) == was ? 5u : 4u;  // (the longest changed: all but Noise)
        if (dab > 0) EXPECT_EQ(chain->graph.incrementalCount() - before, made) << "dab " << dab;
    }
    Graph::setDiffFullResults(false);
    strands = longestLengthened(*strands);
    std::size_t before = chain->graph.incrementalCount();
    EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, longest));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 1u);  // TestDiff alone
    Graph::setDiffFullResults(true);
}

// Hands on the guides the host set as the external "paths".
class HeldGuidesOp : public Operator {
public:
    std::string typeName() const override { return "TestHeldGuides"; }
    std::vector<PortSpec> inputs() const override { return {}; }
    PortSpec output() const override { return {"guides", PortType::Guides}; }
    std::vector<std::string> externalInputs() const override { return {"paths"}; }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>&) const override { return ctx.external("paths"); }
};

// held -> diff, then two looks of it (noise, curl) blended by the mask, merged with a third
// (length), culled, a switch (picking the cull), a Tangle (no incremental path: it reads
// neighbors) and a Braid along held paths -> out.
std::unique_ptr<Chain> makeUtilityChain(const ChainEdit& edit = {}) {
    auto chain = std::make_unique<Chain>();
    Graph& g = chain->graph;
    auto add = [&](const std::string& name, std::unique_ptr<Operator> op) {
        chain->ids[name] = g.addNode(std::move(op));
        g.setLabel(chain->ids[name], name);
    };
    auto& registry = OperatorRegistry::instance();
    add("held", std::make_unique<HeldStrandsOp>());
    add("diff", std::make_unique<DiffOp>());
    for (const char* type : {op::kNoise, op::kCurl, op::kLength, op::kBlend, op::kMerge, op::kCull, op::kSwitch, op::kTangle,
                             op::kBraid, op::kOutput}) {
        add(type, registry.create(type));
    }
    add("paths", std::make_unique<HeldGuidesOp>());
    chain->node(op::kNoise).params().set("amplitude", 0.05);
    chain->node(op::kCurl).params().set("radius", 0.02);
    chain->node(op::kLength).params().set("keep", 0.7);
    MapRef mask;
    mask.source = MapRef::Source::Attribute;
    mask.name = "mask";
    chain->node(op::kBlend).params().set("mix", mask);
    chain->node(op::kCull).params().set("keep", 0.6);
    chain->node(op::kTangle).params().set("radius", 0.3);
    chain->node(op::kBraid).params().set("region", 0.6);
    auto link = [&](const char* from, const char* to, std::size_t slot) { g.connect(chain->ids[from], chain->ids[to], slot); };
    link("held", "diff", 0);
    link("diff", op::kNoise, 0);
    link("diff", op::kCurl, 0);
    link("diff", op::kLength, 0);
    link(op::kNoise, op::kBlend, 0);
    link(op::kCurl, op::kBlend, 1);
    link(op::kBlend, op::kMerge, 0);
    link(op::kLength, op::kMerge, 1);
    link(op::kMerge, op::kCull, 0);
    link(op::kCull, op::kSwitch, 0);
    link("diff", op::kSwitch, 1);
    link(op::kSwitch, op::kTangle, 0);
    link(op::kTangle, op::kBraid, 0);
    link("paths", op::kBraid, 1);
    link(op::kBraid, op::kOutput, 0);
    chain->ids["out"] = chain->ids[op::kOutput];
    GuideSet paths;
    paths.add(1, test::straightCurve(Vec3f(0.2f, 0.0f, 0.1f), Vec3f(0.2f, -1.5f, 0.4f), 8));
    paths.add(2, test::straightCurve(Vec3f(-0.5f, 0.3f, -0.2f), Vec3f(-0.7f, -1.2f, 0.0f), 8));
    const auto held = std::make_shared<const GuideSet>(std::move(paths));
    chain->ext.set("paths", GuidesPtr(held), held->contentHash());
    if (edit) edit(*chain);
    return chain;
}

TEST(Incremental, SwitchMergeBlendCullAndBraidFollowDabs) {
    // Blend mixes again the strands either look changed in, Merge writes each side's changed
    // strands over their places, Cull cuts again its kept strands that changed, Switch hands on
    // its picked input's changes, Braid braids again the changed strands along the same paths;
    // Tangle (its neighbors) evaluates in full and says which strands its result changed in.
    const Verified verified;
    auto chain = makeUtilityChain();
    auto fresh = [](const StrandsPtr& strands, const ChainEdit& edit = {}) { return makeUtilityChain(edit)->run(strands)->contentHash(); };
    StrandsPtr strands = someStrands(400, 21);
    ASSERT_EQ(chain->run(strands)->contentHash(), fresh(strands));
    Rng rng(8);
    for (int dab = 0; dab < 6; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 2 + static_cast<std::size_t>(dab) * 6, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands)) << "dab " << dab;
        // TestDiff, Noise, Curl, Length, Blend, Merge, Cull, Switch, Braid.
        EXPECT_EQ(chain->graph.incrementalCount() - before, 9u) << "dab " << dab;
    }
    // A keep read from the mask a dab changes: a kept strand may go (Cull in full, then the nodes
    // after it), results as ever.
    const ChainEdit masked = [](Chain& c) {
        MapRef mask;
        mask.source = MapRef::Source::Attribute;
        mask.name = "mask";
        c.node(op::kCull).params().set("keep", mask);
    };
    masked(*chain);
    for (int dab = 0; dab < 6; ++dab) {
        strands = dabbed(*strands, 8, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands, masked)) << "dab " << dab;
    }
    // The other input switched to, merged with a seam: still a fresh chain's results.
    const ChainEdit seam = [&](Chain& c) {
        masked(c);
        c.node(op::kSwitch).params().set("index", std::int64_t{1});
        c.node(op::kMerge).params().set("blend", 0.2);
    };
    seam(*chain);
    for (int dab = 0; dab < 3; ++dab) {
        strands = dabbed(*strands, 5, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands, seam)) << "dab " << dab;
    }
}

// held -> diff -> split (a, b, c by thirds), then a -> noise, b -> curl -> length, merged, and c
// merged in as it comes -> out.
std::unique_ptr<Chain> makeSplitChain(const ChainEdit& edit = {}) {
    auto chain = std::make_unique<Chain>();
    Graph& g = chain->graph;
    auto add = [&](const std::string& name, std::unique_ptr<Operator> op) {
        chain->ids[name] = g.addNode(std::move(op));
        g.setLabel(chain->ids[name], name);
    };
    auto& registry = OperatorRegistry::instance();
    add("held", std::make_unique<HeldStrandsOp>());
    add("diff", std::make_unique<DiffOp>());
    add("split", registry.create(op::kSplit));
    add("noise", registry.create(op::kNoise));
    add("curl", registry.create(op::kCurl));
    add("length", registry.create(op::kLength));
    add("merge", registry.create(op::kMerge));
    add("merge2", registry.create(op::kMerge));
    add("out", registry.create(op::kOutput));
    chain->node("split").params().set("weightC", 1.0);
    chain->node("noise").params().set("amplitude", 0.05);
    chain->node("curl").params().set("radius", 0.02);
    chain->node("length").params().set("keep", 0.7);
    auto link = [&](const char* from, const char* to, std::size_t slot, std::size_t output = 0) {
        g.connect(chain->ids[from], chain->ids[to], slot, output);
    };
    link("held", "diff", 0);
    link("diff", "split", 0);
    link("split", "noise", 0, 0);
    link("split", "curl", 0, 1);
    link("curl", "length", 0);
    link("noise", "merge", 0);
    link("length", "merge", 1);
    link("merge", "merge2", 0);
    link("split", "merge2", 1, 2);
    link("merge2", "out", 0);
    if (edit) edit(*chain);
    return chain;
}

std::size_t splitOutputSize(const Chain& c, std::size_t output) {
    const StrandsPtr strands = valueAs<StrandsPtr>(c.graph.cachedValue(c["split"], output));
    return strands ? strands->strandCount() : 0;
}

TEST(Incremental, ExpressionsOfIndexFollowDabsExactly) {
    // `index` is a strand's place in its set (0 to 1): a node evaluates incrementally only on the
    // strands its last result was made from (the same IDs in the same places), where every
    // strand's index is what it was, so an expression of it reads only the strand itself. Where a
    // dab changes which strands a set holds (a Cull's kept strands, a Split's outputs), the nodes
    // after it evaluate in full. Every incremental result checked against a full evaluation.
    const Verified verified;
    auto expression = [](const char* text) {
        MapRef map;
        map.source = MapRef::Source::Expression;
        map.name = text;
        return map;
    };
    const ChainEdit indexed = [&](Chain& c) {
        c.node(op::kNoise).params().set("amplitude", expression("0.02 + 0.06 * index"));
        c.node(op::kLength).params().set("keep", expression("0.5 + 0.5 * index"));
        c.node(op::kCull).params().set("keep", expression("mask > 0.3 ? 1 : 0.5 * index"));
    };
    auto chain = makeUtilityChain(indexed);
    auto fresh = [&](const StrandsPtr& strands) { return makeUtilityChain(indexed)->run(strands)->contentHash(); };
    StrandsPtr strands = someStrands(400, 31);
    ASSERT_EQ(chain->run(strands)->contentHash(), fresh(strands));
    Rng rng(12);
    const std::size_t before = chain->graph.incrementalCount();
    for (int dab = 0; dab < 8; ++dab) {
        strands = dabbed(*strands, 3 + static_cast<std::size_t>(dab) * 5, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands)) << "dab " << dab;
    }
    EXPECT_GE(chain->graph.incrementalCount() - before, 8u * 4u);  // TestDiff, Noise, Length, Blend at least
    // Split by a map of the mask a dab changes: strands move between its outputs, and the index
    // the Noise after output a reads changes for every strand after them there.
    const ChainEdit split = [&](Chain& c) {
        c.node("split").params().set("mode", std::string("map"));
        c.node("split").params().set("select", expression("0.5 * mask + 0.5 * index"));
        c.node("noise").params().set("amplitude", expression("0.02 + 0.06 * index"));
        c.node("length").params().set("keep", expression("index"));
    };
    auto splitChain = makeSplitChain(split);
    strands = someStrands(300, 32);
    ASSERT_EQ(splitChain->run(strands)->contentHash(), makeSplitChain(split)->run(strands)->contentHash());
    for (int dab = 0; dab < 8; ++dab) {
        strands = dabbed(*strands, 4 + static_cast<std::size_t>(dab) * 3, rng);
        EXPECT_EQ(splitChain->run(strands)->contentHash(), makeSplitChain(split)->run(strands)->contentHash()) << "dab " << dab;
    }
}

TEST(Incremental, SplitHandsEachOutputItsOwnChangedStrands) {
    // A dab upstream of Split reaches its outputs a, b and c: each writes the changed strands it
    // holds over their places, says them in its own indices, and the looks on each branch and
    // the merges follow them.
    const Verified verified;
    auto chain = makeSplitChain();
    auto fresh = [](const StrandsPtr& strands, const ChainEdit& edit = {}) { return makeSplitChain(edit)->run(strands)->contentHash(); };
    StrandsPtr strands = someStrands(600, 31);
    ASSERT_EQ(chain->run(strands)->contentHash(), fresh(strands));
    for (std::size_t k = 0; k < 3; ++k) EXPECT_GT(splitOutputSize(*chain, k), 100u) << "output " << k;
    Rng rng(4);
    for (int dab = 0; dab < 5; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 3 + static_cast<std::size_t>(dab) * 9, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands)) << "dab " << dab;
        // TestDiff, Split, Noise, Curl, Length and both Merges.
        EXPECT_EQ(chain->graph.incrementalCount() - before, 7u) << "dab " << dab;
    }
    // A param edit on one branch after Split: that branch in full, with the dab; Split and the
    // other branch go on incrementally, and so do the merges (the branch made anew holds the
    // strands it held: they write all of its strands); the next dab is incremental throughout.
    const ChainEdit curlier = [](Chain& c) { c.node("curl").params().set("radius", 0.04); };
    curlier(*chain);
    std::size_t before = chain->graph.incrementalCount();
    strands = dabbed(*strands, 6, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands, curlier));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 5u);  // TestDiff, Split, Noise and both Merges
    before = chain->graph.incrementalCount();
    strands = dabbed(*strands, 6, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands, curlier));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 7u);
    // Every strand to a: a is the input itself and changes as it does; b and c stay empty (the
    // Merges hand a on as it comes, its changes with it).
    const ChainEdit allToA = [&](Chain& c) {
        curlier(c);
        c.node("split").params().set("weightB", 0.0);
        c.node("split").params().set("weightC", 0.0);
    };
    allToA(*chain);
    for (int dab = 0; dab < 3; ++dab) {
        before = chain->graph.incrementalCount();
        const std::uint64_t serial = chain->graph.resultSerial((*chain)["out"]);
        strands = dabbed(*strands, 5, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands, allToA)) << "dab " << dab;
        EXPECT_EQ(splitOutputSize(*chain, 0), strands->strandCount());
        if (dab == 0) continue;
        // TestDiff, Split, Noise, and Curl and Length on b (empty, unchanged).
        EXPECT_EQ(chain->graph.incrementalCount() - before, 5u) << "dab " << dab;
        const ChangesPtr changes = chain->graph.changesSince((*chain)["out"], serial);
        ASSERT_TRUE(changes);
        EXPECT_FALSE(changes->all);
        EXPECT_FALSE(changes->strands.empty());
    }
}

TEST(Incremental, StrandsMovingBetweenSplitOutputsLeaveAndJoinThem) {
    // Split by a map of the mask a dab changes: a strand whose mask crosses a share's bound moves
    // to another output. The two outputs it left and joined are cut again in full (their strands
    // changed), the third goes on incrementally, and every result is a fresh chain's.
    const Verified verified;
    const ChainEdit byMask = [](Chain& c) {
        MapRef mask;
        mask.source = MapRef::Source::Attribute;
        mask.name = "mask";
        c.node("split").params().set("mode", std::string("map"));
        c.node("split").params().set("select", mask);
    };
    auto chain = makeSplitChain(byMask);
    auto fresh = [&](const StrandsPtr& strands) { return makeSplitChain(byMask)->run(strands)->contentHash(); };
    StrandsPtr strands = someStrands(500, 12);
    ASSERT_EQ(chain->run(strands)->contentHash(), fresh(strands));
    // Points moved alone: no strand changes output, everything incremental.
    {
        auto moved = std::make_shared<StrandSet>(*strands);
        for (const std::size_t s : {3u, 77u, 140u, 401u}) moved->strandPoints(s)[4] += Vec3f(0.05f, 0.0f, 0.0f);
        strands = moved;
        const std::size_t before = chain->graph.incrementalCount();
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands));
        EXPECT_EQ(chain->graph.incrementalCount() - before, 7u);
    }
    // One strand of a (mask under a third) moved to c, another of a moved too but kept in a.
    {
        const std::vector<float>& mask = *strands->strandChannel("mask");
        std::size_t leaving = 0, staying = 0;
        while (mask[leaving] >= 0.3f) ++leaving;
        staying = leaving + 1;
        while (mask[staying] >= 0.3f) ++staying;
        auto moved = std::make_shared<StrandSet>(*strands);
        (*moved->mutableStrandChannel("mask"))[leaving] = 0.9f;
        moved->strandPoints(staying)[2] += Vec3f(0.0f, 0.0f, 0.05f);
        moved->strandPoints(leaving)[3] += Vec3f(0.0f, 0.03f, 0.0f);
        const std::size_t a = splitOutputSize(*chain, 0), b = splitOutputSize(*chain, 1), c = splitOutputSize(*chain, 2);
        strands = moved;
        const std::size_t before = chain->graph.incrementalCount();
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands));
        EXPECT_EQ(splitOutputSize(*chain, 0), a - 1);
        EXPECT_EQ(splitOutputSize(*chain, 1), b);
        EXPECT_EQ(splitOutputSize(*chain, 2), c + 1);
        // TestDiff, Split, and b's Curl and Length; a's Noise and both Merges in full.
        EXPECT_EQ(chain->graph.incrementalCount() - before, 4u);
    }
    // Random dabs, the mask changing in some: strands come and go, results as ever.
    Rng rng(6);
    for (int dab = 0; dab < 6; ++dab) {
        strands = dabbed(*strands, 4 + static_cast<std::size_t>(dab) * 5, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands)) << "dab " << dab;
    }
}

TEST(Incremental, VerificationCatchesAnOperatorThatGetsItWrong) {
    const Verified verified;
    auto chain = makeChain({}, true);
    StrandsPtr strands = someStrands(50, 6);
    chain->run(strands);
    Rng rng(1);
    strands = dabbed(*strands, 3, rng);
    try {
        chain->run(strands);
        FAIL() << "a stale incremental result went unnoticed";
    } catch (const GraphError& e) {
        EXPECT_NE(std::string(e.what()).find("evaluated incrementally"), std::string::npos) << e.what();
    }
}

// ---- Brush strokes on a session ------------------------------------------------------------------

// `count` primaries of 7 points along the unit sphere's normals, each with a painted "clump" value
// and a "tint" layer, and `children` children each, rooted.
std::string sphereGuides(const ScalpMesh& scalp, int count, std::size_t children) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    Rng rng(3);
    std::vector<GuideId> ids;
    for (int i = 0; i < count; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(0.2f, 1), rng.uniform(-1, 1)).normalized();
        ids.push_back(h.addGuide(primary, test::straightCurve(n, n * 0.7f + Vec3f(0.15f, -0.1f, 0.0f), 6)));
    }
    h.evaluate(&scalp);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        h.setAttribute(ids[i], "clump", rng.uniform(0, 1));
        h.setPointAttribute(ids[i], "tint", {rng.uniform(0, 1), rng.uniform(0, 1), rng.uniform(0, 1)});
    }
    if (children > 0) {
        ChildGenerationParams params;
        params.count = children;
        h.generateChildren(primary, "children", params);
    }
    h.evaluate(&scalp);
    return h.toJson();
}

// The view a fresh session gives with this one's inputs (its stored texts, the live stroke's
// guides): every node evaluated in full, nothing kept.
std::uint64_t freshView(GroomSession& session, const ViewRequest& request, Bounds* bounds = nullptr) {
    GroomSession fresh(session.data());
    if (const auto preview = session.previewHierarchy()) fresh.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*preview));
    const ViewResult& view = fresh.evaluate(request);
    EXPECT_TRUE(view.errors.empty()) << (view.errors.empty() ? "" : view.errors[0]);
    if (bounds) *bounds = view.bounds;
    return view.strands ? view.strands->contentHash() : 0;
}

bool sameBounds(const Bounds& a, const Bounds& b) {
    return a.valid == b.valid && std::memcmp(&a.lo, &b.lo, sizeof(Vec3f)) == 0 && std::memcmp(&a.hi, &b.hi, sizeof(Vec3f)) == 0;
}

// Strokes of `dabs` points with each brush mode in turn through BrushTool on `session`; after the
// press, every dab and every release, its view is checked against a fresh session's. Returns how
// many nodes evaluated incrementally.
std::size_t strokeAndCompare(GroomSession& session, SessionStrokeHost& host, const ViewRequest& request,
                             const std::vector<std::string>& modes, int dabs, const std::function<void(int)>& midStroke = {}) {
    const ToolView view = ToolView::fromMatrix(test::lookAtProjection({0.0f, 1.8f, 4.5f}, {0.0f, 0.8f, 0.0f}, 40.0, 1.5), 1200, 800);
    float x0 = 0.0f, y0 = 0.0f;
    EXPECT_TRUE(view.project(Vec3f(0.0f, 1.0f, 0.35f).normalized(), x0, y0));
    const std::size_t before = session.incrementalCount();
    auto check = [&](const std::string& what) {
        const ViewResult& seen = session.evaluate(request);
        EXPECT_TRUE(seen.errors.empty()) << what << ": " << (seen.errors.empty() ? "" : seen.errors[0]);
        ASSERT_TRUE(seen.strands) << what;
        Bounds bounds;  // (measured again only in the blocks of strands that changed)
        EXPECT_EQ(seen.strands->contentHash(), freshView(session, request, &bounds)) << what;
        EXPECT_TRUE(sameBounds(seen.bounds, bounds)) << what;
    };
    check("before");
    int stroke = 0;
    for (const std::string& mode : modes) {
        BrushTool tool;
        tool.settings.setMode(mode);
        tool.settings.screenRadius = 90.0f;
        tool.settings.brush.strength = 0.8f;
        const float sx = x0 - 50.0f + 13.0f * static_cast<float>(stroke++);
        EXPECT_TRUE(tool.begin(host, view, sx, y0, 1.0f)) << mode;
        check(mode + " press");
        for (int k = 1; k < dabs; ++k) {
            tool.moveTo(host, view, sx + 9.0f * static_cast<float>(k), y0 + 2.0f * static_cast<float>(k), 1.0f);
            if (midStroke) midStroke(k);
            check(mode + " dab " + std::to_string(k));
        }
        tool.end(host, view);
        check(mode + " release");
    }
    return session.incrementalCount() - before;
}

const char* kInterpolateGraph = R"({"version": 1, "output": "out",
  "nodes": [
    {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 900.0}},
    {"name": "interp", "type": "Interpolate", "params": {"limitDistance": 0.45}},
    {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 9}}],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interp", "input": "strands"},
    {"from": "guides", "to": "interp", "input": "guides"},
    {"from": "scalp", "to": "interp", "input": "scalp"},
    {"from": "interp", "to": "out", "input": "strands"}]})";

GroomData sphereGroom(const std::string& graph, int guides, std::size_t children) {
    const auto scalp = test::makeSphere(24, 48, 1.0f);
    GroomData data;
    data.scalp = scalp;
    data.graphJson = graph;
    data.hierarchyJson = sphereGuides(*scalp, guides, children);
    return data;
}

TEST(Incremental, InterpolateBlendsAgainOnlyTheStrandsADabReaches) {
    // Guide attributes and layers into channels, a radius (the influence channel); combing,
    // smoothing, painting the clump value, sliding roots (the strands around them bound again),
    // lengthening and cutting; the stroke at half density and at the view's.
    const Verified verified;
    for (const float sculpt : {0.5f, 1.0f}) {
        GroomSession session(sphereGroom(kInterpolateGraph, 40, 0));
        SessionStrokeHost host(session);
        ViewRequest request;
        request.sculptDensity = sculpt;
        const std::size_t incremental =
            strokeAndCompare(session, host, request, {"comb", "smooth", "clump", "slide", "length", "cut"}, 5);
        EXPECT_GE(incremental, 24u) << "sculptDensity " << sculpt;  // at least every dab after a press
    }
}

TEST(Incremental, GuideWeightFromAGuideLayerFollowsStrokes) {
    // guideWeight read from the guides' painted clump value: painting it weighs those guides again
    // (the strands they reach bound again), combing keeps the weights, sliding moves their roots.
    const Verified verified;
    std::string graph = kInterpolateGraph;
    const std::string interp = R"("params": {"limitDistance": 0.45})";
    graph.replace(graph.find(interp), interp.size(), R"("params": {"guideWeight": {"guide": "clump", "min": 0.1, "max": 1.0}})");
    GroomSession session(sphereGroom(graph, 40, 0));
    SessionStrokeHost host(session);
    ViewRequest request;
    request.sculptDensity = 0.5f;
    EXPECT_GT(strokeAndCompare(session, host, request, {"clump", "comb", "slide"}, 5), 0u);
}

TEST(Incremental, GuidesAddedOrErasedMidStrokeAndUndo) {
    // The kept guides change (one erased, one added through the preview): Interpolate evaluates in
    // full, and the next dabs are incremental again. Undo and redo after the strokes.
    const Verified verified;
    GroomSession session(sphereGroom(kInterpolateGraph, 30, 2));
    SessionStrokeHost host(session);
    ViewRequest request;
    request.sculptDensity = 0.5f;
    auto edit = [&](int dab) {
        const auto preview = session.previewHierarchy();
        if (!preview || preview->guides().empty()) return;
        if (dab == 2) preview->removeGuides({preview->guides().back().id});
        if (dab == 3) {
            const Vec3f n = Vec3f(0.1f, 1.0f, 0.3f).normalized();
            preview->addGuide(preview->groups().front().id, test::straightCurve(n, n * 0.5f, 5));
            preview->evaluate(session.scalp().get());
        }
    };
    EXPECT_GT(strokeAndCompare(session, host, request, {"comb", "lift"}, 6, edit), 0u);
    const ViewResult& now = session.evaluate(request);
    const std::uint64_t committed = now.strands->contentHash();
    ASSERT_TRUE(host.undo());
    EXPECT_EQ(session.evaluate(request).strands->contentHash(), freshView(session, request));
    ASSERT_TRUE(host.redo());
    EXPECT_EQ(session.evaluate(request).strands->contentHash(), committed);
}

// A look graph: Interpolate and Clump, its own centers (its strength from the guides' painted
// clump value), Noise, Curl, Width along a guide layer (per point), Length, Resample, Output.
const char* kLookGraph = R"({"version": 1, "output": "out",
  "nodes": [
    {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 700.0}},
    {"name": "interp", "type": "Interpolate"},
    {"name": "clump", "type": "Clump", "params": {"count": 30, "strength": {"attribute": "clump", "min": 0.2, "max": 0.9}}},
    {"name": "noise", "type": "Noise", "params": {"amplitude": 0.02, "frequency": 4.0}},
    {"name": "curl", "type": "Curl", "params": {"radius": 0.01}},
    {"name": "width", "type": "Width", "params": {"width": {"attribute": "tint", "max": 0.01}}},
    {"name": "length", "type": "Transform", "params": {"scale": 0.9, "scaleRandom": 0.2}},
    {"name": "resample", "type": "Resample", "params": {"segments": 9, "mode": "even"}},
    {"name": "out", "type": "Output"}],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interp", "input": "strands"},
    {"from": "guides", "to": "interp", "input": "guides"},
    {"from": "scalp", "to": "interp", "input": "scalp"},
    {"from": "interp", "to": "clump", "input": "strands"},
    {"from": "clump", "to": "noise", "input": "strands"},
    {"from": "noise", "to": "curl", "input": "strands"},
    {"from": "curl", "to": "width", "input": "strands"},
    {"from": "width", "to": "length", "input": "strands"},
    {"from": "length", "to": "resample", "input": "strands"},
    {"from": "resample", "to": "out", "input": "strands"}]})";

TEST(Incremental, LookGraphStrokesGiveWhatAFreshEvaluationGives) {
    // Every node after the guides follows a stroke's changes: Interpolate the guides', Clump
    // the centers' strands, Clump the strands and the members of the centers that moved, the
    // per-strand nodes their input's. With children (a dab on a primary moves its children).
    const Verified verified;
    for (const float sculpt : {0.6f, 1.0f}) {
        GroomSession session(sphereGroom(kLookGraph, 24, 3));
        SessionStrokeHost host(session);
        ViewRequest request;
        request.sculptDensity = sculpt;
        const std::size_t incremental = strokeAndCompare(session, host, request, {"comb", "clump", "curl", "slide", "smooth"}, 5);
        // Every dab after a press runs at least Interpolate, Clump, Noise, Curl, Width
        // and Length incrementally.
        EXPECT_GE(incremental, 5u * 4u * 7u) << "sculptDensity " << sculpt;
    }
}

TEST(Incremental, ARepellingClumpFollowsStrokesExactly) {
    // A negative Clump strength (strands pushed away from their centers, lengths kept), mapped per
    // strand from the guides' painted value: strokes as exact as with a pull.
    const Verified verified;
    std::string graph = kLookGraph;
    const std::string strength = R"("strength": {"attribute": "clump", "min": 0.2, "max": 0.9}})";
    graph.replace(graph.find(strength), strength.size(),
                  R"("strength": {"attribute": "clump", "min": -0.9, "max": -0.2}, "repelRadius": 0.25})");
    GroomSession session(sphereGroom(graph, 24, 3));
    SessionStrokeHost host(session);
    ViewRequest request;
    request.sculptDensity = 0.6f;
    EXPECT_GE(strokeAndCompare(session, host, request, {"comb", "clump", "smooth"}, 4), 3u * 3u * 7u);
}

TEST(Incremental, AClumpOnScatteredCentersFollowsStrokesExactly) {
    // Centers from a sparse Scatter fed straight in (centersFrom inputPoints: each clump's axis is the
    // strand it gathers nearest its center, extended past its tip): a dab moves strands and with
    // them some axes; the strands that moved and the members of the axes that moved clump again.
    const Verified verified;
    std::string graph = kLookGraph;
    const std::string clump = R"({"name": "clump", "type": "Clump", "params": {"count": 30,)";
    graph.replace(graph.find(clump), clump.size(),
                  R"({"name": "spots", "type": "Scatter", "params": {"density": 15.0, "seed": 5, "previewAll": true}},
    {"name": "clump", "type": "Clump", "params": {"centersFrom": "inputPoints", "pastTip": "extend",)");
    const std::string link = R"({"from": "interp", "to": "clump", "input": "strands"},)";
    graph.replace(graph.find(link), link.size(),
                  link + R"( {"from": "scalp", "to": "spots", "input": "scalp"}, {"from": "spots", "to": "clump", "input": "centers"},)");
    GroomSession session(sphereGroom(graph, 24, 3));
    SessionStrokeHost host(session);
    ViewRequest request;
    request.sculptDensity = 0.6f;
    EXPECT_GE(strokeAndCompare(session, host, request, {"comb", "clump", "smooth"}, 4), 3u * 3u * 7u);
}

TEST(Incremental, AWarmedStrokeDocumentMakesTheFirstDabIncremental) {
    // The stroke document evaluated ahead (a host's idle time: warmStroke) holds the groom as it
    // is, so a stroke's press makes again only what it changed; every view is a fresh session's.
    // Its evaluations are not counted; a graph edit warms it again; released, it goes.
    const Verified verified;
    ViewRequest request;
    request.sculptDensity = 0.6f;
    // The nodes a stroke (a press, a dab, the release) evaluates in full.
    auto inFull = [&](GroomSession& session, SessionStrokeHost& host) {
        const std::size_t evaluations = session.evaluationCount(), incrementals = session.incrementalCount();
        strokeAndCompare(session, host, request, {"comb"}, 2);
        return (session.evaluationCount() - evaluations) - (session.incrementalCount() - incrementals);
    };
    std::size_t cold = 0;
    {
        GroomSession session(sphereGroom(kLookGraph, 24, 3));
        SessionStrokeHost host(session);
        session.evaluate(request);
        cold = inFull(session, host);
    }
    GroomSession session(sphereGroom(kLookGraph, 24, 3));
    SessionStrokeHost host(session);
    EXPECT_FALSE(session.warmStroke(request)) << "nothing evaluated yet: no inputs to warm with";
    session.evaluate(request);
    const std::size_t counted = session.evaluationCount();
    EXPECT_TRUE(session.warmStroke(request));
    EXPECT_TRUE(session.strokeDocument());
    EXPECT_FALSE(session.warmStroke(request)) << "warm already";
    EXPECT_EQ(session.evaluationCount(), counted) << "warming is not counted";
    ViewRequest full = request;
    full.sculptDensity = 1.0f;
    EXPECT_FALSE(session.warmStroke(full)) << "a stroke at the view's density uses the view's document";
    // Cold, the press evaluates the stroke document in full (Scatter to Output); warm, only what
    // reads the guides as a whole (the Guides node).
    const std::size_t warm = inFull(session, host);
    EXPECT_GE(cold, warm + 10u) << "in full: cold " << cold << ", warm " << warm;
    // After the stroke, an edit of the graph: warmed again, and the next stroke as a fresh session's.
    std::string graph = kLookGraph;
    const std::string amplitude = "\"amplitude\": 0.02";
    graph.replace(graph.find(amplitude), amplitude.size(), "\"amplitude\": 0.03");
    session.setGraphJson(graph);
    session.evaluate(request);
    EXPECT_TRUE(session.warmStroke(request));
    EXPECT_GE(strokeAndCompare(session, host, request, {"lift", "smooth"}, 3), 7u * 5u);
    const std::size_t before = session.evaluationCount();
    session.releaseStroke();
    EXPECT_FALSE(session.strokeDocument());
    EXPECT_EQ(session.evaluationCount(), before) << "a released stroke's evaluations stay counted";
    EXPECT_GT(strokeAndCompare(session, host, request, {"comb"}, 2), 0u);
}

TEST(Incremental, AStrokeDocumentWarmedAwayFromTheSessionIsAsWarm) {
    // detachWarm's copy evaluated on another thread and adopted is the stroke document warmStroke
    // makes: the press after it evaluates as little, and every view is a fresh session's. A warm
    // is never chained on, nor adopted as the view's document; one the session moved past (a
    // stroke pressed meanwhile evaluated its own) is left.
    const Verified verified;
    ViewRequest request;
    request.sculptDensity = 0.6f;
    auto inFull = [&](GroomSession& session, SessionStrokeHost& host) {
        const std::size_t evaluations = session.evaluationCount(), incrementals = session.incrementalCount();
        strokeAndCompare(session, host, request, {"comb"}, 2);
        return (session.evaluationCount() - evaluations) - (session.incrementalCount() - incrementals);
    };
    std::size_t warmed = 0;
    {
        GroomSession session(sphereGroom(kLookGraph, 24, 3));
        SessionStrokeHost host(session);
        session.evaluate(request);
        session.warmStroke(request);
        warmed = inFull(session, host);
    }
    GroomSession session(sphereGroom(kLookGraph, 24, 3));
    SessionStrokeHost host(session);
    EXPECT_EQ(session.detachWarm(request), nullptr) << "nothing evaluated yet: no inputs to warm with";
    session.evaluate(request);
    ViewRequest full = request;
    full.sculptDensity = 1.0f;
    EXPECT_EQ(session.detachWarm(full), nullptr) << "a stroke at the view's density uses the view's document";
    const std::size_t counted = session.evaluationCount();
    std::shared_ptr<DetachedEvaluation> warm = session.detachWarm(request);
    ASSERT_NE(warm, nullptr);
    EXPECT_TRUE(warm->warm());
    EXPECT_EQ(session.detach(request, warm), nullptr) << "never chained on a warm";
    std::thread([&] { warm->run(); }).join();
    EXPECT_FALSE(session.adopt(*warm)) << "not the view's document";
    EXPECT_TRUE(session.adoptWarm(*warm));
    EXPECT_EQ(session.evaluationCount(), counted) << "warming is not counted";
    EXPECT_FALSE(session.warmStroke(request)) << "warm already";
    EXPECT_EQ(inFull(session, host), warmed) << "the press as after warmStroke";
    // A warm the stroke document moved past is left (the stroke evaluated it meanwhile).
    std::shared_ptr<DetachedEvaluation> stale = session.detachWarm(request);
    ASSERT_NE(stale, nullptr);
    strokeAndCompare(session, host, request, {"lift"}, 2);
    stale->run();
    EXPECT_FALSE(session.adoptWarm(*stale));
    EXPECT_GT(strokeAndCompare(session, host, request, {"comb"}, 2), 0u);
}

TEST(Incremental, AStrokesDabsEvaluatedAwayFromTheSessionLandAsDabsHere) {
    // detachStroke lends the stroke document to a run on another thread with a copy of the
    // preview guides while the tool goes on dabbing on the session; adopted, the evaluation that
    // follows makes nothing again and shows the copy's strands (a fresh session's for those
    // guides). The stroke commits what it commits with every dab evaluated here.
    const Verified verified;
    ViewRequest request;
    request.sculptDensity = 0.6f;
    const ToolView view = ToolView::fromMatrix(test::lookAtProjection({0.0f, 1.8f, 4.5f}, {0.0f, 0.8f, 0.0f}, 40.0, 1.5), 1200, 800);
    float x0 = 0.0f, y0 = 0.0f;
    ASSERT_TRUE(view.project(Vec3f(0.0f, 1.0f, 0.35f).normalized(), x0, y0));
    struct Outcome {
        std::string guides;
        std::uint64_t strands = 0;
        int landed = 0;
    };
    auto stroke = [&](bool away) {
        Outcome out;
        GroomSession session(sphereGroom(kLookGraph, 24, 3));
        SessionStrokeHost host(session);
        session.evaluate(request);
        EXPECT_EQ(session.detachStroke(request), nullptr) << "no stroke";
        BrushTool tool;
        tool.settings.setMode("comb");
        tool.settings.screenRadius = 90.0f;
        tool.settings.brush.strength = 0.8f;
        EXPECT_TRUE(tool.begin(host, view, x0, y0, 1.0f));
        ViewRequest full = request;
        full.sculptDensity = 1.0f;
        EXPECT_EQ(session.detachStroke(full), nullptr) << "a stroke at the view's density uses the view's document";
        session.evaluate(request);  // the press, here
        std::shared_ptr<DetachedEvaluation> run;
        std::thread worker;
        for (int k = 1; k <= 9; ++k) {
            // The tool edits its preview while a run evaluates the copy (every other dab).
            tool.moveTo(host, view, x0 + 9.0f * static_cast<float>(k), y0 + 2.0f * static_cast<float>(k), 1.0f);
            if (!away) {
                session.evaluate(request);
                continue;
            }
            if (!run) {
                run = session.detachStroke(request);
                EXPECT_TRUE(run && run->stroke() && session.strokeLent());
                if (!run) continue;
                EXPECT_EQ(session.detachStroke(request), nullptr) << "one lent at a time";
                worker = std::thread([run] { run->run(); });
                continue;
            }
            worker.join();
            const std::shared_ptr<GuideHierarchy> live = session.previewHierarchy();
            EXPECT_TRUE(session.adoptStroke(*run));
            EXPECT_FALSE(session.strokeLent());
            EXPECT_NE(session.previewHierarchy(), live) << "the copy the run evaluated stands for the tool's";
            const std::size_t evaluations = session.evaluationCount();
            const ViewResult& seen = session.evaluate(request);
            EXPECT_EQ(session.evaluationCount(), evaluations) << "dab " << k << ": evaluated again";
            EXPECT_TRUE(seen.strands && seen.strands->contentHash() == freshView(session, request)) << "dab " << k;
            session.setPreviewHierarchy(live, session.previewFinal());
            ++out.landed;
            run.reset();
        }
        if (worker.joinable()) worker.join();
        if (run) session.adoptStroke(*run);
        tool.end(host, view);
        out.guides = host.storedText(StrokeField::Guides);
        const ViewResult& committed = session.evaluate(request);
        out.strands = committed.strands ? committed.strands->contentHash() : 0;
        EXPECT_EQ(out.strands, freshView(session, request));
        return out;
    };
    const Outcome here = stroke(false), away = stroke(true);
    EXPECT_EQ(away.landed, 4);
    EXPECT_FALSE(here.guides.empty());
    EXPECT_EQ(away.guides, here.guides);
    EXPECT_EQ(away.strands, here.strands);
}

TEST(Incremental, NodesTakeTheirLastResultsArraysInsteadOfCopyingThem) {
    // A node's last result nobody else holds gives its points to the node's next result (no
    // copy of the whole array): Interpolate, Clump and Noise here on every dab (Curl's are shared
    // with Width's result, Length's with what Resample hands on); every view as a fresh session's,
    // every incremental result checked against a full evaluation and its last result (verified).
    const Verified verified;
    GroomSession session(sphereGroom(kLookGraph, 24, 0));
    SessionStrokeHost host(session);
    ViewRequest request;
    request.sculptDensity = 0.6f;
    session.evaluate(request);
    const std::size_t before = detail::arraysTaken();
    strokeAndCompare(session, host, request, {"comb", "clump"}, 5);
    EXPECT_GE(detail::arraysTaken() - before, 2u * 3u * 4u) << "Interpolate, Clump and Noise on each dab after a press";
}

TEST(Incremental, AStrokeAfterAGraphEditIsAsIncrementalAsWithout) {
    // A graph edit swaps the document (setGraphJson: what the edit does not reach is adopted). The
    // adopted results keep their serials and what they were made from, so the stroke right after
    // evaluates as incrementally as one without the edit (Clump, before the edited
    // Noise, included); every view as a fresh session's.
    const Verified verified;
    ViewRequest request;  // sculptDensity 1: strokes evaluate the view's own document
    auto inFull = [&](bool edit) {
        GroomSession session(sphereGroom(kLookGraph, 24, 0));
        SessionStrokeHost host(session);
        session.evaluate(request);
        std::string graph = kLookGraph;
        if (edit) {
            const std::string amplitude = "\"amplitude\": 0.02";
            graph.replace(graph.find(amplitude), amplitude.size(), "\"amplitude\": 0.025");
        }
        session.setGraphJson(graph);
        session.evaluate(request);
        const std::size_t evaluations = session.evaluationCount(), incrementals = session.incrementalCount();
        strokeAndCompare(session, host, request, {"comb"}, 3);
        return (session.evaluationCount() - evaluations) - (session.incrementalCount() - incrementals);
    };
    EXPECT_EQ(inFull(true), inFull(false));
}

TEST(Incremental, AWidthOrColorEditKeepsThePointsOfWhatFollows) {
    // A Width or HairColor edit changes no point (StrandChanges::pointsKept): Merge, Collide and
    // Instance after it keep their last points (the very arrays) and make only the widths and
    // channels again; every view as a fresh session's, each incremental result checked against a
    // full evaluation. An Instance whose probability reads a channel the edit changes is placed again.
    const Verified verified;
    const std::string graph = R"({"version": 1, "output": "out",
      "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 600.0}},
        {"name": "interp", "type": "Interpolate"},
        {"name": "noise", "type": "Noise", "params": {"amplitude": 0.05, "frequency": 3.0}},
        {"name": "width", "type": "Width", "params": {"width": 0.01, "randomness": 0.3}},
        {"name": "color", "type": "HairColor", "params": {"melanin": 0.4}},
        {"name": "whiskers", "type": "Scatter", "params": {"density": 40.0, "seed": 5}},
        {"name": "whiskerInterp", "type": "Interpolate"},
        {"name": "whiskerColor", "type": "HairColor", "params": {"melanin": 0.1}},
        {"name": "merge", "type": "Merge"},
        {"name": "collide", "type": "Collide", "params": {"offset": 0.05}},
        {"name": "snow", "type": "Instance", "params": {"probability": 0.3, "tMin": 0.4, "scaleByWidth": true}},
        {"name": "resample", "type": "Resample", "params": {"segments": 7}},
        {"name": "out", "type": "Output"}],
      "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"}, {"from": "scatter", "to": "interp", "input": "strands"},
        {"from": "guides", "to": "interp", "input": "guides"}, {"from": "scalp", "to": "interp", "input": "scalp"},
        {"from": "interp", "to": "noise", "input": "strands"}, {"from": "noise", "to": "width", "input": "strands"},
        {"from": "width", "to": "color", "input": "strands"},
        {"from": "scalp", "to": "whiskers", "input": "scalp"}, {"from": "whiskers", "to": "whiskerInterp", "input": "strands"},
        {"from": "guides", "to": "whiskerInterp", "input": "guides"}, {"from": "scalp", "to": "whiskerInterp", "input": "scalp"},
        {"from": "whiskerInterp", "to": "whiskerColor", "input": "strands"},
        {"from": "color", "to": "merge", "input": "a"}, {"from": "whiskerColor", "to": "merge", "input": "b"},
        {"from": "merge", "to": "collide", "input": "strands"}, {"from": "scalp", "to": "collide", "input": "scalp"},
        {"from": "collide", "to": "snow", "input": "strands"}, {"from": "snow", "to": "out", "input": "strands"}]})";
    GroomSession session(sphereGroom(graph, 24, 0));
    ViewRequest request;
    ASSERT_TRUE(session.evaluate(request).strands);
    auto cached = [&](const char* name) {
        const GraphDocument& doc = *session.viewDocument();
        return valueAs<StrandsPtr>(doc.graph.cachedValue(doc.nodesByName.at(name)));
    };
    auto kind = [&](const char* name) {
        const GraphDocument& doc = *session.viewDocument();
        const auto it = doc.graph.lastRunKinds().find(doc.nodesByName.at(name));
        return it == doc.graph.lastRunKinds().end() ? std::string("cached") : it->second;
    };
    auto edit = [&](std::string& text, const std::string& from, const std::string& to) {
        const std::size_t at = text.find(from);
        ASSERT_NE(at, std::string::npos) << from;
        text.replace(at, from.size(), to);
    };
    std::string edited = graph;
    const std::pair<std::string, std::string> edits[] = {
        {R"("width": 0.01,)", R"("width": 0.013,)"},                     // the fur's widths
        {R"("melanin": 0.4)", R"("melanin": 0.55)"},                     // the fur's color
        {R"("melanin": 0.1)", R"("melanin": 0.2)"}};                     // the whiskers' (merge's b)
    for (const auto& [from, to] : edits) {
        SCOPED_TRACE(to);
        const StrandsPtr collided = cached("collide"), snowed = cached("snow");
        ASSERT_TRUE(collided && snowed && snowed->instances() && !snowed->instances()->empty());
        edit(edited, from, to);
        session.setGraphJson(edited);
        const ViewResult& view = session.evaluate(request);
        ASSERT_TRUE(view.strands);
        EXPECT_EQ(view.strands->contentHash(), freshView(session, request));
        for (const char* name : {"merge", "collide", "snow"}) EXPECT_EQ(kind(name).rfind("incremental", 0), 0u) << name << ": " << kind(name);
        EXPECT_EQ(cached("collide")->positions().data(), collided->positions().data());  // the very points
        EXPECT_EQ(cached("snow")->instances(), snowed->instances());                       // and instances
        EXPECT_NE(cached("snow"), snowed);
        ASSERT_TRUE(view.strandsChanges) << "the view says how its strands changed";
        EXPECT_TRUE(view.strandsChanges->pointsKept);
    }
    // A probability read from a channel the edit changes: the instances placed again.
    std::string byColor = edited;
    edit(byColor, R"("probability": 0.3)", R"("probability": {"attribute": "melanin", "min": 0.0, "max": 0.8})");
    session.setGraphJson(byColor);
    ASSERT_TRUE(session.evaluate(request).strands);
    const auto placedBefore = cached("snow")->instances();
    edit(byColor, R"("melanin": 0.55)", R"("melanin": 0.7)");
    session.setGraphJson(byColor);
    const ViewResult& recolored = session.evaluate(request);
    ASSERT_TRUE(recolored.strands);
    EXPECT_EQ(recolored.strands->contentHash(), freshView(session, request));
    EXPECT_EQ(kind("snow").rfind("full", 0), 0u) << kind("snow");
    EXPECT_NE(cached("snow")->instances(), placedBefore);
    EXPECT_EQ(kind("collide").rfind("incremental", 0), 0u) << kind("collide");
}

TEST(Incremental, PartsPosesAndDensityChanges) {
    // A part across the top (Interpolate and Clump keep to their side), guides posed (a
    // simulation's), the view's density changed mid-stroke (another set of strands: in full,
    // then incremental again), a point budget.
    const Verified verified;
    GroomData data = sphereGroom(kLookGraph, 24, 0);
    PartSet parts;
    std::vector<Vec3f> line;
    for (int i = 0; i <= 20; ++i) line.push_back(Vec3f(-0.8f + 0.08f * static_cast<float>(i), 1.0f, 0.1f).normalized());
    parts.add("top", line, *data.scalp, 40);
    data.partsJson = parts.toJson();
    GroomSession session(data);
    {
        const HierarchyPtr authored = session.authoredHierarchy();
        auto poses = std::make_shared<GuideSet>();
        for (std::size_t i = 0; i < authored->guides().size(); i += 3) {
            const HierarchyGuide& g = authored->guides()[i];
            std::vector<Vec3f> points = g.points;
            for (std::size_t k = 1; k < points.size(); ++k) points[k] += Vec3f(0.0f, 0.0f, 0.02f * static_cast<float>(k));
            poses->add(g.id, points);
        }
        session.setGuidePoses(poses);
    }
    SessionStrokeHost host(session);
    ViewRequest request;
    request.sculptDensity = 0.7f;
    request.pointBudget = 40000.0;
    auto densities = [&](int dab) {
        if (dab == 2) request.density = 0.8f;
        if (dab == 4) request.density = 1.0f;
    };
    EXPECT_GT(strokeAndCompare(session, host, request, {"comb", "lift"}, 6, densities), 20u);
}

// Bend combing, HairColor with a tip blend (per-point colors), Resample by curvature (each strand's count
// by its turning) and then evenly to fewer points (a new layout, kept from dab to dab).
const char* kShapeGraph = R"({"version": 1, "output": "out",
  "nodes": [
    {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 700.0}},
    {"name": "interp", "type": "Interpolate"},
    {"name": "comb", "type": "Bend", "params": {"angle": 25.0, "along": "direction", "direction": [1.0, -1.0, 0.0]}},
    {"name": "curvature", "type": "Resample", "params": {"segments": 12, "mode": "curvature", "maxAngle": 4.0}},
    {"name": "color", "type": "HairColor", "params": {"tipStrength": 0.4, "melanin": {"attribute": "clump"}, "melaninRandom": 0.2}},
    {"name": "even", "type": "Resample", "params": {"segments": 6, "mode": "even"}},
    {"name": "out", "type": "Output"}],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interp", "input": "strands"},
    {"from": "guides", "to": "interp", "input": "guides"},
    {"from": "scalp", "to": "interp", "input": "scalp"},
    {"from": "interp", "to": "comb", "input": "strands"},
    {"from": "comb", "to": "curvature", "input": "strands"},
    {"from": "curvature", "to": "color", "input": "strands"},
    {"from": "color", "to": "even", "input": "strands"},
    {"from": "even", "to": "out", "input": "strands"}]})";

TEST(Incremental, CombColorAndResampleFollowAStroke) {
    // Resample by curvature falls back when a dab changes a strand's count (then the next dabs
    // go on from there); evenly to another count it keeps the last result's layout.
    const Verified verified;
    GroomSession session(sphereGroom(kShapeGraph, 24, 2));
    SessionStrokeHost host(session);
    ViewRequest request;
    request.sculptDensity = 0.7f;
    EXPECT_GT(strokeAndCompare(session, host, request, {"comb", "curl", "straighten", "clump"}, 5), 40u);
}

TEST(Incremental, BandedGuardHairsAndIridescenceFollowAStroke) {
    // Bands over HairColor's tip (on guard hairs picked at random) and FeatherColor's iridescence
    // channel: each made again for the strands a dab moved, as a full evaluation makes them.
    std::string graph = kShapeGraph;
    const std::string link = R"({"from": "color", "to": "even", "input": "strands"})";
    graph.replace(graph.find(link), link.size(),
                  R"({"from": "color", "to": "bands", "input": "strands"}, {"from": "bands", "to": "sheen", "input": "strands"},
                     {"from": "sheen", "to": "even", "input": "strands"})");
    const std::string node = R"({"name": "even", "type": "Resample",)";
    graph.replace(graph.find(node), node.size(),
                  R"({"name": "bands", "type": "Bands", "params": {"strength": 0.8, "count": 3, "guards": 0.6, "undercoatDye": [0.6, 0.6, 0.6]}},
                     {"name": "sheen", "type": "FeatherColor", "params": {"pattern": "none", "iridescence": 0.5}},
                     )" + node);
    const Verified verified;
    GroomSession session(sphereGroom(graph.c_str(), 24, 2));
    SessionStrokeHost host(session);
    ViewRequest request;
    request.sculptDensity = 0.7f;
    EXPECT_GT(strokeAndCompare(session, host, request, {"comb", "clump"}, 4), 20u);
    const ViewResult& view = session.evaluate(ViewRequest{});
    ASSERT_TRUE(view.strands);
    EXPECT_TRUE(view.strands->pointChannel(kIridescenceChannel));
}

TEST(Incremental, TheDefaultGraphFollowsAStroke) {
    const Verified verified;
    std::string graph = defaultGraphJson();
    const std::string from = "\"type\": \"Scatter\"";
    graph.replace(graph.find(from), from.size(), from + ", \"params\": {\"density\": 700.0}");
    GroomSession session(sphereGroom(graph, 24, 0));
    SessionStrokeHost host(session);
    ViewRequest request;
    request.sculptDensity = 0.5f;
    EXPECT_GT(strokeAndCompare(session, host, request, {"comb", "length"}, 5), 16u);
}

// Interpolate, then Collide (the scalp and a collider mesh above it), Lift, Part, Flow (along an
// arrow), Smooth, Flyaway, Attribute (a map of the guides' clump value), Wind, Jiggle (the scalp
// moved over the last frames), RestoreLength (to Interpolate's lengths), Instance, and last Twist
// around the guides (which a stroke changes: in full).
const char* kDeformGraph = R"({"version": 1, "output": "out",
  "nodes": [
    {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"}, {"name": "collider", "type": "ColliderMesh"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 700.0}},
    {"name": "interp", "type": "Interpolate"},
    {"name": "collide", "type": "Collide", "params": {"offset": 0.04, "maxDepth": 0.4, "iterations": 3}},
    {"name": "lift", "type": "Bend", "params": {"angle": 15.0, "end": 0.0}},
    {"name": "part", "type": "Part", "params": {"width": 0.4}},
    {"name": "flow", "type": "Flow", "params": {"affect": 0.4}},
    {"name": "smooth", "type": "Smooth"},
    {"name": "flyaway", "type": "Flyaway", "params": {"fraction": 0.2}},
    {"name": "mask", "type": "Attribute", "params": {"name": "painted", "value": {"attribute": "clump"}}},
    {"name": "twist", "type": "Twist", "params": {"turns": 0.4}},
    {"name": "wind", "type": "Wind", "params": {"strength": 0.2}},
    {"name": "jiggle", "type": "Jiggle"},
    {"name": "restore", "type": "RestoreLength", "params": {"affect": 0.6}},
    {"name": "instance", "type": "Instance", "params": {"probability": 0.2}},
    {"name": "out", "type": "Output"}],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interp", "input": "strands"},
    {"from": "guides", "to": "interp", "input": "guides"},
    {"from": "scalp", "to": "interp", "input": "scalp"},
    {"from": "interp", "to": "collide", "input": "strands"},
    {"from": "scalp", "to": "collide", "input": "scalp"},
    {"from": "collider", "to": "collide", "input": "collider"},
    {"from": "collide", "to": "lift", "input": "strands"},
    {"from": "lift", "to": "part", "input": "strands"},
    {"from": "part", "to": "flow", "input": "strands"},
    {"from": "flow", "to": "smooth", "input": "strands"},
    {"from": "smooth", "to": "flyaway", "input": "strands"},
    {"from": "flyaway", "to": "mask", "input": "strands"},
    {"from": "mask", "to": "wind", "input": "strands"},
    {"from": "wind", "to": "jiggle", "input": "strands"},
    {"from": "jiggle", "to": "restore", "input": "strands"},
    {"from": "interp", "to": "restore", "input": "reference"},
    {"from": "restore", "to": "instance", "input": "strands"},
    {"from": "instance", "to": "twist", "input": "strands"},
    {"from": "guides", "to": "twist", "input": "axes"},
    {"from": "twist", "to": "out", "input": "strands"}]})";

TEST(Incremental, CollideAndTheDeformersFollowAStroke) {
    // Every node from Interpolate to Instance follows a stroke's changes: each dab after a press
    // runs the 12 of them incrementally (Interpolate, Collide, Lift, Part, Flow, Smooth, Flyaway,
    // Attribute, Wind, Jiggle, RestoreLength, Instance), and every view is a fresh session's.
    // Twist's axes are the guides the stroke moves: it evaluates in full.
    const Verified verified;
    GroomData data = sphereGroom(kDeformGraph, 24, 2);
    PartSet parts;
    std::vector<Vec3f> line;
    for (int i = 0; i <= 20; ++i) line.push_back(Vec3f(-0.8f + 0.08f * static_cast<float>(i), 1.0f, 0.1f).normalized());
    parts.add("top", line, *data.scalp, 40);
    data.partsJson = parts.toJson();
    FlowSet flow;
    const Vec3f at = Vec3f(0.2f, 1.0f, 0.3f).normalized();
    flow.pins.push_back(FlowSet::makePin(*data.scalp, data.scalp->closestPoint(at), Vec3f(1.0f, 0.0f, -0.2f)));
    data.flowJson = flow.toJson();
    std::vector<Vec3f> points = test::makeSphere(10, 20, 0.45f)->points();
    for (Vec3f& p : points) p += Vec3f(0.3f, 1.35f, 0.2f);
    data.meshes["collider"] = std::make_shared<const ScalpMesh>(std::move(points), test::makeSphere(10, 20, 0.45f)->triangles());
    std::vector<std::vector<Vec3f>> frames;
    for (int k = 1; k <= 4; ++k) {
        std::vector<Vec3f> was = data.scalp->points();
        for (Vec3f& p : was) p += Vec3f(0.02f * static_cast<float>(k), 0.0f, -0.01f * static_cast<float>(k * k));
        frames.push_back(std::move(was));
    }
    data.scalpHistory = ScalpHistory::make(1.0 / 24.0, std::move(frames));
    GroomSession session(data);
    SessionStrokeHost host(session);
    ViewRequest request;
    request.sculptDensity = 0.7f;
    const std::vector<std::string> modes = {"comb", "slide", "clump", "length", "smooth"};
    const std::size_t incremental = strokeAndCompare(session, host, request, modes, 5);
    EXPECT_GE(incremental, modes.size() * 4u * 12u);
}

// A host that keeps its strand lines' buffers and writes again only the changed strands' vertex
// ranges when an item says so (DisplayItem::updated), hiding a kept item, and keeps the lit
// strands' own colors while their key stays (writing the changed strands' when it moves, as
// Maya's override does): after every update, each shown item's buffers must be what writing its
// lines whole gives.
struct PartialHost {
    struct Buffers {
        std::vector<float> positions, normals, tangents, colors;
        std::uint64_t layout = 0, colorsKey = 0;
        bool hidden = false;
    };
    DisplayBuilder builder;
    DisplaySettings settings;
    std::map<std::string, Buffers> buffers;
    std::size_t partial = 0, whole = 0, kept = 0;
    std::size_t colorsKept = 0, colorsInPart = 0;  // partial writes that kept the colors, wrote some

    PartialHost() {
        builder.setRetain(false);
        builder.setWriteLines(true);
        settings.sculptDisplay = 1;  // dimmed while a stroke is live (the default draws them as they are)
    }
    void draw(const ViewResult& view, bool stroke) {
        const DisplayFrame& frame = builder.update(DisplayInputs::fromView(view, stroke), settings);
        for (const DisplayItem& item : frame.items) {
            if (!item.rebuilt) continue;
            Buffers& b = buffers[item.name];
            if (item.keep) {
                b.hidden = true;
                ++kept;
                continue;
            }
            if (!item.geometry || !item.geometry->lines) {
                b = Buffers{};
                continue;
            }
            const StrandLines& lines = *item.geometry->lines;
            const std::size_t n = lines.vertexCount();
            b.hidden = false;
            const StrandColors* colors = lines.colors.get();
            if (item.updated && b.layout == lines.layoutKey() && b.positions.size() == n * 3 && (!lines.lit || b.normals.size() == n * 3) &&
                (colors ? b.colors.size() == n * 4 : b.colors.empty())) {
                const bool recolor = colors && colors->key != b.colorsKey;
                for (const auto& [first, end] : strandRuns(item.updated->strands, 8)) {
                    const std::size_t begin = lines.strands->strandBegin(first), at = begin * 3;
                    lines.writePositions(b.positions.data() + at, first, end);
                    if (lines.lit) lines.writeFrames(b.normals.data() + at, b.tangents.data() + at, first, end);
                    if (recolor) lines.writeColors(b.colors.data() + begin * 4, first, end);
                }
                ++partial;
                if (colors) {
                    ++(recolor ? colorsInPart : colorsKept);
                    b.colorsKey = colors->key;
                }
            } else {
                b.positions.assign(n * 3, 0.0f);
                lines.writePositions(b.positions.data());
                b.normals.clear();
                b.tangents.clear();
                if (lines.lit) {
                    for (auto* v : {&b.normals, &b.tangents}) v->assign(n * 3, 0.0f);
                    lines.writeFrames(b.normals.data(), b.tangents.data());
                }
                b.colors.assign(colors ? n * 4 : 0, 0.0f);
                if (colors) lines.writeColors(b.colors.data());
                b.colorsKey = colors ? colors->key : 0;
                b.layout = lines.layoutKey();
                ++whole;
            }
            // What writing it whole gives.
            std::vector<float> positions(n * 3), normals(lines.lit ? n * 3 : 0), tangents(normals.size());
            std::vector<float> rgba(colors ? n * 4 : 0);
            lines.writePositions(positions.data());
            if (lines.lit) lines.writeFrames(normals.data(), tangents.data());
            if (colors) lines.writeColors(rgba.data());
            EXPECT_TRUE(positions == b.positions) << item.name;
            EXPECT_TRUE(normals == b.normals && tangents == b.tangents) << item.name;
            EXPECT_TRUE(rgba == b.colors) << item.name;
        }
    }
};

TEST(Incremental, AViewportWritesOnlyTheChangedStrandsAndHoldsWhatAWholeWriteGives) {
    // Strokes with the strands dimmed while they last (sculptDisplay 1) and lit after: the dimmed
    // strands are written whole at the first press and in part after; the lit ones are kept hidden
    // through a stroke and written again in part at its release.
    const Verified verified;
    GroomSession session(sphereGroom(kLookGraph, 24, 0));
    SessionStrokeHost host(session);
    ViewRequest request;
    request.sculptDensity = 0.6f;
    PartialHost viewport;
    viewport.draw(session.evaluate(request), false);
    const ToolView view = ToolView::fromMatrix(test::lookAtProjection({0.0f, 1.8f, 4.5f}, {0.0f, 0.8f, 0.0f}, 40.0, 1.5), 1200, 800);
    float x0 = 0.0f, y0 = 0.0f;
    ASSERT_TRUE(view.project(Vec3f(0.0f, 1.0f, 0.35f).normalized(), x0, y0));
    for (int stroke = 0; stroke < 3; ++stroke) {
        BrushTool tool;
        tool.settings.setMode(stroke == 1 ? "smooth" : "comb");
        tool.settings.screenRadius = 90.0f;
        const float sx = x0 - 40.0f + 15.0f * static_cast<float>(stroke);
        ASSERT_TRUE(tool.begin(host, view, sx, y0, 1.0f));
        viewport.draw(session.evaluate(request), session.hasPreview());
        for (int k = 1; k < 5; ++k) {
            tool.moveTo(host, view, sx + 9.0f * static_cast<float>(k), y0, 1.0f);
            viewport.draw(session.evaluate(request), session.hasPreview());
        }
        tool.end(host, view);
        viewport.draw(session.evaluate(request), session.hasPreview());
    }
    // Lit strands whole once, dimmed ones whole at the first press; the rest in part.
    EXPECT_EQ(viewport.whole, 2u);
    EXPECT_GE(viewport.partial, 3u * 5u);
    EXPECT_EQ(viewport.kept, 6u);  // lit ones at each press, dimmed ones at each release
}

TEST(Incremental, LitStrandsKeepTheirOwnColorsThroughAStroke) {
    // The look graph colored by HairColor (varied per strand, blended toward a tip color along the
    // strands by a mask of the points' height, so a dab that moves strands changes their colors):
    // the lit strands carry their colors (strandLookColors), drawn as ever through strokes (the
    // sculpt display off): each dab writes the changed strands' points, frames and colors, and
    // the host's buffers hold what writing them whole gives.
    const Verified verified;
    std::string graph = kLookGraph;
    const std::string from = R"j({"from": "resample", "to": "out", "input": "strands"})j";
    graph.replace(graph.find(from), from.size(),
                  R"j({"from": "resample", "to": "color", "input": "strands"}, {"from": "color", "to": "out", "input": "strands"})j");
    const std::string node = R"j({"name": "out", "type": "Output"})j";
    graph.replace(graph.find(node), node.size(),
                  R"j({"name": "color", "type": "HairColor", "params": {"melaninRandom": 0.3, "tipColor": [0.9, 0.9, 0.2],
                       "tipStrength": {"expression": "smoothstep(0.6, 1.4, py)", "max": 0.7}}}, )j" + node);
    GroomSession session(sphereGroom(graph, 24, 0));
    SessionStrokeHost host(session);
    ViewRequest request;
    request.sculptDensity = 1.0f;
    PartialHost viewport;
    viewport.settings.sculptDisplay = 0;
    const ViewResult& first = session.evaluate(request);
    ASSERT_TRUE(first.strands->pointChannel(kColorChannels[0])) << "HairColor writes per-point colors";
    viewport.draw(first, false);
    ASSERT_EQ(viewport.buffers["strands"].colors.size(), first.strands->pointCount() * 4);
    const ToolView view = ToolView::fromMatrix(test::lookAtProjection({0.0f, 1.8f, 4.5f}, {0.0f, 0.8f, 0.0f}, 40.0, 1.5), 1200, 800);
    float x0 = 0.0f, y0 = 0.0f;
    ASSERT_TRUE(view.project(Vec3f(0.0f, 1.0f, 0.35f).normalized(), x0, y0));
    for (int stroke = 0; stroke < 2; ++stroke) {
        BrushTool tool;
        tool.settings.setMode("comb");
        tool.settings.screenRadius = 90.0f;
        const float sx = x0 - 40.0f + 15.0f * static_cast<float>(stroke);
        ASSERT_TRUE(tool.begin(host, view, sx, y0, 1.0f));
        viewport.draw(session.evaluate(request), session.hasPreview());
        for (int k = 1; k < 4; ++k) {
            tool.moveTo(host, view, sx + 9.0f * static_cast<float>(k), y0, 1.0f);
            viewport.draw(session.evaluate(request), session.hasPreview());
        }
        tool.end(host, view);
        viewport.draw(session.evaluate(request), session.hasPreview());
    }
    EXPECT_EQ(viewport.whole, 1u);
    EXPECT_GE(viewport.colorsInPart, 4u) << "dabs write the changed strands' colors";
    EXPECT_EQ(viewport.partial, viewport.colorsInPart + viewport.colorsKept);
}

TEST(Incremental, APreviewHoldingHiddenCoreStrandsFollowsStrokes) {
    // A preview under its core share (preview_core.h: 33k strands, a core of a quarter, the view at
    // 0.1 and its strokes at 0.06) evaluates the core strands it does not show and hides them: its
    // strokes are as incremental and exact as any, and the changes it hands the viewport are the
    // shown strands' (written in part, what a whole write gives).
    const Verified verified;
    std::string graph = kLookGraph;
    const std::string density = R"("density": 700.0)";
    graph.replace(graph.find(density), density.size(), R"("density": 2600.0)");
    GroomSession session(sphereGroom(graph, 24, 3));
    SessionStrokeHost host(session);
    ViewRequest request;
    request.density = 0.1f;
    request.sculptDensity = 0.6f;
    const ViewResult& first = session.evaluate(request);
    ASSERT_TRUE(first.strands);
    EXPECT_GT(first.strands->strandCount(), 2500u);  // a tenth of about 33k
    EXPECT_LT(first.strands->strandCount(), 4000u);
    EXPECT_GE(strokeAndCompare(session, host, request, {"comb", "clump", "smooth"}, 4), 3u * 3u * 7u);
    PartialHost viewport;
    viewport.draw(session.evaluate(request), false);
    const ToolView view = ToolView::fromMatrix(test::lookAtProjection({0.0f, 1.8f, 4.5f}, {0.0f, 0.8f, 0.0f}, 40.0, 1.5), 1200, 800);
    float x0 = 0.0f, y0 = 0.0f;
    ASSERT_TRUE(view.project(Vec3f(0.0f, 1.0f, 0.35f).normalized(), x0, y0));
    BrushTool tool;
    tool.settings.setMode("comb");
    tool.settings.screenRadius = 90.0f;
    ASSERT_TRUE(tool.begin(host, view, x0, y0, 1.0f));
    viewport.draw(session.evaluate(request), session.hasPreview());
    for (int k = 1; k < 5; ++k) {
        tool.moveTo(host, view, x0 + 9.0f * static_cast<float>(k), y0, 1.0f);
        viewport.draw(session.evaluate(request), session.hasPreview());
    }
    tool.end(host, view);
    viewport.draw(session.evaluate(request), session.hasPreview());
    EXPECT_GE(viewport.partial, 4u);
}

TEST(Incremental, WarmedReleasedAndForgottenStrokesDrawWhatAWholeWriteDraws) {
    // A host warming the stroke document between strokes, then putting the brush away (the stroke
    // document released, the dimmed strands' buffers let go: forgetKept) and taking it up again:
    // every item holds what writing it whole gives, the dimmed strands written whole again.
    const Verified verified;
    GroomSession session(sphereGroom(kLookGraph, 24, 0));
    SessionStrokeHost host(session);
    ViewRequest request;
    request.sculptDensity = 0.6f;
    PartialHost viewport;
    viewport.draw(session.evaluate(request), false);
    const ToolView view = ToolView::fromMatrix(test::lookAtProjection({0.0f, 1.8f, 4.5f}, {0.0f, 0.8f, 0.0f}, 40.0, 1.5), 1200, 800);
    float x0 = 0.0f, y0 = 0.0f;
    ASSERT_TRUE(view.project(Vec3f(0.0f, 1.0f, 0.35f).normalized(), x0, y0));
    EXPECT_FALSE(viewport.builder.forgetKept(DisplayItemId::DimStrands)) << "nothing kept yet";
    for (int stroke = 0; stroke < 4; ++stroke) {
        if (stroke == 2) {
            session.releaseStroke();
            EXPECT_TRUE(viewport.builder.forgetKept(DisplayItemId::DimStrands));
            EXPECT_FALSE(viewport.builder.forgetKept(DisplayItemId::DimStrands)) << "once";
            viewport.buffers.erase("dimStrands");
        }
        session.warmStroke(request);
        BrushTool tool;
        tool.settings.setMode("comb");
        tool.settings.screenRadius = 90.0f;
        const float sx = x0 - 40.0f + 15.0f * static_cast<float>(stroke);
        ASSERT_TRUE(tool.begin(host, view, sx, y0, 1.0f));
        viewport.draw(session.evaluate(request), session.hasPreview());
        for (int k = 1; k < 4; ++k) {
            tool.moveTo(host, view, sx + 9.0f * static_cast<float>(k), y0, 1.0f);
            viewport.draw(session.evaluate(request), session.hasPreview());
        }
        tool.end(host, view);
        const ViewResult& seen = session.evaluate(request);
        EXPECT_EQ(seen.strands->contentHash(), freshView(session, request)) << "stroke " << stroke;
        viewport.draw(seen, session.hasPreview());
    }
    // Lit strands whole once; dimmed ones whole at the first press and after they were let go.
    EXPECT_EQ(viewport.whole, 3u);
}

TEST(Incremental, DimmedStrandsAGpuWroteAreWrittenWholeAtTheNextPress) {
    // A host whose GPU preview wrote a stroke's dimmed strands in place (Maya's under OpenGL) says
    // so at the release (DisplayBuilder::forgetHeld): the item stays kept hidden, and the next
    // stroke's press writes all its points (the host's check: what a whole write gives).
    const Verified verified;
    GroomSession session(sphereGroom(kLookGraph, 24, 0));
    SessionStrokeHost host(session);
    ViewRequest request;
    request.sculptDensity = 0.6f;
    PartialHost viewport;
    viewport.draw(session.evaluate(request), false);
    const ToolView view = ToolView::fromMatrix(test::lookAtProjection({0.0f, 1.8f, 4.5f}, {0.0f, 0.8f, 0.0f}, 40.0, 1.5), 1200, 800);
    float x0 = 0.0f, y0 = 0.0f;
    ASSERT_TRUE(view.project(Vec3f(0.0f, 1.0f, 0.35f).normalized(), x0, y0));
    std::size_t kept = 0;
    for (int stroke = 0; stroke < 2; ++stroke) {
        BrushTool tool;
        tool.settings.setMode("comb");
        tool.settings.screenRadius = 90.0f;
        const float sx = x0 - 40.0f + 15.0f * static_cast<float>(stroke);
        ASSERT_TRUE(tool.begin(host, view, sx, y0, 1.0f));
        viewport.draw(session.evaluate(request), session.hasPreview());
        tool.moveTo(host, view, sx + 9.0f, y0, 1.0f);
        // The GPU's dab: the dimmed strands' points written by someone else.
        for (float& p : viewport.buffers["dimStrands"].positions) p += 0.5f;
        tool.end(host, view);
        kept = viewport.kept;
        viewport.builder.forgetHeld(DisplayItemId::DimStrands);
        viewport.draw(session.evaluate(request), session.hasPreview());
        EXPECT_EQ(viewport.kept, kept + 1) << "the dimmed strands kept hidden at the release";
    }
    EXPECT_GE(viewport.whole, 3u);  // the dimmed strands whole at the second press
}

TEST(Incremental, EveryShippedPresetFollowsAStrokeExactly) {
    // Every shipped preset, and the tests' own (tests/data/presets), on the reference groom
    // (PresetLooks': tests/data/strokes, a unit sphere with guides, the preset fitted to its
    // size): a comb stroke over the guides, every node's
    // incremental result checked against a full evaluation (Verified) and the view against a
    // fresh session's, bit for bit, after the press, each dab and the release.
    const Verified verified;
    const GroomFile reference = readGroomFile(std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom");
    ASSERT_TRUE(reference.data.scalp);
    const std::optional<double> size = presetScalpSize(*reference.data.scalp);
    const std::string shipped = GROOMLAB_SHIPPED_PRESETS;
    const PathResolver paths = searchPathResolver({reference.directory, shipped});
    const ToolView view = ToolView::fromMatrix(test::lookAtProjection({0.0f, 1.8f, 4.5f}, {0.0f, 0.8f, 0.0f}, 40.0, 1.5), 1200, 800);
    float x0 = 0.0f, y0 = 0.0f;
    ASSERT_TRUE(view.project(Vec3f(0.3f, 1.0f, 0.2f).normalized(), x0, y0));
    ViewRequest request;
    request.density = 0.05f;
    std::size_t incremental = 0, presets = 0;
    for (const PresetInfo& p : listPresets({shipped, std::string(GROOMLAB_TEST_DATA) + "/presets"})) {
        std::ifstream in(std::filesystem::u8path(p.path), std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        GroomData data = reference.data;
        data.graphJson = presetGraphJson(text.str(), size);
        GroomSession session(data);
        session.setPathResolver(paths);
        if (!session.evaluate(request).errors.empty()) continue;  // (painted_maps: a map the reference lacks)
        ++presets;
        auto fresh = [&] {
            GroomSession again(session.data());
            again.setPathResolver(paths);
            if (const auto preview = session.previewHierarchy()) again.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*preview));
            const ViewResult& made = again.evaluate(request);
            return made.strands ? made.strands->contentHash() : 0;
        };
        auto check = [&](const std::string& what) {
            const ViewResult& seen = session.evaluate(request);
            EXPECT_TRUE(seen.errors.empty()) << p.name << " " << what;
            ASSERT_TRUE(seen.strands) << p.name << " " << what;
            EXPECT_EQ(seen.strands->contentHash(), fresh()) << p.name << " " << what;
        };
        SessionStrokeHost host(session);
        const std::size_t before = session.incrementalCount();
        BrushTool tool;
        tool.settings.setMode("comb");
        tool.settings.screenRadius = 120.0f;
        tool.settings.brush.strength = 0.8f;
        if (!tool.begin(host, view, x0 - 40.0f, y0, 1.0f)) {
            ADD_FAILURE() << p.name << ": the stroke reached no guide";
            continue;
        }
        check("press");
        for (int k = 1; k < 3; ++k) {
            tool.moveTo(host, view, x0 - 40.0f + 20.0f * static_cast<float>(k), y0 + 4.0f * static_cast<float>(k), 1.0f);
            check("dab " + std::to_string(k));
        }
        tool.end(host, view);
        check("release");
        incremental += session.incrementalCount() - before;
    }
    EXPECT_GT(presets, 30u);
    EXPECT_GT(incremental, presets) << "the strokes evaluated nothing incrementally: the test checks little";
}

// held -> diff -> transform -> direction -> bend -> cut (a collider) -> render -> clump (guides as
// centers, varied per clump) -> out, on strands scattered on a sphere (rooted, so each node's
// root frame is the scalp's).
std::unique_ptr<Chain> makeParityChain() {
    auto chain = std::make_unique<Chain>();
    Graph& g = chain->graph;
    auto add = [&](const std::string& name, std::unique_ptr<Operator> op) {
        chain->ids[name] = g.addNode(std::move(op));
        g.setLabel(chain->ids[name], name);
    };
    auto& registry = OperatorRegistry::instance();
    add("held", std::make_unique<HeldStrandsOp>());
    add("diff", std::make_unique<DiffOp>());
    for (const char* type : {op::kTransform, op::kDirection, op::kBend, op::kMeshCut, op::kRenderData, op::kHairColor, op::kClump, op::kGuides,
                             op::kColliderMesh, op::kOutput}) {
        add(type, registry.create(type));
    }
    chain->node(op::kTransform).params().set("rotateRandom", Vec3f{5.0f, 20.0f, 0.0f});
    chain->node(op::kTransform).params().set("scaleRandom", 0.2);
    chain->node(op::kDirection).params().set("strength", 0.5);
    chain->node(op::kDirection).params().set("rollRandom", 40.0);
    chain->node(op::kBend).params().set("angle", -50.0);
    chain->node(op::kBend).params().set("randomness", 0.5);
    chain->node(op::kColliderMesh).params().set("name", std::string("collider"));
    chain->node(op::kRenderData).params().set("opacityProfile", Ramp::linear(1.0f, 0.3f));
    MapRef byLength;
    byLength.source = MapRef::Source::Expression;
    byLength.name = "length";  // reads each strand's points: the strands a dab moved are written again
    chain->node(op::kRenderData).params().set("opacity", byLength);
    chain->node(op::kHairColor).params().set("roughness", byLength);
    chain->node(op::kHairColor).params().set("specularRandom", 0.3);
    chain->node(op::kHairColor).params().set("tilt", 4.0);
    chain->node(op::kClump).params().set("volumize", 0.5);
    chain->node(op::kClump).params().set("strengthRandom", 0.5);
    chain->node(op::kClump).params().set("copyAmount", 0.5);
    chain->node(op::kClump).params().set("centersFrom", std::string("inputPoints"));  // the guides below
    const char* order[] = {"held", "diff", op::kTransform, op::kDirection, op::kBend, op::kMeshCut, op::kRenderData, op::kHairColor, op::kClump,
                           op::kOutput};
    for (std::size_t k = 0; k + 1 < std::size(order); ++k) g.connect(chain->ids[order[k]], chain->ids[order[k + 1]], 0);
    g.connect((*chain)[op::kColliderMesh], (*chain)[op::kMeshCut], 1);
    g.connect((*chain)[op::kGuides], (*chain)[op::kClump], 1);
    chain->ids["out"] = chain->ids[op::kOutput];
    const auto scalp = test::makeSphere(16, 32, 0.9f);
    chain->ext.set("scalp", MeshPtr(scalp), scalp->contentHash());
    std::vector<Vec3f> points = test::makeSphere(10, 20, 0.5f)->points();
    for (Vec3f& p : points) p += Vec3f(0.3f, 1.2f, 0.1f);
    const auto collider = std::make_shared<const ScalpMesh>(std::move(points), test::makeSphere(10, 20, 0.5f)->triangles());
    chain->ext.set("mesh:collider", MeshPtr(collider), collider->contentHash());
    auto centers = std::make_shared<GuideSet>();
    Rng rng(5);
    for (std::uint64_t i = 0; i < 12; ++i) {
        const Vec3f root = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized() * 0.9f;
        centers->add(i + 1, test::straightCurve(root, root * 0.5f, 6));
    }
    chain->ext.set("guides", GuidesPtr(centers), 0xC3E7E25ull);
    return chain;
}

// Strands scattered on the parity chain's sphere, with the "mask" channel the dabs repaint.
StrandsPtr scatteredOnSphere() {
    Graph g;
    ExternalInputs ext;
    const auto scalp = test::makeSphere(16, 32, 0.9f);
    ext.set("scalp", MeshPtr(scalp), scalp->contentHash());
    const NodeId scalpNode = g.addNode(op::kScalp), scatter = g.addNode(op::kScatter);
    g.node(scatter).params().set("density", 60.0);
    g.connect(scalpNode, scatter, 0);
    EvalContext ctx;
    ctx.externals = &ext;
    auto strands = std::make_shared<StrandSet>(*g.evaluateStrands(scatter, ctx));
    strands->addStrandChannel("mask", 0.5f);
    return strands;
}

TEST(Incremental, TheParityNodesFollowRandomDabs) {
    // Transform, Direction, Bend, MeshCut (its collider the same), RenderData and HairColor (a map reading each
    // strand's points) and a Clump varied per clump (volumize, variance, copy, clump noise) make
    // again only the strands a dab moved: every result a fresh chain's.
    const Verified verified;
    auto chain = makeParityChain();
    StrandsPtr strands = scatteredOnSphere();
    ASSERT_GT(strands->strandCount(), 300u);
    ASSERT_EQ(chain->run(strands)->contentHash(), makeParityChain()->run(strands)->contentHash());
    Rng rng(21);
    for (int dab = 0; dab < 8; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 1 + static_cast<std::size_t>(rng.nextFloat() * 30.0f), rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), makeParityChain()->run(strands)->contentHash()) << "dab " << dab;
        // TestDiff and the 7 operators after it (Output hands its input on).
        EXPECT_EQ(chain->graph.incrementalCount() - before, 8u) << "dab " << dab;
    }
}

// Fur grown from one guide group, whiskers made of another (ToStrands), merged, then a Width:
// the wolf's layout (Guides by group, Merge, a node after it).
const char* kTwoGroupGraph = R"({"version": 1, "output": "out",
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "furGuides", "type": "Guides", "params": {"group": "fur"}},
    {"name": "scatter", "type": "Scatter", "params": {"density": 400.0}},
    {"name": "interp", "type": "Interpolate"},
    {"name": "whiskerGuides", "type": "Guides", "params": {"group": "whiskers"}},
    {"name": "toStrands", "type": "ToStrands"},
    {"name": "whiskerWidth", "type": "Width", "params": {"width": 0.002}},
    {"name": "merge", "type": "Merge"},
    {"name": "width", "type": "Width", "params": {"width": 0.004}},
    {"name": "out", "type": "Output"}],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interp", "input": "strands"},
    {"from": "furGuides", "to": "interp", "input": "guides"},
    {"from": "scalp", "to": "interp", "input": "scalp"},
    {"from": "whiskerGuides", "to": "toStrands", "input": "guides"},
    {"from": "toStrands", "to": "whiskerWidth", "input": "strands"},
    {"from": "interp", "to": "merge", "input": "a"},
    {"from": "whiskerWidth", "to": "merge", "input": "b"},
    {"from": "merge", "to": "width", "input": "strands"},
    {"from": "width", "to": "out", "input": "strands"}]})";

GuideHierarchy twoGroups(const ScalpMesh& scalp) {
    GuideHierarchy h;
    const GroupId fur = h.addGroup("fur"), whiskers = h.addGroup("whiskers");
    Rng rng(9);
    for (int i = 0; i < 24; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(0.2f, 1), rng.uniform(-1, 1)).normalized();
        h.addGuide(fur, test::straightCurve(n, n * 0.6f + Vec3f(0.1f, -0.1f, 0.0f), 6));
    }
    for (int i = 0; i < 4; ++i) {
        const Vec3f n = Vec3f(0.3f * static_cast<float>(i) - 0.45f, 0.4f, 1.0f).normalized();
        h.addGuide(whiskers, test::straightCurve(n, n * 0.9f, 8));
    }
    h.evaluate(&scalp);
    // Bound again from where they sit, as a brushed guide is (setPoints): the roots an edit binds
    // them to from then on, to the bit (bound from the curves as drawn, the first time, they may
    // differ in the last bits).
    for (const HierarchyGuide& g : std::vector<HierarchyGuide>(h.guides())) h.setPoints(g.id, g.points);
    h.evaluate(&scalp);
    return h;
}

// The hierarchy with every guide of `group` moved a little (more toward the tip).
std::shared_ptr<GuideHierarchy> movedGroup(const GuideHierarchy& from, const std::string& group, float by) {
    auto h = std::make_shared<GuideHierarchy>(from);
    const GroupId id = h->findGroup(group);
    for (const HierarchyGuide& g : from.guides()) {
        if (g.group != id) continue;
        std::vector<Vec3f> points = g.points;
        for (std::size_t k = 1; k < points.size(); ++k) points[k] += Vec3f(by * static_cast<float>(k), 0.0f, 0.0f);
        h->setPoints(g.id, std::move(points));
    }
    return h;
}

TEST(Incremental, AGuidesNodeIsKeyedByItsOwnGroups) {
    // A dab on the whiskers' guides (a brush's preview guides) leaves the fur's Guides node and its
    // branch cached (their key is the fur group's guides, not the whole hierarchy's), and the other
    // way round; Merge and the Width after it go on incrementally, writing the side that changed.
    const Verified verified;
    const auto scalp = test::makeSphere(24, 48, 1.0f);
    const GuideHierarchy base = twoGroups(*scalp);
    GroomData data;
    data.scalp = scalp;
    data.graphJson = kTwoGroupGraph;
    GroomSession session(data);
    session.setHierarchy(std::make_shared<const GuideHierarchy>(base));
    // A fresh session's view of the same guides (every node in full).
    auto fresh = [&](const std::shared_ptr<GuideHierarchy>& h) {
        GroomSession other(data);
        other.setHierarchy(std::make_shared<const GuideHierarchy>(base));
        other.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*h));
        const ViewResult& view = other.evaluate(ViewRequest{});
        EXPECT_TRUE(view.errors.empty());
        return view.strands ? view.strands->contentHash() : 0;
    };
    ViewRequest request;
    const ViewResult& first = session.evaluate(request);
    ASSERT_TRUE(first.errors.empty()) << first.errors[0];
    const std::size_t whiskerStrands = 4, all = first.strands->strandCount();
    ASSERT_GT(all, 10 * whiskerStrands);

    auto runOf = [&](const std::string& node) {
        const auto runs = session.stats().runs;
        const auto it = runs.find(node);
        return it == runs.end() ? std::string() : it->second;
    };
    // The whiskers moved: their branch made again; the fur's is not looked at past Merge's input.
    // The guides as the session shows them (a stroke's preview starts from them), previewed as
    // they are first (the press).
    const GuideHierarchy shown = *session.hierarchy();
    session.setPreviewHierarchy(std::make_shared<GuideHierarchy>(shown));
    ASSERT_TRUE(session.evaluate(request).errors.empty());
    std::size_t evaluations = session.evaluationCount();
    const auto whiskersMoved = movedGroup(shown, "whiskers", 0.01f);
    session.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*whiskersMoved));
    const ViewResult& whiskers = session.evaluate(request);
    ASSERT_TRUE(whiskers.errors.empty()) << whiskers.errors[0];
    EXPECT_EQ(whiskers.strands->contentHash(), fresh(whiskersMoved));
    EXPECT_EQ(runOf("merge"), "incremental " + std::to_string(whiskerStrands)) << "the whiskers' side, every strand of it";
    EXPECT_EQ(runOf("width"), "incremental " + std::to_string(whiskerStrands));
    EXPECT_EQ(runOf("whiskerGuides").rfind("full", 0), 0u) << runOf("whiskerGuides");
    // whiskerGuides, toStrands, whiskerWidth, merge, width (and Output).
    EXPECT_LE(session.evaluationCount() - evaluations, 6u) << "the fur's branch stays cached";

    // The fur moved: the whiskers' branch cached, Merge writes the fur strands Interpolate made again.
    evaluations = session.evaluationCount();
    const auto furMoved = movedGroup(*whiskersMoved, "fur", 0.005f);
    session.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*furMoved));
    const ViewResult& fur = session.evaluate(request);
    ASSERT_TRUE(fur.errors.empty()) << fur.errors[0];
    EXPECT_EQ(fur.strands->contentHash(), fresh(furMoved));
    EXPECT_EQ(runOf("whiskerWidth"), "cached");
    EXPECT_EQ(runOf("furGuides").rfind("full", 0), 0u) << runOf("furGuides");
    EXPECT_EQ(runOf("merge").rfind("incremental", 0), 0u) << runOf("merge");
    EXPECT_LE(session.evaluationCount() - evaluations, 6u) << "furGuides, interp, merge, width (the whiskers' branch cached)";

    // A whisker added: Merge's sides no longer hold the strands its last result held (in full).
    auto added = std::make_shared<GuideHierarchy>(*furMoved);
    const Vec3f n = Vec3f(0.6f, 0.3f, 1.0f).normalized();
    added->addGuide(added->findGroup("whiskers"), test::straightCurve(n, n * 0.8f, 8));
    added->evaluate(scalp.get());
    session.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*added));
    const ViewResult& more = session.evaluate(request);
    ASSERT_TRUE(more.errors.empty()) << more.errors[0];
    EXPECT_EQ(more.strands->strandCount(), all + 1);
    EXPECT_EQ(more.strands->contentHash(), fresh(added));
    EXPECT_EQ(runOf("merge").rfind("full", 0), 0u) << runOf("merge");
}

// Hands on the strands the host set as the external "other" (a second side's).
class HeldOtherOp : public HeldStrandsOp {
public:
    std::string typeName() const override { return "TestHeldOther"; }
    std::vector<std::string> externalInputs() const override { return {"other"}; }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>&) const override { return ctx.external("other"); }
};

TEST(Incremental, MergeWritesASideMadeAnewAsThatSidesStrands) {
    // One side of a Merge made in full (a node of its branch edited: no list of its changes) while
    // the other is unchanged: Merge lists that side's strands, as they are the strands it held, so
    // the node after it goes on incrementally. A side holding most strands merges in full (no
    // faster), so does one holding other strands (a count).
    const Verified verified;
    Graph g;
    std::map<std::string, NodeId> ids;
    auto& registry = OperatorRegistry::instance();
    auto add = [&](const std::string& name, std::unique_ptr<Operator> op) {
        ids[name] = g.addNode(std::move(op));
        g.setLabel(ids[name], name);
    };
    add("held", std::make_unique<HeldStrandsOp>());
    add("other", std::make_unique<HeldOtherOp>());
    add("noiseA", registry.create(op::kNoise));
    add("noiseB", registry.create(op::kNoise));
    add("merge", registry.create(op::kMerge));
    add("width", registry.create(op::kWidth));
    g.connect(ids["held"], ids["noiseA"], 0);
    g.connect(ids["noiseA"], ids["merge"], 0);
    g.connect(ids["other"], ids["noiseB"], 0);
    g.connect(ids["noiseB"], ids["merge"], 1);
    g.connect(ids["merge"], ids["width"], 0);
    g.node(ids["noiseA"]).params().set("amplitude", 0.01);
    g.node(ids["noiseB"]).params().set("amplitude", 0.01);
    g.node(ids["width"]).params().set("width", 0.003);

    // a: 12 strands (IDs from 1000), b: 200 of their own IDs (none renamed by the merge).
    const auto a = someStrands(12, 5);
    auto b = std::make_shared<StrandSet>(*someStrands(200, 6));
    for (std::size_t s = 0; s < b->strandCount(); ++s) b->mutableIds()[s] = 5000 + s;
    ExternalInputs ext;
    EvalContext ctx;
    ctx.externals = &ext;
    auto run = [&](const StrandsPtr& sideA, const StrandsPtr& sideB) {
        ext.set("strands", sideA, sideA->contentHash());
        ext.set("other", sideB, sideB->contentHash());
        // (Each incremental result checked against the operator's full one: Verified.)
        return g.evaluateStrands(ids["width"], ctx);
    };
    auto kind = [&](const char* node) { return g.lastRunKinds().at(ids[node]); };
    ASSERT_TRUE(run(a, b));

    // a's Noise edited: a made anew (12 of 212 strands), b unchanged.
    g.node(ids["noiseA"]).params().set("amplitude", 0.02);
    run(a, b);
    EXPECT_EQ(kind("noiseA").rfind("full", 0), 0u) << kind("noiseA");
    EXPECT_EQ(kind("merge"), "incremental 12");
    EXPECT_EQ(kind("width"), "incremental 12");

    // b's Noise edited: b made anew, but it holds most strands: in full.
    g.node(ids["noiseB"]).params().set("amplitude", 0.02);
    run(a, b);
    EXPECT_EQ(kind("merge").rfind("full", 0), 0u) << kind("merge");

    // a with a strand fewer: other strands, in full (and right).
    g.node(ids["noiseA"]).params().set("amplitude", 0.03);
    run(someStrands(11, 5), b);
    EXPECT_EQ(kind("merge").rfind("full", 0), 0u) << kind("merge");
}

}  // namespace
}  // namespace groomlab
