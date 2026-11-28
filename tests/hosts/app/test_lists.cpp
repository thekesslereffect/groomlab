// SPDX-License-Identifier: Apache-2.0
// The Groom Tools' lists (engine/ui/lists.json, as Maya's tool_tabs.py): the guide groups, the
// maps and the correctives, their bars and right-click menus built from the definitions with the
// window's bindings, and the document's operations they run, each one undo step (files written or
// deleted on the way undone too). The offscreen platform; the prompts answered by their labels.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "app_actions.h"
#include "correctives_panel.h"
#include "groom_document.h"
#include "groomlab/correctives.h"
#include "groomlab/guide_selection.h"
#include "groom_tools.h"
#include "guides_panel.h"
#include "list_bar.h"
#include "main_window.h"
#include "maps_panel.h"
#include "overlay.h"
#include "brush_panel.h"
#include "card_parts.h"
#include "groomlab/display.h"
#include "tube_panel.h"
#include "viewport.h"

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
    // + New Group in the header (a + glyph); a group's segments, eye and menu on its row.
    EXPECT_EQ(panel->bar()->ids(), (std::vector<std::string>{"group.new"}));
    EXPECT_EQ(panel->bar()->button("group.new")->text(), QStringLiteral("+ New Group"));
    EXPECT_FALSE(panel->bar()->button("group.new")->icon().isNull()) << "a glyph in the header";
    EXPECT_EQ(shown(panel->editsMenu()),
              (std::vector<std::string>{"Children", "-", "Mirror", "-", "Reparent...", "Move to Group", "Copy Guides", "Paste Guides",
                                        "Clear Selection"}));

    // The rows: the color dot and name, the guides it holds, the eye; a group picked, the
    // right-click menu for it.
    panel->refresh();
    QTreeWidget* tree = panel->tree();
    ASSERT_GT(tree->topLevelItemCount(), 0);
    EXPECT_EQ(tree->columnCount(), 3);
    const std::string root = panel->currentGroup();
    ASSERT_FALSE(root.empty());
    EXPECT_EQ(panel->menuState(), (std::set<std::string>{"group"}));
    // Its row: one line, its icons in ROW_ORDER (the segments, the eye, the menu last).
    for (const char* icon : {"groupSegments_", "groupEye_", "groupMenu_"}) {
        EXPECT_TRUE(panel->findChild<QToolButton*>(QLatin1String(icon) + QString::fromStdString(root))) << icon;
    }
    std::unique_ptr<QMenu> menu(panel->contextMenu(rowOf(tree, tree->currentItem())));
    ASSERT_TRUE(menu);
    // The row's menu (lists.json's groups context): Rename first, the edits, Generate Children a
    // submenu of counts, Delete last (in the theme's red).
    auto contains = [](const std::vector<std::string>& items, const std::string& text) {
        return std::find(items.begin(), items.end(), text) != items.end();
    };
    std::vector<std::string> items = shown(menu.get());
    ASSERT_FALSE(items.empty());
    EXPECT_EQ(items.front(), "Rename");
    EXPECT_EQ(items.back(), "Delete");
    EXPECT_TRUE(menu->actions().back()->property("destructive").toBool());
    EXPECT_TRUE(contains(items, "Generate Children"));
    EXPECT_TRUE(contains(items, "Select Guides"));
    EXPECT_TRUE(contains(items, "Color..."));
    EXPECT_FALSE(contains(items, "Default Color"));
    QMenu* children = menu->findChild<QMenu*>(QStringLiteral("menu_generateChildren"));
    ASSERT_TRUE(children);
    EXPECT_EQ(shown(children), (std::vector<std::string>{"1 per guide", "2 per guide", "3 per guide", "4 per guide", "6 per guide", "8 per guide"}));

    // A color of its own: one undo step; Default Color offered then; the display draws it.
    ASSERT_TRUE(panel->setGroupColor(root, "#FF8800"));
    EXPECT_EQ(doc->groupColorsText(), root + ":#ff8800");
    EXPECT_EQ(doc->undoText(), "Undo color of " + root);
    EXPECT_EQ(panel->menuState(), (std::set<std::string>{"group", "group.color"}));
    menu.reset(panel->contextMenu(rowOf(tree, tree->currentItem())));
    items = shown(menu.get());
    EXPECT_TRUE(contains(items, "Default Color"));
    EXPECT_EQ(items.back(), "Delete");
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

