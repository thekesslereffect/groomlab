// SPDX-License-Identifier: Apache-2.0
// Driver curves posing guides (drive_guides.h) and the DriveGuides node.

#include <cmath>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "groomlab/drive_guides.h"
#include "groomlab/graph.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_session.h"
#include "groomlab/operators.h"
#include "groomlab/ramp.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// p turned by `degrees` about the z axis through `pivot`.
Vec3f turnZ(const Vec3f& p, const Vec3f& pivot, float degrees) {
    const float a = degrees * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
    const Vec3f d = p - pivot;
    return pivot + Vec3f{d.x * c - d.y * s, d.x * s + d.y * c, d.z};
}

std::vector<Vec3f> turned(std::vector<Vec3f> points, const Vec3f& pivot, float degrees) {
    for (Vec3f& p : points) p = turnZ(p, pivot, degrees);
    return points;
}

// A driver straight up from the origin, 8 segments of 1/8.
GuideSet upright(std::uint64_t id = 7, Vec3f root = {0, 0, 0}) {
    GuideSet set;
    set.add(id, test::straightCurve(root, {0, 1, 0}, 8));
    return set;
}

// Guides straight up beside it at `offsets` (x), 10 segments each.
StrandSet besides(const std::vector<float>& offsets, Vec3f base = {0, 0, 0}) {
    StrandSet out;
    for (std::size_t k = 0; k < offsets.size(); ++k) {
        out.addStrand(static_cast<StrandId>(k + 1), test::straightCurve(base + Vec3f{offsets[k], 0, 0}, {0, 1, 0}, 10));
    }
    return out;
}

// Every curve attached to every bound driver.
DriverAttachments allAttached(const StrandSet& curves, const GuideSet& bound) {
    DriverAttachments a;
    std::vector<std::uint64_t> ids;
    for (const Guide& d : bound.guides()) ids.push_back(d.id);
    for (const StrandId id : curves.ids()) a.byGuide[id] = ids;
    a.hash = 1;
    return a;
}

StrandSet drive(const StrandSet& curves, const GuideSet& bound, const GuideSet& now, float amount = 1.0f,
                const Ramp& profile = Ramp::constant(1.0f), const DriverAttachments* attached = nullptr) {
    StrandSet out = curves;
    const DriveBinding binding = bindToDrivers(curves, bound, attached ? *attached : allAttached(curves, bound));
    driveCurves(out, binding, bound, now, std::vector<float>(curves.strandCount(), amount), profile);
    return out;
}

TEST(DriveGuides, DriversAsBoundLeaveTheGuidesAsTheyAre) {
    const GuideSet bound = upright();
    const StrandSet curves = besides({0.05f, -0.1f, 0.2f});
    const StrandSet out = drive(curves, bound, bound);
    for (std::size_t s = 0; s < curves.strandCount(); ++s) {
        for (std::size_t i = 0; i < curves.strandPointCount(s); ++i) {
            EXPECT_LT((out.strandPoints(s)[i] - curves.strandPoints(s)[i]).length(), 1e-5f) << s << ":" << i;
        }
    }
}

TEST(DriveGuides, AGuideTurnsRoundItsDriverAsTheDriverTwists) {
    // The driver as bound but twisted 90 degrees at its tip (none at its root): a guide beside it
    // at +x is carried a quarter turn round it at the tip, half that halfway, its length kept off
    // the driver; the bind's own twist is the zero.
    GuideSet bound = upright();
    GuideSet now = bound;
    now.guides()[0].pointAttributes[kDriverTwistLayer] = driverTwistLayer(now.guides()[0].points, 90.0);
    const StrandSet curves = besides({0.1f});
    const StrandSet out = drive(curves, bound, now);
    const Vec3f tip = out.strandPoints(0)[10];
    EXPECT_NEAR(tip.y, 1.0f, 1e-4f);
    EXPECT_NEAR(std::abs(tip.z), 0.1f, 2e-3f) << "a quarter turn round the driver at the tip";
    EXPECT_NEAR(tip.x, 0.0f, 2e-3f);
    const Vec3f mid = out.strandPoints(0)[5];
    EXPECT_NEAR(std::atan2(std::abs(mid.z), mid.x), 3.14159265f / 4.0f, 0.02f) << "half of it halfway";
    EXPECT_NEAR(Vec3f(mid.x, 0, mid.z).length(), 0.1f, 1e-3f);
    // Bound with that twist already: no turn.
    bound = now;
    const StrandSet same = drive(curves, bound, now);
    EXPECT_LT((same.strandPoints(0)[10] - curves.strandPoints(0)[10]).length(), 1e-5f);
    // The twist goes through the bind's JSON.
    DriverBind bind;
    bind.drivers = now;
    const DriverBind back = DriverBind::fromJson(bind.toJson());
    ASSERT_TRUE(back.drivers.guides()[0].pointAttributes.count(kDriverTwistLayer));
    EXPECT_EQ(back.drivers.guides()[0].pointAttributes.at(kDriverTwistLayer),
              now.guides()[0].pointAttributes.at(kDriverTwistLayer));
}

