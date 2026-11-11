// SPDX-License-Identifier: Apache-2.0
#include <cmath>
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <functional>

#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include "../core/test_helpers.h"
#include "groomlab/alembic_io.h"
#include "groomlab/engine_export.h"
#include "groomlab/export_view.h"
#include "groomlab/graph.h"
#include "groomlab/hair_color.h"
#include "groomlab/groom_scene.h"
#include "groomlab/operators.h"

namespace groomlab {
namespace {

std::string tempPath(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

bool leftTemp(const std::string& path) { return std::filesystem::exists(tempPathFor(std::filesystem::u8path(path))); }

StrandSet makeStrands(std::size_t count, float xOffset) {
    StrandSet s;
    for (std::size_t i = 0; i < count; ++i) {
        const float x = static_cast<float>(i) + xOffset;
        s.addStrand(1000 + i, {{x, 0, 0}, {x, 1, 0}, {x, 2, 0}}, {0.02f, 0.01f, 0.0f});
    }
    return s;
}

TEST(Alembic, ALighterFeatherExportWidensTheBarbsKept) {
    // engine_export.h setBarbLod, every host's (Maya's game.py, the Blender helper, the app): the
    // lod's share of barbs, each kept one 1/sqrt(share) wider unless a width is given.
    EngineExportOptions e;
    setBarbLod(e, "mid");
    EXPECT_FLOAT_EQ(e.barbDensity, 0.5f);
    EXPECT_NEAR(e.barbWidthScale, 1.0 / std::sqrt(0.5), 1e-6);
    setBarbLod(e, "low", 0.1);
    EXPECT_FLOAT_EQ(e.barbDensity, 0.1f) << "a density given wins over the lod's";
    setBarbLod(e, "", 0.25, 1.5);
    EXPECT_FLOAT_EQ(e.barbWidthScale, 1.5f);
    setBarbLod(e, "");
    EXPECT_FLOAT_EQ(e.barbDensity, 1.0f);
    EXPECT_FLOAT_EQ(e.barbWidthScale, 1.0f);
    EXPECT_THROW(setBarbLod(e, "ultra"), std::invalid_argument);
    EXPECT_THROW(setBarbLod(e, "", 0.0), std::invalid_argument);
    EXPECT_THROW(setBarbLod(e, "", 0.5, 0.0), std::invalid_argument);
}

TEST(Alembic, RoundTripsAnimatedCurves) {
    const std::string path = tempPath("groomlab_roundtrip.abc");
    {
        AlembicCurvesWriter writer(path, "groom", 1.0 / 24.0, 1.0 / 24.0);
        writer.writeFrame(makeStrands(3, 0.0f));
        writer.writeFrame(makeStrands(5, 0.5f));  // topology changes between frames
        EXPECT_EQ(writer.frameCount(), 2u);
    }

    const auto frames = readAlembicCurves(path, "groom");
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_NEAR(frames[0].seconds, 1.0 / 24.0, 1e-9);
    EXPECT_NEAR(frames[1].seconds, 2.0 / 24.0, 1e-9);

    const StrandSet& a = frames[0].strands;
    ASSERT_EQ(a.strandCount(), 3u);
    EXPECT_EQ(a.pointCount(), 9u);
    EXPECT_EQ(a.ids()[2], 1002u);
    EXPECT_EQ(a.strandPoints(1)[2], Vec3f(1, 2, 0));
    EXPECT_FLOAT_EQ(a.widths()[0], 0.02f);
    EXPECT_FLOAT_EQ(a.widths()[2], 0.0f);

    const StrandSet& b = frames[1].strands;
    ASSERT_EQ(b.strandCount(), 5u);
    EXPECT_FLOAT_EQ(b.strandPoints(4)[0].x, 4.5f);
    EXPECT_TRUE(b.validate());
    std::remove(path.c_str());
}

TEST(Alembic, CurvesBlendBetweenSamples) {
    const std::string path = tempPath("groomlab_blend.abc");
    {
        AlembicCurvesWriter writer(path, "groom", 1.0 / 24.0, 1.0 / 24.0);
        writer.writeFrame(makeStrands(3, 0.0f));
        writer.writeFrame(makeStrands(3, 1.0f));  // same curves, moved +1 in x
        writer.writeFrame(makeStrands(4, 0.0f));  // different curves: no blending into it
    }
    const StrandSet mid = readAlembicCurvesAt(path, 1.25 / 24.0);
    ASSERT_EQ(mid.strandCount(), 3u);
    EXPECT_NEAR(mid.strandPoints(0)[1].x, 0.25f, 1e-5f);
    EXPECT_EQ(mid.ids()[2], 1002u);
    EXPECT_NEAR(readAlembicCurvesAt(path, 2.0 / 24.0).strandPoints(0)[0].x, 1.0f, 1e-6f);  // on a sample
    const StrandSet across = readAlembicCurvesAt(path, 2.4 / 24.0);  // nearest sample, not blended
    ASSERT_EQ(across.strandCount(), 3u);
    EXPECT_NEAR(across.strandPoints(0)[0].x, 1.0f, 1e-6f);
    std::remove(path.c_str());
}

TEST(Alembic, EmptyFrameIsValid) {
    const std::string path = tempPath("groomlab_empty.abc");
    {
        AlembicCurvesWriter writer(path, "groom", 0.0, 1.0 / 24.0);
        writer.writeFrame(StrandSet{});
    }
    const auto frames = readAlembicCurves(path);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].strands.strandCount(), 0u);
    std::remove(path.c_str());
}

TEST(Alembic, ErrorsAreAlembicErrors) {
    EXPECT_THROW(readAlembicCurves(tempPath("groomlab_does_not_exist.abc")), AlembicError);
    EXPECT_THROW(AlembicCurvesWriter("Z:/no/such/dir/x.abc", "g", 0.0, 1.0), AlembicError);

    const std::string path = tempPath("groomlab_named.abc");
    {
        AlembicCurvesWriter writer(path, "groom", 0.0, 1.0);
        writer.writeFrame(makeStrands(1, 0.0f));
    }
    EXPECT_THROW(readAlembicCurves(path, "other"), AlembicError);
    std::remove(path.c_str());
}

TEST(Alembic, UnrealGroomHasTheGroomAttributes) {
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    const std::string path = tempPath("groomlab_unreal.abc");
    {
        UnrealGroomWriter writer(path, "hair", "hair", 1.0 / 24.0, 1.0 / 24.0);
        StrandSet guides = makeStrands(2, 0.25f);
        writer.writeFrame(makeStrands(4, 0.0f), &guides, nullptr);
        EXPECT_EQ(writer.frameCount(), 1u);
    }
    Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
    Abc::IObject hair(archive.getTop(), "hair");
    ASSERT_TRUE(hair.valid());
    AbcG::ICurves strands(hair, "strands");
    AbcG::ICurves guides(hair, "guides");
    ASSERT_TRUE(strands.valid());
    ASSERT_TRUE(guides.valid());
    EXPECT_EQ(strands.getSchema().getValue().getNumCurves(), 4u);
    EXPECT_EQ(guides.getSchema().getValue().getNumCurves(), 2u);

    const Abc::ICompoundProperty arb = strands.getSchema().getArbGeomParams();
    for (const char* name : {"groom_version_major", "groom_version_minor", "groom_group_id", "groom_id", "groom_width",
                             "groom_group_name"}) {
        EXPECT_TRUE(arb.getPropertyHeader(name) != nullptr) << name;
    }
    AbcG::IInt32GeomParam major(arb, "groom_version_major");
    EXPECT_EQ(major.getExpandedValue().getVals()->get()[0], 1);
    AbcG::IInt32GeomParam ids(arb, "groom_id");
    EXPECT_EQ(ids.getExpandedValue().getVals()->get()[1], 1);  // dense, in export order
    const Abc::ICompoundProperty guideArb = guides.getSchema().getArbGeomParams();
    AbcG::IInt32GeomParam guide(guideArb, "groom_guide");
    ASSERT_TRUE(guide.valid());
    EXPECT_EQ(guide.getExpandedValue().getVals()->get()[0], 1);
    EXPECT_TRUE(arb.getPropertyHeader("groom_guide") == nullptr);  // strands are not guides
    EXPECT_FALSE(leftTemp(path));
    std::remove(path.c_str());
}

TEST(Alembic, UnrealPointsAreInUnrealsSpace) {
    // Unreal's groom importer takes the points as its own (Z up, left-handed, cm) with its
    // default conversion (Rotation 0, Scale 1); its FBX importer turns a Y-up scene's (x, y, z)
    // into (x, z, y) and a Z-up one's into (x, -y, z). The groom goes the same way, so it lands
    // on the head imported from the same scene.
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    StrandSet hair;
    hair.addStrand(7, {{1, 2, 3}, {4, 5, 6}}, {0.02f, 0.01f});
    StrandSet guides;
    guides.addStrand(9, {{-1, 0.5f, 2}, {-1, 1.5f, 2}}, {0.1f, 0.1f});
    struct Case {
        const char* upAxis;
        double cmPerUnit;
        Vec3f first, second, guide;
        float width;
    };
    for (const Case& c : {Case{"Y", 1.0, {1, 3, 2}, {4, 6, 5}, {-1, 2, 0.5f}, 0.02f},
                          Case{"Z", 1.0, {1, -2, 3}, {4, -5, 6}, {-1, -0.5f, 2}, 0.02f},
                          Case{"Z", 100.0, {100, -200, 300}, {400, -500, 600}, {-100, -50, 200}, 2.0f}}) {
        SCOPED_TRACE(std::string(c.upAxis) + " " + std::to_string(c.cmPerUnit));
        const std::string path = tempPath("groomlab_unreal_space.abc");
        {
            UnrealGroomWriter writer(path, "hair", "hair", 0.0, 1.0 / 24.0);
            if (std::string(c.upAxis) != "Y" || c.cmPerUnit != 1.0) writer.setSourceSpace(c.upAxis, c.cmPerUnit);
            writer.writeFrame(hair, &guides, nullptr);
            EXPECT_THROW(writer.setSourceSpace("Y", 1.0), AlembicError);  // after the first frame
        }
        const auto frames = readAlembicCurves(path);  // the strands: the first curves object
        ASSERT_EQ(frames.size(), 1u);
        const StrandSet& s = frames[0].strands;
        ASSERT_EQ(s.pointCount(), 2u);
        for (const auto& [got, want] : {std::pair{s.positions()[0], c.first}, std::pair{s.positions()[1], c.second}}) {
            EXPECT_FLOAT_EQ(got.x, want.x);
            EXPECT_FLOAT_EQ(got.y, want.y);
            EXPECT_FLOAT_EQ(got.z, want.z);
        }
        EXPECT_FLOAT_EQ(s.widths()[0], c.width);  // widths in centimeters as the points
        Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
        Abc::IObject top(archive.getTop(), "hair");
        AbcG::ICurves guideCurves(top, "guides");
        ASSERT_TRUE(guideCurves.valid());
        const Imath::V3f g = guideCurves.getSchema().getValue().getPositions()->get()[0];
        EXPECT_FLOAT_EQ(g.x, c.guide.x);
        EXPECT_FLOAT_EQ(g.y, c.guide.y);
        EXPECT_FLOAT_EQ(g.z, c.guide.z);
        AbcG::IXform xform(archive.getTop(), "hair");  // the identity: the points carry the turn
        AbcG::XformSample xs;
        xform.getSchema().get(xs);
        EXPECT_TRUE(xs.getMatrix() == Imath::M44d());
        std::remove(path.c_str());
    }
    UnrealGroomWriter writer(tempPath("groomlab_unreal_space_bad.abc"), "hair", "hair", 0.0, 1.0 / 24.0);
    EXPECT_THROW(writer.setSourceSpace("X", 1.0), std::invalid_argument);
    EXPECT_THROW(writer.setSourceSpace("Y", 0.0), std::invalid_argument);
    writer.close();
    std::remove(tempPath("groomlab_unreal_space_bad.abc").c_str());

    // Every host's engine export passes its space through the options.
    EngineExportOptions o;
    EXPECT_EQ(o.upAxis, "Y");
    EXPECT_EQ(o.centimetersPerUnit, 1.0);
    o.upAxis = "X";
    EXPECT_FALSE(o.problem().empty());
    o.upAxis = "Z";
    o.centimetersPerUnit = 0.0;
    EXPECT_FALSE(o.problem().empty());
    o.centimetersPerUnit = 100.0;  // a Z-up meter scene
    EXPECT_TRUE(o.problem().empty());
    const std::string path = tempPath("groomlab_engine_unreal_space.abc");
    {
        EngineGroomWriter w(path, "hair", 0.0, 1.0 / 24.0, o);
        w.writeFrame(hair, nullptr, nullptr);
    }
    const auto frames = readAlembicCurves(path);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].strands.positions()[0], Vec3f(100, -200, 300));
    EXPECT_FLOAT_EQ(frames[0].strands.widths()[0], 2.0f);
    std::remove(path.c_str());
}