TEST(AppLists, ATubeLevelKeptInViewWhileAnotherLevelIsCombed) {
    // Maya's gui_roots tube_level_pin: a tube hierarchy's level kept in view while another level's
    // guides are combed. The tubes' Level (the guides display), a level row's tube in Guide groups
    // and the Roots card's Lock view are one setting, the document's tubeViewGroup; picking a level
    // shows its guides alone, the pin stays; the Tubes switch is the one way the tubes show with
    // every tool, drawn with their look.
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(sampleCopy("groomlab_app_lists_tube_pin"))));
    GroomDocument* doc = window.document();
    auto* tools = window.findChild<GroomTools*>();
    auto* panel = window.findChild<GuidesPanel*>();
    ASSERT_TRUE(tools && panel);
    TubePanel* tubes = tools->tubePanel();
    tubes->refreshRoots();
    tubes->addLevel();
    ASSERT_EQ(tubes->levels().size(), 2u);
    const std::string base = tubes->levels()[0], deeper = tubes->levels()[1];
    auto tubeOf = [&](const std::string& group) {
        return panel->findChild<QToolButton*>(QStringLiteral("groupTube_") + QString::fromStdString(group));
    };
    tools->tile("comb")->click();
    EXPECT_FALSE(window.displaySettings().toolTubes) << "Comb: no tube tool up";
    EXPECT_TRUE(window.displaySettings().tubes) << "the tubes shown at first";
    {
        auto* pill = tools->findChild<QToolButton*>(QStringLiteral("display_tubes"));
        ASSERT_TRUE(pill);
        pill->setChecked(false);  // (off, so the pin is seen to turn them on)
        QCoreApplication::processEvents();
    }
    EXPECT_FALSE(window.displaySettings().tubes);
    panel->refresh();
    ASSERT_TRUE(tubeOf(deeper)) << "a level row has its tube";
    EXPECT_TRUE(tubeOf(base));
    EXPECT_FALSE(tubeOf(deeper)->property("kept").toBool());
    tubeOf(deeper)->click();
    QCoreApplication::processEvents();
    EXPECT_EQ(doc->tubeViewGroup(), deeper) << "level 2's tubes kept in view";
    EXPECT_EQ(window.displaySettings().tubeViewGroup, deeper);
    EXPECT_TRUE(window.displaySettings().tubes) << "pinned with Comb up: the Tubes switch on";
    auto* tubesPill = tools->findChild<QToolButton*>(QStringLiteral("display_tubes"));
    ASSERT_TRUE(tubesPill) << "the guides display's Tubes pill";
    EXPECT_TRUE(tubesPill->isChecked());
    auto* lock = tubes->findChild<QCheckBox*>(QStringLiteral("tubeLockView"));
    ASSERT_TRUE(lock);
    EXPECT_TRUE(lock->isChecked()) << "so does the Roots card's Lock view";
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);  // (the rows made again)
    ASSERT_TRUE(tubeOf(deeper));
    EXPECT_TRUE(tubeOf(deeper)->property("kept").toBool()) << "its row's tube in the accent";
    // Every row keeps room for a level's tube (the eyes in a column): the same strip width.
    {
        QTreeWidget* tree = panel->tree();
        std::set<int> widths;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            if (QWidget* strip = tree->itemWidget(*it, 2)) widths.insert(strip->sizeHint().width());
        }
        EXPECT_EQ(widths.size(), 1u) << "rows with and without a tube line up";
    }
    // Picking level 1 in the list: its guides alone through the level view, the eyes untouched,
    // the pin stays.
    emit panel->groupClicked(base);
    EXPECT_EQ(doc->mixedView(), base + ":1");
    EXPECT_EQ(window.displaySettings().mixedView, base + ":1") << "and drawn so";
    EXPECT_TRUE(doc->hiddenGroups().empty()) << "no eye flipped";
    EXPECT_EQ(doc->tubeViewGroup(), deeper) << "the pin stays while level 1 is combed";
    // A level row's eye hides that group, the level view or not (the eye always wins).
    auto guidePoints = [&] {
        const DisplayFrame& frame = doc->updateDisplay(window.displaySettings());
        const auto& lines = frame.item(DisplayItemId::Guides).geometry;
        return lines ? lines->positions.size() : std::size_t(0);
    };
    const std::size_t shownPoints = guidePoints();
    EXPECT_GT(shownPoints, 0u);
    QToolButton* eye = panel->findChild<QToolButton*>(QStringLiteral("groupEye_") + QString::fromStdString(base));
    ASSERT_TRUE(eye);
    eye->click();
    QCoreApplication::processEvents();
    EXPECT_TRUE(doc->hiddenGroups().count(base)) << "the eye hides level 1";
    EXPECT_NE(window.displaySettings().hiddenGuideGroups.find(base), std::string::npos);
    EXPECT_LT(guidePoints(), shownPoints) << "its guides gone from the viewport under the level view";
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    eye = panel->findChild<QToolButton*>(QStringLiteral("groupEye_") + QString::fromStdString(base));
    ASSERT_TRUE(eye);
    eye->click();
    QCoreApplication::processEvents();
    EXPECT_FALSE(doc->hiddenGroups().count(base));
    EXPECT_EQ(guidePoints(), shownPoints);
    EXPECT_TRUE(tools->card()->editing().contains(QStringLiteral("tubes: Level 2"))) << tools->card()->editing().toStdString();
    // The Tube tool shows them either way; the look passed whichever tool is up.
    tools->tile("shape")->click();
    EXPECT_TRUE(window.displaySettings().toolTubes);
    tools->tile("comb")->click();
    EXPECT_FALSE(window.displaySettings().toolTubes);
    EXPECT_TRUE(window.displaySettings().tubeWires) << "Wireframe: the look, whatever tool";
    // The Tubes pill off: the Tubes switch (Properties > Display > Tubes).
    tubesPill->click();
    EXPECT_FALSE(window.displaySettings().tubes);
    // The kept level's row tube again: no pin, no Lock view.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    ASSERT_TRUE(tubeOf(deeper));
    tubeOf(deeper)->click();
    QCoreApplication::processEvents();
    EXPECT_EQ(doc->tubeViewGroup(), "");
    EXPECT_EQ(window.displaySettings().tubeViewGroup, "");
    EXPECT_FALSE(lock->isChecked());
    EXPECT_FALSE(tools->card()->editing().contains(QStringLiteral("tubes:")));
    // Lock view (the Roots card's ⋯): the same setting.
    lock->setChecked(true);
    ASSERT_GT(tubes->shownLevel(), 0);
    EXPECT_EQ(doc->tubeViewGroup(), tubes->levels()[static_cast<std::size_t>(tubes->shownLevel() - 1)]);
    lock->setChecked(false);
    EXPECT_EQ(doc->tubeViewGroup(), "");
    // A group of no tube hierarchy picked: no level view, every group as its eye says.
    ASSERT_TRUE(panel->addGroup({}, "loose"));
    emit panel->groupClicked("loose");
    EXPECT_EQ(doc->mixedView(), "");
}