TEST(DriveGuides, AGuideTurnsWithItsDriverItsRootStaying) {
    // The driver swung 40 degrees about its root: every point of a guide beside it but its root
    // where the same swing takes it.
    const GuideSet bound = upright();
    GuideSet now;
    now.add(7, turned(bound.guides()[0].points, {0, 0, 0}, 40.0f));
    const StrandSet curves = besides({0.05f});
    const StrandSet out = drive(curves, bound, now);
    EXPECT_EQ(out.strandPoints(0)[0], curves.strandPoints(0)[0]) << "the root stays on the scalp";
    for (std::size_t i = 1; i < curves.strandPointCount(0); ++i) {
        const Vec3f want = turnZ(curves.strandPoints(0)[i], {0, 0, 0}, 40.0f);
        EXPECT_LT((out.strandPoints(0)[i] - want).length(), 2e-4f) << i;
    }
}

TEST(DriveGuides, AGuideKeepsItsPlaceRoundADriverThatBends) {
    // The driver's upper half bent over 90 degrees at its middle: a guide beside it (on the bend's
    // outside) keeps about its distance from it all along, and about its length (the outside of a
    // bend a little longer, as a tube's).
    const GuideSet bound = upright();
    std::vector<Vec3f> bent = bound.guides()[0].points;
    for (std::size_t i = 5; i < bent.size(); ++i) bent[i] = turnZ(bent[i], bent[4], -90.0f);
    GuideSet now;
    now.add(7, bent);
    const StrandSet curves = besides({-0.04f});
    const StrandSet out = drive(curves, bound, now);
    auto distanceTo = [](const std::vector<Vec3f>& line, const Vec3f& p) {
        float best = 1e9f;
        for (std::size_t k = 0; k + 1 < line.size(); ++k) {
            const Vec3f ab = line[k + 1] - line[k];
            const float t = std::clamp((p - line[k]).dot(ab) / ab.dot(ab), 0.0f, 1.0f);
            best = std::min(best, (p - (line[k] + ab * t)).length());
        }
        return best;
    };
    float before = 0.0f, after = 0.0f;
    for (std::size_t i = 1; i < curves.strandPointCount(0); ++i) {
        EXPECT_NEAR(distanceTo(bent, out.strandPoints(0)[i]), 0.04f, 0.012f) << i;
        before += (curves.strandPoints(0)[i] - curves.strandPoints(0)[i - 1]).length();
        after += (out.strandPoints(0)[i] - out.strandPoints(0)[i - 1]).length();
    }
    EXPECT_NEAR(after, before, before * 0.1f) << "about the length kept";
    EXPECT_GT(out.strandPoints(0)[10].x, 0.4f) << "the tip over with the driver's";
}

TEST(DriveGuides, StrengthAndProfileScaleTheMove) {
    const GuideSet bound = upright();
    GuideSet now;
    now.add(7, turned(bound.guides()[0].points, {0, 0, 0}, 30.0f));
    const StrandSet curves = besides({0.05f});
    const StrandSet full = drive(curves, bound, now), half = drive(curves, bound, now, 0.5f);
    const Vec3f tip = curves.strandPoints(0)[10];
    EXPECT_LT(((half.strandPoints(0)[10] - tip) - (full.strandPoints(0)[10] - tip) * 0.5f).length(), 1e-5f);
    const StrandSet rooted = drive(curves, bound, now, 1.0f, Ramp::linear(0.0f, 1.0f));
    EXPECT_LT((rooted.strandPoints(0)[1] - curves.strandPoints(0)[1]).length(),
              (full.strandPoints(0)[1] - curves.strandPoints(0)[1]).length() * 0.2f)
        << "near the root, little of the move along a rising profile";
    EXPECT_LT((rooted.strandPoints(0)[10] - full.strandPoints(0)[10]).length(), 1e-5f) << "the tip all of it";
}

