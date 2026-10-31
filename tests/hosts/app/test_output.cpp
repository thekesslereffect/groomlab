// SPDX-License-Identifier: Apache-2.0
// The GroomLab menu's output and housekeeping in the app (app_output_actions.cpp,
// main_window_output.cpp, groom_document_output.cpp): Add Sample Mesh, the Add menu's nodes before
// the output, Simulate > Add Simulate Node, Import > Alembic Curves, the Utilities (To Mesh,
// Bake Scale Maps, Find Missing Files), Export > Scales Instancer (USD), the Strand Count HUD, the
// themes switched live with the user's themes folder, File > Open Recent and autosave. The
// offscreen platform; the glass prompts answered by their labels, no dialog ever opens.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QTabBar>
#include <QDockWidget>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "app_actions.h"
#include "app_shell.h"
#include "groom_document.h"
#include "look.h"
#include "main_window.h"
#include "mesh_io.h"
#include "overlay.h"
#include "param_panel.h"
#include "theme.h"

namespace groomlab::app {

QString testSettingsFolder();  // test_app.cpp: the suite's settings folder

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

const std::string kSampleGroom = std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom";

// A fresh folder under the temp folder with a copy of the sample groom in it.
std::string sampleCopy(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path copy = dir / "sample.groom";
    fs::copy_file(fs::u8path(kSampleGroom), copy, fs::copy_options::overwrite_existing);
    return copy.u8string();
}

std::string nodeOfType(const GroomDocument& doc, const std::string& type) {
    for (const GroomDocument::NodeInfo& n : doc.nodes()) {
        if (n.type == type) return n.name;
    }
    return {};
}

std::size_t countOfType(const GroomDocument& doc, const std::string& type) {
    std::size_t n = 0;
    for (const GroomDocument::NodeInfo& node : doc.nodes()) n += node.type == type;
    return n;
}

bool linked(const GroomDocument& doc, const std::string& from, const std::string& to, const std::string& input) {
    for (const GroomDocument::ConnectionInfo& c : doc.connections()) {
        if (c.from == from && c.to == to && c.input == input) return true;
    }
    return false;
}

// What feeds the output's strands, down the chain: the node types in order (the output first).
std::vector<std::string> chainToOutput(const GroomDocument& doc) {
    std::vector<std::string> types;
    std::string at = doc.graph().value("output", std::string());
    for (int i = 0; i < 32 && !at.empty(); ++i) {
        std::string next;
        for (const GroomDocument::ConnectionInfo& c : doc.connections()) {
            if (c.to == at && (c.input == "strands" || c.input == "a")) next = c.from;
        }
        if (next.empty()) break;
        for (const GroomDocument::NodeInfo& n : doc.nodes()) {
            if (n.name == next) types.push_back(n.type);
        }
        at = next;
    }
    return types;
}

std::string readText(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> shown(const QMenu* menu) {
    std::vector<std::string> out;
    for (const QAction* a : menu->actions()) {
        if (!a->isVisible()) continue;
        out.push_back(a->isSeparator() ? "-" : a->text().section(QLatin1Char('\t'), 0, 0).toStdString());
    }
    return out;
}

// The test's own settings file (ctest runs the tests side by side, each a process sharing the
// suite's ini otherwise: another test's opened groom would land in this one's recent files).
struct OwnSettings {
    explicit OwnSettings(const std::string& name) {
        const fs::path dir = fs::temp_directory_path() / name;
        fs::remove_all(dir);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString(dir.u8string()));
    }
    ~OwnSettings() { QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, testSettingsFolder()); }
};

QMenu* submenu(const QMenu* menu, const QString& title) {
    for (QAction* a : menu->actions()) {
        if (a->menu() && a->text() == title) return a->menu();
    }
    return nullptr;
}

TEST(AppOutput, SampleMeshesStartAGroom) {
    MainWindow window;
    for (const QString& name : MainWindow::sampleMeshNames()) EXPECT_FALSE(MainWindow::sampleMeshFile(name).isEmpty()) << name.toStdString();
    const ui::Bindings bindings = AppActions::bindings(window);
    ASSERT_TRUE(bindings.count("sample.manny"));
    EXPECT_TRUE(bindings.at("sample.manny").enabled());
    bindings.at("sample.manny").trigger(nullptr, false);
    ASSERT_TRUE(window.document());
    EXPECT_EQ(fs::u8path(window.document()->file().source).filename().u8string(), "manny_head.obj");
    // The wolf grows on its body alone; its eyes, nails and mouth a prop beside it.
    ASSERT_TRUE(window.addSampleMesh(QStringLiteral("wolf")));
    const auto whole = readObjMesh(MainWindow::sampleMeshFile(QStringLiteral("wolf")).toStdString());
    const auto body = readObjMesh(MainWindow::sampleMeshFile(QStringLiteral("wolf")).toStdString(), "wolf_body");
    ASSERT_TRUE(window.document()->file().data.scalp);
    EXPECT_EQ(window.document()->file().data.scalp->triangleCount(), body->triangleCount());
    EXPECT_LT(body->triangleCount(), whole->triangleCount());
    EXPECT_LT(body->pointCount(), whole->pointCount()) << "only the points the body's faces use";
    SurfacePtr parts;
    for (const SurfacePtr& p : window.props()) {
        if (p->name == "wolf_parts") parts = p;
    }
    ASSERT_TRUE(parts);
    EXPECT_EQ(parts->mesh->triangleCount() + body->triangleCount(), whole->triangleCount());
    // Again: the parts replaced, not doubled.
    ASSERT_TRUE(window.addSampleMesh(QStringLiteral("wolf")));
    EXPECT_EQ(std::count_if(window.props().begin(), window.props().end(), [](const SurfacePtr& p) { return p->name == "wolf_parts"; }), 1);
    EXPECT_FALSE(window.addSampleMesh(QStringLiteral("buddy"))) << "Buddy is a rig: Maya's and Blender's";
    EXPECT_THROW(readObjMesh(MainWindow::sampleMeshFile(QStringLiteral("wolf")).toStdString(), "no_such_group"), MeshFileError);
}

