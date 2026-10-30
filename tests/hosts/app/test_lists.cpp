// SPDX-License-Identifier: Apache-2.0
// The Groom Tools' lists (engine/ui/lists.json, as Maya's tool_tabs.py): the guide groups, the
// maps and the correctives, their bars and right-click menus built from the definitions with the
// window's bindings, and the document's operations they run, each one undo step (files written or
// deleted on the way undone too). The offscreen platform; the prompts answered by their labels.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <QAction>
#include <QImage>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "app_actions.h"
#include "correctives_panel.h"
#include "groom_document.h"
#include "groomlab/correctives.h"
#include "groomlab/guide_selection.h"
#include "guides_panel.h"
#include "list_bar.h"
#include "main_window.h"
#include "maps_panel.h"
#include "overlay.h"

namespace groomlab::app {
namespace {

using nlohmann::json;

const std::string kSampleGroom = std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom";

std::string sampleCopy(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path copy = dir / "sample.groom";
    std::filesystem::copy_file(std::filesystem::u8path(kSampleGroom), copy, std::filesystem::copy_options::overwrite_existing);
    return copy.u8string();
}

std::string nodeOfType(const GroomDocument& doc, const std::string& type) {
    for (const GroomDocument::NodeInfo& n : doc.nodes()) {
        if (n.type == type) return n.name;
    }
    return {};
}

// The menu's entries as shown: texts without their keys, "-" for a separator.
std::vector<std::string> shown(const QMenu* menu) {
    std::vector<std::string> out;
    for (const QAction* a : menu->actions()) {
        if (!a->isVisible()) continue;
        out.push_back(a->isSeparator() ? "-" : a->text().section(QLatin1Char('\t'), 0, 0).toStdString());
    }
    return out;
}

QAction* action(const QMenu* menu, const std::string& text) {
    for (QAction* a : menu->actions()) {
        if (a->text().section(QLatin1Char('\t'), 0, 0).toStdString() == text) return a;
    }
    return nullptr;
}

QPoint rowOf(QTreeWidget* tree, QTreeWidgetItem* item) { return tree->visualItemRect(item).center(); }

TEST(AppLists, GuideGroupsBarMenusAndEdits) {
    MainWindow window;
    window.resize(1280, 800);
    window.show();  // (the offscreen platform: nothing on screen; the prompts show over the viewport)
    ASSERT_TRUE(window.openGroom(QString::fromStdString(sampleCopy("groomlab_app_lists_groups"))));
    GroomDocument* doc = window.document();
    auto* panel = window.findChild<GuidesPanel*>();
    ASSERT_TRUE(panel);
    const ui::Bindings bindings = AppActions::bindings(window);

    // The bar and Edit Guides as lists.json writes them (From Curves is Maya's).
    // + New Group in the header (a + glyph), the picked group's glyphs in its box.
    EXPECT_EQ(panel->bar()->ids(), (std::vector<std::string>{"group.new"}));
    EXPECT_EQ(panel->itemBar()->ids(), (std::vector<std::string>{"group.rename", "group.select", "group.delete"}));
    EXPECT_EQ(panel->bar()->button("group.new")->text(), QStringLiteral("+ New Group"));
    EXPECT_FALSE(panel->bar()->button("group.new")->icon().isNull()) << "a glyph in the header";
    EXPECT_FALSE(panel->itemBar()->button("group.delete")->icon().isNull()) << "a glyph";
    EXPECT_EQ(shown(panel->editsMenu()),
              (std::vector<std::string>{"Children", "-", "Sync +X → -X", "Sync -X → +X", "Mirror Selection", "Mirror Group...", "-", "Reparent...",
                                        "Promote / Demote...", "Clear Selection"}));

    // The rows: the color dot and name, the guides it holds, the eye; a group picked, the
    // right-click menu for it.
    panel->refresh();
    QTreeWidget* tree = panel->tree();
    ASSERT_GT(tree->topLevelItemCount(), 0);
    EXPECT_EQ(tree->columnCount(), 3);
    const std::string root = panel->currentGroup();
    ASSERT_FALSE(root.empty());
    EXPECT_EQ(panel->menuState(), (std::set<std::string>{"group"}));
    std::unique_ptr<QMenu> menu(panel->contextMenu(rowOf(tree, tree->currentItem())));
    ASSERT_TRUE(menu);
    EXPECT_EQ(shown(menu.get()), (std::vector<std::string>{"New Group...", "Rename", "Color...", "Select Guides", "Reset Segments", "-", "Delete"}));

    // A color of its own: one undo step; Default Color offered then; the display draws it.
    ASSERT_TRUE(panel->setGroupColor(root, "#FF8800"));
    EXPECT_EQ(doc->groupColorsText(), root + ":#ff8800");
    EXPECT_EQ(doc->undoText(), "Undo color of " + root);
    EXPECT_EQ(panel->menuState(), (std::set<std::string>{"group", "group.color"}));
    menu.reset(panel->contextMenu(rowOf(tree, tree->currentItem())));
    EXPECT_EQ(shown(menu.get()), (std::vector<std::string>{"New Group...", "Rename", "Color...", "Default Color", "Select Guides", "Reset Segments", "-", "Delete"}));
    EXPECT_FALSE(panel->setGroupColor(root, "orange")) << "#rrggbb only";
    EXPECT_EQ(doc->undo(), "");
    EXPECT_TRUE(doc->groupColors().empty());
    EXPECT_EQ(doc->redo(), "");
    EXPECT_EQ(doc->groupColors().at(root), "#ff8800");

    // Renamed: its color goes with it, one step (undo brings both back).
    ASSERT_TRUE(panel->renameGroup(root, "crown"));
    EXPECT_EQ(doc->groupColors().count("crown"), 1u);
    EXPECT_EQ(doc->groupColors().count(root), 0u);
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->groupColors().count(root), 1u);
    ASSERT_NE(doc->session().authoredHierarchy()->findGroup(root), kNoGroup);
    panel->refresh();