TEST(Alembic, UnrealIdsAreDenseAndChannelsTravel) {
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    const std::string path = tempPath("groomlab_unreal_channels.abc");
    StrandSet s = makeStrands(3, 0.0f);
    s.mutableIds()[1] = 0xFFFFFFFF12345678ull;  // a 64-bit hash: no int32 could hold it
    s.addStrandChannel(kGroupIdChannel, 0.0f)[2] = 2.0f;
    s.addStrandChannel(kClumpIdChannel, -1.0f)[0] = 7.0f;
    s.addStrandChannel(kMelaninChannel, 0.5f);
    {
        UnrealGroomWriter writer(path, "hair", "hair", 0.0, 1.0 / 24.0);
        writer.writeFrame(s, nullptr, nullptr);
    }
    EXPECT_FALSE(leftTemp(path));
    Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
    AbcG::ICurves strands(Abc::IObject(archive.getTop(), "hair"), "strands");
    ASSERT_TRUE(strands.valid());
    const Abc::ICompoundProperty arb = strands.getSchema().getArbGeomParams();
    const auto ids = AbcG::IInt32GeomParam(arb, "groom_id").getExpandedValue().getVals();
    EXPECT_EQ(ids->get()[0], 0);
    EXPECT_EQ(ids->get()[1], 1);
    EXPECT_EQ(ids->get()[2], 2);
    const auto low = AbcG::IUInt32GeomParam(arb, "groomlab_strand_id_low").getExpandedValue().getVals();
    const auto high = AbcG::IUInt32GeomParam(arb, "groomlab_strand_id_high").getExpandedValue().getVals();
    EXPECT_EQ(low->get()[1], 0x12345678u);
    EXPECT_EQ(high->get()[1], 0xFFFFFFFFu);
    EXPECT_EQ(low->get()[0], 1000u);
    const auto groups = AbcG::IInt32GeomParam(arb, "groom_group_id").getExpandedValue().getVals();
    EXPECT_EQ(groups->get()[0], 0);
    EXPECT_EQ(groups->get()[2], 2);
    ASSERT_TRUE(arb.getPropertyHeader("groom_clump_id") != nullptr);
    const auto clumps = AbcG::IInt32GeomParam(arb, "groom_clump_id").getExpandedValue().getVals();
    EXPECT_EQ(clumps->get()[0], 7);
    EXPECT_EQ(clumps->get()[1], -1);
    ASSERT_TRUE(arb.getPropertyHeader("groom_color") != nullptr);
    const auto colors = AbcG::IV3fGeomParam(arb, "groom_color").getExpandedValue().getVals();
    ASSERT_EQ(colors->size(), 9u);  // per vertex: three strands of three points
    EXPECT_GT((*colors)[0].x, 0.0f);
    EXPECT_LT((*colors)[0].x, 1.0f);  // half melanin: brown
    std::remove(path.c_str());
}

TEST(Alembic, UnrealWritesExplicitColorAndRoughness) {
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    const std::string path = tempPath("groomlab_unreal_color.abc");
    StrandSet s = makeStrands(3, 0.0f);
    s.addStrandChannel(kMelaninChannel, 1.0f);  // black, but the explicit color wins
    s.addStrandChannel(kColorChannels[0], 0.1f)[2] = 0.9f;
    s.addStrandChannel(kColorChannels[1], 0.2f);
    s.addStrandChannel(kColorChannels[2], 0.3f);
    s.addStrandChannel(kRoughnessChannel, 0.25f)[1] = 2.0f;  // clamped to 1
    {
        UnrealGroomWriter writer(path, "hair", "hair", 0.0, 1.0 / 24.0);
        writer.writeFrame(s, nullptr, nullptr);
    }
    EXPECT_FALSE(leftTemp(path));
    Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
    AbcG::ICurves strands(Abc::IObject(archive.getTop(), "hair"), "strands");
    ASSERT_TRUE(strands.valid());
    const Abc::ICompoundProperty arb = strands.getSchema().getArbGeomParams();
    ASSERT_TRUE(arb.getPropertyHeader("groom_color") != nullptr);
    AbcG::IV3fGeomParam color(arb, "groom_color");
    EXPECT_EQ(color.getScope(), AbcG::kVertexScope);  // Epic's schema: per vertex
    const auto colors = color.getExpandedValue().getVals();
    ASSERT_EQ(colors->size(), s.pointCount());
    // groom_color: the hair's appearance (hair_color.h hairAppearanceColor; Unreal draws it as
    // the hair's color), the strand color repeated along each strand.
    const std::array<float, 3> shown = hairAppearanceColor({0.1f, 0.2f, 0.3f});
    EXPECT_FLOAT_EQ((*colors)[0].x, shown[0]);
    EXPECT_FLOAT_EQ((*colors)[0].y, shown[1]);
    EXPECT_FLOAT_EQ((*colors)[0].z, shown[2]);
    EXPECT_GT(shown[0], 0.1f);
    EXPECT_FLOAT_EQ((*colors)[s.strandBegin(0) + s.strandPointCount(0) - 1].x, shown[0]);
    const float light = hairAppearanceColor({0.9f, 0.2f, 0.3f})[0];
    EXPECT_FLOAT_EQ((*colors)[s.strandBegin(2)].x, light);
    EXPECT_FLOAT_EQ((*colors)[s.strandBegin(2) + 1].x, light);
    ASSERT_TRUE(arb.getPropertyHeader("groom_roughness") != nullptr);
    AbcG::IFloatGeomParam roughness(arb, "groom_roughness");
    EXPECT_EQ(roughness.getScope(), AbcG::kUniformScope);
    const auto values = roughness.getExpandedValue().getVals();
    ASSERT_EQ(values->size(), 3u);
    EXPECT_FLOAT_EQ(values->get()[0], 0.25f);
    EXPECT_FLOAT_EQ(values->get()[1], 1.0f);
    // The channels travel as floats too, beside the color.
    EXPECT_TRUE(arb.getPropertyHeader("groom_clump_id") == nullptr);
    std::remove(path.c_str());
}

