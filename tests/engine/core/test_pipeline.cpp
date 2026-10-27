// SPDX-License-Identifier: Apache-2.0
// M4 pipeline: rest pose, groom evaluation and files, guide poses, USD export.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "groomlab/cards.h"
#include "groomlab/curve.h"
#include "groomlab/display.h"
#include "groomlab/export_view.h"
#include "groomlab/frame_time.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/groom_file.h"
#include "groomlab/hair_color.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/lod.h"
#include "groomlab/paths.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/texture.h"
#include "groomlab/usd_writer.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

namespace fs = std::filesystem;

// Scatter -> Interpolate (hierarchy guides) -> Clump (its own centers) -> Width.
const char* kLookGraph = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "clump", "type": "Clump", "params": {"count": 40, "strength": 0.6}},
    {"name": "width", "type": "Width"},
    {"name": "output", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 6}}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "clump", "input": "strands"},
    {"from": "clump", "to": "width", "input": "strands"},
    {"from": "width", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";

std::shared_ptr<const ScalpMesh> sphere() { return test::makeSphere(12, 24); }

// The sphere squashed and sheared: every triangle changes area.
std::vector<Vec3f> deformed(const ScalpMesh& mesh) {
    std::vector<Vec3f> pts = mesh.points();
    for (Vec3f& p : pts) p = {p.x * 1.4f + 0.3f * p.y, p.y * 0.7f, p.z * 1.1f};
    return pts;
}

// Primary guides on the upper half of a unit sphere, with generated children.
std::string hierarchyJson() {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    TubeProfile tube;
    tube.width = {0.15f, 0.1f};
    for (int i = 0; i < 8; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 8.0f;
        const Vec3f n = Vec3f{std::cos(a) * 0.6f, 0.8f, std::sin(a) * 0.6f}.normalized();
        h.addGuide(primary, test::straightCurve(n, n * 0.8f + Vec3f{0.0f, 0.0f, 0.2f}, 6), tube);
    }
    ChildGenerationParams params;
    params.count = 4;
    h.generateChildren(primary, "children", params);
    const auto scalp = sphere();
    h.evaluate(scalp.get());
    return h.toJson();
}

GroomData groomData(MeshPtr scalp) {
    GroomData d;
    d.graphJson = kLookGraph;
    d.hierarchyJson = hierarchyJson();
    d.scalp = std::move(scalp);
    return d;
}

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// ---------------------------------------------------------------------------

TEST(RestPose, KeepsTheSameStrandsOnADeformingScalp) {
    const auto rest = sphere();
    const StrandsPtr atRest = Groom(groomData(rest)).evaluate(1.0f, 0.0);
    ASSERT_GT(atRest->strandCount(), 500u);

    // Without a rest pose the deformed areas change the strand count.
    const auto moved = std::make_shared<ScalpMesh>(rest->withPoints(deformed(*rest)));
    const StrandsPtr noRest = Groom(groomData(moved)).evaluate(1.0f, 0.0);
    EXPECT_NE(noRest->strandCount(), atRest->strandCount());

    // With one, the same strands (IDs, roots) and the same clumps.
    auto withRest = std::make_shared<ScalpMesh>(rest->withPoints(deformed(*rest)));
    withRest->setRestPoints(rest->points());
    const StrandsPtr blended = Groom(groomData(withRest)).evaluate(1.0f, 0.0);
    ASSERT_EQ(blended->strandCount(), atRest->strandCount());
    EXPECT_EQ(blended->ids(), atRest->ids());
    ASSERT_NE(blended->strandChannel("clumpId"), nullptr);
    EXPECT_EQ(*blended->strandChannel("clumpId"), *atRest->strandChannel("clumpId"));
    // And the strands moved with the scalp.
    EXPECT_GT((blended->strandPoints(7)[0] - atRest->strandPoints(7)[0]).length(), 0.01f);
    EXPECT_NEAR((blended->strandPoints(7)[0] - withRest->position(blended->roots()[7])).length(), 0.0f, 1e-5f);
}

TEST(RestPose, WithPointsKeepsMeshData) {
    auto mesh = std::make_shared<ScalpMesh>(*test::makePlane(2));
    std::vector<ScalpMesh::UV> uvs(mesh->triangleCount() * 3, {0.25f, 0.75f});
    mesh->setCornerUVs(uvs);
    mesh->setCornerColors("mask", std::vector<ScalpMesh::Color>(mesh->triangleCount() * 3, {1, 0, 0, 1}));
    mesh->setRestPoints(mesh->points());
    std::vector<Vec3f> pts = mesh->points();
    for (Vec3f& p : pts) p.y += 1.0f;
    const ScalpMesh moved = mesh->withPoints(pts);
    EXPECT_TRUE(moved.hasUVs());
    EXPECT_TRUE(moved.hasColorSet("mask"));
    EXPECT_TRUE(moved.hasRest());
    EXPECT_FLOAT_EQ(moved.position({0, 0.2f, 0.2f}).y, 1.0f);
    EXPECT_FLOAT_EQ(moved.restPositionOr({0, 0.2f, 0.2f}, {}).y, 0.0f);
    EXPECT_NE(moved.contentHash(), mesh->contentHash());
    EXPECT_THROW(mesh->setRestPoints({{0, 0, 0}}), std::invalid_argument);
    EXPECT_THROW((void)mesh->withPoints({{0, 0, 0}}), std::invalid_argument);
}

TEST(GroomFile, RoundTripsAndEvaluatesIdentically) {
    auto scalp = std::make_shared<ScalpMesh>(sphere()->withPoints(deformed(*sphere())));
    scalp->setRestPoints(sphere()->points());
    std::vector<ScalpMesh::UV> uvs(scalp->triangleCount() * 3);
    for (std::size_t i = 0; i < uvs.size(); ++i) uvs[i] = {0.001f * static_cast<float>(i % 997), 0.3f};
    scalp->setCornerUVs(uvs);
    scalp->setCornerColors("mask", std::vector<ScalpMesh::Color>(scalp->triangleCount() * 3, {0.1f, 0.2f, 0.3f, 1}));

    GroomFile file;
    file.data = groomData(scalp);
    auto flat = std::make_shared<GuideSet>();
    flat->add(77, test::straightCurve({0, -1, 0}, {0, -0.5f, 0.1f}, 5));
    flat->rootOnMesh(*scalp);
    file.data.guides = flat;
    file.scalpCache = {"caches/scalp.abc", "headShape"};
    file.framesPerSecond = 30.0;
    file.source = "test";
    // The painted maps' catalog: a gray vertex map, a color one (its channels kept, so it comes
    // back as a color map however gray its values), a texture map's file.
    file.data.maps["mask"] = {"vertices", "", 1};
    file.data.maps["tint"] = {"vertices", "", 4};
    file.data.maps["detail"] = {"texture", "masks/hair/detail.png", 1};

    const std::string text = groomFileToJson(file);
    const GroomFile back = groomFileFromJson(text, "C:/shots/a");
    EXPECT_EQ(back.framesPerSecond, 30.0);
    EXPECT_EQ(back.source, "test");
    ASSERT_EQ(back.data.maps.size(), 3u);
    EXPECT_EQ(back.data.maps.at("mask").channels, 1);
    EXPECT_EQ(back.data.maps.at("tint").channels, 4);
    EXPECT_EQ(back.data.maps.at("detail").kind, "texture");
    EXPECT_EQ(back.data.maps.at("detail").texture, "masks/hair/detail.png");
    EXPECT_EQ(fs::path(back.scalpCache.file), fs::path("C:/shots/a/caches/scalp.abc").lexically_normal());
    EXPECT_EQ(back.scalpCache.object, "headShape");
    EXPECT_TRUE(back.guideCache.empty());
    ASSERT_TRUE(back.data.scalp);
    EXPECT_EQ(back.data.scalp->contentHash(), scalp->contentHash());  // points, triangles, UVs, colors, rest
    ASSERT_TRUE(back.data.guides);
    EXPECT_EQ(back.data.guides->contentHash(), flat->contentHash());
    EXPECT_EQ(groomFileToJson(groomFileFromJson(text)), text);  // exact text round trip

    const StrandsPtr a = Groom(file.data).evaluate(1.0f, 0.5);
    const StrandsPtr b = Groom(back.data).evaluate(1.0f, 0.5);
    ASSERT_EQ(a->strandCount(), b->strandCount());
    EXPECT_EQ(a->ids(), b->ids());
    EXPECT_EQ(a->positions(), b->positions());  // exact, not approximate
    EXPECT_EQ(a->widths(), b->widths());

    const fs::path path = fs::temp_directory_path() / "groomlab_test.groom";
    writeGroomFile(path.u8string(), file);
    const GroomFile fromDisk = readGroomFile(path.u8string());
    EXPECT_EQ(fromDisk.scalpCache.file, (path.parent_path() / "caches/scalp.abc").lexically_normal().string());
    fs::remove(path);
}

TEST(Paths, OneRuleForStoredPaths) {
    // How every host stores a path relative to a scene, a groom file or a groom's folder (paths.h):
    // inside a folder, plain; beside a groom's folder on the same drive, with ".." (a scene's
    // keeps it absolute); resolvable again over the same folders; variables and relative paths
    // as they are.
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "groomlab_paths";
    fs::remove_all(root);
    fs::create_directories(root / "shot" / "groomlab" / "masks");
    fs::create_directories(root / "project" / "sourceimages");
    fs::create_directories(root / "shared" / "textures");
    const std::string scene = (root / "shot").generic_u8string(), project = (root / "project").generic_u8string();
    for (const fs::path& f : {root / "shot" / "groomlab" / "masks" / "a.png", root / "project" / "sourceimages" / "b.png",
                              root / "shared" / "textures" / "c.png"}) {
        std::ofstream(f) << "x";
    }
    const std::string c = (root / "shared" / "textures" / "c.png").u8string();
    for (const bool beside : {false, true}) {
        EXPECT_EQ(portablePath((root / "shot" / "groomlab" / "masks" / "a.png").u8string(), {scene, project}, beside),
                  "groomlab/masks/a.png");
        EXPECT_EQ(portablePath((root / "project" / "sourceimages" / "b.png").u8string(), {scene, project}, beside),
                  "sourceimages/b.png");
    }
    EXPECT_EQ(portablePath(c, {scene, project}, false), fs::u8path(c).generic_u8string());  // a scene's: the machine's
    const std::string shared = pathFromGroomFolder(c, scene);
    EXPECT_EQ(shared, "../shared/textures/c.png");  // a groom's: the folder beside it comes along
    const PathResolver resolve = searchPathResolver({scene, project});
    EXPECT_TRUE(fs::equivalent(fs::u8path(resolve(shared)), root / "shared" / "textures" / "c.png"));
    EXPECT_TRUE(fs::equivalent(fs::u8path(resolve("sourceimages/b.png")), root / "project" / "sourceimages" / "b.png"));
    // Not yet on disk (a file about to be written): the same rule.
    EXPECT_EQ(pathFromGroomFolder((root / "shot" / "groomlab" / "new" / "d.png").u8string(), scene), "groomlab/new/d.png");
    EXPECT_EQ(pathFromGroomFolder("$SHOW/textures/c.png", scene), "$SHOW/textures/c.png");
    EXPECT_EQ(pathFromGroomFolder("masks\\a.png", scene), "masks/a.png");
    EXPECT_EQ(pathFromGroomFolder("", scene), "");
#ifdef _WIN32
    // Another drive has no relative path: absolute, forward slashes.
    const std::string drive = root.root_name().u8string() == "Z:" ? "Y:" : "Z:";
    EXPECT_EQ(pathFromGroomFolder(drive + "\\library\\e.png", scene), drive + "/library/e.png");
#endif
    fs::remove_all(root);
}

// A path a groom file stores, as every host resolves it (groom_file.h resolveGroomPath: the app's
// and Blender's maps): variables expanded; relative, the file's folder (or the one it was saved to
// since) and then its search paths, the first that holds it.
TEST(Paths, AGroomFilesPathsResolveThroughItsFolderAndSearchPaths) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "groomlab_groom_paths";
    fs::remove_all(root);
    fs::create_directories(root / "groom" / "maps");
    fs::create_directories(root / "project" / "sourceimages");
    std::ofstream(root / "groom" / "maps" / "density.png") << "x";
    std::ofstream(root / "project" / "sourceimages" / "clumps.png") << "x";
#ifdef _WIN32
    _putenv_s("GROOMLAB_TEST_PROJECT", (root / "project").u8string().c_str());
#else
    setenv("GROOMLAB_TEST_PROJECT", (root / "project").u8string().c_str(), 1);
#endif
    GroomFile file;
    file.directory = (root / "groom").u8string();
    file.searchPaths = {"$GROOMLAB_TEST_PROJECT/sourceimages"};
    auto same = [](const std::string& a, const fs::path& b) { return fs::equivalent(fs::u8path(a), b); };
    EXPECT_TRUE(same(resolveGroomPath(file, "maps/density.png"), root / "groom" / "maps" / "density.png"));
    EXPECT_TRUE(same(resolveGroomPath(file, "clumps.png"), root / "project" / "sourceimages" / "clumps.png"));  // a search path
    EXPECT_TRUE(same(resolveGroomPath(file, "${GROOMLAB_TEST_PROJECT}/sourceimages/clumps.png"),
                     root / "project" / "sourceimages" / "clumps.png"));  // a variable
    // Found nowhere: the folder's join. Saved elsewhere since: that folder first.
    EXPECT_EQ(fs::u8path(resolveGroomPath(file, "maps/none.png")), (root / "groom" / "maps" / "none.png").lexically_normal());
    EXPECT_TRUE(same(resolveGroomPath(file, "sourceimages/clumps.png", (root / "project").u8string()),
                     root / "project" / "sourceimages" / "clumps.png"));
    EXPECT_EQ(resolveGroomPath(file, ""), "");
    EXPECT_EQ(resolveGroomPath(GroomFile{}, "maps/a.png"), "maps/a.png");  // no folder, no search paths: as stored
    fs::remove_all(root);
}