TEST(AppOutput, AddMenuNodesGoBeforeTheOutput) {
    MainWindow window;
    ASSERT_TRUE(window.openGroom(QString::fromStdString(sampleCopy("groomlab_app_output_add"))));
    GroomDocument* doc = window.document();
    const ui::Bindings bindings = AppActions::bindings(window);
    const std::vector<std::string> chain = chainToOutput(*doc);

    // Wind: just before the output (the chain one longer), one undo step.
    bindings.at("add.wind").trigger(nullptr, false);
    const std::string wind = nodeOfType(*doc, "Wind");
    ASSERT_FALSE(wind.empty()) << window.lastReport().toStdString();
    EXPECT_EQ(chainToOutput(*doc).front(), "Wind");
    EXPECT_EQ(chainToOutput(*doc).size(), chain.size() + 1);
    EXPECT_EQ(doc->param(wind, "strength"), json(0.3));
    EXPECT_EQ(doc->undoText(), "Undo add Wind");
    EXPECT_TRUE(doc->nodeErrors().empty());
    EXPECT_EQ(doc->undo(), "");
    EXPECT_TRUE(nodeOfType(*doc, "Wind").empty());
    EXPECT_EQ(chainToOutput(*doc), chain);

    // Jiggle the same; a Cards node ending the chain stays last (Maya's motion_anchor).
    std::string cards;
    ASSERT_EQ(doc->addCardsNode(&cards), "");
    bindings.at("add.jiggle").trigger(nullptr, false);
    ASSERT_FALSE(nodeOfType(*doc, "Jiggle").empty());
    const std::vector<std::string> withCards = chainToOutput(*doc);
    ASSERT_GE(withCards.size(), 2u);
    EXPECT_EQ(withCards[0], "Cards");
    EXPECT_EQ(withCards[1], "Jiggle");

    // Simulate: between the guides and the Interpolate they feed.
    bindings.at("sim.add").trigger(nullptr, false);
    const std::string sim = nodeOfType(*doc, "Simulate");
    ASSERT_FALSE(sim.empty()) << window.lastReport().toStdString();
    const std::string guides = nodeOfType(*doc, "Guides");
    EXPECT_TRUE(linked(*doc, guides, sim, "guides"));
    bool feedsInterpolate = false;
    for (const GroomDocument::ConnectionInfo& c : doc->connections()) {
        if (c.from == sim) {
            for (const GroomDocument::NodeInfo& n : doc->nodes()) feedsInterpolate = feedsInterpolate || (n.name == c.to && n.type == "Interpolate");
        }
    }
    EXPECT_TRUE(feedsInterpolate);
    EXPECT_FALSE(doc->simulateNodes().empty());

    // Collider: a prop mesh keeps the strands off it (a ColliderMesh feeding a Collide before the output).
    const fs::path collar = fs::path(doc->path()).parent_path() / "collar.obj";
    {
        std::ofstream obj(collar);
        obj << "o collar\nv -5 -20 -5\nv 5 -20 -5\nv 5 -20 5\nv -5 -20 5\nv 0 -15 0\nf 1 2 5\nf 2 3 5\nf 3 4 5\nf 4 1 5\nf 1 4 3 2\n";
    }
    bindings.at("add.collider").trigger(nullptr, false);
    EXPECT_TRUE(nodeOfType(*doc, "ColliderMesh").empty()) << "no prop yet: nothing added";
    ASSERT_TRUE(window.addProp(QString::fromStdString(collar.u8string()), QString(), 0));
    bindings.at("add.collider").trigger(nullptr, false);
    const std::string collider = nodeOfType(*doc, "ColliderMesh"), collide = nodeOfType(*doc, "Collide");
    ASSERT_FALSE(collider.empty()) << window.lastReport().toStdString();
    EXPECT_EQ(doc->param(collider, "name"), json("collar"));
    EXPECT_TRUE(linked(*doc, collider, collide, "collider"));
    EXPECT_EQ(chainToOutput(*doc).front(), "Collide") << "just before the output, as Maya's add_collider";
    EXPECT_TRUE(window.addCollider(QStringLiteral("collar")));
    EXPECT_EQ(countOfType(*doc, "ColliderMesh"), 1u) << "the same mesh again: the graph left alone";
}