TEST(AppLists, TheGuidesDisplayIsOneRowOfWhatShowsThenTheLook) {
    // Maya's guides display: one pill row of what shows (Guides, Parts, Flow, Tubes, Instances),
    // then the guides' look (Width, Points and Colors as segments, Smoothed), then the tubes' look
    // (Shaded, Wireframe, Colors, Level colors); no Level menu, no Show tubes row.
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(sampleCopy("groomlab_app_lists_guide_display"))));
    auto* tools = window.findChild<GroomTools*>();
    ASSERT_TRUE(tools);
    PopupPanel* popup = tools->guideDisplayPopup();
    ASSERT_TRUE(popup);
    QWidget* shown = popup->findChild<QWidget*>(QStringLiteral("guideDisplayChips"));
    ASSERT_TRUE(shown);
    std::vector<std::string> pills;
    for (QToolButton* b : shown->findChildren<QToolButton*>()) pills.push_back(b->text().toStdString());
    EXPECT_EQ(pills, (std::vector<std::string>{"Guides", "Parts", "Flow", "Tubes", "Instances"}));
    EXPECT_EQ(popup->findChild<QComboBox*>(QStringLiteral("tubeViewLevel")), nullptr);
    EXPECT_EQ(popup->findChild<QCheckBox*>(QStringLiteral("displayTubes")), nullptr);
    // Guides: the header's guides eye's setting.
    auto* guides = popup->findChild<QToolButton*>(QStringLiteral("display_guides"));
    ASSERT_TRUE(guides);
    guides->click();
    EXPECT_FALSE(window.displaySettings().guides);
    EXPECT_FALSE(tools->guidesVisible()) << "the header's eye follows";
    guides->click();
    EXPECT_TRUE(window.displaySettings().guides);
    // Parts: the part lines (DisplaySettings::parts).
    auto* parts = popup->findChild<QToolButton*>(QStringLiteral("display_parts"));
    ASSERT_TRUE(parts);
    EXPECT_TRUE(parts->isChecked());
    parts->click();
    EXPECT_FALSE(window.displaySettings().parts);
    parts->click();
    EXPECT_TRUE(window.displaySettings().parts);
    // Points and Colors: segments.
    auto* ends = popup->findChild<QToolButton*>(QStringLiteral("display_guidePoints_1"));
    ASSERT_TRUE(ends);
    EXPECT_EQ(ends->text(), QStringLiteral("Ends"));
    ends->click();
    EXPECT_EQ(window.displaySettings().guidePoints, 1);
    auto* layer = popup->findChild<QToolButton*>(QStringLiteral("display_guideColorMode_1"));
    ASSERT_TRUE(layer);
    EXPECT_EQ(layer->text(), QStringLiteral("Layer"));
    auto* group = popup->findChild<QToolButton*>(QStringLiteral("display_guideColorMode_0"));
    ASSERT_TRUE(group);
    EXPECT_TRUE(group->isChecked());
    // Smoothed: on draws smooth curves (4 pieces a segment), off straight (1).
    auto* smoothed = popup->findChild<QToolButton*>(QStringLiteral("display_guideSmoothing"));
    ASSERT_TRUE(smoothed);
    const bool wasSmooth = smoothed->isChecked();
    smoothed->click();
    smoothed->click();
    EXPECT_EQ(smoothed->isChecked(), wasSmooth);
    if (smoothed->isChecked()) smoothed->click();
    EXPECT_EQ(window.displaySettings().guideSmoothing, 1) << "off: straight";
    smoothed->click();
    EXPECT_GE(window.displaySettings().guideSmoothing, 4) << "on: smooth";
    // On top: the guides, parts and flow over the hair, or depth tested (the renderer's).
    auto* onTop = popup->findChild<QToolButton*>(QStringLiteral("display_overlaysOnTop"));
    ASSERT_TRUE(onTop) << "the guides' On top";
    Viewport* view = window.findChild<Viewport*>();
    ASSERT_TRUE(view);
    EXPECT_TRUE(onTop->isChecked());
    EXPECT_TRUE(view->renderer().overlaysOnTop());
    onTop->click();
    EXPECT_FALSE(view->renderer().overlaysOnTop()) << "off: depth tested";
    onTop->click();
    EXPECT_TRUE(view->renderer().overlaysOnTop());
    // The tubes' look under it.
    for (const char* look : {"tubeShadedPill", "tubeWireframePill", "tubeColorsPill", "tubeLevelColorsPill"}) {
        EXPECT_TRUE(popup->findChild<QToolButton*>(QLatin1String(look))) << look;
    }
    // Sized to its rows at its width (the wrapped pill rows leave no gaps).
    popup->showUnder(tools);
    QVBoxLayout* rows = popup->rows();
    ASSERT_TRUE(rows->hasHeightForWidth());
    EXPECT_EQ(popup->height(), rows->totalHeightForWidth(popup->width()));
    if (const char* grab = std::getenv("GROOMLAB_APP_GRAB")) popup->grab().save(QString::fromUtf8(grab) + QStringLiteral("/guide_display.png"));
    popup->hide();
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
    // One line per map, its menu on the row; the picked map's info (Read by, Use for, Show on)
    // under the list, no ⓘ to open (issue #138).
    EXPECT_FALSE(panel->findChild<QToolButton*>(QStringLiteral("mapInfo_") + QString::fromStdString(map)));
    EXPECT_TRUE(panel->infoBlock()->isVisibleTo(panel));
    EXPECT_TRUE(panel->findChild<QToolButton*>(QStringLiteral("mapMenu_") + QString::fromStdString(map)));
    std::unique_ptr<QMenu> menu(panel->contextMenu(panel->list()->visualItemRect(panel->list()->currentItem()).center()));
    EXPECT_EQ(shown(menu.get()), (std::vector<std::string>{"Rename...", "Convert to", "Make Color (r g b a)", "-", "Relink Image...", "-", "Delete"}));
    QMenu* convert = menu->findChild<QMenu*>(QStringLiteral("menu_map.convert"));
    ASSERT_TRUE(convert);
    EXPECT_EQ(shown(convert).size(), 3u) << "the three kinds";
    // Use for: the settings the graph has a node for (Scatter's density reads it already: greyed).
    {
        QMenu useFor;
        panel->fillUseFor(&useFor);
        bool density = false;
        for (QAction* a : useFor.actions()) {
            if (a->text().contains(QString::fromStdString(scatter + ".density"))) {
                density = true;
                EXPECT_FALSE(a->isEnabled()) << "it reads the map already";
            }
        }
        EXPECT_TRUE(density);
    }
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
    EXPECT_EQ(shown(menu.get()), (std::vector<std::string>{"Rename...", "Convert to", "Make Color (r g b a)", "-", "Relink Image...", "Reload Image",
                                                         "Show in Explorer", "-", "Delete", "Delete Its File Too"}));
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
    auto* lostRow = qobject_cast<ListRow*>(panel->list()->itemWidget(panel->list()->currentItem()));
    ASSERT_TRUE(lostRow) << "one line per map: its row";
    EXPECT_TRUE(lostRow->label()->text().startsWith(QStringLiteral("⚠ ")));
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
    EXPECT_EQ(panel->bar()->ids(), (std::vector<std::string>{"corrective.fixFrame", "corrective.new"}));
    // Two correctives: the stack top first, the base groom at the bottom.
    bindings.at("corrective.new").trigger(nullptr, false);
    doc->setFrame(10.0);
    bindings.at("corrective.fixFrame").trigger(nullptr, false);
    EXPECT_EQ(panel->layerNames(), (std::vector<std::string>{"sculpt1", "shot10"}));
    QTreeWidget* tree = panel->tree();
    ASSERT_EQ(tree->topLevelItemCount(), 3);
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QStringLiteral("shot10"));
    EXPECT_EQ(tree->topLevelItem(2)->text(0), QStringLiteral("Base groom"));
    EXPECT_EQ(panel->currentLayer(), "shot10") << "a new corrective is what brushes sculpt into";
    EXPECT_EQ(panel->layerFrame("shot10"), std::optional<double>(10.0));
    EXPECT_FALSE(panel->layerFrame("sculpt1")) << "every frame";
    // The right-click menu on a corrective; none on the base groom.
    std::unique_ptr<QMenu> menu(panel->contextMenu(rowOf(tree, tree->topLevelItem(1))));
    ASSERT_TRUE(menu);
    EXPECT_EQ(panel->currentLayer(), "sculpt1");
    EXPECT_EQ(shown(menu.get()), (std::vector<std::string>{"Rename", "-", "Move Up", "Move Down", "-", "Go to Its Frame", "Key Weight Here",
                                                         "Clear Keys (every frame)", "-", "Delete"}));
    // One line per corrective: its weight (a popup), Solo, the eye, the menu.
    for (const char* icon : {"correctiveWeight_sculpt1", "correctiveSolo_sculpt1", "correctiveEye_sculpt1", "correctiveMenu_sculpt1"}) {
        EXPECT_TRUE(panel->findChild<QToolButton*>(QLatin1String(icon))) << icon;
    }
    EXPECT_EQ(panel->contextMenu(rowOf(tree, tree->topLevelItem(2))), nullptr);
    // Up, solo (again: all on), each one undo step.
    bindings.at("corrective.up").trigger(nullptr, false);
    EXPECT_EQ(panel->layerNames(), (std::vector<std::string>{"shot10", "sculpt1"}));
    EXPECT_EQ(doc->undoText(), "Undo move corrective stroke");
    ASSERT_TRUE(panel->soloLayer("sculpt1"));
    Correctives c = Correctives::fromJson(doc->storedText(StrokeField::Correctives));
    EXPECT_FALSE(c.layer("shot10")->enabled);
    EXPECT_TRUE(c.layer("sculpt1")->enabled);
    ASSERT_TRUE(panel->soloLayer("sculpt1"));
    c = Correctives::fromJson(doc->storedText(StrokeField::Correctives));
    EXPECT_TRUE(c.layer("shot10")->enabled);
    // Keyed: a fix's fades become keys, the weight keyed here; Clear Keys: on every frame again.
    doc->setFrame(12.0);
    ASSERT_TRUE(panel->keyWeight("shot10"));
    c = Correctives::fromJson(doc->storedText(StrokeField::Correctives));
    EXPECT_GE(c.layer("shot10")->weightCurve.size(), 3u);
    EXPECT_EQ(tree->topLevelItem(1)->text(1).left(1), QStringLiteral("◆"));
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
    EXPECT_EQ(panel->layerNames(), std::vector<std::string>{"sculpt1"});
    // A frame moves the weights alone: no rebuild.
    const int rebuilds = panel->rebuilds();
    panel->frameChanged();
    EXPECT_EQ(panel->rebuilds(), rebuilds);
}

