// SPDX-License-Identifier: Apache-2.0
// GroomLab > Card Atlas... in the app (card_atlas_window.h): a document whose grooms are Card Atlas
// blocks (card_blocks.h), made, laid out, edited (the root band turned or resized: the groom moved
// onto it by its UVs), baked into one texture set, its maps and card shapes exported, saved and
// opened again. Maya's test_card_atlas.py is the same in Maya.

#include <cstdlib>
#include <filesystem>

#include <QComboBox>
#include <QCoreApplication>
#include <QListWidget>

#include <gtest/gtest.h>

#include "card_atlas_window.h"
#include "groomlab/guide_hierarchy.h"
#include "groom_document.h"
#include "main_window.h"

namespace groomlab::app {
namespace {

namespace fs = std::filesystem;

TEST(AppCardAtlas, BlocksMadeLaidOutEditedBakedExportedSavedAndOpened) {
    const fs::path dir = fs::temp_directory_path() / "groomlab_app_card_atlas";
    fs::remove_all(dir);
    fs::create_directories(dir);
    MainWindow window;
    window.resize(1280, 720);
    CardAtlasWindow* atlas = window.openCardAtlas();
    ASSERT_TRUE(atlas);

    // New Atlas with no document: a document of one block.
    ASSERT_TRUE(atlas->newAtlas());
    ASSERT_EQ(window.groomCount(), 1);
    EXPECT_FALSE(window.groomAt(0)->cardBlock().empty());
    // Two more: three blocks, laid out apart, side by side in the scene.
    ASSERT_TRUE(atlas->addBlock());
    ASSERT_TRUE(atlas->addBlock());
    ASSERT_EQ(window.groomCount(), 3);
    CardBlockAtlas a = atlas->atlas();
    ASSERT_EQ(a.blocks.size(), 3u);
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = i + 1; j < 3; ++j) EXPECT_FALSE(cardCellsOverlap(a.blocks[i].cell, a.blocks[j].cell));
        if (i > 0) EXPECT_GT(a.blocks[i].origin.x, a.blocks[i - 1].origin.x);
    }
    EXPECT_EQ(atlas->findChild<QListWidget*>(QStringLiteral("cardAtlasBlocks"))->count(), 3);
    // A block picked makes its groom the active one.
    ASSERT_TRUE(atlas->pickBlock(1));
    EXPECT_EQ(window.activeGroom(), atlas->blockGrooms()[1]);

    // Its root band tilted and widened: the groom moved onto the new band, its block written.
    const std::size_t strandsBefore = window.groomAt(atlas->blockGrooms()[1])->fullDensityStrands()->strandCount();
    // Each guide's shape about its root: the tilt moves the guides with the band, not turns them.
    const auto shapes = [&]() {
        std::vector<std::vector<Vec3f>> out;
        for (const HierarchyGuide& g : window.groomAt(atlas->blockGrooms()[1])->session().authoredHierarchy()->guides()) {
            std::vector<Vec3f> s;
            for (const Vec3f& p : g.points) s.push_back(p - g.points.front());
            out.push_back(std::move(s));
        }
        return out;
    };
    const std::vector<std::vector<Vec3f>> shaped = shapes();
    const std::size_t guidesBefore = shaped.size();
    ASSERT_TRUE(atlas->editBlock(1, 3.0f, 12.0f, 0.5f, {35.0f, 0.0f, 0.0f}));
    const std::vector<std::vector<Vec3f>> tilted = shapes();
    ASSERT_EQ(tilted.size(), shaped.size());
    for (std::size_t i = 0; i < shaped.size(); ++i) {
        ASSERT_EQ(tilted[i].size(), shaped[i].size());
        for (std::size_t k = 0; k < shaped[i].size(); ++k) EXPECT_NEAR((tilted[i][k] - shaped[i][k]).length(), 0.0f, 1e-3f) << i << " " << k;
    }
    a = atlas->atlas();
    EXPECT_FLOAT_EQ(a.blocks[1].width, 3.0f);
    EXPECT_FLOAT_EQ(a.blocks[1].rootRotation.x, 35.0f);
    const StrandsPtr after = window.groomAt(atlas->blockGrooms()[1])->fullDensityStrands();
    ASSERT_TRUE(after);
    EXPECT_GT(after->strandCount(), strandsBefore);  // a wider band: more hair at the same density
    EXPECT_EQ(window.groomAt(atlas->blockGrooms()[1])->session().authoredHierarchy()->guides().size(), guidesBefore);  // the same guides