TEST(AppOutput, FeathersAndBraidsAskInThePrompt) {
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(sampleCopy("groomlab_app_output_feathers"))));
    GroomDocument* doc = window.document();
    const ui::Bindings bindings = AppActions::bindings(window);
    const QStringList designs = MainWindow::featherDesigns();
    ASSERT_TRUE(designs.contains(QStringLiteral("contour")));
    ASSERT_TRUE(designs.contains(QStringLiteral("down")));

    // An assortment of scattered feathers: two designs through a Split, merged with the hair. The
    // prompt's words: "scattered feathers" and "one feather per guide", never shafts.
    bindings.at("add.feathers").trigger(nullptr, false);
    ASSERT_TRUE(window.overlay()->promptShown());
    EXPECT_EQ(window.overlay()->promptLine(), QStringLiteral("contour"));
    {
        QString shown;
        for (const QLabel* l : window.overlay()->prompt()->findChildren<QLabel*>()) shown += l->text() + QLatin1Char('\n');
        EXPECT_TRUE(shown.contains(QStringLiteral("Scattered feathers (shaped by the guides)"))) << shown.toStdString();
        EXPECT_TRUE(shown.contains(QStringLiteral("One feather per guide (hand-placed)"))) << shown.toStdString();
        EXPECT_FALSE(shown.contains(QStringLiteral("shaft"), Qt::CaseInsensitive)) << shown.toStdString();
    }
    window.overlay()->setPromptLine(QStringLiteral("contour 3, down 1"));
    ASSERT_TRUE(window.overlay()->choosePrompt(QStringLiteral("Scattered feathers")));
    EXPECT_EQ(countOfType(*doc, "Compound"), 2u) << window.lastReport().toStdString();
    const std::string split = nodeOfType(*doc, "Split");
    ASSERT_FALSE(split.empty());
    EXPECT_EQ(doc->param(split, "weightA"), json(3.0));
    EXPECT_EQ(doc->param(split, "weightB"), json(1.0));
    EXPECT_EQ(chainToOutput(*doc).front(), "Merge") << "the feathers merged with the hair before the output";
    EXPECT_EQ(doc->undoText(), "Undo add feathers");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(countOfType(*doc, "Compound"), 0u);

    // On the feathers group's guides: the group made, the hair's Guides leaving it out.
    const std::string hair = nodeOfType(*doc, "Guides");
    ASSERT_TRUE(window.addFeathers({QStringLiteral("contour")}, true));
    ASSERT_NE(doc->session().authoredHierarchy()->findGroup("feathers"), kNoGroup);
    EXPECT_NE(doc->param(hair, "exclude").get<std::string>().find("feathers"), std::string::npos);
    bool shafts = false;
    for (const GroomDocument::NodeInfo& n : doc->nodes()) shafts = shafts || (n.type == "Guides" && doc->param(n.name, "group") == json("feathers"));
    EXPECT_TRUE(shafts);
    EXPECT_FALSE(window.addFeathers({QStringLiteral("no_such_design")}, false));
    EXPECT_EQ(doc->featherGuideGroups(), std::vector<std::string>{"feathers"});
    // The feathers group has no guides yet: the Draw tool draws into it.
    EXPECT_EQ(window.brush().toolId(), "draw");
    EXPECT_EQ(window.brush().settings().brush.group, "feathers");

    // One feather per guide in a group of its own, from the prompt: the groom has a feathers group,
    // so one per guide comes first; the group asked next (a new name: flight_left).
    bindings.at("add.feathers").trigger(nullptr, false);
    ASSERT_TRUE(window.overlay()->promptShown());
    EXPECT_EQ(window.overlay()->promptLine(), QStringLiteral("flight"));
    ASSERT_TRUE(window.overlay()->choosePrompt(QStringLiteral("One feather per guide")));
    ASSERT_TRUE(window.overlay()->promptShown()) << "the guide group is asked next";
    EXPECT_EQ(window.overlay()->promptLine(), QStringLiteral("feathers"));
    window.overlay()->setPromptLine(QStringLiteral("flight_left"));
    ASSERT_TRUE(window.overlay()->choosePrompt(QStringLiteral("Add Feathers")));
    ASSERT_NE(doc->session().authoredHierarchy()->findGroup("flight_left"), kNoGroup) << window.lastReport().toStdString();
    EXPECT_NE(doc->param(hair, "exclude").get<std::string>().find("flight_left"), std::string::npos);
    EXPECT_EQ(doc->featherGuideGroups(), (std::vector<std::string>{"feathers", "flight_left"}));
    EXPECT_EQ(window.brush().settings().brush.group, "flight_left");

    // Braid on Curves: the style picked in the prompt, a Braid after the Interpolate along the braids group.
    bindings.at("add.braid").trigger(nullptr, false);
    ASSERT_TRUE(window.overlay()->promptShown());
    ASSERT_TRUE(window.overlay()->choosePrompt(QStringLiteral("Rope")));
    const std::string braid = nodeOfType(*doc, "Braid");
    ASSERT_FALSE(braid.empty()) << window.lastReport().toStdString();
    EXPECT_EQ(doc->param(braid, "style"), json("rope"));
    ASSERT_NE(doc->session().authoredHierarchy()->findGroup("braids"), kNoGroup);
    EXPECT_NE(doc->param(hair, "exclude").get<std::string>().find("braids"), std::string::npos);
    EXPECT_TRUE(linked(*doc, "interpolate", braid, "strands") || !nodeOfType(*doc, "Interpolate").empty());
}