TEST(Alembic, UnrealCarriesBandsPerPointAndTheIridescence) {
    // HairColor's bands: a color per point, which groom_color carries point for point and reads
    // back as written; its iridescence channel as groom_iridescence (and groom_ao when asked).
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    StrandSet s = makeStrands(3, 0.0f);
    std::array<std::vector<float>*, 3> point{};
    for (int k = 0; k < 3; ++k) point[k] = &s.addPointChannel(kColorChannels[k], 0.0f);
    for (int k = 0; k < 3; ++k) s.addStrandChannel(kColorChannels[k], 0.5f);
    std::vector<float>& iridescence = s.addPointChannel(kIridescenceChannel, 0.0f);
    for (std::size_t i = 0; i < s.pointCount(); ++i) {
        (*point[0])[i] = 0.1f * static_cast<float>(i);  // a band of its own at every point
        (*point[1])[i] = i % 3 == 1 ? 0.9f : 0.1f;
        (*point[2])[i] = 0.25f;
        iridescence[i] = i % 2 == 0 ? 0.75f : 0.0f;
    }
    for (const bool ao : {false, true}) {
        const std::string path = tempPath(ao ? "groomlab_unreal_bands_ao.abc" : "groomlab_unreal_bands.abc");
        {
            EngineExportOptions options;
            options.iridescenceAo = ao;
            EngineGroomWriter writer(path, "hair", 0.0, 1.0 / 24.0, options);
            writer.writeFrame(s, nullptr, nullptr);
        }
        Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
        AbcG::ICurves strands(Abc::IObject(archive.getTop(), "hair"), "strands");
        ASSERT_TRUE(strands.valid());
        const Abc::ICompoundProperty arb = strands.getSchema().getArbGeomParams();
        AbcG::IV3fGeomParam color(arb, "groom_color");
        const auto colors = color.getExpandedValue().getVals();
        ASSERT_EQ(colors->size(), s.pointCount());
        for (std::size_t i = 0; i < s.pointCount(); ++i) {  // the appearance of each point's color
            const std::array<float, 3> shown = hairAppearanceColor({(*point[0])[i], (*point[1])[i], 0.25f});
            EXPECT_FLOAT_EQ((*colors)[i].x, shown[0]) << i;
            EXPECT_FLOAT_EQ((*colors)[i].y, shown[1]) << i;
            EXPECT_FLOAT_EQ((*colors)[i].z, shown[2]) << i;
        }
        ASSERT_TRUE(arb.getPropertyHeader("groom_iridescence") != nullptr);
        AbcG::IFloatGeomParam mask(arb, "groom_iridescence");
        EXPECT_EQ(mask.getScope(), AbcG::kVertexScope);
        const auto values = mask.getExpandedValue().getVals();
        ASSERT_EQ(values->size(), s.pointCount());
        for (std::size_t i = 0; i < s.pointCount(); ++i) EXPECT_FLOAT_EQ(values->get()[i], iridescence[i]) << i;
        EXPECT_EQ(arb.getPropertyHeader("groom_ao") != nullptr, ao);
        if (ao) {
            const auto aoValues = AbcG::IFloatGeomParam(arb, "groom_ao").getExpandedValue().getVals();
            ASSERT_EQ(aoValues->size(), s.pointCount());
            EXPECT_FLOAT_EQ(aoValues->get()[0], 0.75f);
        }
        std::remove(path.c_str());
    }
    // Without the channel, neither attribute.
    const std::string plain = tempPath("groomlab_unreal_no_iridescence.abc");
    {
        UnrealGroomWriter writer(plain, "hair", "hair", 0.0, 1.0 / 24.0);
        writer.setIridescenceAsAo(true);
        writer.writeFrame(makeStrands(2, 0.0f), nullptr, nullptr);
    }
    Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), plain);
    AbcG::ICurves strands(Abc::IObject(archive.getTop(), "hair"), "strands");
    const Abc::ICompoundProperty arb = strands.getSchema().getArbGeomParams();
    EXPECT_TRUE(arb.getPropertyHeader("groom_iridescence") == nullptr);
    EXPECT_TRUE(arb.getPropertyHeader("groom_ao") == nullptr);
    std::remove(plain.c_str());
}