    // The card fits the hair: its guides combed 3 across toward their tips, the card as wide as the hair.
    {
        GroomDocument* doc = window.groomAt(atlas->blockGrooms()[2]);
        ASSERT_TRUE(atlas->pickBlock(2));
        const float before = cardBlockFrame(atlas->atlas().blocks[2]).width();
        doc->editGuides("comb across", [](GuideHierarchy& h) {
            for (const HierarchyGuide& g : std::vector<HierarchyGuide>(h.guides())) {
                std::vector<Vec3f> points = g.points;
                for (std::size_t k = 0; k < points.size(); ++k) points[k].x += 3.0f * static_cast<float>(k) / static_cast<float>(points.size() - 1);
                h.setPoints(g.id, std::move(points));
            }
        });
        ASSERT_TRUE(atlas->fitCards());
        const CardFrame fitted = cardBlockFrame(atlas->atlas().blocks[2]);
        EXPECT_GT(fitted.width(), before + 2.0f);
        EXPECT_FALSE(atlas->fitCards());  // nothing more to fit
        a = atlas->atlas();
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = i + 1; j < 3; ++j) EXPECT_FALSE(cardCellsOverlap(a.blocks[i].cell, a.blocks[j].cell));
        }
    }

    // Dragged on the atlas: placed by hand, kept through a new block's layout.
    const std::array<float, 4> cell{0.6f, 0.1f, 0.75f, 0.7f};
    ASSERT_TRUE(atlas->moveBlock(0, cell));
    ASSERT_TRUE(atlas->duplicateBlock());
    a = atlas->atlas();
    ASSERT_EQ(a.blocks.size(), 4u);
    EXPECT_EQ(a.blocks[0].cell, cell);
    EXPECT_TRUE(a.blocks[0].placed);
    for (std::size_t i = 1; i < 4; ++i) EXPECT_FALSE(cardCellsOverlap(a.blocks[0].cell, a.blocks[i].cell));
    ASSERT_TRUE(atlas->autoLayout());
    EXPECT_FALSE(atlas->atlas().blocks[0].placed);

    // The preview: the map picked shows.
    ASSERT_TRUE(atlas->bakePreview(128));
    atlas->mapBox()->setCurrentText(QStringLiteral("Alpha"));
    EXPECT_FALSE(atlas->canvas()->image().isNull());
    EXPECT_EQ(atlas->canvas()->image().width(), 128);
    if (const char* grab = std::getenv("GROOMLAB_APP_GRAB")) {  // a picture of the window to look at
        atlas->resize(1100, 700);
        atlas->show();
        QCoreApplication::processEvents();
        atlas->grab().save(QString::fromUtf8(grab) + QStringLiteral("/card_atlas_app.png"));
    }

    // The exports.
    CardAtlasSettings s = atlas->atlas().settings;
    s.name = "fringe";
    ASSERT_TRUE(atlas->setSettings(s));
    const std::vector<std::string> maps = atlas->exportMaps(QString::fromStdString((dir / "maps").u8string()), 128);
    ASSERT_FALSE(maps.empty()) << atlas->status().toStdString();
    EXPECT_TRUE(fs::exists(dir / "maps" / "fringe_Color.tga"));
    ASSERT_TRUE(atlas->exportShapes(QString::fromStdString((dir / "cards.fbx").u8string()), 6, 20.0f));
    EXPECT_GT(fs::file_size(dir / "cards.fbx"), 1000u);

    // Saved and opened again: the same atlas.
    const CardBlockAtlas saved = atlas->atlas();
    const QString path = QString::fromStdString((dir / "fringe.groom").u8string());
    ASSERT_TRUE(window.saveTo(path));
    ASSERT_TRUE(window.openGroom(path));
    QCoreApplication::processEvents();
    const CardBlockAtlas opened = atlas->atlas();
    ASSERT_EQ(opened.blocks.size(), saved.blocks.size());
    EXPECT_EQ(opened.settings.name, "fringe");
    for (std::size_t i = 0; i < saved.blocks.size(); ++i) {
        EXPECT_EQ(opened.blocks[i].cell, saved.blocks[i].cell);
        EXPECT_FLOAT_EQ(opened.blocks[i].rootRotation.x, saved.blocks[i].rootRotation.x);
    }
    // The file is Maya's atlas too: every groom a block.
    const std::vector<GroomFile> files = readGroomFiles(path.toStdString());
    ASSERT_EQ(files.size(), saved.blocks.size());
    for (const GroomFile& f : files) EXPECT_FALSE(f.cardBlock.empty());

    // Removed: one block fewer.
    ASSERT_TRUE(atlas->pickBlock(3));
    ASSERT_TRUE(atlas->removeBlock());
    EXPECT_EQ(atlas->atlas().blocks.size(), 3u);
    atlas->close();
    fs::remove_all(dir);
}

}  // namespace
}  // namespace groomlab::app