TEST(AppOutput, HandPlacedFlightFeathersPresetDrawsIntoItsGroup) {
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(sampleCopy("groomlab_app_output_hand_placed"))));
    GroomDocument* doc = window.document();
    // Applied: the feathers group made, empty, and the Draw tool drawing into it.
    ASSERT_TRUE(window.applyPreset(QStringLiteral("hand_placed_feathers"))) << window.lastReport().toStdString();
    HierarchyPtr h = doc->session().authoredHierarchy();
    ASSERT_TRUE(h);
    const GroupId group = h->findGroup("feathers");
    ASSERT_NE(group, kNoGroup);
    EXPECT_TRUE(h->guidesInGroup(group).empty());
    EXPECT_EQ(window.brush().toolId(), "draw");
    EXPECT_EQ(window.brush().settings().brush.group, "feathers");
    EXPECT_EQ(doc->featherGuideGroups(), std::vector<std::string>{"feathers"});
    ViewRequest request;
    request.density = 1.0f;
    {
        const ViewResult& view = doc->evaluate(request);
        EXPECT_TRUE(view.errors.empty()) << view.errors.front();
        EXPECT_TRUE(!view.strands || view.strands->strandCount() == 0u) << "no feather before a guide is drawn";
    }
    // Every guide drawn into the group is one flight feather: 70 barbs a side and the shaft.
    std::vector<std::vector<Vec3f>> drawn;
    for (const HierarchyGuide& g : h->guides()) {
        if (drawn.size() < 3 && g.group != group) drawn.push_back(g.points);
    }
    ASSERT_EQ(drawn.size(), 3u);
    ASSERT_EQ(doc->editGuides("draw guides", [&](GuideHierarchy& hierarchy) {
        for (const std::vector<Vec3f>& points : drawn) hierarchy.addGuide(hierarchy.findGroup("feathers"), points);
    }), "");
    const ViewResult& view = doc->evaluate(request);
    EXPECT_TRUE(view.errors.empty()) << view.errors.front();
    ASSERT_TRUE(view.strands);
    EXPECT_EQ(view.strands->strandCount(), 3u * (1u + 140u));
}