    // Edit Guides: children for the selected guides (one undo step), mirror and clear the selection.
    const HierarchyPtr h = doc->session().authoredHierarchy();
    const std::vector<GuideId> rootGuides = guidesAt(*h, -1, root);
    ASSERT_FALSE(rootGuides.empty());
    doc->setSelection({rootGuides.front()});
    const std::size_t before = h->guides().size();
    ASSERT_TRUE(panel->generateChildren(root, 3));
    EXPECT_EQ(doc->undoText(), "Undo generate children");
    EXPECT_GE(doc->session().authoredHierarchy()->guides().size(), before);
    {
        // Each swatch in the color its group draws in: the child group its own automatic hue (not
        // its parent's, whose own color is the orange), filled inside the dashed outline.
        const HierarchyPtr after = doc->session().authoredHierarchy();
        const GroupId rootId = after->findGroup(root);
        const GuideGroup* child = nullptr;
        for (const GuideGroup& g : after->groups()) {
            if (g.parent == rootId) child = &g;
        }
        ASSERT_TRUE(child);
        const auto automatic = autoGuideColors(*after, guideHueSeed(doc->guideColorSeed()));
        panel->refresh();
        auto swatchAt = [&](const std::string& name) {
            const QList<QTreeWidgetItem*> found = tree->findItems(QString::fromStdString(name), Qt::MatchExactly | Qt::MatchRecursive, 0);
            return found.isEmpty() ? QColor() : found.front()->icon(0).pixmap(QSize(12, 12)).toImage().pixelColor(6, 6);
        };
        const QColor want(QString::fromStdString(guideColorHex(automatic.at(child->id)))), got = swatchAt(child->name);
        EXPECT_NEAR(got.red(), want.red(), 3);
        EXPECT_NEAR(got.green(), want.green(), 3);
        EXPECT_NEAR(got.blue(), want.blue(), 3);
        EXPECT_NE(swatchAt(root).rgb(), got.rgb());
        EXPECT_NE(automatic.at(rootId), automatic.at(child->id));
    }
    EXPECT_EQ(doc->undo(), "");
    doc->setSelection(rootGuides);
    panel->mirrorSelection();
    for (const GuideId id : doc->selection()) EXPECT_TRUE(doc->session().authoredHierarchy()->guide(id));
    panel->clearSelection();
    EXPECT_TRUE(doc->selection().empty());
    EXPECT_FALSE(panel->reparentSelected(rootGuides.front())) << "nothing selected";