TEST(GroomFile, RejectsOtherFilesAndNewerVersions) {
    EXPECT_THROW(groomFileFromJson("not json"), GroomFileError);
    EXPECT_THROW(groomFileFromJson(R"({"version": 1})"), GroomFileError);
    EXPECT_THROW(groomFileFromJson(R"({"format": "groomlab.groom", "version": 99})"), GroomFileError);
    EXPECT_THROW(groomFileFromJson(R"({"format": "groomlab.groom", "version": 1, "scalp": {"points": [1, 2]}})"),
                 GroomFileError);
    EXPECT_THROW(readGroomFile("no/such/file.groom"), GroomFileError);
    const GroomFile empty = groomFileFromJson(R"({"format": "groomlab.groom", "version": 1})");
    EXPECT_FALSE(empty.data.scalp);
    EXPECT_NO_THROW(Groom{empty.data}.evaluate(1.0f, 0.0));  // default graph, nothing to grow on
}

TEST(GuidePoses, ChildrenFollowPosedParents) {
    const auto scalp = sphere();
    GuideHierarchy h = GuideHierarchy::fromJson(hierarchyJson());
    h.evaluate(scalp.get());
    const GuideHierarchy authored = h;

    const HierarchyGuide& parent = h.guides().front();
    ASSERT_FALSE(parent.isChild());
    const std::vector<GuideId> children = h.childrenOf(parent.id);
    ASSERT_FALSE(children.empty());

    // Swing the parent's tip sideways, as a simulation would.
    GuideSet poses;
    std::vector<Vec3f> swung = parent.points;
    for (std::size_t i = 1; i < swung.size(); ++i) swung[i] += Vec3f{0.5f, 0.0f, 0.0f} * (static_cast<float>(i) / (swung.size() - 1));
    poses.add(parent.id, swung);
    poses.add(999999, swung);  // unknown IDs are ignored
    applyGuidePoses(h, poses, scalp.get());

    EXPECT_EQ(h.guide(parent.id)->points, swung);
    const Vec3f tipBefore = authored.guide(children[0])->points.back();
    const Vec3f tipAfter = h.guide(children[0])->points.back();
    EXPECT_GT(tipAfter.x - tipBefore.x, 0.3f);
    // Guides under other parents are untouched.
    const HierarchyGuide& other = h.guides()[1];
    EXPECT_EQ(other.points, authored.guide(other.id)->points);

    // A fresh evaluate restores the authored shape.
    h.evaluate(scalp.get());
    EXPECT_LT((h.guide(parent.id)->points.back() - authored.guide(parent.id)->points.back()).length(), 1e-5f);

    // Through Groom: poses move the strands grown from the posed guides.
    GroomData d = groomData(scalp);
    Groom groom(d);
    const StrandsPtr before = groom.evaluate(1.0f, 0.0);
    groom.setGuidePoses(std::make_shared<GuideSet>(poses));
    const StrandsPtr after = groom.evaluate(1.0f, 0.0);
    ASSERT_EQ(before->strandCount(), after->strandCount());
    EXPECT_NE(before->positions(), after->positions());
}

// kLookGraph with an Instance node before the output: one instance per strand, 0.8 along it.
// `extra`: more of its params (", \"name\": value...").
std::string instanceGraph(bool pin, const std::string& extra = "") {
    std::string g = kLookGraph;
    const std::string nodes = R"({"name": "output", "type": "Output"})";
    g.replace(g.find(nodes), nodes.size(),
              std::string(R"({"name": "flakes", "type": "Instance", "params": {"probability": 1.0, "tMin": 0.8, "tMax": 0.8, "pinToRest": )") +
                  (pin ? "true" : "false") + extra + "}}, " + nodes);
    const std::string link = R"({"from": "width", "to": "output", "input": "strands"})";
    g.replace(g.find(link), link.size(),
              R"({"from": "width", "to": "flakes", "input": "strands"}, {"from": "flakes", "to": "output", "input": "strands"})");
    return g;
}

// Every hierarchy guide scaled about its root by `factor`, as a stretching simulation might pose them.
GuidesPtr scaledPoses(const MeshPtr& scalp, float factor) {
    GuideHierarchy h = GuideHierarchy::fromJson(hierarchyJson());
    h.evaluate(scalp.get());
    auto poses = std::make_shared<GuideSet>();
    for (const HierarchyGuide& g : h.guides()) {
        std::vector<Vec3f> pts = g.points;
        for (Vec3f& p : pts) p = g.points[0] + (p - g.points[0]) * factor;
        poses->add(g.id, pts);
    }
    return poses;
}

// The point `distance` along a polyline from its first point (its last past the end).
Vec3f pointAtDistance(const Vec3f* p, std::size_t n, float distance) {
    for (std::size_t i = 1; i < n; ++i) {
        const float len = (p[i] - p[i - 1]).length();
        if (distance <= len) return p[i - 1] + (p[i] - p[i - 1]) * (len > 0.0f ? distance / len : 0.0f);
        distance -= len;
    }
    return p[n - 1];
}

// Checks where each instance of `now` sits: `pinned`, t (0.8) of its strand's length in `rest`
// from the root (at most the tip); else 0.8 of its length now. Returns how many strands grew
// by more than `grew` (a factor) from rest to now.
std::size_t expectInstancesAt(const StrandSet& rest, const StrandSet& now, bool pinned, float grew) {
    std::map<StrandId, std::size_t> restIndex, nowIndex;
    for (std::size_t s = 0; s < rest.strandCount(); ++s) restIndex[rest.ids()[s]] = s;
    for (std::size_t s = 0; s < now.strandCount(); ++s) nowIndex[now.ids()[s]] = s;
    const auto placed = now.instances()->resolve(now);
    EXPECT_EQ(placed.size(), now.strandCount());
    std::size_t changed = 0;
    for (const auto& p : placed) {
        const StrandId id = now.instances()->instances[p.index].strand;
        const std::size_t r = restIndex.at(id), s = nowIndex.at(id);
        const float restLength = arcLength(rest.strandPoints(r), rest.strandPointCount(r));
        const float nowLength = arcLength(now.strandPoints(s), now.strandPointCount(s));
        if (nowLength > grew * restLength) ++changed;
        const float distance = pinned ? std::min(0.8f * restLength, nowLength) : 0.8f * nowLength;
        const Vec3f expected = pointAtDistance(now.strandPoints(s), now.strandPointCount(s), distance);
        EXPECT_LT((p.origin - expected).length(), 1e-4f) << "strand " << id << (pinned ? " pinned" : " unpinned");
    }
    return changed;
}

TEST(InstancePinToRest, StretchedStrandsKeepTheirInstancesDistanceFromTheRoot) {
    const auto scalp = sphere();
    for (const bool pin : {false, true}) {
        GroomData d = groomData(scalp);
        d.graphJson = instanceGraph(pin);
        Groom groom(d);
        const StrandsPtr rest = groom.evaluate(1.0f, 0.0);
        ASSERT_TRUE(rest->instances());
        // At rest, pinned or not, the instances sit at t.
        expectInstancesAt(*rest, *rest, false, 2.0f);

        // Stretched (a simulation, a cache): pinned, the same distance from the root; else they slide out.
        groom.setGuidePoses(scaledPoses(scalp, 1.5f));
        const StrandsPtr stretched = groom.evaluate(1.0f, 1.0 / 24.0);
        ASSERT_EQ(stretched->ids(), rest->ids());
        EXPECT_GT(expectInstancesAt(*rest, *stretched, pin, 1.2f), stretched->strandCount() / 2);

        // Shrunk below the rest distance: pinned, at the tip.
        groom.setGuidePoses(scaledPoses(scalp, 0.5f));
        const StrandsPtr shrunk = groom.evaluate(1.0f, 2.0 / 24.0);
        expectInstancesAt(*rest, *shrunk, pin, 0.0f);

        // The rest pose doesn't change with the poses: evaluated once, when first needed.
        EXPECT_EQ(groom.restEvaluations(), pin ? 1u : 0u);
    }
}