TEST(AppOutput, ImportUtilitiesAndScales) {
    MainWindow window;
    window.resize(1280, 800);
    window.show();  // (the prompts show over the viewport)
    const std::string groom = sampleCopy("groomlab_app_output_utilities");
    ASSERT_TRUE(window.openGroom(QString::fromStdString(groom)));
    GroomDocument* doc = window.document();
    const fs::path dir = fs::u8path(groom).parent_path();

    // Import > Alembic Curves: a thin export of the groom read back as its guides.
    const fs::path abc = dir / "curves" / "groom_curves.abc";
    fs::create_directories(abc.parent_path());
    ExportOptions thin;
    thin.density = 0.02f;
    thin.frames.first = thin.frames.last = doc->frame();
    ASSERT_GT(doc->exportAlembic(abc.u8string(), thin).strands, 0u);
    ASSERT_TRUE(window.importAlembicCurves(QString::fromStdString(abc.u8string())));
    EXPECT_EQ(nodeOfType(*doc, "ImportCurves"), "guides");
    EXPECT_EQ(doc->param("guides", "path"), json(abc.u8string()));
    EXPECT_TRUE(doc->nodeErrors().empty());
    EXPECT_FALSE(window.importAlembicCurves(QString::fromStdString((dir / "none.abc").u8string())));

    // Find Missing Files: the curves moved; relinked to the file of that name in the folder picked.
    EXPECT_TRUE(doc->missingFiles().empty());
    const fs::path moved = dir / "elsewhere" / "deeper" / "groom_curves.abc";
    fs::create_directories(moved.parent_path());
    fs::rename(abc, moved);
    const auto missing = doc->missingFiles();
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_EQ(missing.front().kind, "params");
    EXPECT_EQ(missing.front().key, "guides.path");
    AppActions::bindings(window).at("files.findMissing").trigger(nullptr, false);  // (a scripted run: no folder dialog)
    EXPECT_EQ(doc->missingFiles().size(), 1u);
    ASSERT_TRUE(window.findMissingFiles(QString::fromStdString((dir / "elsewhere").u8string())));
    EXPECT_TRUE(doc->missingFiles().empty());
    EXPECT_EQ(fs::u8path(doc->param("guides", "path").get<std::string>()), moved);
    EXPECT_EQ(doc->undoText(), "Undo relink missing files");

    // To Mesh: tubes in an OBJ (a few strands), read back as a mesh.
    const std::string obj = (dir / "strands.obj").u8string();
    ASSERT_TRUE(window.strandsToMesh(QString::fromStdString(obj), false, 4, 50));
    const std::string text = readText(obj);
    EXPECT_EQ(text.rfind("# GroomLab: 50 strands as tubes", 0), 0u) << text.substr(0, 80);
    const auto mesh = readObjMesh(obj);
    EXPECT_GT(mesh->triangleCount(), 50u * 4u);
    EXPECT_TRUE(mesh->hasUVs());
    ASSERT_TRUE(window.strandsToMesh(QString::fromStdString((dir / "ribbons.obj").u8string()), true, 4, 20));
    // The menu item asks tubes or ribbons first.
    AppActions::bindings(window).at("strands.toMesh").trigger(nullptr, false);
    EXPECT_TRUE(window.overlay()->promptShown());
    ASSERT_TRUE(window.overlay()->choosePrompt(QStringLiteral("Cancel")));

    // Scales: none yet; with a scales look, the maps baked and the instancer exported.
    EXPECT_FALSE(window.bakeScaleMaps(QString::fromStdString((dir / "maps").u8string()), 64));
    ASSERT_TRUE(window.applyPreset(QStringLiteral("scales_from_guides"))) << window.lastReport().toStdString();
    ASSERT_TRUE(doc->file().data.scalp->hasUVs());
    ASSERT_TRUE(window.bakeScaleMaps(QString::fromStdString((dir / "maps").u8string()), 64)) << window.lastReport().toStdString();
    EXPECT_TRUE(fs::exists(dir / "maps" / "sample_ScaleNormal.png") || fs::exists(dir / "maps" / "sample_ScaleNormal.1001.png"));
    const std::string usd = (dir / "scales.usda").u8string();
    ASSERT_TRUE(window.exportScalesUsd(QString::fromStdString(usd))) << window.lastReport().toStdString();
    const std::string layer = readText(usd);
    EXPECT_NE(layer.find("scaleInstances"), std::string::npos);
    EXPECT_NE(layer.find("PointInstancer"), std::string::npos);
}

TEST(AppOutput, StrandCountHudToggles) {
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(sampleCopy("groomlab_app_output_hud"))));
    const ui::Bindings bindings = AppActions::bindings(window);
    EXPECT_TRUE(window.hud());
    EXPECT_TRUE(bindings.at("review.hud").checked(nullptr));
    window.setHud(true);  // (the line said now, not after the frames stop)
    EXPECT_NE(window.overlay()->statsText().indexOf(QStringLiteral("guides")), -1) << window.overlay()->statsText().toStdString();
    bindings.at("review.hud").trigger(nullptr, false);
    EXPECT_FALSE(window.hud());
    EXPECT_FALSE(window.overlay()->statsWanted());
    EXPECT_TRUE(window.overlay()->statsText().isEmpty());
    bindings.at("review.hud").trigger(nullptr, true);
    EXPECT_TRUE(window.overlay()->statsWanted());
}

TEST(AppOutput, OpenRecentListsTheFilesAndGreysMissingOnes) {
    const OwnSettings settings("groomlab_app_output_recent_settings");
    const std::string groom = sampleCopy("groomlab_app_output_recent");
    const QString gone = QString::fromStdString((fs::u8path(groom).parent_path() / "gone.groom").u8string());
    QSettings().setValue(QStringLiteral("recentFiles"), QStringList{QString::fromStdString(groom), gone});
    MainWindow window;
    // File > Open Recent in the menu bar, filled when it opens.
    QMenu* file = window.menuBar()->actions().front()->menu();
    ASSERT_TRUE(file);
    QMenu* recent = submenu(file, QStringLiteral("Open Recent"));
    ASSERT_TRUE(recent);
    emit recent->aboutToShow();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_EQ(shown(recent), (std::vector<std::string>{"sample.groom", "gone.groom (missing)", "-", "Remove Missing Files", "Clear Recent Files"}));
    EXPECT_TRUE(recent->actions()[0]->isEnabled());
    EXPECT_FALSE(recent->actions()[1]->isEnabled()) << "a file no longer there is greyed";
    QAction* remove = window.menuBar()->findChild<QAction*>(QStringLiteral("recentRemoveMissing"));
    ASSERT_TRUE(remove);
    remove->trigger();
    EXPECT_EQ(QSettings().value(QStringLiteral("recentFiles")).toStringList(), QStringList{QString::fromStdString(groom)});
    // Opening it from there; Save As adds the new file first.
    recent->actions()[0]->trigger();
    ASSERT_TRUE(window.document());
    EXPECT_EQ(window.document()->path(), groom);
    EXPECT_EQ(QSettings().value(QStringLiteral("recentFiles")).toStringList().size(), 1) << "the same file spelled another way is listed once";
    window.clearRecentFiles();
    EXPECT_TRUE(QSettings().value(QStringLiteral("recentFiles")).toStringList().isEmpty());
}