TEST(Alembic, UnrealCarriesTheColorNodesChainAndHairColorsRoughness) {
    // HairColor (a tip, a highlight) -> Bands -> FeatherColor (iridescence): the chain's last
    // colors per point as groom_color, its iridescence as groom_iridescence, HairColor's roughness
    // per strand as groom_roughness (its specular rides along as a channel of its own).
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    auto in = std::make_shared<StrandSet>();
    for (int i = 0; i < 12; ++i) {
        const float x = 0.1f * static_cast<float>(i);
        in->addStrand(200 + i, test::straightCurve({x, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 10));
    }
    auto apply = [](const char* type, const StrandsPtr& strands, const std::function<void(ParamSet&)>& set) {
        auto node = OperatorRegistry::instance().create(type);
        set(node->params());
        return valueAs<StrandsPtr>(node->evaluate(EvalContext{}, {Value(strands)}));
    };
    StrandsPtr s = apply(op::kHairColor, in, [](ParamSet& p) {
        p.set("tipStrength", 0.8);
        p.set("specular", 0.45);
        p.set("roughness", 0.35);
    });
    s = apply(op::kBands, s, [](ParamSet& p) { p.set("guards", 0.6); });
    s = apply(op::kFeatherColor, s, [](ParamSet& p) {
        p.set("pattern", std::string("none"));
        p.set("iridescence", 0.6);
    });
    ASSERT_TRUE(s->pointChannel(kColorChannels[0]));
    const std::string path = tempPath("groomlab_unreal_color_chain.abc");
    {
        EngineGroomWriter writer(path, "hair", 0.0, 1.0 / 24.0, EngineExportOptions{});
        writer.writeFrame(*s, nullptr, nullptr);
    }
    Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
    AbcG::ICurves strands(Abc::IObject(archive.getTop(), "hair"), "strands");
    ASSERT_TRUE(strands.valid());
    const Abc::ICompoundProperty arb = strands.getSchema().getArbGeomParams();
    AbcG::IV3fGeomParam color(arb, "groom_color");
    const auto colors = color.getExpandedValue().getVals();
    ASSERT_EQ(colors->size(), s->pointCount());
    const std::vector<std::array<float, 3>> chain = pointAppearanceColors(*s);  // as the hair looks
    ASSERT_EQ(chain.size(), s->pointCount());
    for (std::size_t i = 0; i < s->pointCount(); ++i) {
        EXPECT_FLOAT_EQ((*colors)[i].x, chain[i][0]) << i;
        EXPECT_FLOAT_EQ((*colors)[i].y, chain[i][1]) << i;
        EXPECT_FLOAT_EQ((*colors)[i].z, chain[i][2]) << i;
    }
    ASSERT_TRUE(arb.getPropertyHeader("groom_roughness") != nullptr);
    AbcG::IFloatGeomParam roughness(arb, "groom_roughness");
    const auto values = roughness.getExpandedValue().getVals();
    ASSERT_EQ(values->size(), s->strandCount());
    for (std::size_t c = 0; c < values->size(); ++c) EXPECT_FLOAT_EQ(values->get()[c], 0.35f);
    ASSERT_TRUE(arb.getPropertyHeader("groom_iridescence") != nullptr);
    AbcG::IFloatGeomParam iridescence(arb, "groom_iridescence");
    const auto sheen = iridescence.getExpandedValue().getVals();
    ASSERT_EQ(sheen->size(), s->pointCount());
    EXPECT_FLOAT_EQ(sheen->get()[0], 0.6f);
    ASSERT_TRUE(s->strandChannel(kSpecularChannel));
    EXPECT_FLOAT_EQ((*s->strandChannel(kSpecularChannel))[0], 0.45f);
    archive.reset();
    std::remove(path.c_str());
}

// A groom of hair and two feather designs, as Propagate and Merge leave it: barbs carry a
// featherId, the shafts isShaft 1, each design its groupId; HairColor's color on every strand.
StrandSet hairAndFeathers() {
    StrandSet s = makeStrands(3, 0.0f);  // hair: no group
    s.addStrandChannel("featherId", -1.0f);
    s.addStrandChannel("isShaft", 0.0f);
    s.addStrandChannel(kGroupIdChannel, 0.0f);
    s.addStrandChannel(kColorChannels[0], 0.2f);
    s.addStrandChannel(kColorChannels[1], 0.3f);
    s.addStrandChannel(kColorChannels[2], 0.4f);
    s.addStrandChannel(kClumpIdChannel, -1.0f);
    auto feather = [&s](StrandId id, int group, int featherId, bool shaft, int vane) {
        const std::size_t i = s.addStrand(id, {{0, 0, 5}, {0, 1, 5}}, {0.01f, 0.01f});
        (*s.mutableStrandChannel("featherId"))[i] = static_cast<float>(featherId);
        (*s.mutableStrandChannel("isShaft"))[i] = shaft ? 1.0f : 0.0f;
        (*s.mutableStrandChannel(kGroupIdChannel))[i] = static_cast<float>(group);
        (*s.mutableStrandChannel(kClumpIdChannel))[i] = static_cast<float>(vane);
    };
    feather(50, 1, 0, true, -1);  // a contour feather: its shaft and three barbs
    feather(51, 1, 0, false, 0);
    feather(52, 1, 0, false, 0);
    feather(53, 1, 0, false, 1);
    feather(60, 2, 1, true, -1);  // a down feather: its shaft and two barbs
    feather(61, 2, 1, false, 2);
    feather(62, 2, 1, false, 3);
    return s;
}

TEST(Alembic, UnrealGroupsAreObjectsOfTheirOwn) {
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    const std::string path = tempPath("groomlab_unreal_groups.abc");
    const StrandSet groom = hairAndFeathers();
    std::vector<GroomGroup> groups =
        splitGroomGroups(groom, kGroupIdChannel, "bird", {{1, "contour"}, {2, "down feathers"}});
    ASSERT_EQ(groups.size(), 3u);
    const StrandSet guides = makeStrands(2, 0.25f);
    std::vector<UnrealGroomWriter::Group> written;
    for (GroomGroup& g : groups) {
        g.strands = withoutShafts(g.strands);  // the shafts are the rachis mesh
        written.push_back({g.id, g.name, &g.strands, g.feathers ? nullptr : &guides});
    }
    {
        UnrealGroomWriter writer(path, "bird", "bird", 0.0, 1.0 / 24.0);
        writer.writeGroups(written, nullptr);
    }
    EXPECT_FALSE(leftTemp(path));
    Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
    Abc::IObject bird(archive.getTop(), "bird");
    ASSERT_TRUE(bird.valid());
    struct Expect {
        const char* object;
        int id;
        const char* name;
        std::size_t curves;
        int firstGroomId;
    };
    for (const Expect& e : {Expect{"strands_bird", 0, "bird", 3, 0}, Expect{"strands_contour", 1, "contour", 3, 3},
                            Expect{"strands_down_feathers", 2, "down feathers", 2, 6}}) {
        SCOPED_TRACE(e.object);
        AbcG::ICurves curves(bird, e.object);
        ASSERT_TRUE(curves.valid());
        EXPECT_EQ(curves.getSchema().getValue().getNumCurves(), e.curves);  // no shafts
        const Abc::ICompoundProperty arb = curves.getSchema().getArbGeomParams();
        const auto ids = AbcG::IInt32GeomParam(arb, "groom_group_id").getExpandedValue().getVals();
        ASSERT_EQ(ids->size(), e.curves);
        for (std::size_t i = 0; i < ids->size(); ++i) EXPECT_EQ(ids->get()[i], e.id);
        const auto names = AbcG::IStringGeomParam(arb, "groom_group_name").getExpandedValue().getVals();
        EXPECT_EQ(names->get()[0], std::string(e.name));
        // groom_id: unique over the whole groom, group after group.
        const auto groomIds = AbcG::IInt32GeomParam(arb, "groom_id").getExpandedValue().getVals();
        EXPECT_EQ(groomIds->get()[0], e.firstGroomId);
        EXPECT_TRUE(arb.getPropertyHeader("groom_guide") == nullptr);
        // HairColor's color on barbs as on hair; the vanes as clumps.
        ASSERT_TRUE(arb.getPropertyHeader("groom_color") != nullptr);
        EXPECT_FLOAT_EQ(AbcG::IV3fGeomParam(arb, "groom_color").getExpandedValue().getVals()->get()[0].z,
                        hairAppearanceColor({0.2f, 0.3f, 0.4f})[2]);
        EXPECT_TRUE(arb.getPropertyHeader("groom_clump_id") != nullptr);
    }
    // Only the hair has guides (the feathers ride their skinned rachis); none named for the others.
    AbcG::ICurves hairGuides(bird, "guides_bird");
    ASSERT_TRUE(hairGuides.valid());
    const Abc::ICompoundProperty guideArb = hairGuides.getSchema().getArbGeomParams();
    EXPECT_EQ(AbcG::IInt32GeomParam(guideArb, "groom_guide").getExpandedValue().getVals()->get()[0], 1);
    EXPECT_EQ(AbcG::IInt32GeomParam(guideArb, "groom_group_id").getExpandedValue().getVals()->get()[0], 0);
    EXPECT_FALSE(Abc::IObject(bird, "guides_contour").valid());
    EXPECT_FALSE(Abc::IObject(bird, "strands").valid());
    const auto clumps = AbcG::IInt32GeomParam(AbcG::ICurves(bird, "strands_contour").getSchema().getArbGeomParams(),
                                              "groom_clump_id").getExpandedValue().getVals();
    EXPECT_EQ(clumps->get()[2], 1);  // per vane
    std::remove(path.c_str());
}

TEST(Alembic, EngineExportsTakeEveryOptionInEveryHost) {
    // The Unreal and Unity groom exports as Maya's command and the app's panel both write them
    // (engine_export.h): groups, shafts, a lighter groom of barbs, feather guides, barb points.
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    const StrandSet groom = hairAndFeathers();
    EngineExportOptions bad;
    bad.barbDensity = 0.0f;
    EXPECT_FALSE(bad.problem().empty());
    EXPECT_THROW(EngineGroomWriter(tempPath("groomlab_engine_bad.abc"), "bird", 0.0, 1.0 / 24.0, bad), std::invalid_argument);
    bad = EngineExportOptions();
    bad.barbPoints = 1;
    EXPECT_FALSE(bad.problem().empty());

    // Unity: an object per group past 2 strands in parts, the barbs resampled to 4 points.
    EngineExportOptions unity;
    unity.engine = EngineExportOptions::Engine::Unity;
    unity.groups = kGroupIdChannel;
    unity.shafts = false;
    unity.barbPoints = 4;
    unity.maxStrandsPerGroup = 2;
    const std::string path = tempPath("groomlab_engine_unity.abc");
    std::vector<EngineGroomWriter::Object> objects;
    {
        EngineGroomWriter writer(path, "bird", 0.0, 1.0 / 24.0, unity);
        writer.writeFrame(groom, nullptr, nullptr);
        writer.writeFrame(groom, nullptr, nullptr);
        writer.close();
        EXPECT_EQ(writer.frameCount(), 2u);
        objects = writer.objects();
    }
    ASSERT_EQ(objects.size(), 5u);
    EXPECT_EQ(objects[0].path, "/bird/bird_0/bird_0Shape");
    EXPECT_EQ(objects[0].strands, 2u);
    EXPECT_EQ(objects[0].pointsPerStrand, 0u);  // the hair as it is
    EXPECT_EQ(objects[0].minPoints, 3u);
    EXPECT_TRUE(objects[2].feathers);
    EXPECT_EQ(objects[2].pointsPerStrand, 4u);
    std::size_t barbs = 0;
    for (const EngineGroomWriter::Object& o : objects) barbs += o.feathers ? o.strands : 0;
    EXPECT_EQ(barbs, 5u);  // no shafts
    {
        Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
        Abc::IObject bird(archive.getTop(), "bird");
        AbcG::ICurves curves(Abc::IObject(bird, objects[2].group), objects[2].group + "Shape");
        ASSERT_TRUE(curves.valid());
        EXPECT_EQ(curves.getSchema().getValue().getCurvesNumVertices()->get()[0], 4);
        EXPECT_EQ(curves.getSchema().getNumSamples(), 2u);
    }
    std::remove(path.c_str());
    // A lighter groom: half the barbs (by ID, as thinBarbs keeps them), the rest as they were.
    unity.barbDensity = 0.5f;
    unity.barbWidthScale = 2.0f;
    {
        EngineGroomWriter writer(path, "bird", 0.0, 1.0 / 24.0, unity);
        writer.writeFrame(groom, nullptr, nullptr);
        writer.close();
        const StrandSet thin = thinBarbs(groom, 0.5f, 2.0f);
        std::size_t kept = 0, written = 0;
        for (std::size_t s = 0; s < thin.strandCount(); ++s) kept += isBarb(thin, s) ? 1 : 0;
        for (const EngineGroomWriter::Object& o : writer.objects()) written += o.feathers ? o.strands : 0;
        EXPECT_EQ(written, kept);
    }
    std::remove(path.c_str());

    // Unreal: the hair simulates on the primary guides, a feather group on its shafts.
    EngineExportOptions unreal;
    unreal.groups = kGroupIdChannel;
    unreal.shafts = false;
    unreal.featherGuides = true;
    const StrandSet guides = makeStrands(2, 0.25f);
    const std::string ue = tempPath("groomlab_engine_unreal.abc");
    {
        EngineGroomWriter writer(ue, "bird", 0.0, 1.0 / 24.0, unreal);
        writer.writeFrame(groom, &guides, nullptr);
    }
    {
        Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), ue);
        Abc::IObject bird(archive.getTop(), "bird");
        AbcG::ICurves hairGuides(bird, "guides_bird");
        ASSERT_TRUE(hairGuides.valid());
        EXPECT_EQ(hairGuides.getSchema().getValue().getNumCurves(), 2u);
        AbcG::ICurves featherGuides(bird, "guides_group1");
        ASSERT_TRUE(featherGuides.valid());
        EXPECT_EQ(featherGuides.getSchema().getValue().getNumCurves(), 1u);  // its shaft
        EXPECT_EQ(AbcG::ICurves(bird, "strands_group1").getSchema().getValue().getNumCurves(), 3u);  // no shaft
    }
    std::remove(ue.c_str());
}