TEST(DriveGuides, AGuideFollowsTheDriversItIsAttachedToAndNoOthers) {
    GuideSet bound;
    bound.add(1, test::straightCurve({-1, 0, 0}, {0, 1, 0}, 8));
    bound.add(2, test::straightCurve({1, 0, 0}, {0, 1, 0}, 8));
    GuideSet now;
    now.add(1, turned(bound.guides()[0].points, {-1, 0, 0}, 30.0f));
    now.add(2, bound.guides()[1].points);
    const StrandSet curves = besides({-1.0f, 0.0f, 1.0f, 6.0f});
    const StrandSet out = drive(curves, bound, now);
    auto moved = [&](std::size_t s) { return (out.strandPoints(s)[10] - curves.strandPoints(s)[10]).length(); };
    EXPECT_GT(moved(0), 0.4f) << "on the swung driver";
    EXPECT_LT(moved(2), 1e-3f) << "on the still one";
    EXPECT_GT(moved(1), moved(0) * 0.35f);
    EXPECT_LT(moved(1), moved(0) * 0.8f) << "midway, attached to both: about half of each";
    // Attached by hand: the guide on the swung driver to the still one (it stays), the far one to the
    // swung one (it swings, far as it is), the midway one to nothing (it stays).
    DriverAttachments some;
    some.byGuide[1] = {2};
    some.byGuide[4] = {1};
    some.hash = 2;
    const StrandSet picked = drive(curves, bound, now, 1.0f, Ramp::constant(1.0f), &some);
    auto pickedMoved = [&](std::size_t s) { return (picked.strandPoints(s)[10] - curves.strandPoints(s)[10]).length(); };
    EXPECT_LT(pickedMoved(0), 1e-3f) << "on the swung driver, attached to the still one";
    EXPECT_EQ(picked.strandPoints(1)[10], curves.strandPoints(1)[10]) << "attached to nothing: as groomed";
    EXPECT_GT(pickedMoved(3), 1.0f) << "far off, attached to the swung driver: swung round it";
}

TEST(DriveGuides, BindTextRoundTrips) {
    DriverBind bind;
    bind.frame = 12;
    Guide& g = bind.drivers.add(42, test::straightCurve({0.5f, 0, 0.25f}, {0, 1, 0}, 3));
    g.root = {3, 0.25f, 0.5f};
    bind.attached[9] = {42};
    const DriverBind back = DriverBind::fromJson(bind.toJson());
    EXPECT_EQ(back.attached, bind.attached);
    EXPECT_EQ(back.frame, 12);
    ASSERT_EQ(back.drivers.size(), 1u);
    EXPECT_EQ(back.drivers.guides()[0].id, 42u);
    EXPECT_EQ(back.drivers.guides()[0].points, g.points);
    EXPECT_EQ(back.drivers.guides()[0].root.face, 3);
    EXPECT_TRUE(DriverBind::fromJson("").empty());
    EXPECT_THROW(DriverBind::fromJson("[1]"), std::invalid_argument);
}

TEST(DriveGuides, DriversRidingTheScalpDriveNothingOnItsRestPose) {
    // The scalp turned 30 degrees (a head turning) and the drivers turned with it, as a rig moves
    // them: in the rest pose's space (following the scalp) they are as bound, so the guides there
    // stay as groomed; the scalp's own carry turns them.
    const auto plane = test::makePlane(4, 2.0f);
    std::vector<Vec3f> moved = plane->points();
    for (Vec3f& p : moved) p = turnZ(p, {1, 0, 1}, 30.0f);
    const ScalpMesh& rest = *plane;
    const GuideSet drivers = upright(7, {1.0f, 0.0f, 1.0f});
    const DriverBind bind = bindDrivers(drivers, plane.get(), 0);  // bound on the scalp at rest
    ASSERT_EQ(bind.drivers.size(), 1u);
    EXPECT_GE(bind.drivers.guides()[0].root.face, 0) << "anchored on the scalp";
    auto turnedScalp = std::make_shared<ScalpMesh>(moved, plane->triangles());
    turnedScalp->setRestPoints(plane->points());
    auto now = std::make_shared<GuideSet>();
    now->add(7, turned(drivers.guides()[0].points, {1, 0, 1}, 30.0f));
    const DriversInGraph inRest = driversInGraphSpace(bind, now, &rest, turnedScalp.get(), &rest);
    ASSERT_TRUE(inRest.bound && inRest.now);
    for (std::size_t i = 0; i < drivers.guides()[0].points.size(); ++i) {
        EXPECT_LT((inRest.now->guides()[0].points[i] - drivers.guides()[0].points[i]).length(), 1e-4f) << i;
    }
    // On the scalp as it is (not following): the bound drivers turn with it instead.
    const DriversInGraph onScalp = driversInGraphSpace(bind, now, &rest, turnedScalp.get(), turnedScalp.get());
    for (std::size_t i = 0; i < drivers.guides()[0].points.size(); ++i) {
        EXPECT_LT((onScalp.bound->guides()[0].points[i] - now->guides()[0].points[i]).length(), 1e-4f) << i;
    }
}