TEST(AppOutput, ScriptedRunsLeaveTheUsersSettingsAlone) {
    // "The user's" settings: an ini of known bytes, where a window someone sits at reads it.
    const fs::path user = fs::temp_directory_path() / "groomlab_app_output_user_settings";
    fs::remove_all(user);
    fs::create_directories(user / "GroomLab");
    const fs::path ini = user / "GroomLab" / (QCoreApplication::applicationName().toStdString() + ".ini");
    const std::string bytes = "[General]\nrecentFiles=C:/nowhere/kept.groom\n\n[ui]\ntheme=polar\n\n[units]\nlength=mm\n";
    std::ofstream(ini, std::ios::binary) << bytes;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString(user.u8string()));
    ASSERT_EQ(QSettings().value(QStringLiteral("ui/theme")).toString(), QStringLiteral("polar"));
    // A scripted run (no GROOMLAB_SETTINGS_DIR: ctest's is put back after) opens, edits, autosaves,
    // picks a theme and units: all of it in a throwaway folder that goes with the run.
    const QByteArray given = qgetenv("GROOMLAB_SETTINGS_DIR");
    qunsetenv("GROOMLAB_SETTINGS_DIR");
    const std::string groom = sampleCopy("groomlab_app_output_scripted_settings");
    QString throwaway;
    {
        const RunSettings run(true);
        ASSERT_TRUE(run.valid());
        ASSERT_TRUE(run.throwaway());
        throwaway = run.folder();
        EXPECT_FALSE(QSettings().contains(QStringLiteral("ui/theme"))) << "the user's settings are not read either";
        MainWindow window;
        ASSERT_TRUE(window.openGroom(QString::fromStdString(groom)));
        EXPECT_EQ(QSettings().value(QStringLiteral("recentFiles")).toStringList().size(), 1);
        GroomDocument* doc = window.document();
        float x = 0.0f, y = 0.0f;
        doc->nodePosition(doc->nodes().front().name, &x, &y);
        doc->setNodePosition(doc->nodes().front().name, x + 10.0f, y);
        ASSERT_TRUE(window.autosaveNow());
        EXPECT_TRUE(window.autosavePath().startsWith(throwaway)) << window.autosavePath().toStdString();
        theme::saveTheme(QStringLiteral("studio"));
        setUnitChoice("length", "in");
        window.setAutosaveMinutes(10);
    }
    EXPECT_FALSE(QFileInfo::exists(throwaway)) << "the run's settings folder goes with it";
    EXPECT_FALSE(fs::exists(fs::u8path(groom + "~"))) << "no autosave beside the file";
    EXPECT_EQ(readText(ini.u8string()), bytes) << "the user's settings file untouched";
    std::size_t files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(user)) files += entry.is_regular_file();
    EXPECT_EQ(files, 1u) << "nothing else written beside it";
    // GROOMLAB_SETTINGS_DIR given: that folder, for any run.
    qputenv("GROOMLAB_SETTINGS_DIR", QByteArray::fromStdString(user.u8string()));
    {
        const RunSettings run(true);
        EXPECT_FALSE(run.throwaway());
        EXPECT_EQ(run.folder().toStdString(), user.u8string());
        EXPECT_EQ(QSettings().value(QStringLiteral("ui/theme")).toString(), QStringLiteral("polar"));
    }
    if (given.isEmpty()) qunsetenv("GROOMLAB_SETTINGS_DIR");
    else qputenv("GROOMLAB_SETTINGS_DIR", given);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, testSettingsFolder());
    fs::remove_all(user);
}