TEST(InstancePinToRest, RestPoseIsEvaluatedOncePerEditNotPerFrame) {
    // A deforming scalp with a rest pose, one-shot caching off (renders): the rest inputs'
    // hash keeps the rest pose across motion keys and frames.
    auto frame = [](float amount) {
        auto s = std::make_shared<ScalpMesh>(sphere()->withPoints(deformed(*sphere())));
        std::vector<Vec3f> pts = s->points();
        for (Vec3f& p : pts) p = p * (1.0f + amount);
        auto moved = std::make_shared<ScalpMesh>(s->withPoints(pts));
        moved->setRestPoints(sphere()->points());
        return moved;
    };
    GroomData d = groomData(frame(0.0f));
    d.graphJson = instanceGraph(true);
    Groom groom(d);
    groom.setCacheIntermediates(false);
    StrandsPtr first = groom.evaluate(1.0f, 0.0);
    for (int k = 1; k <= 3; ++k) {
        groom.setScalp(frame(0.05f * k));
        groom.setGuidePoses(scaledPoses(sphere(), 1.0f + 0.1f * k));
        const StrandsPtr now = groom.evaluate(1.0f, k / 24.0);
        ASSERT_EQ(now->ids(), first->ids());
    }
    EXPECT_EQ(groom.restEvaluations(), 1u);

    // The rest pose is the groom's at rest: on the rest scalp, as authored. Pinned instances on
    // the unposed groom evaluated on the rest scalp sit exactly at t.
    GroomData atRest = groomData(sphere());
    atRest.graphJson = instanceGraph(true);
    const StrandsPtr restStrands = Groom(atRest).evaluate(1.0f, 0.0);
    const StrandsPtr moved = groom.evaluate(1.0f, 5.0 / 24.0);
    expectInstancesAt(*restStrands, *moved, true, 2.0f);
}

// Checks each fitted instance of `now`: `fraction` of its strand's length in `rest` along +Y,
// `aspect` of that across. Returns how many strands grew by more than `grew` from rest to now.
std::size_t expectFittedToRest(const StrandSet& rest, const StrandSet& now, float fraction, float aspect, float grew) {
    std::map<StrandId, std::size_t> restIndex, nowIndex;
    for (std::size_t s = 0; s < rest.strandCount(); ++s) restIndex[rest.ids()[s]] = s;
    for (std::size_t s = 0; s < now.strandCount(); ++s) nowIndex[now.ids()[s]] = s;
    const auto placed = now.instances()->resolve(now);
    EXPECT_EQ(placed.size(), now.strandCount());
    std::size_t changed = 0;
    for (const auto& p : placed) {
        const StrandId id = now.instances()->instances[p.index].strand;
        const std::size_t r = restIndex.at(id), s = nowIndex.at(id);
        const float restLength = arcLength(rest.strandPoints(r), rest.strandPointCount(r));
        if (arcLength(now.strandPoints(s), now.strandPointCount(s)) > grew * restLength) ++changed;
        EXPECT_NEAR(p.y.length(), fraction * restLength, 1e-4f) << "strand " << id;
        EXPECT_NEAR(p.x.length(), aspect * p.y.length(), 1e-5f) << "strand " << id;
        EXPECT_NEAR(p.z.length(), aspect * p.y.length(), 1e-5f) << "strand " << id;
    }
    return changed;
}

TEST(InstanceFitStrand, SizeComesFromTheRestLengthWhileStrandsStretch) {
    const auto scalp = sphere();
    const std::string fit =
        R"(, "sizeMode": "fitStrand", "fitScale": 0.5, "aspect": 0.2, "scaleRandom": 0.0, "scaleByWidth": false)";
    GroomData d = groomData(scalp);
    d.graphJson = instanceGraph(false, fit);
    Groom groom(d);
    const StrandsPtr rest = groom.evaluate(1.0f, 0.0);
    ASSERT_TRUE(rest->instances());
    EXPECT_EQ(groom.restEvaluations(), 1u);  // fitStrand alone asks for the rest pose
    expectFittedToRest(*rest, *rest, 0.5f, 0.2f, 2.0f);

    // Fixed mode (the same node otherwise) never reads the rest pose, and keeps its size.
    GroomData f = groomData(scalp);
    f.graphJson = instanceGraph(false, R"(, "scaleRandom": 0.0, "scaleByWidth": false)");
    Groom fixed(f);
    fixed.evaluate(1.0f, 0.0);

    // Stretching guides (a simulation): the length now grows, the fitted size holds at the rest length.
    for (int k = 1; k <= 3; ++k) {
        groom.setGuidePoses(scaledPoses(scalp, 1.0f + 0.25f * k));
        const StrandsPtr now = groom.evaluate(1.0f, k / 24.0);
        ASSERT_EQ(now->ids(), rest->ids());
        EXPECT_GT(expectFittedToRest(*rest, *now, 0.5f, 0.2f, 1.0f + 0.1f * k), now->strandCount() / 2);
        fixed.setGuidePoses(scaledPoses(scalp, 1.0f + 0.25f * k));
        const StrandsPtr still = fixed.evaluate(1.0f, k / 24.0);
        for (const auto& p : still->instances()->resolve(*still)) {
            EXPECT_NEAR(p.x.length(), 0.02f, 1e-6f);
            EXPECT_NEAR(p.y.length(), 0.02f, 1e-6f);
        }
    }
    EXPECT_EQ(groom.restEvaluations(), 1u);  // once, not per frame
    EXPECT_EQ(fixed.restEvaluations(), 0u);

    // Renders (caching off) on a deforming scalp with a rest pose: once across frames and motion keys.
    auto frame = [](float amount) {
        auto s = std::make_shared<ScalpMesh>(sphere()->withPoints(deformed(*sphere())));
        std::vector<Vec3f> pts = s->points();
        for (Vec3f& p : pts) p = p * (1.0f + amount);
        auto moved = std::make_shared<ScalpMesh>(s->withPoints(pts));
        moved->setRestPoints(sphere()->points());
        return moved;
    };
    GroomData r = groomData(frame(0.0f));
    r.graphJson = instanceGraph(false, fit);
    Groom render(r);
    render.setCacheIntermediates(false);
    for (int k = 0; k <= 4; ++k) {
        render.setScalp(frame(0.05f * k));
        render.setGuidePoses(scaledPoses(sphere(), 1.0f + 0.1f * k));
        const StrandsPtr now = render.evaluate(1.0f, (k / 2) / 24.0 + (k % 2) * 0.01);
        expectFittedToRest(*rest, *now, 0.5f, 0.2f, 100.0f);  // the same rest pose as the undeformed groom's
    }
    EXPECT_EQ(render.restEvaluations(), 1u);
}

TEST(UsdWriter, WritesInstancesAsAPointInstancer) {
    StrandSet strands;
    strands.addStrand(7, {{0, 0, 0}, {0, 1, 0}});
    strands.addStrand(8, {{1, 0, 0}, {1, 1, 0}});
    auto instances = std::make_shared<InstanceSet>();
    instances->prototypeIndex("snow flake");
    InstanceSet::Instance a;
    a.strand = 7;
    a.id = 11;
    a.t = 0.5f;
    a.scale = 2.0f;
    a.orient = InstanceOrient::Up;
    instances->instances.push_back(a);
    strands.setInstances(instances);
    const fs::path path = fs::temp_directory_path() / "groomlab_instances.usda";
    {
        UsdCurvesWriter w(path.u8string(), "groom", {});
        w.writeFrame(1.0, strands);
    }
    const std::string text = readText(path);
    EXPECT_NE(text.find("def PointInstancer \"instances\""), std::string::npos);
    EXPECT_NE(text.find("rel prototypes = [</groom/instances/prototypes/snow_flake>]"), std::string::npos);
    EXPECT_NE(text.find("point3f[] positions.timeSamples = {\n        1: [(0, 0.5, 0)]"), std::string::npos);
    EXPECT_NE(text.find("float3[] scales.timeSamples = {\n        1: [(2, 2, 2)]"), std::string::npos);
    EXPECT_NE(text.find("int64[] ids.timeSamples = {\n        1: [11]"), std::string::npos);
    EXPECT_NE(text.find("def Xform \"snow_flake\""), std::string::npos);
    EXPECT_NE(text.find("quath[] orientations.timeSamples = {\n        1: [("), std::string::npos);
    fs::remove(path);
}

TEST(UsdWriter, WritesFittedInstancesScalesPerAxis) {
    // Fitted to its strand: 2 along +Y (the rest length times scale), a quarter of that across;
    // the orientation stays a unit quaternion (each axis normalized by its own length).
    StrandSet strands;
    strands.addStrand(7, {{0, 0, 0}, {1, 1, 0}});
    auto instances = std::make_shared<InstanceSet>();
    instances->prototypeIndex("leaf");
    InstanceSet::Instance a;
    a.strand = 7;
    a.id = 11;
    a.t = 0.0f;
    a.scale = 0.5f;
    a.fitStrand = true;
    a.aspect = 0.25f;
    a.restLength = 4.0f;
    instances->instances.push_back(a);
    strands.setInstances(instances);
    const fs::path path = fs::temp_directory_path() / "groomlab_fitted_instances.usda";
    {
        UsdCurvesWriter w(path.u8string(), "groom", {});
        w.writeFrame(1.0, strands);
    }
    const std::string text = readText(path);
    fs::remove(path);
    // The first sample's numbers of an attribute: "(a, b, ...)".
    auto sample = [&](const std::string& attribute, int count) {
        const std::string key = attribute + ".timeSamples = {\n        1: [(";
        const std::size_t at = text.find(key);
        std::vector<float> out(count, 0.0f);
        if (at == std::string::npos) return std::vector<float>{};
        std::istringstream values(text.substr(at + key.size()));
        char comma = 0;
        for (int i = 0; i < count; ++i) values >> out[i] >> comma;
        return values ? out : std::vector<float>{};
    };
    const std::vector<float> scales = sample("float3[] scales", 3);
    ASSERT_EQ(scales.size(), 3u) << text;
    EXPECT_NEAR(scales[0], 0.5f, 1e-5f);
    EXPECT_NEAR(scales[1], 2.0f, 1e-5f);
    EXPECT_NEAR(scales[2], 0.5f, 1e-5f);
    const std::vector<float> q = sample("quath[] orientations", 4);
    ASSERT_EQ(q.size(), 4u) << text;
    EXPECT_NEAR(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1.0f, 1e-4f);
    // The quaternion (w, x, y, z) turns +Y onto the strand's direction, (1, 1, 0) normalized.
    const Vec3f v{0, 1, 0}, u{q[1], q[2], q[3]};
    const Vec3f turned = v + u.cross(u.cross(v) + v * q[0]) * 2.0f;
    EXPECT_NEAR(turned.x, std::sqrt(0.5f), 1e-4f);
    EXPECT_NEAR(turned.y, std::sqrt(0.5f), 1e-4f);
    EXPECT_NEAR(turned.z, 0.0f, 1e-4f);
}