TEST(Alembic, UnrealOneGroupWritesWhatTheUngroupedExportDoes) {
    // A groom of hair alone: grouped or not, the same objects and attributes.
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    const StrandSet hair = makeStrands(4, 0.0f);
    const StrandSet guides = makeStrands(2, 0.25f);
    const std::string plainPath = tempPath("groomlab_unreal_plain.abc");
    const std::string groupedPath = tempPath("groomlab_unreal_grouped.abc");
    {
        UnrealGroomWriter writer(plainPath, "hair", "hair", 0.0, 1.0 / 24.0);
        writer.writeFrame(hair, &guides, nullptr);
    }
    const std::vector<GroomGroup> groups = splitGroomGroups(hair, kGroupIdChannel, "hair", {});
    ASSERT_EQ(groups.size(), 1u);
    {
        UnrealGroomWriter writer(groupedPath, "hair", "hair", 0.0, 1.0 / 24.0);
        writer.writeGroups({{groups[0].id, groups[0].name, &groups[0].strands, &guides}}, nullptr);
        EXPECT_THROW(writer.writeFrame(hair, &guides, nullptr), AlembicError);  // one way per file
    }
    Abc::IArchive plain(Alembic::AbcCoreOgawa::ReadArchive(), plainPath);
    Abc::IArchive grouped(Alembic::AbcCoreOgawa::ReadArchive(), groupedPath);
    for (const char* object : {"strands", "guides"}) {
        SCOPED_TRACE(object);
        AbcG::ICurves a(Abc::IObject(plain.getTop(), "hair"), object);
        AbcG::ICurves b(Abc::IObject(grouped.getTop(), "hair"), object);
        ASSERT_TRUE(a.valid());
        ASSERT_TRUE(b.valid());
        EXPECT_EQ(a.getSchema().getValue().getNumCurves(), b.getSchema().getValue().getNumCurves());
        const Abc::ICompoundProperty pa = a.getSchema().getArbGeomParams(), pb = b.getSchema().getArbGeomParams();
        ASSERT_EQ(pa.getNumProperties(), pb.getNumProperties());
        for (std::size_t i = 0; i < pa.getNumProperties(); ++i) {
            EXPECT_TRUE(pb.getPropertyHeader(pa.getPropertyHeader(i).getName()) != nullptr) << pa.getPropertyHeader(i).getName();
        }
        for (const char* name : {"groom_id", "groom_group_id"}) {
            const auto va = AbcG::IInt32GeomParam(pa, name).getExpandedValue().getVals();
            const auto vb = AbcG::IInt32GeomParam(pb, name).getExpandedValue().getVals();
            ASSERT_EQ(va->size(), vb->size());
            for (std::size_t i = 0; i < va->size(); ++i) EXPECT_EQ(va->get()[i], vb->get()[i]) << name;
        }
    }
    std::remove(plainPath.c_str());
    std::remove(groupedPath.c_str());
}

TEST(Alembic, WritesUvsChannelsAndColorLikeUsd) {
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    const std::string path = tempPath("groomlab_parity.abc");
    ScalpMesh plane(*test::makePlane(2));
    plane.setCornerUVs(std::vector<ScalpMesh::UV>(plane.triangleCount() * 3, {0.5f, 0.25f}));
    StrandSet s = makeStrands(3, 0.0f);
    s.mutableRoots()[0] = StrandRoot{0, 0.2f, 0.2f};  // the others stay unrooted
    s.addStrandChannel(kClumpIdChannel, 0.0f)[1] = 4.0f;
    s.addStrandChannel(kMelaninChannel, 0.0f)[2] = 1.0f;  // blond, blond, black
    {
        AlembicCurvesWriter writer(path, "groom", 0.0, 1.0 / 24.0);
        writer.writeFrame(s, &plane);
        StrandSet fewer = makeStrands(2, 0.0f);  // a frame without the channels writes zeros for them
        writer.writeFrame(fewer, &plane);
    }
    EXPECT_FALSE(leftTemp(path));

    Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
    AbcG::ICurves curves(Abc::IObject(archive.getTop(), "groom"), "groomShape");
    ASSERT_TRUE(curves.valid());
    AbcG::ICurvesSchema& schema = curves.getSchema();
    const AbcG::IV2fGeomParam uvs = schema.getUVsParam();
    ASSERT_TRUE(uvs.valid());
    const auto uvValues = uvs.getExpandedValue().getVals();
    ASSERT_EQ(uvValues->size(), 3u);
    EXPECT_FLOAT_EQ((*uvValues)[0].x, 0.5f);
    EXPECT_FLOAT_EQ((*uvValues)[0].y, 0.25f);
    EXPECT_FLOAT_EQ((*uvValues)[1].x, 0.0f);
    const Abc::ICompoundProperty arb = schema.getArbGeomParams();
    ASSERT_TRUE(arb.getPropertyHeader(kClumpIdChannel) != nullptr);
    ASSERT_TRUE(arb.getPropertyHeader(kMelaninChannel) != nullptr);
    ASSERT_TRUE(arb.getPropertyHeader("Cd") != nullptr);
    const auto clumps = AbcG::IFloatGeomParam(arb, kClumpIdChannel).getExpandedValue().getVals();
    EXPECT_FLOAT_EQ(clumps->get()[1], 4.0f);
    const auto colors = AbcG::IC3fGeomParam(arb, "Cd").getExpandedValue().getVals();
    ASSERT_EQ(colors->size(), 3u);
    EXPECT_FLOAT_EQ((*colors)[0].x, 1.0f);
    EXPECT_LT((*colors)[2].x, 0.2f);  // black hair, toned down as hair_color.h renders it
    const auto second = AbcG::IFloatGeomParam(arb, kClumpIdChannel).getExpandedValue(Abc::ISampleSelector(Abc::index_t(1))).getVals();
    ASSERT_EQ(second->size(), 2u);
    EXPECT_FLOAT_EQ(second->get()[1], 0.0f);

    // GroomLab's own reader still reads the strands the same.
    const auto frames = readAlembicCurves(path, "groom");
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0].strands.ids()[2], 1002u);
    EXPECT_EQ(frames[1].strands.strandCount(), 2u);
    // And the UVs, one per curve (groomlab-cli hash covers them).
    ASSERT_EQ(frames[0].uvs.size(), 3u);
    EXPECT_FLOAT_EQ(frames[0].uvs[0][0], 0.5f);
    EXPECT_FLOAT_EQ(frames[0].uvs[0][1], 0.25f);
    EXPECT_EQ(frames[1].uvs.size(), 2u);
    std::remove(path.c_str());
}