TEST(AppOutput, AutosaveIsOfferedAfterACrash) {
    const OwnSettings settings("groomlab_app_output_autosave_settings");
    QSettings().remove(QStringLiteral("autosave/pending"));
    const std::string groom = sampleCopy("groomlab_app_output_autosave");
    {
        MainWindow window;
        // The interval (the File > Autosave menu): saved, checked as set.
        const ui::Bindings bindings = AppActions::bindings(window);
        bindings.at("file.autosave").trigger(json(10), false);
        EXPECT_EQ(window.autosaveMinutes(), 10);
        EXPECT_TRUE(bindings.at("file.autosave").checked(json(10)));
        bindings.at("file.autosave").trigger(json(5), false);
        ASSERT_TRUE(window.openGroom(QString::fromStdString(groom)));
        EXPECT_FALSE(window.autosaveNow()) << "nothing unsaved";
        GroomDocument* doc = window.document();
        float x = 0.0f, y = 0.0f;
        doc->nodePosition(doc->nodes().front().name, &x, &y);
        doc->setNodePosition(doc->nodes().front().name, x + 10.0f, y);
        ASSERT_TRUE(doc->dirty());
        // A window no one sits at (a test, a scripted capture) keeps it in its settings folder,
        // never beside the file (the test data); a window someone sits at, beside the file.
        EXPECT_EQ(QFileInfo(window.autosavePath()).fileName(), QStringLiteral("sample.groom~"));
        EXPECT_NE(window.autosavePath().indexOf(QStringLiteral("groomlab_app_output_autosave_settings")), -1)
            << window.autosavePath().toStdString();
        window.setInteractive(true);
        ASSERT_TRUE(window.autosaveNow());
        EXPECT_EQ(window.autosavePath().toStdString(), groom + "~");
        EXPECT_TRUE(fs::exists(fs::u8path(groom + "~")));
        EXPECT_TRUE(QSettings().value(QStringLiteral("autosave/pending")).toMap().contains(QString::fromStdString(groom + "~")));
        // (The window goes without closing: as a crash leaves it.)
    }
    // The next start offers it in the glass prompt; Recover opens it in the file's place, unsaved.
    MainWindow next;
    next.resize(1280, 800);
    next.show();
    ASSERT_TRUE(next.offerAutosave());
    ASSERT_TRUE(next.overlay()->promptShown());
    ASSERT_TRUE(next.overlay()->choosePrompt(QStringLiteral("Recover")));
    ASSERT_TRUE(next.document());
    EXPECT_EQ(next.document()->path(), groom);
    EXPECT_TRUE(next.document()->dirty());
    float x = 0.0f, y = 0.0f;
    ASSERT_TRUE(next.document()->nodePosition(next.document()->nodes().front().name, &x, &y));
    // Saved: the autosave and its entry go.
    ASSERT_TRUE(next.saveTo(QString::fromStdString(groom)));
    EXPECT_FALSE(fs::exists(fs::u8path(groom + "~")));
    EXPECT_FALSE(QSettings().value(QStringLiteral("autosave/pending")).toMap().contains(QString::fromStdString(groom + "~")));
    EXPECT_FALSE(next.offerAutosave());
    // An untitled groom's autosave in the settings folder; let go of when another groom opens.
    ASSERT_TRUE(next.addSampleMesh(QStringLiteral("manny")));
    std::string untitled;
    ASSERT_EQ(next.document()->addMotionNode("Wind", json::object(), &untitled), "");
    ASSERT_TRUE(next.autosaveNow());
    EXPECT_NE(next.autosavePath().indexOf(QStringLiteral("autosave/untitled.groom~")), -1);
    const QString copy = next.autosavePath();
    EXPECT_TRUE(QFileInfo::exists(copy));
    ASSERT_TRUE(next.openGroom(QString::fromStdString(groom)));
    EXPECT_FALSE(QFileInfo::exists(copy));
    EXPECT_TRUE(QSettings().value(QStringLiteral("autosave/pending")).toMap().isEmpty());
}