TEST(DriveGuides, TheNodePassesGuidesThroughUnboundAndDrivesThemBound) {
    auto node = OperatorRegistry::instance().create(op::kDriveGuides);
    auto curves = std::make_shared<const StrandSet>(besides({0.05f}));
    ExternalInputs externals;
    EvalContext ctx;
    ctx.externals = &externals;
    const StrandsPtr passed = valueAs<StrandsPtr>(node->evaluate(ctx, {Value(StrandsPtr(curves))}));
    EXPECT_EQ(passed, curves) << "no bind: the input itself";
    EXPECT_FALSE(node->note(ctx).empty());

    auto bound = std::make_shared<const GuideSet>(upright());
    auto now = std::make_shared<GuideSet>();
    now->add(7, turned(bound->guides()[0].points, {0, 0, 0}, 40.0f));
    externals.set(kDriverBindExternal, GuidesPtr(bound), bound->contentHash());
    externals.set(kDriversExternal, GuidesPtr(now), now->contentHash());
    EXPECT_EQ(valueAs<StrandsPtr>(node->evaluate(ctx, {Value(StrandsPtr(curves))})), curves) << "nothing attached: the input";
    auto attached = std::make_shared<DriverAttachments>(allAttached(*curves, *bound));
    externals.set(kDriverAttachExternal, DriverAttachPtr(attached), attached->hash);
    const StrandsPtr out = valueAs<StrandsPtr>(node->evaluate(ctx, {Value(StrandsPtr(curves))}));
    const StrandSet want = drive(*curves, *bound, *now);
    for (std::size_t i = 0; i < want.strandPointCount(0); ++i) EXPECT_EQ(out->strandPoints(0)[i], want.strandPoints(0)[i]) << i;
    EXPECT_TRUE(node->note(ctx).empty());
    node->params().set("strength", 0.0);
    EXPECT_EQ(valueAs<StrandsPtr>(node->evaluate(ctx, {Value(StrandsPtr(curves))})), curves) << "strength 0: the input";
}

// ---- In a session ------------------------------------------------------------------------------

const char* kDriveGraph = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "drive", "type": "DriveGuides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 120.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "drive", "input": "strands"},
    {"from": "drive", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";

// A sphere scalp (its rest pose itself), ten flat guides round its top and one driver up its middle.
GroomData driveData() {
    GroomData d;
    d.graphJson = kDriveGraph;
    auto scalp = std::make_shared<ScalpMesh>(*test::makeSphere(12, 24));
    scalp->setRestPoints(scalp->points());
    d.scalp = scalp;
    auto guides = std::make_shared<GuideSet>();
    for (int i = 0; i < 10; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 10.0f;
        const Vec3f n = Vec3f{std::cos(a) * 0.3f, 0.95f, std::sin(a) * 0.3f}.normalized();
        guides->add(static_cast<std::uint64_t>(i + 1), test::straightCurve(n, n * 0.8f, 8));
    }
    guides->rootOnMesh(*scalp);
    d.guides = guides;
    auto drivers = std::make_shared<GuideSet>();
    drivers->add(3, test::straightCurve({0.0f, 1.0f, 0.0f}, {0.0f, 0.8f, 0.0f}, 8));
    d.drivers = drivers;
    return d;
}

StrandsPtr viewOf(GroomSession& session) {
    ViewRequest request;
    const ViewResult& view = session.evaluate(request);
    EXPECT_TRUE(view.errors.empty()) << (view.errors.empty() ? "" : view.errors.front());
    return view.strands;
}

float farthest(const StrandSet& a, const StrandSet& b) {
    EXPECT_EQ(a.ids(), b.ids());
    float most = 0.0f;
    for (std::size_t i = 0; i < a.positions().size(); ++i) most = std::max(most, (a.positions()[i] - b.positions()[i]).length());
    return most;
}

// The session's drivers bound now, with guides `ids` attached to driver 3 (every guide: 1-10).
void bindAttached(GroomSession& session, const std::vector<std::uint64_t>& ids = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}) {
    DriverBind bind = DriverBind::fromJson(session.bindDrivers(0));
    for (const std::uint64_t id : ids) bind.attached[id] = {3};
    session.setDriverBindJson(bind.toJson());
}