// What Unity's hair package reads (through com.unity.formats.alembic): points, per-point
// widths and "uvs" with a value per point; a uniform point count saves it resampling.
TEST(Alembic, UnityOptionsScaleResampleAndWriteUvsPerPoint) {
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    const std::string path = tempPath("groomlab_unity.abc");
    ScalpMesh plane(*test::makePlane(2));
    plane.setCornerUVs(std::vector<ScalpMesh::UV>(plane.triangleCount() * 3, {0.5f, 0.25f}));
    StrandSet s;
    s.addStrand(7, {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}}, {0.02f, 0.02f, 0.0f}, StrandRoot{0, 0.2f, 0.2f});
    s.addStrand(8, {{1, 0, 0}, {1, 1, 0}, {1, 2, 0}, {1, 3, 0}, {1, 4, 0}}, {0.04f, 0.03f, 0.02f, 0.01f, 0.0f});
    {
        AlembicCurvesWriter::Options options;
        options.scale = 0.01f;
        options.pointsPerStrand = 6;
        options.vertexUVs = true;
        AlembicCurvesWriter writer(path, "groom", 0.0, 1.0 / 24.0, options);
        writer.writeFrame(s, &plane);
    }
    const auto frames = readAlembicCurves(path, "groom");
    ASSERT_EQ(frames.size(), 1u);
    const StrandSet& read = frames[0].strands;
    ASSERT_EQ(read.strandCount(), 2u);
    EXPECT_EQ(read.uniformPointCount(), 6u);
    EXPECT_EQ(read.ids()[1], 8u);
    // Scaled about the origin, evenly spaced: the second strand's points every 0.8 cm = 0.008.
    EXPECT_NEAR(read.strandPoints(1)[5].y, 0.04f, 1e-6f);
    EXPECT_NEAR(read.strandPoints(1)[1].y, 0.008f, 1e-6f);
    EXPECT_NEAR(read.strandPoints(1)[0].x, 0.01f, 1e-6f);
    // Widths scale with the points and follow the resampling.
    EXPECT_NEAR(read.widths()[6], 0.0004f, 1e-7f);
    EXPECT_NEAR(read.widths()[11], 0.0f, 1e-7f);
    EXPECT_NEAR(read.widths()[0], 0.0002f, 1e-7f);

    {  // closed before the file is written again
        Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
        AbcG::ICurves curves(Abc::IObject(archive.getTop(), "groom"), "groomShape");
        ASSERT_TRUE(curves.valid());
        const AbcG::IV2fGeomParam uvs = curves.getSchema().getUVsParam();
        ASSERT_TRUE(uvs.valid());
        EXPECT_EQ(uvs.getScope(), AbcG::kVertexScope);
        const auto uvValues = uvs.getExpandedValue().getVals();
        ASSERT_EQ(uvValues->size(), 12u);  // one per point, as Unity's importer wants
        for (std::size_t k = 0; k < 6; ++k) {
            EXPECT_FLOAT_EQ((*uvValues)[k].x, 0.5f);      // the rooted strand's root UV all along it
            EXPECT_FLOAT_EQ((*uvValues)[6 + k].x, 0.0f);  // the unrooted one: zeros
        }
        const AbcG::IFloatGeomParam widths = curves.getSchema().getWidthsParam();
        EXPECT_EQ(widths.getScope(), AbcG::kVertexScope);
    }
    std::remove(path.c_str());

    // The defaults stay as they were: UVs once per curve, points as given.
    {
        AlembicCurvesWriter writer(path, "groom", 0.0, 1.0 / 24.0);
        writer.writeFrame(s, &plane);
    }
    {
        Abc::IArchive plain(Alembic::AbcCoreOgawa::ReadArchive(), path);
        AbcG::ICurves plainCurves(Abc::IObject(plain.getTop(), "groom"), "groomShape");
        EXPECT_EQ(plainCurves.getSchema().getUVsParam().getScope(), AbcG::kUniformScope);
        EXPECT_EQ(readAlembicCurves(path, "groom")[0].strands.pointCount(), 8u);
    }
    std::remove(path.c_str());

    EXPECT_THROW(AlembicCurvesWriter(path, "groom", 0.0, 1.0, AlembicCurvesWriter::Options{0.0f, 0, false}),
                 AlembicError);
    EXPECT_THROW(AlembicCurvesWriter(path, "groom", 0.0, 1.0, AlembicCurvesWriter::Options{1.0f, 1, false}),
                 AlembicError);
}

// Feathers for Unity's hair package: a curves object (a Unity strand group) per groom group,
// groups past the strand limit in parts, barbs with points of their own, root UVs per point
// and no root width at 0.
TEST(Alembic, UnityGroupsAreCurvesObjectsOfTheirOwn) {
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    const std::string path = tempPath("groomlab_unity_groups.abc");
    ScalpMesh plane(*test::makePlane(2));
    plane.setCornerUVs(std::vector<ScalpMesh::UV>(plane.triangleCount() * 3, {0.5f, 0.25f}));
    StrandSet groom = hairAndFeathers();
    for (StrandRoot& r : groom.mutableRoots()) r = StrandRoot{0, 0.2f, 0.2f};
    groom.mutableWidths()[groom.strandBegin(5)] = 0.0f;  // a barb whose root width is 0
    // The shafts are the rachis mesh; groups past a limit of 2 strands (the hair's 3, the contour
    // design's 3 barbs) go in parts, the down design's 2 barbs stay whole.
    std::vector<GroomGroup> groups = limitGroupSize(
        splitGroomGroups(withoutShafts(groom), kGroupIdChannel, "bird", {{1, "contour"}, {2, "down feathers"}}), 2);
    ASSERT_EQ(groups.size(), 5u);
    std::vector<AlembicCurvesWriter::Group> written;
    for (const GroomGroup& g : groups) written.push_back({g.name, &g.strands, g.feathers ? 4u : 0u});
    AlembicCurvesWriter::Options options;
    options.vertexUVs = true;
    options.positiveRootWidths = true;
    {
        AlembicCurvesWriter writer(path, "bird", 0.0, 1.0 / 24.0, options);
        writer.writeGroups(written, &plane);
        EXPECT_EQ(writer.objectPaths(),
                  (std::vector<std::string>{"/bird/bird_0/bird_0Shape", "/bird/bird_1/bird_1Shape",
                                            "/bird/contour_0/contour_0Shape", "/bird/contour_1/contour_1Shape",
                                            "/bird/down_feathers/down_feathersShape"}));
        EXPECT_THROW(writer.writeFrame(groom, &plane), AlembicError);  // one way per file
    }
    EXPECT_FALSE(leftTemp(path));
    {
        Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
        Abc::IObject bird(archive.getTop(), "bird");
        ASSERT_TRUE(bird.valid());
        ASSERT_EQ(bird.getNumChildren(), 5u);  // in the order given: Unity's strand groups
        struct Expect {
            const char* object;
            std::size_t curves;
            std::size_t points;  // per curve
        };
        // The hair's points as they are (3), the barbs' resampled to their own 4.
        const Expect expected[] = {{"bird_0", 2, 3}, {"bird_1", 1, 3}, {"contour_0", 2, 4}, {"contour_1", 1, 4},
                                   {"down_feathers", 2, 4}};
        for (std::size_t i = 0; i < 5; ++i) {
            const Expect& e = expected[i];
            SCOPED_TRACE(e.object);
            EXPECT_EQ(bird.getChildHeader(i).getName(), e.object);
            AbcG::ICurves curves(Abc::IObject(bird, e.object), std::string(e.object) + "Shape");
            ASSERT_TRUE(curves.valid());
            const AbcG::ICurvesSchema::Sample sample = curves.getSchema().getValue();
            ASSERT_EQ(sample.getNumCurves(), e.curves);
            for (std::size_t c = 0; c < e.curves; ++c) EXPECT_EQ(sample.getCurvesNumVertices()->get()[c], static_cast<int>(e.points));
            const std::size_t points = e.curves * e.points;
            const AbcG::IV2fGeomParam uvs = curves.getSchema().getUVsParam();
            ASSERT_TRUE(uvs.valid());
            EXPECT_EQ(uvs.getScope(), AbcG::kVertexScope);
            const auto uvValues = uvs.getExpandedValue().getVals();
            ASSERT_EQ(uvValues->size(), points);  // one per point, each strand's root UV
            for (std::size_t k = 0; k < points; ++k) EXPECT_FLOAT_EQ((*uvValues)[k].x, 0.5f);
            const auto widths = curves.getSchema().getWidthsParam().getExpandedValue().getVals();
            ASSERT_EQ(widths->size(), points);
            for (std::size_t c = 0; c < e.curves; ++c) EXPECT_GT((*widths)[c * e.points], 0.0f);
        }
    }
    std::remove(path.c_str());

    // One group: the layout (and data) writeFrame gives.
    const StrandSet hair = makeStrands(4, 0.0f);
    const std::string plainPath = tempPath("groomlab_unity_plain.abc");
    {
        AlembicCurvesWriter writer(plainPath, "hair", 0.0, 1.0 / 24.0, options);
        writer.writeFrame(hair, &plane);
    }
    {
        AlembicCurvesWriter writer(path, "hair", 0.0, 1.0 / 24.0, options);
        writer.writeGroups({{"hair", &hair, 0}}, &plane);
        EXPECT_EQ(writer.objectPaths(), std::vector<std::string>{"/hair/hairShape"});
    }
    const auto a = readAlembicCurves(plainPath, "hair"), b = readAlembicCurves(path, "hair");
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(a[0].strands.positions(), b[0].strands.positions());
    EXPECT_EQ(a[0].strands.widths(), b[0].strands.widths());
    EXPECT_EQ(a[0].strands.ids(), b[0].strands.ids());
    EXPECT_GT(a[0].strands.widths()[0], 0.0f);  // makeStrands' tips are 0: only roots are kept above
    EXPECT_FLOAT_EQ(a[0].strands.widths()[2], 0.0f);
    std::remove(plainPath.c_str());
    std::remove(path.c_str());
    EXPECT_THROW(
        {
            AlembicCurvesWriter writer(path, "hair", 0.0, 1.0 / 24.0, options);
            writer.writeGroups({{"a", &hair, 0}, {"a", &hair, 0}}, nullptr);
        },
        AlembicError);  // names tell groups apart
    std::remove(path.c_str());
}

TEST(Alembic, RefusesMalformedCurves) {
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    const std::string path = tempPath("groomlab_malformed.abc");
    {
        // Vertex counts adding up past the positions: an out-of-bounds read waiting to happen.
        Abc::OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), path);
        AbcG::OCurves curves(archive.getTop(), "bad");
        const std::vector<Imath::V3f> pts(3, Imath::V3f(0.0f, 0.0f, 0.0f));
        const std::vector<std::int32_t> counts{2, 4};
        const Abc::P3fArraySample positions(pts);
        const Abc::Int32ArraySample vertexCounts(counts);
        const AbcG::OCurvesSchema::Sample sample(positions, vertexCounts);
        curves.getSchema().set(sample);
    }
    EXPECT_THROW(readAlembicCurves(path), AlembicError);
    EXPECT_THROW(readAlembicCurvesAt(path, 0.0), AlembicError);
    try {
        readAlembicCurves(path);
    } catch (const AlembicError& e) {
        EXPECT_NE(std::string(e.what()).find("add up to 6"), std::string::npos) << e.what();
    }
    std::remove(path.c_str());
}