TEST(AppOutput, ThemesSwitchLiveWithTheUsersFolder) {
    const fs::path user = fs::temp_directory_path() / "groomlab_app_output_themes";
    fs::remove_all(user);
    qputenv("GROOMLAB_USER_THEMES", QByteArray::fromStdString(user.u8string()));
    struct Restore {
        QString saved = theme::savedTheme();
        fs::path dir;
        ~Restore() {
            theme::setTheme(theme::defaultTheme());
            theme::saveTheme(saved);
            qunsetenv("GROOMLAB_USER_THEMES");
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    } restore{theme::savedTheme(), user};
    ASSERT_EQ(theme::userThemeDir().toStdString(), QString::fromStdString(user.u8string()).replace(QLatin1Char('\\'), QLatin1Char('/')).toStdString());
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    const ui::Bindings bindings = AppActions::bindings(window);

    // Open Themes Folder: made (a window someone sits at opens it too).
    bindings.at("theme.openFolder").trigger(nullptr, false);
    EXPECT_TRUE(fs::is_directory(user));

    // A theme picked: worn at once (the window's sheet and the widgets' own), saved.
    const QString backdropBefore = theme::backdrop().name();
    ASSERT_TRUE(window.chooseTheme(QStringLiteral("playful")));
    EXPECT_EQ(theme::currentTheme(), QStringLiteral("playful"));
    EXPECT_EQ(theme::savedTheme(), QStringLiteral("playful"));
    EXPECT_EQ(window.styleSheet(), theme::styleSheet());
    if (theme::backdrop().name() != backdropBefore) {
        EXPECT_TRUE(window.overlay()->styleSheet().contains(theme::backdrop().name())) << window.overlay()->styleSheet().toStdString();
        EXPECT_FALSE(window.overlay()->styleSheet().contains(backdropBefore));
    }
    EXPECT_TRUE(window.findChild<QAction*>(QStringLiteral("theme_playful"))->isChecked());

    // New Theme from Current: its name asked in the prompt; a copy in the folder, worn.
    bindings.at("theme.new").trigger(nullptr, false);
    ASSERT_TRUE(window.overlay()->promptShown());
    window.overlay()->setPromptLine(QStringLiteral("batchh_mine"));
    ASSERT_TRUE(window.overlay()->choosePrompt(QStringLiteral("Create")));
    const fs::path file = user / "batchh_mine.json";
    ASSERT_TRUE(fs::exists(file));
    EXPECT_TRUE(theme::themeNames().contains(QStringLiteral("batchh_mine")));
    EXPECT_EQ(theme::currentTheme(), QStringLiteral("batchh_mine"));
    json data = json::parse(readText(file.u8string()));
    EXPECT_FALSE(data.contains("extends"));
    EXPECT_EQ(data.value("name", std::string()), "batchh_mine");
    EXPECT_FALSE(window.newTheme(QStringLiteral("batchh_mine"))) << "there already";
    EXPECT_FALSE(window.newTheme(QStringLiteral("bad/name")));

    // Edited and reloaded: the window wears the file's colors.
    data["ui"]["glass"] = false;
    data["ui"]["panel"] = "#123456";
    std::ofstream(file) << data.dump(2);
    bindings.at("theme.reload").trigger(nullptr, false);
    EXPECT_EQ(theme::uiColor("panel"), QColor(0x12, 0x34, 0x56));
    EXPECT_TRUE(window.styleSheet().contains(QStringLiteral("#123456")));
    // GROOMLAB_THEME_PATH's folders come first.
    const fs::path more = user / "more";
    fs::create_directories(more);
    std::ofstream(more / "batchh_extra.json") << R"({"name": "batchh_extra", "extends": "charcoal"})";
    qputenv("GROOMLAB_THEME_PATH", QByteArray::fromStdString(more.u8string()));
    EXPECT_TRUE(theme::themeNames().contains(QStringLiteral("batchh_extra")));
    EXPECT_EQ(theme::themeSearchDirs().front().toStdString(), QString::fromStdString(more.u8string()).replace(QLatin1Char('\\'), QLatin1Char('/')).toStdString());
    qunsetenv("GROOMLAB_THEME_PATH");
    ASSERT_TRUE(window.chooseTheme(theme::defaultTheme()));
    EXPECT_EQ(window.styleSheet(), theme::styleSheet());
}

// GroomLab > Studio lights the viewport in the mode at hand: it no longer switches to Look mode,
// which hid the Groom Tools; the Properties dock shows its Lighting tab, lookdev on.
TEST(AppOutput, StudioLightingKeepsTheModeAndTheGroomTools) {
    MainWindow window;
    ASSERT_TRUE(window.openGroom(QString::fromStdString(sampleCopy("groomlab_app_output_studio"))));
    window.show();
    ASSERT_TRUE(window.setUiMode(QStringLiteral("groom")));
    QCoreApplication::processEvents();
    AppShell* shell = window.shell();
    ASSERT_FALSE(shell->groomToolsDock()->isHidden());
    // The Properties dock's tabs: Attributes, then the look's Material, Lighting and Render.
    QTabBar* tabs = shell->propertiesTabs();
    ASSERT_TRUE(tabs);
    ASSERT_EQ(tabs->count(), 4);
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("Attributes"));
    EXPECT_EQ(tabs->tabText(2), QStringLiteral("Lighting"));
    EXPECT_FALSE(tabs->isHidden());
    EXPECT_EQ(shell->propertiesTab(), AppShell::PropertiesTab::Attributes);
    const ui::Bindings bindings = AppActions::bindings(window);
    ASSERT_TRUE(bindings.count("studio.lighting"));
    const std::string preset = lightingPresetNames().back();
    bindings.at("studio.lighting").trigger(json(preset), false);
    QCoreApplication::processEvents();
    EXPECT_EQ(shell->mode(), OverlayHost::Mode::Groom);
    EXPECT_FALSE(shell->groomToolsDock()->isHidden());
    EXPECT_EQ(shell->propertiesTab(), AppShell::PropertiesTab::Lighting);
    EXPECT_EQ(shell->lookCard()->tab(), LookCard::Tab::Lighting);
    EXPECT_TRUE(bindings.at("studio.lighting").checked(json(preset)));
    // Back to Attributes: the groom's settings again; Cards mode shows its own panel, no tabs.
    shell->setPropertiesTab(AppShell::PropertiesTab::Attributes);
    EXPECT_TRUE(shell->panelShown(AppShell::Panel::Display));
    ASSERT_TRUE(window.setUiMode(QStringLiteral("cards")));
    EXPECT_TRUE(tabs->isHidden());
}

}  // namespace
}  // namespace groomlab::app