TEST(UsdWriter, GivesPrototypesUniquePrimNamesAndQuotesNothing) {
    // "a-b" and "a_b" make the same identifier: numbered apart. A quote in a name
    // (or the comment) must not end the doc string it is written into.
    StrandSet strands;
    strands.addStrand(7, {{0, 0, 0}, {0, 1, 0}});
    auto instances = std::make_shared<InstanceSet>();
    for (const char* name : {"a-b", "a_b", "say \"hi\"\\"}) instances->prototypeIndex(name);
    for (std::uint32_t p = 0; p < 3; ++p) {
        InstanceSet::Instance i;
        i.strand = 7;
        i.id = 20 + p;
        i.prototype = p;
        instances->instances.push_back(i);
    }
    strands.setInstances(instances);
    const fs::path path = fs::temp_directory_path() / "groomlab_prototypes.usda";
    {
        UsdCurvesWriter::Options options;
        options.comment = "shot \"12\"\nline two";
        UsdCurvesWriter w(path.u8string(), "groom", options);
        w.writeFrame(1.0, strands);
    }
    const std::string text = readText(path);
    fs::remove(path);
    EXPECT_NE(text.find("rel prototypes = [</groom/instances/prototypes/a_b>, </groom/instances/prototypes/a_b_2>, "
                        "</groom/instances/prototypes/say__hi__>]"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("def Xform \"a_b_2\" (\n                doc = \"GroomLab prototype 'a_b': "), std::string::npos) << text;
    EXPECT_NE(text.find("doc = \"GroomLab prototype 'say 'hi' ': "), std::string::npos) << text;
    EXPECT_NE(text.find(": shot '12' line two\"\n"), std::string::npos) << text;
}

TEST(UsdWriter, WritesTimeSampledBasisCurves) {
    const auto rest = sphere();
    auto scalp = std::make_shared<ScalpMesh>(*rest);
    scalp->setCornerUVs(std::vector<ScalpMesh::UV>(scalp->triangleCount() * 3, {0.5f, 0.25f}));
    Groom groom(groomData(scalp));
    const StrandsPtr f1 = groom.evaluate(0.2f, 1.0 / 24.0);

    const fs::path path = fs::temp_directory_path() / "groomlab_test.usda";
    {
        UsdCurvesWriter::Options options;
        options.comment = "shot \"010\"";
        UsdCurvesWriter w(path.u8string(), "groom:1", options);
        w.writeFrame(1.0, *f1, scalp.get());
        w.writeFrame(2.0, *f1, scalp.get());
        EXPECT_THROW(w.writeFrame(2.0, *f1, scalp.get()), UsdError);
        EXPECT_EQ(w.frameCount(), 2u);
    }  // closed by the destructor
    const std::string text = readText(path);
    EXPECT_EQ(text.rfind("#usda 1.0\n", 0), 0u);
    EXPECT_NE(text.find("defaultPrim = \"groom_1\""), std::string::npos);
    EXPECT_NE(text.find("doc = \"GroomLab"), std::string::npos);
    EXPECT_NE(text.find("startTimeCode = 1\n"), std::string::npos);
    EXPECT_NE(text.find("endTimeCode = 2\n"), std::string::npos);
    EXPECT_NE(text.find("def BasisCurves \"strands\""), std::string::npos);
    EXPECT_NE(text.find("uniform token type = \"linear\""), std::string::npos);
    EXPECT_NE(text.find("point3f[] points.timeSamples = {\n        1: [("), std::string::npos);
    EXPECT_NE(text.find("        2: [("), std::string::npos);
    EXPECT_NE(text.find("int64[] primvars:strandId (\n            interpolation = \"uniform\""), std::string::npos);
    EXPECT_NE(text.find("texCoord2f[] primvars:st.timeSamples = {\n        1: [(0.5, 0.25)"), std::string::npos);
    EXPECT_NE(text.find("float[] primvars:clumpId"), std::string::npos);
    EXPECT_NE(text.find("float[] widths (\n            interpolation = \"vertex\""), std::string::npos);
    // One count per strand in the first sample.
    const std::size_t counts = text.find("int[] curveVertexCounts.timeSamples = {\n        1: [");
    ASSERT_NE(counts, std::string::npos);
    const std::size_t lineEnd = text.find('\n', counts + 45);
    const std::string line = text.substr(counts, lineEnd - counts);
    EXPECT_EQ(static_cast<std::size_t>(std::count(line.begin(), line.end(), ',')), f1->strandCount());

    for (const auto& entry : fs::directory_iterator(path.parent_path())) {
        EXPECT_EQ(entry.path().filename().u8string().find("groomlab_test.usda.spool"), std::string::npos);
    }
    fs::remove(path);
    EXPECT_EQ(usdIdentifier("9lives:a|b"), "_9lives_a_b");
}

TEST(UsdWriter, WritesDisplayColorAndUniquePrimvarNames) {
    StrandSet strands;
    strands.addStrand(1, {{0, 0, 0}, {0, 1, 0}});
    strands.addStrand(2, {{1, 0, 0}, {1, 1, 0}});
    strands.addStrandChannel("melanin", 0.0f)[1] = 1.0f;  // blond, then black
    strands.addStrandChannel("a-b", 1.0f);
    strands.addStrandChannel("a_b", 2.0f);
    strands.addStrandChannel("st", 3.0f);  // reserved for the root UV
    const fs::path path = fs::temp_directory_path() / "groomlab_color.usda";
    {
        UsdCurvesWriter w(path.u8string(), "groom", {});
        w.writeFrame(1.0, strands);
    }
    EXPECT_FALSE(fs::exists(tempPathFor(path)));
    const std::string text = readText(path);
    EXPECT_NE(text.find("color3f[] primvars:displayColor (\n            interpolation = \"uniform\""), std::string::npos);
    EXPECT_NE(text.find("primvars:displayColor.timeSamples = {\n        1: [(1, 1, 1), ("), std::string::npos);
    // displayColor is the hair's appearance: medium brown (melanin 0.35) a medium brown, not the
    // dark base color a hair BSDF takes.
    {
        StrandSet brown;
        brown.addStrand(1, {{0, 0, 0}, {0, 1, 0}});
        brown.addStrandChannel("melanin", 0.35f);
        brown.addStrandChannel("melaninRedness", 0.2f);
        const fs::path brownPath = fs::temp_directory_path() / "groomlab_brown.usda";
        {
            UsdCurvesWriter w(brownPath.u8string(), "groom", {});
            w.writeFrame(1.0, brown);
        }
        const std::string brownText = readText(brownPath);
        const std::size_t at = brownText.find("primvars:displayColor.timeSamples = {\n        1: [(");
        ASSERT_NE(at, std::string::npos);
        float r = 0.0f, g = 0.0f, b = 0.0f;
        ASSERT_EQ(std::sscanf(brownText.c_str() + at + std::string("primvars:displayColor.timeSamples = {\n        1: [(").size(),
                              "%f, %f, %f", &r, &g, &b),
                  3);
        const std::array<float, 3> want = hairAppearanceColor(melaninColor(0.35f, 0.2f));
        EXPECT_NEAR(r, want[0], 1e-3f);
        EXPECT_NEAR(b, want[2], 1e-3f);
        EXPECT_GT(r, 0.2f);  // sRGB about 145, 108, 66
        EXPECT_GT(r, g);
        EXPECT_GT(g, b);
        fs::remove(brownPath);
    }
    EXPECT_NE(text.find("float[] primvars:a_b ("), std::string::npos);
    EXPECT_NE(text.find("float[] primvars:a_b_2 ("), std::string::npos);
    EXPECT_NE(text.find("float[] primvars:st_2 ("), std::string::npos);
    EXPECT_EQ(text.find("float[] primvars:st ("), std::string::npos);
    EXPECT_NE(text.find("float[] primvars:melanin ("), std::string::npos);
    fs::remove(path);
}

// ---------------------------------------------------------------------------

TEST(GroomFile, RefusesNonFiniteValuesAndReadsNullAsZero) {
    GroomFile file;
    auto flat = std::make_shared<GuideSet>();
    flat->add(1, {{0, 0, 0}, {0, std::numeric_limits<float>::quiet_NaN(), 0}});
    file.data.guides = flat;
    try {
        groomFileToJson(file);
        FAIL() << "a NaN was written";
    } catch (const GroomFileError& e) {
        EXPECT_NE(std::string(e.what()).find("guides.points"), std::string::npos) << e.what();
    }
    flat->guides()[0].points[1].y = std::numeric_limits<float>::infinity();
    EXPECT_THROW(groomFileToJson(file), GroomFileError);

    // Another writer's null (JSON has no NaN) reads as 0; anything else is an error.
    const GroomFile back = groomFileFromJson(
        R"({"format": "groomlab.groom", "version": 1, "guides": [{"id": 5, "root": [-1, 0, 0], "points": [0, null, 0, 1, 1, 1]}]})");
    ASSERT_TRUE(back.data.guides);
    EXPECT_EQ(back.data.guides->guides()[0].points[0].y, 0.0f);
    EXPECT_EQ(back.data.guides->guides()[0].points[1].z, 1.0f);
    EXPECT_THROW(groomFileFromJson(R"({"format": "groomlab.groom", "version": 1, "guides": [{"id": 5, "root": [-1, 0, 0], "points": [0, "x", 0]}]})"),
                 GroomFileError);
}

TEST(GroomFile, StoresFloatsShortAndExactWithUnits) {
    GroomFile file;
    auto flat = std::make_shared<GuideSet>();
    flat->add(1, {{0.1f, 1.0f / 3.0f, 123456.789f}, {0.25f, -2.5f, 3.0f}});
    file.data.guides = flat;
    const std::string text = groomFileToJson(file);
    EXPECT_NE(text.find("\"points\":[0.1,0.33333334,123456.79,0.25,-2.5,3]"), std::string::npos) << text;
    EXPECT_NE(text.find("\"units\":\"cm\""), std::string::npos);
    const GroomFile back = groomFileFromJson(text);
    ASSERT_TRUE(back.data.guides);
    EXPECT_EQ(back.data.guides->guides()[0].points, flat->guides()[0].points);  // exact
    EXPECT_EQ(back.units, "cm");
    file.units = "m";
    EXPECT_EQ(groomFileFromJson(groomFileToJson(file)).units, "m");
    EXPECT_EQ(groomFileFromJson(R"({"format": "groomlab.groom", "version": 1})").units, "cm");  // the default
}

TEST(GroomFile, KeepsTheGuideGroupsDisplayAsMayasAttributes) {
    // The groups' own colors and the hidden groups, in Maya's attributes' text; absent when empty.
    GroomFile file;
    EXPECT_EQ(groomFileToJson(file).find("guideGroupColors"), std::string::npos);
    EXPECT_EQ(groomFileToJson(file).find("hiddenGuideGroups"), std::string::npos);
    file.guideGroupColors = "crown:#ff8800 nape:#3d7eff";
    file.hiddenGuideGroups = "nape fringe";
    const GroomFile back = groomFileFromJson(groomFileToJson(file));
    EXPECT_EQ(back.guideGroupColors, file.guideGroupColors);
    EXPECT_EQ(back.hiddenGuideGroups, file.hiddenGuideGroups);
    EXPECT_EQ(parseGuideGroupColors(back.guideGroupColors).size(), 2u);
    EXPECT_EQ(parseHiddenGuideGroups(back.hiddenGuideGroups), (std::set<std::string>{"nape", "fringe"}));
}

TEST(GroomFile, WritesAnEmptyColorSet) {
    // A scalp without triangles has color sets of no colors: written as empty lists.
    GroomFile file;
    auto scalp = std::make_shared<ScalpMesh>(std::vector<Vec3f>{{0, 0, 0}}, std::vector<ScalpMesh::Triangle>{});
    scalp->setCornerColors("mask", {});
    file.data.scalp = scalp;
    const GroomFile back = groomFileFromJson(groomFileToJson(file));
    ASSERT_TRUE(back.data.scalp);
    EXPECT_TRUE(back.data.scalp->hasColorSet("mask"));
    EXPECT_EQ(back.data.scalp->contentHash(), scalp->contentHash());
}

TEST(GroomFile, WritesAtomically) {
    const fs::path path = fs::temp_directory_path() / "groomlab_atomic.groom";
    GroomFile file;
    file.source = "one";
    writeGroomFile(path.u8string(), file);
    EXPECT_FALSE(fs::exists(tempPathFor(path)));
    EXPECT_EQ(readGroomFile(path.u8string()).source, "one");
    file.source = "two";
    writeGroomFile(path.u8string(), file);  // replaces the file in place
    EXPECT_EQ(readGroomFile(path.u8string()).source, "two");
    EXPECT_FALSE(fs::exists(tempPathFor(path)));
    EXPECT_THROW(writeGroomFile((fs::temp_directory_path() / "groomlab_no_such_dir" / "x.groom").u8string(), file),
                 GroomFileError);
    fs::remove(path);
}

TEST(GroomFile, WritesItsUpAxisAndCardBake) {
    GroomFile file;
    const std::string text = groomFileToJson(file);
    EXPECT_NE(text.find("\"upAxis\":\"Y\""), std::string::npos) << text;  // always written
    EXPECT_EQ(text.find("cardBake"), std::string::npos);                     // none: not written
    EXPECT_EQ(groomFileFromJson(R"({"format": "groomlab.groom", "version": 1})").upAxis, "Y");  // the default
    file.upAxis = "Z";
    file.cardBake = {"hair_cards/hair_", "cards:00ff"};
    const GroomFile back = groomFileFromJson(groomFileToJson(file), "C:/shots/a");
    EXPECT_EQ(back.upAxis, "Z");
    EXPECT_EQ(fs::path(back.cardBake.prefix), fs::path("C:/shots/a/hair_cards/hair_").lexically_normal());  // beside the file
    EXPECT_EQ(back.cardBake.strands, "cards:00ff");
    file.upAxis = "X";
    EXPECT_THROW(groomFileToJson(file), GroomFileError);
    EXPECT_THROW(groomFileFromJson(R"({"format": "groomlab.groom", "version": 1, "upAxis": "up"})"), GroomFileError);
}

TEST(GroomFile, TurnsIntoAnotherUpAxis) {
    // A Z-up file read Y-up: the scalp, its rest pose, the flat guides and the hierarchy turn a
    // quarter about X, rooted guides (their shapes in their roots' frames) and unrooted ones (world
    // shapes) alike; the groom grows the same, turned.
    auto scalp = std::make_shared<ScalpMesh>(sphere()->withPoints(deformed(*sphere())));
    scalp->setRestPoints(sphere()->points());
    scalp->setCornerUVs(std::vector<ScalpMesh::UV>(scalp->triangleCount() * 3, {0.25f, 0.5f}));
    GroomFile file;
    file.data = groomData(scalp);
    file.upAxis = "Z";
    auto flat = std::make_shared<GuideSet>();
    flat->add(77, test::straightCurve({0, -1, 0}, {0, -0.5f, 0.1f}, 5));
    file.data.guides = flat;
    auto turn = [](const Vec3f& p) { return Vec3f(p.x, p.z, -p.y); };

    for (const bool rooted : {true, false}) {
        GroomFile z = file;
        if (!rooted) {  // world shapes only (as a host writes guides it never bound)
            GuideHierarchy h;
            const GroupId g = h.addGroup("primary");
            for (int i = 0; i < 6; ++i) {
                const float a = 6.2831853f * static_cast<float>(i) / 6.0f;
                const Vec3f n = Vec3f{std::cos(a) * 0.5f, 0.85f, std::sin(a) * 0.5f}.normalized();
                h.addGuide(g, test::straightCurve(n, n * 0.7f + Vec3f{0.1f, 0.0f, 0.1f}, 5));
            }
            z.data.hierarchyJson = h.toJson();
        }
        GroomFile y = z;
        convertUpAxis(y, "Y");
        EXPECT_EQ(y.upAxis, "Y");
        ASSERT_EQ(y.data.scalp->pointCount(), scalp->pointCount());
        for (std::size_t i = 0; i < scalp->pointCount(); ++i) {
            EXPECT_EQ(y.data.scalp->points()[i], turn(scalp->points()[i]));
            EXPECT_EQ(y.data.scalp->restPoints()[i], turn(scalp->restPoints()[i]));
        }
        EXPECT_EQ(y.data.scalp->triangles(), scalp->triangles());
        EXPECT_EQ(y.data.scalp->cornerUVs(), scalp->cornerUVs());
        EXPECT_EQ(y.data.guides->guides()[0].points[4], turn(flat->guides()[0].points[4]));

        const StrandsPtr a = Groom(z.data).evaluate(1.0f, 0.5);
        const StrandsPtr b = Groom(y.data).evaluate(1.0f, 0.5);
        ASSERT_GT(a->strandCount(), 100u);
        ASSERT_EQ(a->ids(), b->ids());
        // (Interpolate keeps its strands clear of the bent scalp through its rest pose's field, each
        // point taken there by its nearest vertex's frame: where two vertices are about as near,
        // rounding picks one, and the lift differs by the bend between them. A few points near
        // the skin, by a percent of the radius at most.)
        float worst = 0.0f;
        std::size_t apart = 0;
        for (std::size_t i = 0; i < a->positions().size(); ++i) {
            const float d = (b->positions()[i] - turn(a->positions()[i])).length();
            worst = std::max(worst, d);
            if (d >= 1e-3f) ++apart;
        }
        EXPECT_LT(worst, 0.02f) << (rooted ? "rooted" : "unrooted");
        EXPECT_LT(apart, a->positions().size() / 200) << (rooted ? "rooted" : "unrooted");

        // And back: the points exactly as they were (a turn swaps and negates).
        convertUpAxis(y, "Z");
        EXPECT_EQ(y.data.scalp->contentHash(), z.data.scalp->contentHash());
        EXPECT_EQ(GuideHierarchy::fromJson(y.data.hierarchyJson).toJson(), GuideHierarchy::fromJson(z.data.hierarchyJson).toJson());
        convertUpAxis(y, "Z");  // already: nothing
        EXPECT_EQ(y.data.scalp->contentHash(), z.data.scalp->contentHash());
    }
    EXPECT_THROW(convertUpAxis(file, "W"), GroomFileError);
}

// The look graph with a Gravity (straight down in a Z-up world, as a Z-up host writes it), a
// Direction leaving its direction out (its default, Y-up's down: a direction of that file's world
// all the same) and (with `collide`) a ColliderMesh "ball" through a Collide, before the widths.
std::string worldDirectionGraph(bool collide) {
    nlohmann::json g = nlohmann::json::parse(kLookGraph);
    g["nodes"].push_back({{"name", "gravity"}, {"type", "Gravity"}, {"params", {{"direction", {0.0, 0.0, -1.0}}, {"strength", 0.8}}}});
    g["nodes"].push_back({{"name", "comb"}, {"type", "Direction"}, {"params", {{"strength", 0.4}, {"along", "direction"}}}});
    if (collide) {
        g["nodes"].push_back({{"name", "ball"}, {"type", "ColliderMesh"}, {"params", {{"name", "ball"}}}});
        g["nodes"].push_back({{"name", "collide"}, {"type", "Collide"}, {"params", {{"offset", 0.02}}}});
    }
    auto& c = g["connections"];
    for (auto it = c.begin(); it != c.end(); ++it) {
        if ((*it)["from"] == "clump" && (*it)["to"] == "width") {
            c.erase(it);
            break;
        }
    }
    c.push_back({{"from", "clump"}, {"to", "gravity"}, {"input", "strands"}});
    c.push_back({{"from", "gravity"}, {"to", "comb"}, {"input", "strands"}});
    if (collide) {
        c.push_back({{"from", "comb"}, {"to", "collide"}, {"input", "strands"}});
        c.push_back({{"from", "ball"}, {"to", "collide"}, {"input", "collider"}});
        c.push_back({{"from", "collide"}, {"to", "width"}, {"input", "strands"}});
    } else {
        c.push_back({{"from", "comb"}, {"to", "width"}, {"input", "strands"}});
    }
    return g.dump();
}

// A small ball where the hair falls (Z-up: below and to the side of the head), with a rest pose.
MeshPtr ball() {
    const auto unit = test::makeSphere(8, 16);
    std::vector<Vec3f> points = unit->points(), rest = unit->points();
    for (Vec3f& p : points) p = p * 0.35f + Vec3f{0.6f, 0.2f, -0.9f};
    for (Vec3f& p : rest) p = p * 0.35f + Vec3f{0.6f, 0.2f, -0.85f};
    auto mesh = std::make_shared<ScalpMesh>(std::move(points), unit->triangles());
    mesh->setRestPoints(std::move(rest));
    return mesh;
}

nlohmann::json nodeParams(const std::string& graphJson, const std::string& node) {
    const nlohmann::json graph = nlohmann::json::parse(graphJson);
    for (const auto& n : graph["nodes"]) {
        if (n["name"] == node) return n.value("params", nlohmann::json::object());
    }
    return nullptr;
}

TEST(GroomFile, TurnsWorldDirectionsWithItsPoints) {
    // Every param that is a world direction says so in its spec (worldDirection), and turns with
    // the points: a Z-up groom read Y-up grows the same hair, turned, Gravity and Direction (by its
    // default) included; a collider of the file's turns with it. (Collide itself is left out of the
    // comparison: a point deep in a mesh is pushed out by its nearest side, which a rounding can flip.)
    const auto specs = nlohmann::json::parse(operatorSpecsJson());
    std::set<std::string> flagged;
    for (const auto& op : specs) {
        for (const auto& p : op["params"]) {
            if (p.value("worldDirection", false)) flagged.insert(op["type"].get<std::string>() + "." + p["name"].get<std::string>());
        }
    }
    EXPECT_EQ(flagged, (std::set<std::string>{"Displace.direction", "Gravity.direction", "Clump.flattenDirection",
                                              "Mirror.axis", "Simulate.currentDirection", "Simulate.gravityDirection",
                                              "Simulate.windDirection", "Wind.direction", "Bend.direction", "Direction.direction", "Plume.direction",
                                              "Transform.pivot", "Curl.weightDirection"}));

    auto scalp = std::make_shared<ScalpMesh>(sphere()->withPoints(sphere()->points()));
    GroomFile z;
    z.data = groomData(scalp);
    z.data.graphJson = worldDirectionGraph(false);
    z.data.meshes["ball"] = ball();
    z.upAxis = "Z";
    GroomFile y = z;
    convertUpAxis(y, "Y");
    auto turn = [](const Vec3f& p) { return Vec3f(p.x, p.z, -p.y); };
    EXPECT_EQ(nodeParams(y.data.graphJson, "gravity")["direction"], nlohmann::json::array({0.0, -1.0, -0.0}));
    EXPECT_EQ(nodeParams(y.data.graphJson, "comb")["direction"], nlohmann::json::array({0.0, 0.0, 1.0}));  // its default, turned
    ASSERT_TRUE(y.data.meshes.count("ball"));
    const ScalpMesh& b = *y.data.meshes.at("ball");
    EXPECT_EQ(b.points()[3], turn(z.data.meshes.at("ball")->points()[3]));
    EXPECT_EQ(b.restPoints()[3], turn(z.data.meshes.at("ball")->restPoints()[3]));

    const StrandsPtr a = Groom(z.data).evaluate(1.0f, 0.5);
    const StrandsPtr c = Groom(y.data).evaluate(1.0f, 0.5);
    ASSERT_GT(a->strandCount(), 100u);
    ASSERT_EQ(a->ids(), c->ids());
    float worst = 0.0f;
    for (std::size_t i = 0; i < a->positions().size(); ++i) worst = std::max(worst, (c->positions()[i] - turn(a->positions()[i])).length());
    EXPECT_LT(worst, 1e-3f);
    // Without turning the params, Gravity would pull sideways in the turned world.
    GroomFile unturned = y;
    unturned.data.graphJson = z.data.graphJson;
    const StrandsPtr wrong = Groom(unturned.data).evaluate(1.0f, 0.5);
    float off = 0.0f;
    for (std::size_t i = 0; i < a->positions().size(); ++i) off = std::max(off, (wrong->positions()[i] - turn(a->positions()[i])).length());
    EXPECT_GT(off, 0.05f);

    // And back: the params as they were (a turn swaps and negates).
    convertUpAxis(y, "Z");
    EXPECT_EQ(nodeParams(y.data.graphJson, "gravity")["direction"], nlohmann::json::array({0.0, 0.0, -1.0}));
    EXPECT_EQ(nodeParams(y.data.graphJson, "comb")["direction"], nlohmann::json::array({0.0, -1.0, 0.0}));
    EXPECT_EQ(y.data.meshes.at("ball")->contentHash(), z.data.meshes.at("ball")->contentHash());
}

TEST(GroomFile, TurnsMirrorPlanesAndCompoundsWithTheWorld) {
    // Mirror's axis is a world axis: Y-up's y is Z-up's z; Y-up's z is Z-up's -y, which puts the
    // plane's offset and kept side on the other side of 0. Inside a compound too, Wind and
    // Simulate's directions as vectors.
    nlohmann::json inner = {{"nodes", nlohmann::json::array({{{"name", "in"}, {"type", "Input"}},
                                                              {{"name", "wind"}, {"type", "Wind"}, {"params", {{"direction", {1.0, 2.0, 3.0}}}}},
                                                              {{"name", "out"}, {"type", "Output"}}})},
                            {"connections", nlohmann::json::array({{{"from", "in"}, {"to", "wind"}, {"input", "strands"}},
                                                                    {{"from", "wind"}, {"to", "out"}, {"input", "strands"}}})},
                            {"output", "out"}};
    nlohmann::json graph = {
        {"version", 1},
        {"nodes", nlohmann::json::array({
                      {{"name", "my"}, {"type", "Mirror"}, {"params", {{"axis", "y"}, {"offset", 0.5}, {"side", "positive"}}}},
                      {{"name", "mz"}, {"type", "Mirror"}, {"params", {{"axis", "z"}, {"offset", 0.5}, {"side", "positive"}}}},
                      {{"name", "mx"}, {"type", "Mirror"}, {"params", {{"axis", "x"}, {"offset", 0.5}}}},
                      {{"name", "sim"}, {"type", "Simulate"}, {"params", {{"windDirection", {0.0, 0.0, 1.0}}}}},
                      {{"name", "compound"}, {"type", "Compound"}, {"params", {{"definition", nlohmann::json{{"graph", inner}}.dump()}}}},
                  })},
        {"connections", nlohmann::json::array()},
        {"output", "mx"}};
    GroomFile file;
    file.data.graphJson = graph.dump();
    convertUpAxis(file, "Z");  // Y-up to Z-up: (x, y, z) is (x, -z, y)
    const std::string& g = file.data.graphJson;
    EXPECT_EQ(nodeParams(g, "my")["axis"], "z");
    EXPECT_EQ(nodeParams(g, "my")["offset"], 0.5);
    EXPECT_EQ(nodeParams(g, "my")["side"], "positive");
    EXPECT_EQ(nodeParams(g, "mz")["axis"], "y");
    EXPECT_EQ(nodeParams(g, "mz")["offset"], -0.5);
    EXPECT_EQ(nodeParams(g, "mz")["side"], "negative");
    EXPECT_EQ(nodeParams(g, "mx")["axis"], "x");
    EXPECT_EQ(nodeParams(g, "mx")["offset"], 0.5);
    EXPECT_EQ(nodeParams(g, "sim")["windDirection"], nlohmann::json::array({0.0, -1.0, 0.0}));
    EXPECT_EQ(nodeParams(g, "sim")["gravityDirection"], nlohmann::json::array({0.0, -0.0, -1.0}));  // its default, turned
    const auto definition = nlohmann::json::parse(nodeParams(g, "compound")["definition"].get<std::string>());
    EXPECT_EQ(nodeParams(definition["graph"].dump(), "wind")["direction"], nlohmann::json::array({1.0, -3.0, 2.0}));
    convertUpAxis(file, "Y");  // and back
    for (const char* node : {"my", "mz", "mx"}) {
        for (const char* param : {"axis", "offset"}) EXPECT_EQ(nodeParams(file.data.graphJson, node)[param], nodeParams(graph.dump(), node)[param]) << node;
    }
    EXPECT_EQ(nodeParams(file.data.graphJson, "mz")["side"], "positive");
}

TEST(GroomFile, KeepsColliders) {
    // The ColliderMesh nodes' meshes are stored with the groom (points, triangles, polygons, the
    // render's subdivision, the rest pose): a reader's Collide is the writer's, bit for bit.
    auto scalp = std::make_shared<ScalpMesh>(sphere()->withPoints(sphere()->points()));
    GroomFile file;
    file.data = groomData(scalp);
    file.data.graphJson = worldDirectionGraph(true);
    auto collider = std::make_shared<ScalpMesh>(*ball());
    collider->setRenderSubdivision(1, "test");
    file.data.meshes["ball"] = collider;
    const GroomFile back = groomFileFromJson(groomFileToJson(file));
    ASSERT_EQ(back.data.meshes.size(), 1u);
    const ScalpMesh& b = *back.data.meshes.at("ball");
    EXPECT_EQ(b.contentHash(), collider->contentHash());
    EXPECT_EQ(b.restPoints(), collider->restPoints());
    EXPECT_EQ(b.renderSubdivision(), 1);
    const StrandsPtr written = Groom(file.data).evaluate(1.0f, 0.0);
    const StrandsPtr read = Groom(back.data).evaluate(1.0f, 0.0);
    ASSERT_EQ(written->positions().size(), read->positions().size());
    EXPECT_EQ(0, std::memcmp(written->positions().data(), read->positions().data(), written->positions().size() * sizeof(Vec3f)));
    GroomData without = file.data;
    without.meshes.clear();
    EXPECT_NE(Groom(without).evaluate(1.0f, 0.0)->positions(), written->positions());  // the ball does push the hair
    EXPECT_EQ(groomFileToJson(GroomFile{}).find("colliders"), std::string::npos);  // none: not written
}

TEST(GroomFile, KnowsTheScalpItWasMadeOn) {
    // The scalp's fingerprint (its point count and triangles) is written with it; a session fed
    // a scalp of another vertex order or triangulation says so in its errors, a deformed one of the
    // same topology does not.
    const auto scalp = sphere();
    GroomFile file;
    file.data = groomData(scalp);
    const std::string text = groomFileToJson(file);
    EXPECT_NE(text.find("\"topology\":\"" + scalpFingerprintText(scalp->topologyFingerprint()) + "\""), std::string::npos);
    const GroomFile back = groomFileFromJson(text);
    EXPECT_EQ(back.data.scalpFingerprint, scalp->topologyFingerprint());
    EXPECT_EQ(parseScalpFingerprint(scalpFingerprintText(back.data.scalpFingerprint)), back.data.scalpFingerprint);

    auto errorsWith = [&](MeshPtr fed) {
        GroomSession session(back.data);
        session.setScalp(std::move(fed));
        ViewRequest request;
        return session.evaluate(request).errors;
    };
    auto has = [](const std::vector<std::string>& errors) {
        return std::find(errors.begin(), errors.end(), std::string(kScalpTopologyChanged)) != errors.end();
    };
    EXPECT_FALSE(has(errorsWith(scalp)));
    EXPECT_FALSE(has(errorsWith(std::make_shared<ScalpMesh>(scalp->withPoints(deformed(*scalp))))));  // a pose
    // Each triangle's corners turned (the same faces, another corner first): other triangles.
    std::vector<ScalpMesh::Triangle> turned = scalp->triangles();
    for (auto& t : turned) t = {t[1], t[2], t[0]};
    const std::vector<std::string> errors = errorsWith(std::make_shared<ScalpMesh>(scalp->points(), turned));
    EXPECT_TRUE(has(errors));
    // The strands come all the same.
    GroomSession session(back.data);
    session.setScalp(std::make_shared<ScalpMesh>(scalp->points(), turned));
    ViewRequest request;
    EXPECT_GT(session.evaluate(request).strands->strandCount(), 100u);
    session.setScalpFingerprint(0);  // moved there on purpose: not known, nothing said
    EXPECT_FALSE(has(session.evaluate(request).errors));
    // A file whose scalp has no stored fingerprint takes its own.
    nlohmann::json j = nlohmann::json::parse(text);
    j["scalp"].erase("topology");
    EXPECT_EQ(groomFileFromJson(j.dump()).data.scalpFingerprint, scalp->topologyFingerprint());
}

TEST(FrameTime, OneRuleForEveryHost) {
    // frame / fps, an NTSC rate exact however it is spelled: Maya's 23.976023976..., a typed 23.976.
    for (const double fps : {24.0, 25.0, 30.0, 12.5}) {
        for (int f = -10; f <= 500; ++f) EXPECT_EQ(frameToSeconds(f, fps), f / fps);
        EXPECT_EQ(canonicalFramesPerSecond(fps), fps);
    }
    const struct {
        double spelled, base;
    } ntsc[] = {{23.976, 24.0}, {24000.0 / 1001.0, 24.0}, {23.976023976023978, 24.0}, {29.97, 30.0}, {30000.0 / 1001.0, 30.0},
                {59.94, 60.0}, {47.952, 48.0}, {119.88, 120.0}};
    for (const auto& r : ntsc) {
        EXPECT_EQ(canonicalFramesPerSecond(r.spelled), r.base * 1000.0 / 1001.0) << r.spelled;
        for (int f = -10; f <= 2000; ++f) {
            const double seconds = frameToSeconds(f, r.spelled);
            EXPECT_EQ(seconds, f * 1001.0 / (r.base * 1000.0)) << r.spelled << " " << f;
            EXPECT_EQ(std::lround(secondsToFrame(seconds, r.spelled)), f);
            EXPECT_NEAR(secondsToFrame(seconds, r.spelled), f, 1e-9);
        }
        EXPECT_EQ(frameToSeconds(7.25, r.spelled), 7.25 * 1001.0 / (r.base * 1000.0));  // a motion key
    }
    EXPECT_EQ(frameToSeconds(48.0, 0.0), 2.0);  // a bad rate is 24
    EXPECT_EQ(frameToSeconds(48.0, -5.0), 2.0);
    EXPECT_EQ(canonicalFramesPerSecond(23.98), 23.98);  // not an NTSC rate
    // The session's rate is the rule's (Simulate's steps are the same whoever set it).
    GroomSession session;
    session.setFramesPerSecond(23.976);
    EXPECT_EQ(session.framesPerSecond(), 24000.0 / 1001.0);
}

TEST(GroomFile, PaintedMapsComeBackFromTheScalp) {
    // A host writes its vertex maps as color sets of the scalp (withVertexMaps) with the catalog;
    // a reader takes the values back (scalpMapsFromGroomData), gray or color as written.
    const auto raw = sphere();
    ScalpMaps maps;
    ScalpMapEntry mask{"vertices", {}, "", 1};
    ScalpMapEntry tint{"vertices", {}, "", 4};
    ScalpMapEntry plain{"vertices", {}, "", 4};  // a color map painted gray so far
    for (std::size_t v = 0; v < raw->pointCount(); ++v) {
        mask.values.push_back(static_cast<float>(v % 10) / 10.0f);
        tint.values.insert(tint.values.end(), {0.5f, static_cast<float>(v % 7) / 7.0f, 0.25f, 1.0f});
        plain.values.insert(plain.values.end(), {0.3f, 0.3f, 0.3f, 1.0f});
    }
    maps.maps = {{"mask", mask}, {"tint", tint}, {"plain", plain}, {"detail", {"texture", {}, "masks/hair/detail.png", 4}}};
    GroomData data;
    data.scalp = withVertexMaps(raw, maps, "", nullptr, nullptr);
    data.maps = maps.catalog();
    data.maps["layer"] = {"guides", "", 1};  // a guide layer: the hierarchy's

    const GroomFile back = groomFileFromJson(groomFileToJson(GroomFile{data}));
    const ScalpMaps read = scalpMapsFromGroomData(back.data);
    ASSERT_EQ(read.maps.size(), 4u);
    EXPECT_EQ(read.maps.at("mask").channels, 1);
    EXPECT_EQ(read.maps.at("mask").values, mask.values);
    EXPECT_EQ(read.maps.at("tint").channels, 4);
    EXPECT_EQ(read.maps.at("tint").values, tint.values);
    EXPECT_EQ(read.maps.at("plain").channels, 4);
    EXPECT_EQ(read.maps.at("plain").values, plain.values);
    EXPECT_EQ(read.maps.at("detail").kind, "texture");
    EXPECT_EQ(read.maps.at("detail").texture, "masks/hair/detail.png");
    EXPECT_EQ(read.maps.at("detail").channels, 4);
    EXPECT_EQ(read.toJson(), maps.toJson());
}

// ---------------------------------------------------------------------------

TEST(ExportView, DescribesCountsUvsColorsAndBounds) {
    ScalpMesh mesh(*test::makePlane(2));
    mesh.setCornerUVs(std::vector<ScalpMesh::UV>(mesh.triangleCount() * 3, {0.5f, 0.25f}));
    StrandSet s;
    s.addStrand(1, {{0, 0, 0}, {0, 2, 0}}, {1.0f, 0.5f}, StrandRoot{0, 0.2f, 0.2f});
    s.addStrand(2, {{3, 0, 0}}, {}, StrandRoot{-1, 0.0f, 0.0f});
    ExportView v = describeExport(s, &mesh);
    EXPECT_EQ(v.counts, (std::vector<std::int32_t>{2, 1}));
    ASSERT_TRUE(v.hasUVs());
    EXPECT_FLOAT_EQ(v.rootUVs[0][0], 0.5f);
    EXPECT_FLOAT_EQ(v.rootUVs[0][1], 0.25f);
    EXPECT_FLOAT_EQ(v.rootUVs[1][0], 0.0f);  // not rooted
    EXPECT_FALSE(v.hasColors());
    EXPECT_FLOAT_EQ(v.radius(1), 0.25f);
    EXPECT_FLOAT_EQ(v.width(2), 1.0f);
    EXPECT_EQ(v.boundsLo, Vec3f(-0.5f, -0.5f, -0.5f));
    EXPECT_EQ(v.boundsHi, Vec3f(3.5f, 2.25f, 0.5f));
    EXPECT_TRUE(v.strandChannels.empty());

    s.addStrandChannel(kMelaninChannel, 1.0f);
    v = describeExport(s, nullptr);
    EXPECT_FALSE(v.hasUVs());
    ASSERT_TRUE(v.hasColors());
    EXPECT_LT(v.colors[0][0], 0.2f);  // black hair (toned down, as hair_color.h renders it)
    EXPECT_EQ(v.strandChannels, std::vector<std::string>{kMelaninChannel});
    EXPECT_EQ(describeExport(StrandSet{}, nullptr).boundsLo, Vec3f(0, 0, 0));

    const std::vector<std::string> channels = {"a-b", "a_b", "st", "strandId", "9x"};
    EXPECT_EQ(usdChannelNames(channels), (std::vector<std::string>{"a_b", "a_b_2", "st_2", "strandId_2", "_9x"}));
}

// ---------------------------------------------------------------------------

// A binary PGM, 8-bit (maxval 255) or 16-bit (maxval 65535, big-endian).
template <class F>
void writePgm(const fs::path& path, int w, int h, int maxval, F value) {
    std::ofstream f(path, std::ios::binary);
    f << "P5\n" << w << " " << h << "\n" << maxval << "\n";
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int v = value(x, y);
            if (maxval > 255) f.put(static_cast<char>((v >> 8) & 0xFF));
            f.put(static_cast<char>(v & 0xFF));
        }
    }
}