    // Promote / Demote: a child group up to the root, then down under the root again.
    ASSERT_TRUE(panel->addGroup(root, "fringe"));
    EXPECT_EQ(panel->currentGroup(), "fringe");
    EXPECT_EQ(panel->rootOf("fringe"), root);
    ASSERT_TRUE(panel->promoteGroup("fringe"));
    EXPECT_EQ(doc->session().authoredHierarchy()->groupDepth(doc->session().authoredHierarchy()->findGroup("fringe")), 0);
    EXPECT_EQ(panel->siblingsOf("fringe"), std::vector<std::string>{root});
    ASSERT_TRUE(panel->demoteGroup("fringe", root));
    EXPECT_EQ(doc->undoText(), "Undo demote group");
    EXPECT_EQ(doc->session().authoredHierarchy()->groupDepth(doc->session().authoredHierarchy()->findGroup("fringe")), 1);

    // The bindings: Delete asks over the viewport first (never a dialog); Mirror Group asks the side.
    panel->setCurrentGroup("fringe");
    bindings.at("group.delete").trigger(nullptr, false);
    ASSERT_TRUE(window.overlay()->promptShown());
    ASSERT_TRUE(window.overlay()->choosePrompt(QStringLiteral("Delete")));
    EXPECT_EQ(doc->session().authoredHierarchy()->findGroup("fringe"), kNoGroup);
    panel->setCurrentGroup(root);
    bindings.at("guides.mirrorGroup").trigger(nullptr, false);
    ASSERT_TRUE(window.overlay()->promptShown());
    ASSERT_TRUE(window.overlay()->choosePrompt(QStringLiteral("+X → -X")));
    EXPECT_EQ(doc->undoText(), "Undo mirror group");
    // New Group: its name typed, top-level (Add) or under the picked group.
    bindings.at("group.new").trigger(nullptr, false);
    ASSERT_TRUE(window.overlay()->promptShown());
    window.overlay()->setPromptLine(QStringLiteral("bangs"));
    ASSERT_TRUE(window.overlay()->choosePrompt(QStringLiteral("Under %1").arg(QString::fromStdString(root))));
    const HierarchyPtr after = doc->session().authoredHierarchy();
    ASSERT_NE(after->findGroup("bangs"), kNoGroup);
    EXPECT_EQ(after->group(after->findGroup("bangs"))->parent, after->findGroup(root));
    EXPECT_EQ(window.brush().settings().brush.group, "bangs") << "new guides go into it";