TEST(Alembic, CurvesReaderStaysOpenAcrossTimes) {
    const std::string path = tempPath("groomlab_reader.abc");
    {
        AlembicCurvesWriter writer(path, "groom", 0.0, 1.0 / 24.0);
        writer.writeFrame(makeStrands(3, 0.0f));
        writer.writeFrame(makeStrands(3, 1.0f));
    }
    AlembicCurvesReader reader(path);
    EXPECT_FLOAT_EQ(reader.at(0.0).strandPoints(0)[0].x, 0.0f);
    EXPECT_FLOAT_EQ(reader.at(1.0 / 24.0).strandPoints(0)[0].x, 1.0f);
    EXPECT_NEAR(reader.at(0.5 / 24.0).strandPoints(0)[0].x, 0.5f, 1e-6f);
    EXPECT_THROW(AlembicCurvesReader(tempPath("groomlab_does_not_exist.abc")), AlembicError);
    std::remove(path.c_str());
}

TEST(Alembic, MeshReaderKnowsWhetherItAnimates) {
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    const std::string still = tempPath("groomlab_still_mesh.abc");
    const std::string moving = tempPath("groomlab_moving_mesh.abc");
    auto write = [](const std::string& path, int samples) {
        Abc::OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), path);
        const std::uint32_t ts = archive.addTimeSampling(AbcG::TimeSampling(1.0 / 24.0, 0.0));
        AbcG::OPolyMesh mesh(archive.getTop(), "head", ts);
        const std::vector<std::int32_t> indices{0, 1, 2}, counts{3};
        for (int i = 0; i < samples; ++i) {
            const std::vector<Imath::V3f> pts{{0, 0, 0}, {1, 0, 0}, {0, static_cast<float>(1 + i), 0}};
            mesh.getSchema().set(AbcG::OPolyMeshSchema::Sample(Abc::P3fArraySample(pts), Abc::Int32ArraySample(indices),
                                                               Abc::Int32ArraySample(counts)));
        }
    };
    write(still, 1);
    write(moving, 3);
    EXPECT_FALSE(AlembicMeshReader(still).animated());
    EXPECT_TRUE(AlembicMeshReader(moving).animated());
    std::remove(still.c_str());
    std::remove(moving.c_str());
}

TEST(Alembic, MeshReaderGivesTheMeshWithItsFacesAndUVs) {
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    // A quad and a triangle in the xz plane, wound as AbcExport writes a Maya mesh facing +y
    // (reversed: clockwise seen from above), with face-varying UVs.
    const std::string path = tempPath("groomlab_faces_mesh.abc");
    {
        Abc::OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), path);
        const std::uint32_t ts = archive.addTimeSampling(AbcG::TimeSampling(1.0 / 24.0, 0.0));
        AbcG::OPolyMesh mesh(archive.getTop(), "head", ts);
        const std::vector<Imath::V3f> pts{{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {2, 0, 0}};
        // Maya's order is 0 3 2 1 and 1 2 4 (counter-clockwise from +y); AbcExport writes each reversed.
        const std::vector<std::int32_t> indices{1, 2, 3, 0, 4, 2, 1}, counts{4, 3};
        const std::vector<Imath::V2f> uv{{1, 0}, {1, 1}, {0, 1}, {0, 0}, {2, 0}, {1, 1}, {1, 0}};
        const Abc::V2fArraySample uvValues(uv);
        const AbcG::OV2fGeomParam::Sample uvs(uvValues, AbcG::kFacevaryingScope);
        const Abc::P3fArraySample pointsSample(pts);
        const Abc::Int32ArraySample indicesSample(indices), countsSample(counts);
        AbcG::OPolyMeshSchema::Sample sample(pointsSample, indicesSample, countsSample);
        sample.setUVs(uvs);
        mesh.getSchema().set(sample);
    }
    const MeshPtr scalp = AlembicMeshReader(path).meshAt(0.0);
    ASSERT_TRUE(scalp);
    EXPECT_EQ(scalp->pointCount(), 5u);
    ASSERT_EQ(scalp->triangleCount(), 3u);  // the quad as two, the triangle as one
    EXPECT_TRUE(scalp->hasUVs());
    EXPECT_EQ(scalp->cornerUVs().size(), 9u);
    // Every triangle faces +y: the reversed fan undoes the file's winding.
    for (std::size_t t = 0; t < scalp->triangleCount(); ++t) {
        StrandRoot root;
        root.face = static_cast<std::int32_t>(t);
        root.u = root.v = 0.25f;
        EXPECT_GT(scalp->normal(root).y, 0.9f) << "triangle " << t;
    }
    // The quad's first corner, vertex 1 at (1, 0, 0), keeps its UV (1, 0).
    StrandRoot corner;
    corner.face = 0;
    corner.u = 0.0f;
    corner.v = 0.0f;
    const ScalpMesh::Triangle& t0 = scalp->triangles()[0];
    EXPECT_EQ(t0[0], 1);
    const ScalpMesh::UV at0 = scalp->uvAt(corner);
    EXPECT_FLOAT_EQ(at0[0], 1.0f);
    EXPECT_FLOAT_EQ(at0[1], 0.0f);
    std::remove(path.c_str());
}

TEST(Alembic, GroomSceneFeedsJiggleTheScalpCachesMotion) {
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    // A plane scalp cached at 24 fps: still for 4 frames, sliding +x over frames 4-8, still after.
    const MeshPtr plane = test::makePlane(4);
    const std::string cache = tempPath("groomlab_jiggle_scalp.abc");
    auto offset = [](int frame) { return 0.25f * static_cast<float>(std::clamp(frame - 4, 0, 4)); };
    {
        Abc::OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), cache);
        const std::uint32_t ts = archive.addTimeSampling(AbcG::TimeSampling(1.0 / 24.0, 0.0));
        AbcG::OPolyMesh mesh(archive.getTop(), "head", ts);
        std::vector<std::int32_t> indices, counts;
        for (const ScalpMesh::Triangle& t : plane->triangles()) {
            indices.insert(indices.end(), t.begin(), t.end());
            counts.push_back(3);
        }
        for (int f = 0; f <= 20; ++f) {
            std::vector<Imath::V3f> pts;
            for (const Vec3f& p : plane->points()) pts.emplace_back(p.x + offset(f), p.y, p.z);
            mesh.getSchema().set(AbcG::OPolyMeshSchema::Sample(Abc::P3fArraySample(pts), Abc::Int32ArraySample(indices),
                                                               Abc::Int32ArraySample(counts)));
        }
    }
    auto graph = [](bool bypass) {
        return std::string(R"({"version": 1, "nodes": [
            {"name": "scalp", "type": "Scalp"},
            {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}},
            {"name": "jiggle", "type": "Jiggle", "params": {"history": 8}, "bypass": )") +
               (bypass ? "true" : "false") + R"(},
            {"name": "output", "type": "Output"}],
          "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                          {"from": "scatter", "to": "jiggle", "input": "strands"},
                          {"from": "jiggle", "to": "output", "input": "strands"}], "output": "output"})";
    };
    auto sceneWith = [&](bool bypass, bool cached) {
        GroomFile file;
        file.data.scalp = plane;
        file.data.graphJson = graph(bypass);
        if (cached) file.scalpCache = {cache, "head"};
        return std::make_unique<GroomScene>(std::move(file));
    };
    auto jiggly = sceneWith(false, true), plain = sceneWith(true, true);
    for (int f = 0; f <= 20; ++f) {
        const double seconds = f / 24.0;
        const StrandsPtr a = jiggly->evaluate(seconds), b = plain->evaluate(seconds);
        ASSERT_TRUE(a && b);
        EXPECT_TRUE(jiggly->nodeNotes().empty());
        if (f <= 4 || f >= 8 + 8) {  // still over the whole history: frames f-8..f-1
            EXPECT_EQ(a->positions(), b->positions()) << "frame " << f << ": the scalp was still over the history";
        } else {
            EXPECT_NE(a->positions(), b->positions()) << "frame " << f << ": the scalp moved in the last 8 frames";
        }
    }
    // Scrubbing back gives the same frame as playing to it.
    const StrandsPtr played = jiggly->evaluate(9 / 24.0);
    EXPECT_EQ(sceneWith(false, true)->evaluate(9 / 24.0)->positions(), played->positions());
    // No scalp cache: nothing moves; Jiggle passes the strands through and says why.
    const auto still = sceneWith(false, false);
    const StrandsPtr s = still->evaluate(0.5);
    EXPECT_EQ(s->positions(), sceneWith(true, false)->evaluate(0.5)->positions());
    ASSERT_EQ(still->nodeNotes().size(), 1u);
    EXPECT_EQ(still->nodeNotes()[0].first, "jiggle");
    // Time: the scalp cache animates (and a Jiggle node reads it); without either nothing moves.
    EXPECT_TRUE(jiggly->readsTime());
    EXPECT_TRUE(plain->readsTime());
    EXPECT_TRUE(still->readsTime());  // a Jiggle node (it would read the scalp's motion)
    {
        GroomFile file;
        file.data.scalp = plane;
        file.data.graphJson = R"({"version": 1, "nodes": [{"name": "scalp", "type": "Scalp"},
            {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}}, {"name": "output", "type": "Output"}],
          "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                          {"from": "scatter", "to": "output", "input": "strands"}], "output": "output"})";
        EXPECT_FALSE(GroomScene(file).readsTime());
        // Following the scalp from a groom file: the cache's frames carry the groom at rest.
        auto rest = std::make_shared<ScalpMesh>(*plane);
        rest->setRestPoints(plane->points());
        file.data.scalp = rest;
        file.data.followScalp = true;
        file.scalpCache = {cache, "head"};
        GroomScene follow(file);
        EXPECT_TRUE(follow.readsTime());
        const StrandsPtr atRest = follow.evaluate(0.0), slid = follow.evaluate(8 / 24.0);
        EXPECT_TRUE(follow.session().following());
        ASSERT_EQ(atRest->pointCount(), slid->pointCount());
        for (std::size_t i = 0; i < atRest->pointCount(); ++i) {
            const Vec3f d = slid->positions()[i] - atRest->positions()[i];
            EXPECT_NEAR(d.x, 1.0f, 1e-5f);
            EXPECT_NEAR(d.y, 0.0f, 1e-5f);
            EXPECT_NEAR(d.z, 0.0f, 1e-5f);
        }
    }
    jiggly.reset();  // closes the cache
    plain.reset();
    std::error_code ec;
    std::filesystem::remove(cache, ec);
}