TEST(DriveGuides, ASessionBindsItsDriversAndTheStrandsFollowThem) {
    GroomSession session(driveData());
    const StrandsPtr unbound = viewOf(session);
    ASSERT_FALSE(session.bindDrivers(0).empty());
    bindAttached(session);
    EXPECT_FALSE(DriverBind::fromJson(session.bindDrivers(0)).attached.empty()) << "binding again keeps the attachments";
    EXPECT_LT(farthest(*viewOf(session), *unbound), 1e-4f) << "the drivers as bound: the groom as it was";
    auto swung = std::make_shared<GuideSet>();
    swung->add(3, turned(session.drivers()->guides()[0].points, {0.0f, 1.0f, 0.0f}, 40.0f));
    session.setDrivers(swung);
    EXPECT_GT(farthest(*viewOf(session), *unbound), 0.2f) << "swung over: the strands with it";
    {
        ViewRequest request;
        const ViewResult& view = session.evaluate(request);
        ASSERT_TRUE(view.movingGuides) << "the driven guides drawn over the groomed ones";
        EXPECT_EQ(view.movingGuides->size(), 10u);
    }
    session.setDriverBindJson("");
    EXPECT_EQ(farthest(*viewOf(session), *unbound), 0.0f) << "no bind: the groom as it was";
}

TEST(DriveGuides, DriversMovingWithTheScalpDriveNothingWhileFollowing) {
    // The head turned and the driver with it (a rig moving it as the head turns): the guides
    // stay as groomed on the rest pose and the strands are carried as without drivers.
    GroomData data = driveData();
    data.followScalp = true;
    GroomData plain = data;
    plain.drivers = nullptr;
    GroomSession session(data), still(plain);
    viewOf(session);
    bindAttached(session);
    std::vector<Vec3f> moved = data.scalp->points();
    for (Vec3f& p : moved) p = turnZ(p, {0.0f, 0.0f, 0.0f}, 25.0f);
    session.updateScalpPoints(moved);
    still.updateScalpPoints(moved);
    auto turnedDriver = std::make_shared<GuideSet>();
    turnedDriver->add(3, turned(data.drivers->guides()[0].points, {0.0f, 0.0f, 0.0f}, 25.0f));
    session.setDrivers(turnedDriver);
    EXPECT_LT(farthest(*viewOf(session), *viewOf(still)), 1e-3f);
    // Swung over on top of the head's turn: the strands with it.
    auto swung = std::make_shared<GuideSet>();
    swung->add(3, turned(turnedDriver->guides()[0].points, turnedDriver->guides()[0].points[0], 40.0f));
    session.setDrivers(swung);
    EXPECT_GT(farthest(*viewOf(session), *viewOf(still)), 0.2f);
}

TEST(DriveGuides, GroomFilesKeepTheDriversAndTheirBind) {
    GroomFile file;
    file.data = driveData();
    DriverBind bind;
    bind.frame = 4;
    bind.drivers.add(3, test::straightCurve({0.0f, 1.0f, 0.0f}, {0.0f, 0.8f, 0.0f}, 8)).root = {5, 0.2f, 0.3f};
    file.data.driverBindJson = bind.toJson();
    file.driverCache = {"shot.drivers.abc", "hairDrivers"};
    const GroomFile back = groomFileFromJson(groomFileToJson(file));
    EXPECT_EQ(back.driverCache.file, "shot.drivers.abc");  // the rig's animation, for the farm
    EXPECT_EQ(back.driverCache.object, "hairDrivers");
    ASSERT_TRUE(back.data.drivers);
    EXPECT_EQ(back.data.drivers->guides()[0].id, 3u);
    EXPECT_EQ(back.data.drivers->guides()[0].points, file.data.drivers->guides()[0].points);
    const DriverBind read = DriverBind::fromJson(back.data.driverBindJson);
    EXPECT_EQ(read.frame, 4);
    ASSERT_EQ(read.drivers.size(), 1u);
    EXPECT_EQ(read.drivers.guides()[0].root.face, 5);
}

}  // namespace
}  // namespace groomlab