    // The tree is rebuilt only when what it shows changed.
    panel->refresh();
    const int rebuilds = panel->rebuilds();
    panel->invalidate();
    EXPECT_EQ(panel->currentGroup(), "bangs") << "asked for its group: caught up";
    EXPECT_EQ(panel->rebuilds(), rebuilds) << "nothing changed: no rebuild";
    doc->setGroupHidden("bangs", true);
    panel->refresh();
    EXPECT_EQ(panel->rebuilds(), rebuilds + 1);
}

TEST(AppLists, MapsBarMenuAndOperations) {
    const std::string path = sampleCopy("groomlab_app_lists_maps");
    MainWindow window;
    window.resize(1280, 800);
    window.show();  // (the offscreen platform: nothing on screen; the prompts show over the viewport)
    ASSERT_TRUE(window.openGroom(QString::fromStdString(path)));
    GroomDocument* doc = window.document();
    MapsPanel* panel = window.mapsPanel();
    const std::filesystem::path folder = std::filesystem::u8path(path).parent_path();

    EXPECT_EQ(panel->bar()->ids(), (std::vector<std::string>{"map.new", "map.import"}));
    EXPECT_EQ(panel->itemBar()->ids(), (std::vector<std::string>{"map.rename", "map.convert", "map.delete"}));
    QToolButton* convert = panel->itemBar()->button("map.convert");
    ASSERT_TRUE(convert && convert->menu());
    EXPECT_EQ(shown(convert->menu()).size(), 3u) << "the three kinds";

    // A map a setting reads: Read by says so; deleting it gives the setting its own value back.
    const std::string scatter = nodeOfType(*doc, "Scatter");
    ASSERT_FALSE(scatter.empty());
    std::string map;
    bool made = false;
    ASSERT_EQ(doc->paintParam(scatter, "density", &map, "vertices", &made), "");
    panel->refresh();
    panel->selectMap(map);
    EXPECT_EQ(doc->mapUsers(map), (std::vector<std::pair<std::string, std::string>>{{scatter, "density"}}));
    EXPECT_NE(panel->usersText().indexOf(QString::fromStdString(scatter + ".density")), -1) << panel->usersText().toStdString();
    EXPECT_EQ(panel->menuState(), (std::set<std::string>{"map", "map.scalp"}));
    std::unique_ptr<QMenu> menu(panel->contextMenu(panel->list()->visualItemRect(panel->list()->currentItem()).center()));
    EXPECT_EQ(shown(menu.get()), (std::vector<std::string>{"New Map...", "Import Image as Map...", "Rename...", "Convert to", "Make Color (r g b a)", "-",
                                                         "Relink Image...", "-", "Delete"}));
    ASSERT_EQ(panel->removeSelected(), "");
    EXPECT_FALSE(doc->map(map));
    EXPECT_FALSE(doc->paramOrDefault(scatter, "density").is_object()) << "unlinked: the node's own value again";
    EXPECT_EQ(doc->undo(), "");
    ASSERT_TRUE(doc->map(map));
    EXPECT_TRUE(doc->paramOrDefault(scatter, "density").is_object()) << "one step: the map and its link back";

    // Convert: guides and back, then a texture (its file undone with the step); channels; fill.
    ASSERT_EQ(doc->convertMap(map, "guides"), "");
    EXPECT_EQ(doc->map(map)->kind, "guides");
    EXPECT_EQ(doc->undoText(), "Undo convert map " + map);
    EXPECT_EQ(doc->convertMap(map, "vertices"), "");
    EXPECT_EQ(doc->map(map)->kind, "vertices");
    ASSERT_EQ(doc->convertMap(map, "texture", 64), "");
    ASSERT_EQ(doc->map(map)->kind, "texture");
    const std::string file = doc->textureMapFile(map);
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(file))) << file;
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->map(map)->kind, "vertices");
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(file))) << "the file it wrote went with the undo";
    EXPECT_EQ(doc->redo(), "");
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(file)));
    EXPECT_EQ(doc->undo(), "");
    ASSERT_EQ(doc->setMapChannels(map, kColorMap), "");
    EXPECT_EQ(doc->map(map)->channels, kColorMap);
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->map(map)->channels, kGrayMap);
    ASSERT_EQ(doc->fillMap(map, 0.25f), "");
    for (const float v : doc->session().scalpMaps().maps.at(map).values) ASSERT_FLOAT_EQ(v, 0.25f);

    // A texture map's file deleted with it (Delete Its File Too, on by default), back with undo.
    ASSERT_EQ(doc->createMap("detail", "texture", false, 0.5f, 64), "");
    const std::string detail = doc->textureMapFile("detail");
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::u8path(detail)));
    panel->refresh();
    panel->selectMap("detail");
    EXPECT_TRUE(panel->deleteFiles());
    EXPECT_EQ(panel->menuState(), (std::set<std::string>{"map", "map.scalp", "map.texture"}));
    menu.reset(panel->contextMenu(panel->list()->visualItemRect(panel->list()->currentItem()).center()));
    EXPECT_EQ(shown(menu.get()), (std::vector<std::string>{"New Map...", "Import Image as Map...", "Rename...", "Convert to", "Make Color (r g b a)", "-",
                                                         "Relink Image...", "Reload Image", "Show in Explorer", "-", "Delete", "Delete Its File Too"}));
    QAction* deleteFiles = action(menu.get(), "Delete Its File Too");
    ASSERT_TRUE(deleteFiles && deleteFiles->isCheckable());
    EXPECT_TRUE(deleteFiles->isChecked());
    ASSERT_EQ(panel->removeSelected(), "");
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(detail)));
    EXPECT_EQ(doc->undo(), "");
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(detail))) << "undo wrote it back";
    // Off: the file stays.
    deleteFiles->trigger();
    EXPECT_FALSE(panel->deleteFiles());
    panel->selectMap("detail");
    ASSERT_EQ(panel->removeSelected(), "");
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(detail)));
    EXPECT_EQ(doc->undo(), "");
    panel->setDeleteFiles(true);

    // Missing: the image gone, the row marked; Relink points it at another image.
    std::filesystem::remove(std::filesystem::u8path(detail));
    EXPECT_EQ(doc->missingMapFile("detail"), detail);
    panel->refresh();
    panel->selectMap("detail");
    EXPECT_EQ(panel->selectedMissing(), detail);
    EXPECT_TRUE(panel->list()->currentItem()->text().startsWith(QStringLiteral("⚠ ")));
    const std::string png = (folder / "mask.png").u8string();
    QImage gray(32, 32, QImage::Format_RGBA8888);
    gray.fill(QColor(128, 128, 128));
    ASSERT_TRUE(gray.save(QString::fromStdString(png)));
    ASSERT_EQ(panel->relinkSelected(QString::fromStdString(png)), "");
    EXPECT_EQ(doc->missingMapFile("detail"), "");
    EXPECT_EQ(panel->selectedMissing(), "");

    // Import: a PNG in place (gray as it is), a JPEG copied into the masks folder (the copy undone).
    ASSERT_EQ(panel->importImage(QString::fromStdString(png)), "");
    ASSERT_TRUE(doc->map("mask"));
    EXPECT_EQ(doc->map("mask")->kind, "texture");
    EXPECT_EQ(doc->map("mask")->channels, kGrayMap);
    EXPECT_EQ(doc->undoText(), "Undo import map mask");
    QImage color(32, 32, QImage::Format_RGB32);
    color.fill(QColor(200, 40, 40));
    const std::string jpg = (folder / "tint photo.jpg").u8string();
    ASSERT_TRUE(color.save(QString::fromStdString(jpg)));
    ASSERT_EQ(panel->importImage(QString::fromStdString(jpg)), "");
    ASSERT_TRUE(doc->map("tint_photo"));
    EXPECT_EQ(doc->map("tint_photo")->channels, kColorMap);
    const std::string copy = doc->textureMapFile("tint_photo");
    EXPECT_NE(copy.find("/masks/"), std::string::npos) << copy;
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(copy)));
    EXPECT_EQ(doc->undo(), "");
    EXPECT_FALSE(doc->map("tint_photo"));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(copy)));
    EXPECT_EQ(doc->reloadMap("mask"), "");
    EXPECT_NE(doc->reloadMap(map), "") << "not a texture map";
}