// Painting a scalp map shows it on the scalp (Maya's paint_map); another Groom Tools tab is done
// painting: the camera again, the map it showed off the scalp (Maya's end_painting, Blender's).
TEST(AppLists, LeavingThePaintTabEndsPainting) {
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(sampleCopy("groomlab_app_lists_paint"))));
    GroomDocument* doc = window.document();
    auto* maps = window.findChild<MapsPanel*>();
    auto* tools = window.findChild<GroomTools*>();
    auto* viewport = window.findChild<Viewport*>();
    ASSERT_TRUE(maps && tools && viewport);
    ASSERT_EQ(doc->createMap("tex", "texture", false, 0.5f, 64), "");
    maps->refresh();
    const int modeBefore = window.displaySettings().colorMode;
    // The Paint tab is painting: entering it starts Paint on the picked map.
    ASSERT_TRUE(tools->showTab(QStringLiteral("paint")));
    EXPECT_EQ(viewport->toolMode(), Viewport::ToolMode::Brush);
    EXPECT_EQ(window.brush().toolId(), "paint");
    maps->selectMap("tex");
    ASSERT_TRUE(maps->paintSelected());
    EXPECT_EQ(viewport->toolMode(), Viewport::ToolMode::Brush);
    EXPECT_EQ(doc->scalpMapDisplay(), "tex");
    // The strands colored by the map, 0 to 1 (Maya's view_map).
    EXPECT_EQ(window.displaySettings().colorMode, kAttributeColors);
    EXPECT_EQ(window.displaySettings().colorSource, "tex");
    EXPECT_FLOAT_EQ(window.displaySettings().colorMin, 0.0f);
    EXPECT_FLOAT_EQ(window.displaySettings().colorMax, 1.0f);
    ASSERT_TRUE(tools->showTab(QStringLiteral("tools")));
    EXPECT_EQ(viewport->toolMode(), Viewport::ToolMode::Camera) << "another tab: done painting";
    EXPECT_EQ(doc->scalpMapDisplay(), "") << "the map painting showed comes off the scalp";
    EXPECT_EQ(window.displaySettings().colorMode, modeBefore) << "the strands as they were";
    // A guide layer painted: the guides show it too (Guide Color: Painted Layer), then as they were.
    ASSERT_EQ(doc->createMap("layer", "guides", false, 0.5f), "");
    maps->refresh();
    ASSERT_TRUE(tools->showTab(QStringLiteral("paint")));
    maps->selectMap("layer");
    ASSERT_TRUE(maps->paintSelected());
    EXPECT_EQ(window.displaySettings().guideColorMode, 1);
    EXPECT_EQ(window.displaySettings().guideColorLayer, "layer");
    ASSERT_TRUE(tools->showTab(QStringLiteral("tools")));
    EXPECT_EQ(window.displaySettings().guideColorMode, 0);
    // A map shown on purpose (the maps panel's eye) stays when painting ends.
    ASSERT_TRUE(tools->showTab(QStringLiteral("paint")));
    maps->selectMap("tex");
    maps->showSelected(true);
    ASSERT_TRUE(maps->paintSelected());
    ASSERT_TRUE(tools->showTab(QStringLiteral("tools")));
    EXPECT_EQ(viewport->toolMode(), Viewport::ToolMode::Camera);
    EXPECT_EQ(doc->scalpMapDisplay(), "tex");
}

}  // namespace
}  // namespace groomlab::app