TEST(Alembic, GroomSceneViewsWhatItEvaluates) {
    // A view request at a time gives the strands evaluate() gives there, with the request's
    // density and point budget honored (the standalone app's path).
    GroomFile file;
    file.data.scalp = test::makePlane(4);
    file.data.graphJson = R"({"version": 1, "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 400.0}},
        {"name": "output", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                      {"from": "scatter", "to": "output", "input": "strands"}], "output": "output"})";
    GroomScene scene(std::move(file));
    const StrandsPtr full = scene.evaluate(0.0, 1.0f);
    ASSERT_TRUE(full);
    ViewRequest request;
    request.density = 1.0f;
    const ViewResult& view = scene.view(request);
    ASSERT_TRUE(view.strands);
    EXPECT_TRUE(view.errors.empty());
    EXPECT_EQ(view.strands->strandCount(), full->strandCount());
    EXPECT_TRUE(view.bounds.valid);
    request.density = 0.25f;
    const ViewResult& quarter = scene.view(request);
    ASSERT_TRUE(quarter.strands);
    EXPECT_LT(quarter.strands->strandCount(), full->strandCount());
    EXPECT_GT(quarter.strands->strandCount(), 0u);
}

// Two scales of a triangle each, moved `dz` along z: what buildScaleMesh gives, by hand.
ScaleMesh twoScales(float dz) {
    ScaleMesh m;
    m.points = {{0, 0, dz}, {1, 0, dz}, {0, 1, dz}, {2, 0, dz}, {3, 0, dz}, {2, 1, dz}};
    m.normals.assign(6, Vec3f{0, 0, 1});
    m.tangents.assign(6, Vec3f{0, 1, 0});
    m.uvs = {{0, 0}, {1, 0}, {0, 1}, {0, 0}, {1, 0}, {0, 1}};
    m.colors = {{0.1f, 0.2f, 0.3f}, {0.1f, 0.2f, 0.3f}, {0.4f, 0.5f, 0.6f}, {0.7f, 0.7f, 0.7f}, {0.7f, 0.7f, 0.7f}, {0.9f, 0.8f, 0.7f}};
    m.triangles = {0, 1, 2, 3, 4, 5};
    for (std::uint32_t k = 0; k < 2; ++k) {
        ScaleMesh::Scale s;
        s.id = 100 + k;
        s.firstVertex = 3 * k;
        s.vertexCount = 3;
        s.firstTriangle = k;
        s.triangleCount = 1;
        s.random = 0.25f + 0.5f * static_cast<float>(k);
        s.rootUv = {0.1f * static_cast<float>(k + 1), 0.5f};
        s.color = {0.3f * static_cast<float>(k), 0.2f, 0.1f};
        s.variant = static_cast<std::uint16_t>(k);
        m.scales.push_back(s);
    }
    return m;
}

TEST(Alembic, WritesScalesAsAnAnimatedMeshWithTheirData) {
    const std::string path = tempPath("groomlab_scales.abc");
    StrandSet strands = makeStrands(2, 0.0f);
    auto instances = std::make_shared<InstanceSet>();
    instances->hidesStrands = true;
    strands.setInstances(instances);
    {
        AlembicCurvesWriter writer(path, "dragon", 1.0 / 24.0, 1.0 / 24.0);
        const ScaleMesh a = twoScales(0.0f), b = twoScales(0.5f);
        writer.writeFrame(strands, nullptr, &a);
        writer.writeFrame(strands, nullptr, &b);
        writer.writeFrame(strands, nullptr, nullptr);  // the scales gone: an empty mesh
    }
    namespace Abc = Alembic::Abc;
    namespace AbcG = Alembic::AbcGeom;
    Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
    Abc::IObject root(archive.getTop(), "dragon");
    ASSERT_TRUE(root.valid());
    // The strands only lay the scales out: hidden.
    Abc::IObject curves(root, "dragonShape");
    ASSERT_TRUE(curves.valid());
    EXPECT_EQ(AbcG::GetVisibility(curves, Abc::ISampleSelector(Abc::index_t(0))), AbcG::kVisibilityHidden);
    AbcG::IPolyMesh mesh(Abc::IObject(root, "scales"), "scalesShape");
    ASSERT_TRUE(mesh.valid());
    AbcG::IPolyMeshSchema& schema = mesh.getSchema();
    ASSERT_EQ(schema.getNumSamples(), 3u);
    const ScaleMesh expected = twoScales(0.5f);
    AbcG::IPolyMeshSchema::Sample second;
    schema.get(second, Abc::ISampleSelector(Abc::index_t(1)));
    ASSERT_EQ(second.getPositions()->size(), 6u);
    for (std::size_t i = 0; i < 6; ++i) EXPECT_EQ((*second.getPositions())[i].z, 0.5f);
    // Corners reversed as AbcExport writes them: a reader that reverses them back gets ours.
    const auto& indices = *second.getFaceIndices();
    ASSERT_EQ(indices.size(), 6u);
    for (std::size_t f = 0; f < 2; ++f) {
        EXPECT_EQ(static_cast<std::uint32_t>(indices[3 * f]), expected.triangles[3 * f]);
        EXPECT_EQ(static_cast<std::uint32_t>(indices[3 * f + 1]), expected.triangles[3 * f + 2]);
        EXPECT_EQ(static_cast<std::uint32_t>(indices[3 * f + 2]), expected.triangles[3 * f + 1]);
    }
    AbcG::IV2fGeomParam::Sample uvs;
    schema.getUVsParam().getExpanded(uvs, Abc::ISampleSelector(Abc::index_t(1)));
    EXPECT_EQ(uvs.getScope(), AbcG::kFacevaryingScope);
    EXPECT_EQ(uvs.getVals()->size(), 6u);
    EXPECT_TRUE(schema.getNormalsParam().valid());
    const Abc::ICompoundProperty arb = schema.getArbGeomParams();
    AbcG::IC3fGeomParam color(arb, "scaleColor"), rootColor(arb, "scaleRootColor");
    AbcG::IFloatGeomParam id(arb, "scaleId");
    AbcG::IV2fGeomParam rootUv(arb, "scaleUV");
    AbcG::IInt32GeomParam variant(arb, "scaleVariant");
    EXPECT_EQ(color.getScope(), AbcG::kVertexScope);
    EXPECT_EQ(rootColor.getScope(), AbcG::kUniformScope);
    const auto colors = color.getExpandedValue(Abc::ISampleSelector(Abc::index_t(1))).getVals();
    ASSERT_EQ(colors->size(), 6u);
    EXPECT_EQ((*colors)[5].x, 0.9f);
    const auto ids = id.getExpandedValue(Abc::ISampleSelector(Abc::index_t(1))).getVals();
    const auto roots = rootUv.getExpandedValue(Abc::ISampleSelector(Abc::index_t(1))).getVals();
    const auto variants = variant.getExpandedValue(Abc::ISampleSelector(Abc::index_t(1))).getVals();
    const auto rootColors = rootColor.getExpandedValue(Abc::ISampleSelector(Abc::index_t(1))).getVals();
    ASSERT_EQ(ids->size(), 2u);
    for (std::size_t f = 0; f < 2; ++f) {
        EXPECT_EQ((*ids)[f], expected.scales[f].random);
        EXPECT_EQ((*roots)[f].x, expected.scales[f].rootUv[0]);
        EXPECT_EQ((*variants)[f], expected.scales[f].variant);
        EXPECT_EQ((*rootColors)[f].x, expected.scales[f].color[0]);
    }
    AbcG::IPolyMeshSchema::Sample third;
    schema.get(third, Abc::ISampleSelector(Abc::index_t(2)));
    EXPECT_EQ(third.getPositions()->size(), 0u);
    std::remove(path.c_str());
}
}  // namespace
}  // namespace groomlab