TEST(Texture, ReadsUtf8PathsSixteenBitsAndUdimTiles) {
    const fs::path dir = fs::temp_directory_path() / fs::u8path("groomlab_t\xC3\xA9x");  // a non-ASCII folder
    fs::create_directories(dir);

    // 16 bits keep values 8 bits cannot hold.
    writePgm(dir / "deep.pgm", 2, 1, 65535, [](int x, int) { return x == 0 ? 1000 : 60000; });
    const TexturePtr deep = loadTexture((dir / "deep.pgm").u8string());
    EXPECT_EQ(deep->width(), 2);
    EXPECT_FALSE(deep->isUdim());
    EXPECT_NEAR(deep->sample(0.25f, 0.5f)[0], 1000.0f / 65535.0f, 1e-6f);
    EXPECT_NEAR(deep->sample(0.75f, 0.5f)[0], 60000.0f / 65535.0f, 1e-6f);

    // UDIM: tile 1001 at u in [0, 1), 1002 at [1, 2); no 1003.
    writePgm(dir / "map.1001.pgm", 2, 2, 255, [](int, int) { return 50; });
    writePgm(dir / "map.1002.pgm", 2, 2, 255, [](int, int) { return 200; });
    const TexturePtr tiles = loadTexture((dir / "map.<UDIM>.pgm").u8string());
    ASSERT_TRUE(tiles->isUdim());
    EXPECT_NEAR(tiles->sample(0.5f, 0.5f)[0], 50.0f / 255.0f, 1e-6f);
    EXPECT_NEAR(tiles->sample(1.5f, 0.5f)[0], 200.0f / 255.0f, 1e-6f);
    EXPECT_EQ(tiles->sample(2.5f, 0.5f)[3], 0.0f);   // a tile with no file: transparent black
    EXPECT_EQ(tiles->sample(0.5f, -0.5f)[0], 0.0f);  // off the grid
    EXPECT_EQ(tiles->width(), 2);
    EXPECT_TRUE(tiles->error().empty());
    EXPECT_NE(fileStamp((dir / "map.<UDIM>.pgm").u8string()), 0u);
    // Naming tile 1001 names the set, since 1002 sits beside it: the same texture.
    const TexturePtr named = loadTexture((dir / "map.1001.pgm").u8string());
    EXPECT_EQ(named.get(), tiles.get());
    // A lone tile number is just a name: a plain image, which repeats.
    writePgm(dir / "lone.1001.pgm", 2, 2, 255, [](int, int) { return 50; });
    const TexturePtr lone = loadTexture((dir / "lone.1001.pgm").u8string());
    EXPECT_FALSE(lone->isUdim());
    EXPECT_NEAR(lone->sample(1.5f, 0.5f)[0], 50.0f / 255.0f, 1e-6f);
    EXPECT_THROW(loadTexture((dir / "none.<UDIM>.pgm").u8string()), TextureError);
    // A number past the tile grid (1001-1100) is just a name too, even beside a tile.
    writePgm(dir / "grid.1001.pgm", 2, 2, 255, [](int, int) { return 50; });
    writePgm(dir / "grid.1500.pgm", 2, 2, 255, [](int, int) { return 200; });
    const TexturePtr past = loadTexture((dir / "grid.1500.pgm").u8string());
    EXPECT_FALSE(past->isUdim());
    EXPECT_NEAR(past->sample(0.5f, 0.5f)[0], 200.0f / 255.0f, 1e-6f);
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------

TEST(Lod, OrthographicCameraSeesABoxAndNeverThins) {
    StrandSet s;
    StrandId id = 0;
    for (const float x : {-30.0f, -3.0f, 0.0f, 3.0f, 30.0f}) {
        for (const float z : {-5.0f, -500.0f}) s.addStrand(id++, {{x, 0, z}, {x, 1, z}}, {0.01f, 0.01f});
    }
    LodCamera cam;  // at the origin, looking down -z
    cam.orthographic = true;
    cam.orthoHalfWidth = 5.0f;
    cam.orthoHalfHeight = 5.0f;
    LodSettings settings;
    settings.frustum = true;
    settings.margin = 0.0f;
    settings.distance = true;
    settings.nearDistance = 10.0f;
    settings.farDistance = 100.0f;
    settings.minDensity = 0.0f;
    const LodPlan ortho = planLod(s, cam, settings);
    EXPECT_EQ(ortho.thinned, 0u);  // no distance in an orthographic view
    EXPECT_EQ(ortho.culled, 4u);   // |x| = 30 is outside the box, near or far
    EXPECT_EQ(ortho.kept.size(), 6u);
    for (const float scale : ortho.widthScale) EXPECT_FLOAT_EQ(scale, 1.0f);

    // The same settings through a 90-degree perspective camera: the wedge culls
    // the wide near strands and the distance thins every far one (density 0).
    cam.orthographic = false;
    cam.tanHalfFovX = cam.tanHalfFovY = 1.0f;
    const LodPlan persp = planLod(s, cam, settings);
    EXPECT_EQ(persp.culled, 2u);
    EXPECT_EQ(persp.thinned, 5u);
    EXPECT_EQ(persp.kept.size(), 3u);
}

// kLookGraph with a Cards node (at most `maxCards` cards) before the output: one LOD of an export.
std::string cardLod(int maxCards) {
    std::string g = kLookGraph;
    const std::string nodes = R"({"name": "output", "type": "Output"})";
    g.replace(g.find(nodes), nodes.size(),
              R"({"name": "cards", "type": "Cards", "params": {"segments": 4, "maxCards": )" + std::to_string(maxCards) +
                  "}}, " + nodes);
    const std::string link = R"({"from": "width", "to": "output", "input": "strands"})";
    g.replace(g.find(link), link.size(),
              R"({"from": "width", "to": "cards", "input": "strands"}, {"from": "cards", "to": "output", "input": "strands"})");
    return g;
}

void expectSameCards(const StrandSet& a, const StrandSet& b, const std::string& what) {
    ASSERT_EQ(a.ids(), b.ids()) << what;
    ASSERT_EQ(a.positions(), b.positions()) << what;
    ASSERT_EQ(a.cardLayers().size(), b.cardLayers().size()) << what;
    for (std::size_t l = 0; l < a.cardLayers().size(); ++l) {
        const CardMesh& x = *a.cardLayers()[l];
        const CardMesh& y = *b.cardLayers()[l];
        ASSERT_EQ(x.cards.size(), y.cards.size()) << what;
        EXPECT_EQ(x.positions, y.positions) << what;
        EXPECT_EQ(x.quads, y.quads) << what;
        EXPECT_EQ(x.u, y.u) << what;
        EXPECT_EQ(x.v, y.v) << what;
        for (std::size_t c = 0; c < x.cards.size(); ++c) EXPECT_EQ(x.cards[c].strands, y.cards[c].strands) << what;
    }
}

TEST(Groom, SwappedLodGraphsReuseWhatComesBeforeTheirCards) {
    // An export's LODs through one Groom: each the same as a fresh Groom of that graph makes, and
    // only Cards and the output run again (Scatter, Interpolate and the clumps are adopted).
    GroomData data = groomData(sphere());
    data.graphJson = cardLod(40);
    Groom groom(data);
    const StrandsPtr first = groom.evaluate(1.0f, 0.0, true);
    ASSERT_TRUE(first);
    ASSERT_FALSE(first->cardLayers().empty());
    for (const int maxCards : {20, 8, 40}) {
        groom.setGraphJson(cardLod(maxCards));
        EXPECT_EQ(groom.data().graphJson, cardLod(maxCards));
        const StrandsPtr swapped = groom.evaluate(1.0f, 0.0, true);
        EXPECT_EQ(groom.evaluationCount(), 2u) << "LOD " << maxCards << ": more than Cards and the output ran";
        GroomData fresh = data;
        fresh.graphJson = cardLod(maxCards);
        const StrandsPtr expected = Groom(fresh).evaluate(1.0f, 0.0, true);
        ASSERT_TRUE(swapped && expected);
        expectSameCards(*swapped, *expected, "LOD " + std::to_string(maxCards));
        EXPECT_LE(swapped->cardLayers()[0]->cards.size(), static_cast<std::size_t>(maxCards));
    }
    // A graph that does not load leaves the groom as it was.
    EXPECT_THROW(groom.setGraphJson("{not json"), GraphError);
    EXPECT_EQ(groom.data().graphJson, cardLod(40));
    const StrandsPtr again = groom.evaluate(1.0f, 0.0, true);
    EXPECT_EQ(groom.evaluationCount(), 2u);  // the last LOD's results, all cached
    expectSameCards(*again, *first, "after a bad graph");
    // A change upstream runs from there on: Scatter and everything after it.
    std::string denser = cardLod(40);
    const std::string density = R"("density": 300.0)";
    denser.replace(denser.find(density), density.size(), R"("density": 320.0)");
    groom.setGraphJson(denser);
    ASSERT_TRUE(groom.evaluate(1.0f, 0.0, true));
    EXPECT_EQ(groom.evaluationCount(), 6u);  // scatter, interpolate, clump, width, cards, output
}

TEST(Groom, ASwappedGraphCarriesTheRestPoseAlong) {
    // Instances pinned to rest: after a swap the rest pose is the new graph's, as a fresh Groom's.
    GroomData data = groomData(sphere());
    data.graphJson = instanceGraph(true);
    Groom groom(data);
    groom.setGuidePoses(scaledPoses(sphere(), 1.5f));
    ASSERT_TRUE(groom.evaluate(1.0f, 0.0));
    EXPECT_EQ(groom.restEvaluations(), 1u);
    const std::string other = instanceGraph(true, R"(, "count": 2)");
    groom.setGraphJson(other);
    const StrandsPtr swapped = groom.evaluate(1.0f, 0.0);
    GroomData fresh = data;
    fresh.graphJson = other;
    Groom expectedGroom(fresh);
    expectedGroom.setGuidePoses(scaledPoses(sphere(), 1.5f));
    const StrandsPtr expected = expectedGroom.evaluate(1.0f, 0.0);
    ASSERT_TRUE(swapped && expected && swapped->instances() && expected->instances());
    ASSERT_EQ(swapped->instances()->instances.size(), expected->instances()->instances.size());
    for (std::size_t i = 0; i < swapped->instances()->instances.size(); ++i) {
        EXPECT_EQ(swapped->instances()->instances[i].restDistance, expected->instances()->instances[i].restDistance);
        EXPECT_EQ(swapped->instances()->instances[i].t, expected->instances()->instances[i].t);
    }
}

TEST(Graph, AClonedDocumentKeepsTheCacheAndThenItsOwn) {
    GraphDocument doc = loadGraphJson(cardLod(30));
    doc.graph.setParamControls(doc.node("clump"), {{"strength", {"clumpStrength"}}});
    auto hierarchy = std::make_shared<GuideHierarchy>(GuideHierarchy::fromJson(hierarchyJson()));
    const auto scalp = sphere();
    hierarchy->evaluate(scalp.get());
    ExternalInputs ext;
    setGroomExternals(ext, scalp, nullptr, hierarchy);
    setControlExternals(ext, {{"clumpStrength", 0.3f}});
    EvalContext ctx;
    ctx.externals = &ext;
    ctx.buildCards = true;
    const StrandsPtr original = doc.graph.evaluateStrands(doc.output, ctx);
    ASSERT_TRUE(original);

    GraphDocument copy = doc.clone();
    EXPECT_EQ(saveGraphJson(copy), saveGraphJson(doc));  // params, links, keyed params, names
    EXPECT_EQ(copy.output, doc.output);
    EXPECT_EQ(copy.graph.evaluateStrands(copy.output, ctx), original);  // the cache came along
    EXPECT_EQ(copy.graph.evaluationCount(), 0u);

    // Its own from then on: an edit to the copy re-runs it there only.
    copy.graph.node(copy.node("cards")).params().set("maxCards", std::int64_t{10});
    const StrandsPtr edited = copy.graph.evaluateStrands(copy.output, ctx);
    EXPECT_EQ(copy.graph.evaluationCount(), 2u);
    GraphDocument fresh = loadGraphJson(saveGraphJson(copy));
    expectSameCards(*edited, *fresh.graph.evaluateStrands(fresh.output, ctx), "edited copy");
    const std::size_t before = doc.graph.evaluationCount();
    EXPECT_EQ(doc.graph.evaluateStrands(doc.output, ctx), original);
    EXPECT_EQ(doc.graph.evaluationCount(), before);
    EXPECT_EQ(doc.graph.node(doc.node("cards")).params().getInt("maxCards"), 30);
}

}  // namespace
}  // namespace groomlab