TEST(AppLists, CorrectivesStackBarMenuAndKeys) {
    MainWindow window;
    window.resize(1280, 800);
    window.show();  // (the offscreen platform: nothing on screen; the prompts show over the viewport)
    ASSERT_TRUE(window.openGroom(QString::fromStdString(sampleCopy("groomlab_app_lists_correctives"))));
    GroomDocument* doc = window.document();
    auto* panel = window.findChild<CorrectivesPanel*>();
    ASSERT_TRUE(panel);
    const ui::Bindings bindings = AppActions::bindings(window);
    EXPECT_EQ(panel->bar()->ids(), (std::vector<std::string>{"corrective.new", "corrective.fixFrame"}));
    EXPECT_EQ(panel->itemBar()->ids(), (std::vector<std::string>{"corrective.up", "corrective.down", "corrective.solo", "corrective.delete"}));
    // Two correctives: the stack top first, the base groom at the bottom.
    bindings.at("corrective.new").trigger(nullptr, false);
    doc->setFrame(10.0);
    bindings.at("corrective.fixFrame").trigger(nullptr, false);
    EXPECT_EQ(panel->layerNames(), (std::vector<std::string>{"layer", "shot10"}));
    QTreeWidget* tree = panel->tree();
    ASSERT_EQ(tree->topLevelItemCount(), 3);
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QStringLiteral("shot10"));
    EXPECT_EQ(tree->topLevelItem(2)->text(0), QStringLiteral("Base groom"));
    EXPECT_EQ(panel->currentLayer(), "shot10") << "a new corrective is what brushes sculpt into";
    EXPECT_EQ(panel->layerFrame("shot10"), std::optional<double>(10.0));
    EXPECT_FALSE(panel->layerFrame("layer")) << "every frame";
    // The right-click menu on a corrective; none on the base groom.
    std::unique_ptr<QMenu> menu(panel->contextMenu(rowOf(tree, tree->topLevelItem(1))));
    ASSERT_TRUE(menu);
    EXPECT_EQ(panel->currentLayer(), "layer");
    EXPECT_EQ(shown(menu.get()),
              (std::vector<std::string>{"Go to Its Frame", "Key Weight Here", "Clear Keys (every frame)", "Rename", "-", "Solo", "Delete"}));
    EXPECT_EQ(panel->contextMenu(rowOf(tree, tree->topLevelItem(2))), nullptr);
    // Up, solo (again: all on), each one undo step.
    bindings.at("corrective.up").trigger(nullptr, false);
    EXPECT_EQ(panel->layerNames(), (std::vector<std::string>{"shot10", "layer"}));
    EXPECT_EQ(doc->undoText(), "Undo move corrective stroke");
    ASSERT_TRUE(panel->soloLayer("layer"));
    Correctives c = Correctives::fromJson(doc->storedText(StrokeField::Correctives));
    EXPECT_FALSE(c.layer("shot10")->enabled);
    EXPECT_TRUE(c.layer("layer")->enabled);
    ASSERT_TRUE(panel->soloLayer("layer"));
    c = Correctives::fromJson(doc->storedText(StrokeField::Correctives));
    EXPECT_TRUE(c.layer("shot10")->enabled);
    // Keyed: a fix's fades become keys, the weight keyed here; Clear Keys: on every frame again.
    doc->setFrame(12.0);
    ASSERT_TRUE(panel->keyWeight("shot10"));
    c = Correctives::fromJson(doc->storedText(StrokeField::Correctives));
    EXPECT_GE(c.layer("shot10")->weightCurve.size(), 3u);
    EXPECT_EQ(tree->topLevelItem(1)->text(2).left(1), QStringLiteral("◆"));
    ASSERT_TRUE(panel->clearKeys("shot10"));
    c = Correctives::fromJson(doc->storedText(StrokeField::Correctives));
    EXPECT_TRUE(c.layer("shot10")->weightCurve.empty());
    EXPECT_TRUE(c.layer("shot10")->holdBefore && c.layer("shot10")->holdAfter);
    EXPECT_EQ(doc->undo(), "");
    // Go to Its Frame: the timeline to the corrective's frame.
    panel->setTargetLayer("shot10");
    bindings.at("corrective.goTo").trigger(nullptr, false);
    EXPECT_DOUBLE_EQ(doc->frame(), 10.0);
    bindings.at("corrective.delete").trigger(nullptr, false);
    EXPECT_EQ(panel->layerNames(), std::vector<std::string>{"layer"});
    // A frame moves the weights alone: no rebuild.
    const int rebuilds = panel->rebuilds();
    panel->frameChanged();
    EXPECT_EQ(panel->rebuilds(), rebuilds);
}

}  // namespace
}  // namespace groomlab::app
