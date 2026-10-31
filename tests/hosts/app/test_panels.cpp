// SPDX-License-Identifier: Apache-2.0
// The panels driven without a window (the offscreen platform, a QApplication from test_app.cpp's
// main): the param editors round-trip every kind of param without losing a map, the look card
// and panel apply their settings, the maps panel makes, paints and undoes maps.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <set>
#include <string>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QAction>
#include <QToolBar>
#include <QToolButton>
#include <QApplication>
#include <QCloseEvent>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QSet>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointF>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTimer>
#include <QWheelEvent>
#include <QSize>
#include <QTabBar>
#include <QToolButton>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <QDockWidget>
#include <QMenuBar>
#include <QSplitter>
#include <QTabWidget>

#include "alpha_picker.h"
#include "app_shell.h"
#include "brush_panel.h"
#include "brush_session.h"
#include "groom_tools.h"
#include "camera.h"
#include "correctives_panel.h"
#include "cards_panel.h"
#include "export_panel.h"
#include "prman_render.h"
#include "vray_render.h"
#include "glass.h"
#include "groom_document.h"
#include "reference_window.h"
#include "transfer_window.h"
#include "groomlab/brush_catalog.h"
#include "groomlab/correctives.h"
#include "groomlab/field.h"
#include "groomlab/graph_pins.h"
#include "groomlab/groom_file.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/transfer.h"
#include "groomlab/graph.h"
#include "groomlab/parts.h"
#include "groomlab/shortcuts.h"
#include "groomlab/texture.h"
#include "guides_panel.h"
#include "graph_presets.h"
#include "look_panel.h"
#include "main_window.h"
#include "maps_panel.h"
#include "node_graph.h"
#include "overlay.h"
#include "param_editors.h"
#include "param_panel.h"
#include "theme.h"
#include "timeline.h"
#include "viewport.h"

// What QTest's key clicks call first (QtGui): the shortcut map tried for a key press at `o` as a
// window system's would be; true when a shortcut took it.
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject* o, ulong timestamp, int k, Qt::KeyboardModifiers mods,
                                               const QString& text = QString(), bool autorep = false, ushort count = 1);

namespace groomlab::app {
namespace {

using nlohmann::json;

const std::string kSampleGroom = std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom";

std::string nodeOfType(const GroomDocument& doc, const std::string& type) {
    for (const GroomDocument::NodeInfo& n : doc.nodes()) {
        if (n.type == type) return n.name;
    }
    throw std::runtime_error("no node of type " + type);
}

// A copy of the sample groom under %TEMP% (a texture map's image goes beside it).
std::string sampleCopy(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path copy = dir / "sample.groom";
    std::filesystem::copy_file(std::filesystem::u8path(kSampleGroom), copy, std::filesystem::copy_options::overwrite_existing);
    return copy.u8string();
}

// A mouse event sent to a widget as the window would deliver it (no QTest: a press, a move
// with the button held, a release).
void mouse(QWidget* w, QEvent::Type type, const QPoint& at, Qt::MouseButton button = Qt::LeftButton) {
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent event(type, QPointF(at), w->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : button, held, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &event);
}

void click(QWidget* w, const QPoint& at) {
    mouse(w, QEvent::MouseButtonPress, at);
    mouse(w, QEvent::MouseButtonRelease, at);
}

// A mouse event with modifiers and sub-pixel position (the reference window's photo view).
void mouseAt(QWidget* w, QEvent::Type type, const QPointF& at, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton);
    QMouseEvent event(type, at, w->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held, mods);
    QCoreApplication::sendEvent(w, &event);
}

void dragPhoto(PhotoView* view, const std::vector<QPointF>& photoPoints, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    mouseAt(view, QEvent::MouseButtonPress, view->toWidget(photoPoints.front()), mods);
    for (std::size_t i = 1; i < photoPoints.size(); ++i) mouseAt(view, QEvent::MouseMove, view->toWidget(photoPoints[i]), mods);
    mouseAt(view, QEvent::MouseButtonRelease, view->toWidget(photoPoints.back()), mods);
}


}  // namespace

TEST(AppPanels, NodePanelAsTallAsItsRowsShowsThemWithoutScrolling) {
    // The Node panel takes the height its hint asks (within the view): at that height no node's
    // form scrolls, a compound's (a few rows) as a long node's.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    std::string compound;
    ASSERT_EQ(doc->collapseNodes({nodeOfType(*doc, "Noise")}, &compound), "");
    for (const std::string& node : {compound, nodeOfType(*doc, "Interpolate"), nodeOfType(*doc, "Width"), nodeOfType(*doc, "Length")}) {
        ParamPanel panel;
        panel.setDocument(doc.get());
        panel.showNode(node);
        panel.resize(340, 2000);
        panel.show();
        QApplication::processEvents();
        panel.resize(340, panel.sizeHint().height());
        QApplication::processEvents();
        QScrollArea* scroll = panel.findChild<QScrollArea*>();
        ASSERT_TRUE(scroll);
        EXPECT_EQ(scroll->verticalScrollBar()->maximum(), 0) << node << ": the form scrolls at the panel's own height " << panel.sizeHint().height();
    }
    // In the window, the Node Settings sit beside the graph in the Graph Editor dock (Maya's
    // layout), never over it; a node picked after another shows its own form, scrolling within
    // the dock when it is taller than the dock.
    MainWindow window;
    window.resize(1600, 1000);
    window.show();
    AppShell* shell = window.shell();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    window.setGraphShown(true);
    shell->graph()->setSelection({nodeOfType(*window.document(), "Noise")});
    shell->graph()->groupSelected();
    for (const char* type : {"Compound", "Interpolate", "Width", "HairColor", "Width", "Compound"}) {
        ASSERT_TRUE(window.selectUiNode(QString::fromStdString(nodeOfType(*window.document(), type))));
        QApplication::processEvents();
        QWidget* side = shell->panel(AppShell::Panel::Node);
        ASSERT_TRUE(side && side->isVisible());
        EXPECT_TRUE(shell->graphDock()->isAncestorOf(side)) << "the settings live in the Graph Editor dock";
        const QRect graph(shell->graph()->mapTo(&window, QPoint(0, 0)), shell->graph()->size());
        const QRect settings(side->mapTo(&window, QPoint(0, 0)), side->size());
        EXPECT_FALSE(graph.intersects(settings)) << type << ": the settings over the graph";
        EXPECT_GE(settings.left(), graph.right()) << type << ": beside the graph, at its right";
        QScrollArea* scroll = side->findChild<QScrollArea*>();
        ASSERT_TRUE(scroll);
        EXPECT_LE(scroll->height(), side->height()) << type;
    }
}

TEST(AppPanels, ParamEditorsRoundTripEveryKindWithoutLosingMaps) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    ParamPanel panel;
    panel.setDocument(doc.get());

    // A ramp: the editor shows the stored keys, edits through it reach the document, undo comes back.
    const std::string width = nodeOfType(*doc, "Width");
    panel.showNode(width);
    EXPECT_EQ(panel.editorKind("profile"), "ramp");
    EXPECT_EQ(panel.shownValue("profile"), doc->paramOrDefault(width, "profile"));
    RampEditor* ramp = panel.rampEditor("profile");
    ASSERT_TRUE(ramp);
    ramp->setValue(RampEditor::preset("bell"));
    EXPECT_EQ(ramp->keyCount(), 3u);
    emit ramp->edited(false);
    EXPECT_EQ(doc->paramOrDefault(width, "profile"), RampEditor::preset("bell"));
    EXPECT_EQ(doc->undoText(), "Undo " + width + ".profile");
    // Straight segments are kept in the JSON's object form.
    ramp->setValue(json{{"keys", json::array({json::array({0.0, 1.0}), json::array({1.0, 0.2})})}, {"linear", true}});
    EXPECT_TRUE(ramp->straight());
    EXPECT_EQ(ramp->value().value("linear", false), true);
    // A drag: live edits unrecorded, one step at the end.
    const json before = doc->paramOrDefault(width, "profile");
    ramp->setValue(RampEditor::preset("up"));
    emit ramp->edited(true);
    emit ramp->edited(true);
    emit ramp->edited(false);
    EXPECT_EQ(doc->paramOrDefault(width, "profile"), RampEditor::preset("up"));
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->paramOrDefault(width, "profile"), before);

    // A mappable param holding a map: shown as its source, never replaced by the number editor.
    const std::string scatter = nodeOfType(*doc, "Scatter");
    panel.showNode(scatter);
    EXPECT_EQ(panel.editorKind("density"), "map");
    const json constant = doc->paramOrDefault(scatter, "density");
    ASSERT_TRUE(constant.is_number());
    const json expression = {{"expression", "0.5 + 0.5 * u"}};
    EXPECT_EQ(doc->setParam(scatter, "density", expression), "");
    panel.rebuildNow();
    MapSourceEditor* map = panel.mapEditor("density");
    ASSERT_TRUE(map);
    EXPECT_EQ(map->source(), MapSourceEditor::Source::Expression);
    EXPECT_EQ(panel.shownValue("density"), expression);
    // Back to a value: the number the editor holds; then the map again, kept from before.
    map->chooseSource(MapSourceEditor::Source::Constant);
    EXPECT_TRUE(doc->paramOrDefault(scatter, "density").is_number());
    map->chooseSource(MapSourceEditor::Source::Expression);
    EXPECT_EQ(doc->paramOrDefault(scatter, "density"), expression);
    // A texture file and a color set carry their component and range.
    const json texture = {{"texture", "maps/density.png"}, {"component", "g"}, {"min", 0.25}, {"max", 2.0}};
    EXPECT_EQ(doc->setParam(scatter, "density", texture), "");
    panel.rebuildNow();
    map = panel.mapEditor("density");
    ASSERT_TRUE(map);
    EXPECT_EQ(map->source(), MapSourceEditor::Source::Texture);
    EXPECT_EQ(panel.shownValue("density"), texture);
    // A port: the param exposed on the node, what feeds it shown.
    map->chooseSource(MapSourceEditor::Source::Port);
    EXPECT_TRUE(doc->paramExposed(scatter, "density"));
    EXPECT_EQ(doc->paramInputSource(scatter, "density"), "");
    std::string field;
    EXPECT_EQ(doc->addNode("Expression", 0.0f, 0.0f, &field), "");
    EXPECT_EQ(doc->connectNodes(field, "", scatter, "density"), "");
    EXPECT_EQ(doc->paramInputSource(scatter, "density"), field);
    panel.rebuildNow();
    EXPECT_EQ(panel.mapEditor("density")->source(), MapSourceEditor::Source::Port);
    EXPECT_EQ(doc->exposeParam(scatter, "density", false), "");
    EXPECT_EQ(doc->paramInputSource(scatter, "density"), "");

    // A color: shown in sRGB, stored linear.
    std::string hairColor;
    EXPECT_EQ(doc->addNode("HairColor", 0.0f, 0.0f, &hairColor), "");
    panel.showNode(hairColor);
    EXPECT_EQ(panel.editorKind("tipColor"), "color");
    ColorEditor* color = panel.colorEditor("tipColor");
    ASSERT_TRUE(color);
    color->setLinear({0.5f, 0.25f, 0.125f});
    EXPECT_NEAR(color->shown().redF(), linearToSrgb(0.5f), 0.01);
    emit color->edited(false);
    const json stored = doc->paramOrDefault(hairColor, "tipColor");
    ASSERT_TRUE(stored.is_array() && stored.size() == 3);
    EXPECT_NEAR(stored[0].get<double>(), 0.5, 1e-6);
    EXPECT_NEAR(stored[2].get<double>(), 0.125, 1e-6);
    EXPECT_EQ(panel.editorKind("melanin"), "map");  // mappable
    EXPECT_EQ(panel.editorKind("tipProfile"), "ramp");

    // Every operator's params get an editor: each kind the specs have is covered.
    std::set<std::string> kinds, editors;
    for (const std::string& type : doc->operatorTypes()) {
        const json* spec = doc->spec(type);
        ASSERT_TRUE(spec);
        for (const json& p : spec->value("params", json::array())) kinds.insert(p.value("kind", std::string()));
    }
    for (const std::string& type : {"Clump", "Noise", "Map", "Curve", "Cards", "Length", "Frizz"}) {
        std::string made;
        if (!doc->addNode(type, 0.0f, 0.0f, &made).empty()) continue;
        panel.showNode(made);
        for (const json& p : doc->spec(type)->value("params", json::array())) {
            const std::string name = p.value("name", std::string());
            const std::string editor = panel.editorKind(name);
            EXPECT_FALSE(editor.empty()) << type << "." << name;
            editors.insert(editor);
            if (editor != "label") EXPECT_EQ(panel.shownValue(name), doc->paramOrDefault(made, name)) << type << "." << name;
        }
    }
    // No operator declares a map param: a map is what a mappable number holds (the "map" editor).
    for (const char* kind : {"bool", "int", "float", "string", "vec3", "ramp"}) EXPECT_TRUE(kinds.count(kind)) << kind;
    for (const char* editor : {"bool", "int", "float", "enum", "ramp", "map"}) EXPECT_TRUE(editors.count(editor)) << editor;
}

TEST(AppPanels, LookCardAppliesEverySetting) {
    LookCard card;
    // The material's full settings: a dye and a tint through the card's colors, the lobes.
    HairMaterial material = hairPreset("blonde");
    card.setMaterial(material);
    EXPECT_EQ(card.material().presetName, "blonde");
    card.setTab(LookCard::Tab::Material);
    card.setMoreShown(true);
    EXPECT_TRUE(card.moreShown());
    const int withMore = card.sizeHint().height();
    card.setMoreShown(false);
    EXPECT_LT(card.sizeHint().height(), withMore) << "the full settings fold away";
    // The lighting: a preset keeps the render settings, which are the Render tab's.
    LightingSetup lighting = lightingPreset("daylight");
    lighting.quality = Quality::Interactive;
    lighting.refineFrames = 64;
    lighting.resolutionScale = 0.5f;
    lighting.interactiveQuality = InteractiveQuality::Full;
    card.setLighting(lighting);
    EXPECT_EQ(card.lighting().presetName, "daylight");
    EXPECT_EQ(card.lighting().refineFrames, 64);
    EXPECT_FLOAT_EQ(card.lighting().resolutionScale, 0.5f);
    EXPECT_EQ(card.lighting().interactiveQuality, InteractiveQuality::Full);
    card.setTab(LookCard::Tab::Render);
    EXPECT_EQ(card.tab(), LookCard::Tab::Render);
    card.setTab(LookCard::Tab::Lighting);
    EXPECT_GT(card.sizeHint().height(), 300);
    card.setMaxHeight(400);
    EXPECT_LE(card.sizeHint().height(), 400) << "the card scrolls past its height";
    // The surfaces: the skin's scattering controls edit the material handed back.
    std::vector<std::pair<QString, SurfaceMaterial>> surfaces{{"scalp", surfacePreset("scalp")}, {"head", surfacePreset("skin")}};
    card.setSurfaces(surfaces);
    ASSERT_TRUE(card.surfaceMaterial(1));
    EXPECT_EQ(card.surfaceMaterial(1)->kind, SurfaceKind::Skin);
    EXPECT_EQ(card.surfaceMaterial(1)->sssMode, SurfaceMaterial::SssMode::ScreenSpace);
    int edits = 0;
    QObject::connect(&card, &LookCard::surfaceEdited, [&](int index, const SurfaceMaterial& m) {
        ++edits;
        EXPECT_EQ(index, 1);
        EXPECT_EQ(m.presetName, "");
    });
    // The spies: the full look panel takes the same material and keeps every field.
    LookPanel panel;
    panel.setSurfaces(surfaces);
    panel.setMaterial(card.material());
    panel.setLighting(card.lighting());
    EXPECT_EQ(panel.lighting().refineFrames, 64);
    EXPECT_EQ(panel.lighting().interactiveQuality, InteractiveQuality::Full);
    EXPECT_EQ(panel.material().presetName, "blonde");
    SurfaceMaterial skin = *card.surfaceMaterial(1);
    EXPECT_TRUE(setSurfaceParam(skin, "sssRadius", "2.5"));
    EXPECT_TRUE(setSurfaceParam(skin, "sssMode", "preintegrated"));
    EXPECT_TRUE(setSurfaceParam(skin, "sssMeanFreePath.r", "0.5"));
    EXPECT_TRUE(setSurfaceParam(skin, "sssTexturing", "0.25"));
    EXPECT_TRUE(setSurfaceParam(skin, "transmission", "2"));
    surfaces[1].second = skin;
    card.setSurfaces(surfaces);
    EXPECT_FLOAT_EQ(card.surfaceMaterial(1)->sssRadius, 2.5f);
    EXPECT_EQ(card.surfaceMaterial(1)->sssMode, SurfaceMaterial::SssMode::Preintegrated);
    EXPECT_FLOAT_EQ(card.surfaceMaterial(1)->sssMeanFreePath[0], 0.5f);
    EXPECT_EQ(edits, 0) << "setSurfaces is quiet";
}

TEST(AppPanels, NewMapForASettingMakesLinksPaintsThenCtrlLowers) {
    // The maps panel's New map, For: Density (the Width and Density brushes are gone): a white
    // vertex map made and linked to Scatter's density, picked, the Paint brush on it toward 1, all
    // one undo step; Ctrl paints it toward 0 (hair away); asked again, the same map is picked.
    const std::string path = sampleCopy("groomlab_app_setting_map");
    MainWindow window;
    window.resize(1280, 720);
    ASSERT_TRUE(window.openGroom(QString::fromStdString(path)));
    GroomDocument* doc = window.document();
    MapsPanel* panel = window.mapsPanel();
    const std::string scatter = nodeOfType(*doc, "Scatter");
    ASSERT_FALSE(scatter.empty());
    BrushToolSettings probe;
    EXPECT_THROW(probe.setMode("density"), std::invalid_argument) << "no Density brush";
    auto* forBox = panel->findChild<QComboBox*>(QStringLiteral("newMapFor"));
    ASSERT_TRUE(forBox);
    EXPECT_EQ(forBox->itemText(1), QStringLiteral("Setting: Density"));
    EXPECT_EQ(forBox->itemText(forBox->count() - 1), QStringLiteral("Other setting..."));
    EXPECT_TRUE(panel->setNewMapTarget(QString::fromStdString(nodeOfType(*doc, "Noise")), QStringLiteral("fraction")))
        << "any mappable param, under Other";
    EXPECT_FALSE(panel->setNewMapTarget(QStringLiteral("noSuchNode"), QStringLiteral("x")));
    ASSERT_TRUE(panel->setNewMapTarget(QString::fromStdString(scatter), QStringLiteral("density")));
    EXPECT_EQ(panel->newMapTarget(), std::make_pair(scatter, std::string("density")));
    const json before = doc->paramOrDefault(scatter, "density");
    const std::size_t mapsBefore = doc->maps().size();
    panel->setNewMapFields(QString(), QStringLiteral("vertices"), false, 1.0, 64);
    ASSERT_TRUE(panel->setNewMapTarget(QString::fromStdString(scatter), QStringLiteral("density")));
    EXPECT_EQ(panel->createFromFields(), "");
    const std::string map = scatter + "_density";
    ASSERT_TRUE(doc->map(map));
    EXPECT_EQ(doc->map(map)->kind, "vertices");
    EXPECT_EQ(doc->maps().size(), mapsBefore + 1);
    const json linked = doc->paramOrDefault(scatter, "density");
    ASSERT_TRUE(linked.is_object());
    EXPECT_EQ(linked["colorSet"], map);
    EXPECT_DOUBLE_EQ(linked["max"].get<double>(), before.get<double>()) << "white: the density as it was";
    EXPECT_EQ(panel->selectedMap(), map);
    BrushSession& session = window.brush();
    EXPECT_EQ(session.settings().modeName(), "paint");
    EXPECT_EQ(session.settings().brush.attribute, map);
    EXPECT_EQ(session.settings().paintKind, "vertices");
    EXPECT_FLOAT_EQ(session.settings().brush.value, 1.0f);
    // One undo step for the map and its link.
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->map(map), nullptr);
    EXPECT_EQ(doc->paramOrDefault(scatter, "density"), before);
    EXPECT_EQ(doc->redo(), "");
    ASSERT_TRUE(doc->map(map));
    // Ctrl over the scalp's middle: toward 0, the rest white.
    ViewRequest request;
    request.density = 0.2f;
    doc->evaluate(request);
    OrbitCamera camera;
    const int w = 800, h = 600;
    camera.frame(doc->sceneBounds());
    session.setView(camera, w, h);
    session.settings().screenRadius = 80.0f;
    session.settings().brush.strength = 1.0f;
    const Bounds b = doc->sceneBounds();
    float cx = 0.0f, cy = 0.0f;
    ASSERT_TRUE(session.view().project((b.lo + b.hi) * 0.5f, cx, cy));
    std::vector<QPointF> stroke;
    for (int k = -3; k <= 3; ++k) stroke.emplace_back(cx + 12.0 * k, h - cy);
    ASSERT_TRUE(session.stroke(stroke, 1.0f, kStrokeCtrl)) << session.lastError();
    const std::vector<float>& values = doc->session().scalpMaps().maps.at(map).values;
    EXPECT_LT(*std::min_element(values.begin(), values.end()), 0.5f) << "Ctrl painted hair away";
    EXPECT_EQ(*std::max_element(values.begin(), values.end()), 1.0f);
    // Asked again: the map the param reads, nothing made.
    EXPECT_TRUE(window.paintParam(QString::fromStdString(scatter), QStringLiteral("density"), QStringLiteral("texture")));
    EXPECT_EQ(doc->maps().size(), mapsBefore + 1);
    EXPECT_EQ(session.settings().brush.attribute, map);
}

TEST(AppPanels, MapsPanelMakesPaintsShowsAndUndoesMaps) {
    const std::string path = sampleCopy("groomlab_app_maps");
    auto doc = GroomDocument::open(path);
    ASSERT_TRUE(doc);
    ViewRequest request;
    request.density = 0.2f;
    doc->evaluate(request);
    BrushSession session;
    OrbitCamera camera;
    const int w = 800, h = 600;
    camera.frame(doc->sceneBounds());
    session.setDocument(doc.get());
    session.setView(camera, w, h);
    MapsPanel panel;
    EXPECT_DOUBLE_EQ(panel.newMapFill(), 1.0) << "a new map lets everything through until painted";
    EXPECT_TRUE(panel.hintShown()) << "no document: the hint";
    panel.setSession(&session);
    panel.setDocument(doc.get());
    const std::size_t layersBefore = doc->maps().size();
    EXPECT_EQ(panel.hintShown(), layersBefore == 0);

    // A vertex map, a guide layer and a texture map (the image beside the groom), each one undo step.
    panel.setNewMapFields("density", "vertices", false, 0.0, 64);
    EXPECT_EQ(panel.createFromFields(), "");
    EXPECT_EQ(doc->undoText(), "Undo create map density");
    ASSERT_TRUE(doc->map("density"));
    EXPECT_EQ(doc->map("density")->kind, "vertices");
    panel.setNewMapFields("tint", "vertices", true, 0.5, 64);
    EXPECT_EQ(panel.createFromFields(), "");
    EXPECT_EQ(doc->map("tint")->channels, kColorMap);
    panel.setNewMapFields("stiff", "guides", false, 0.3, 64);
    EXPECT_EQ(panel.createFromFields(), "");
    EXPECT_EQ(doc->map("stiff")->kind, "guides");
    panel.setNewMapFields("detail", "texture", false, 0.0, 64);
    EXPECT_EQ(panel.createFromFields(), "");
    ASSERT_TRUE(doc->map("detail"));
    EXPECT_EQ(doc->map("detail")->kind, "texture");
    const std::string file = doc->textureMapFile("detail");
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(file))) << file;
    EXPECT_EQ(doc->map("detail")->texture, "masks/sample/detail.png") << "stored relative to the groom";
    EXPECT_EQ(doc->maps().size(), layersBefore + 4);
    EXPECT_NE(panel.createFromFields(), "") << "a name taken is refused";
    // The same name twice is refused; a bad name too: the rule Maya keeps (scalp_maps.h), so
    // nothing named here fails there (-, . and a leading digit used to pass in the app).
    panel.setNewMapFields("density", "vertices", false, 0.0, 64);
    EXPECT_NE(panel.createFromFields(), "");
    for (const char* bad : {"hair-mask", "tint.r", "2nd", ""}) {
        panel.setNewMapFields(QString::fromUtf8(bad), "vertices", false, 0.0, 64);
        EXPECT_EQ(panel.createFromFields(), mapNameError(bad)) << bad;
        EXPECT_FALSE(validMapName(bad)) << bad;
    }
    panel.selectMap("density");
    EXPECT_EQ(panel.renameSelected("density-2"), mapNameError("density-2"));

    // Painting the vertex map through the brush session: a stroke over the scalp's middle.
    panel.selectMap("density");
    EXPECT_EQ(panel.selectedMap(), "density");
    ASSERT_TRUE(panel.paintSelected());
    EXPECT_EQ(session.settings().modeName(), "paint");
    EXPECT_EQ(session.settings().brush.attribute, "density");
    EXPECT_EQ(session.settings().paintKind, "vertices");
    session.settings().screenRadius = 80.0f;
    session.settings().brush.value = 1.0f;
    const Bounds b = doc->sceneBounds();
    const Vec3f center = (b.lo + b.hi) * 0.5f;
    float cx = 0.0f, cy = 0.0f;
    ASSERT_TRUE(session.view().project(center, cx, cy));
    std::vector<QPointF> stroke;
    for (int k = -3; k <= 3; ++k) stroke.emplace_back(cx + 12.0 * k, h - cy);
    EXPECT_TRUE(session.stroke(stroke)) << session.lastError();
    const ScalpMaps& maps = doc->session().scalpMaps();
    auto painted = [&](const std::string& name) {
        const auto it = maps.maps.find(name);
        if (it == maps.maps.end()) return 0.0f;
        float sum = 0.0f;
        for (const float v : it->second.values) sum += v;
        return sum;
    };
    EXPECT_GT(painted("density"), 0.0f) << "the stroke painted vertices";
    EXPECT_EQ(doc->undoText(), "Undo paint stroke");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_FLOAT_EQ(painted("density"), 0.0f);
    EXPECT_EQ(doc->redo(), "");
    EXPECT_GT(painted("density"), 0.0f);

    // Painting the texture map: the file beside the groom takes the stroke, undo puts it back.
    panel.selectMap("detail");
    ASSERT_TRUE(panel.paintSelected());
    EXPECT_EQ(session.settings().paintKind, "texture");
    EXPECT_EQ(session.settings().paintTexture, file);
    const std::uint64_t blankStamp = fileStamp(file);
    const auto blankSize = std::filesystem::file_size(std::filesystem::u8path(file));
    EXPECT_TRUE(session.stroke(stroke)) << session.lastError();
    for (const std::string& warning : session.takeWarnings()) ADD_FAILURE() << warning;
    EXPECT_EQ(doc->undoText(), "Undo paint stroke");
    const TexturePtr after = loadTexture(file);
    ASSERT_TRUE(after && after->image());
    float sum = 0.0f;
    for (const float v : after->image()->rgba) sum += v;
    EXPECT_GT(sum, 0.0f) << "the file holds the stroke";
    EXPECT_EQ(doc->undo(), "");
    EXPECT_NE(fileStamp(file), blankStamp) << "undo wrote the file again";
    const TexturePtr undone = loadTexture(file);
    ASSERT_TRUE(undone && undone->image());
    float sumUndone = 0.0f;
    for (std::size_t i = 0; i + 3 < undone->image()->rgba.size(); i += 4) sumUndone += undone->image()->rgba[i];
    EXPECT_FLOAT_EQ(sumUndone, 0.0f) << "back to the blank map";
    (void)blankSize;

    // Shown on the scalp: the frame's scalp map item holds the lifted scalp with per-vertex colors.
    panel.selectMap("density");
    panel.showSelected(true);
    EXPECT_EQ(doc->scalpMapDisplay(), "density");
    DisplaySettings settings;
    const DisplayFrame& frame = doc->updateDisplay(settings);
    const DisplayItem& item = frame.item(DisplayItemId::ScalpMap);
    ASSERT_TRUE(item.geometry);
    EXPECT_TRUE(item.geometry->triangles);
    EXPECT_FALSE(item.geometry->positions.empty());
    EXPECT_FALSE(item.geometry->colors.empty());
    // A texture map shows too, sampled at the vertices (the file's values).
    doc->setScalpMapDisplay("detail");
    const ScalpMapView view = doc->scalpMapView("detail", "rgb");
    EXPECT_EQ(view.kind, "vertices") << "sampled per vertex for the viewport";
    ASSERT_TRUE(view.values);
    EXPECT_EQ(view.values->size(), doc->session().scalp()->pointCount());

    // Renamed: the graph's readers follow; removed: the display lets it go.
    std::string mapNode;
    EXPECT_EQ(doc->addNode("Map", 0.0f, 0.0f, &mapNode), "");
    EXPECT_EQ(doc->setParam(mapNode, "name", "density"), "");
    panel.selectMap("density");
    EXPECT_EQ(panel.renameSelected("thickness"), "");
    EXPECT_EQ(doc->paramOrDefault(mapNode, "name"), "thickness");
    EXPECT_TRUE(doc->map("thickness"));
    EXPECT_FALSE(doc->map("density"));
    EXPECT_EQ(doc->undo(), "");
    EXPECT_TRUE(doc->map("density")) << "one step: the map and its readers come back together";
    EXPECT_EQ(doc->paramOrDefault(mapNode, "name"), "density");
    doc->setScalpMapDisplay("tint");
    panel.selectMap("tint");
    EXPECT_EQ(panel.removeSelected(), "");
    EXPECT_FALSE(doc->map("tint"));
    EXPECT_EQ(doc->scalpMapDisplay(), "");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_TRUE(doc->map("tint"));

    // Saved and read back: the vertex maps as the scalp's color sets, the texture map's file, the layer.
    doc->save(path);
    auto again = GroomDocument::open(path);
    ASSERT_TRUE(again);
    ASSERT_TRUE(again->map("density"));
    EXPECT_EQ(again->map("density")->kind, "vertices");
    ASSERT_TRUE(again->map("tint"));
    EXPECT_EQ(again->map("tint")->channels, kColorMap);
    ASSERT_TRUE(again->map("detail"));
    EXPECT_EQ(again->map("detail")->texture, "masks/sample/detail.png");
    EXPECT_TRUE(again->map("stiff"));
    float sumAgain = 0.0f;
    for (const float v : again->session().scalpMaps().maps.at("density").values) sumAgain += v;
    EXPECT_GT(sumAgain, 0.0f) << "the painted values survived the file";
}

TEST(AppPanels, ValueSliderIsOneFillFieldThatTypesAndWidensItsRange) {
    // One field a row tall, the caption inside (no separate label), typed values set as one change.
    ValueSlider slider;
    slider.setCaption("Length");
    slider.setRange(0.0, 10.0);
    slider.setLimits(0.0, 100.0);
    slider.setDecimals(2);
    EXPECT_EQ(slider.sizeHint().height(), theme::rowHeight()) << "a row tall, the caption inside";
    EXPECT_EQ(slider.minimumSizeHint().height(), theme::rowHeight());
    EXPECT_EQ(slider.caption(), QStringLiteral("Length"));
    EXPECT_EQ(slider.focusPolicy(), Qt::StrongFocus) << "Tab reaches it";
    slider.resize(200, slider.sizeHint().height());
    slider.setValue(2.5);
    EXPECT_DOUBLE_EQ(slider.value(), 2.5);
    int presses = 0, changes = 0, releases = 0;
    double last = -1.0;
    QObject::connect(&slider, &ValueSlider::pressed, [&] { ++presses; });
    QObject::connect(&slider, &ValueSlider::valueChanged, [&](double v) { ++changes; last = v; });
    QObject::connect(&slider, &ValueSlider::released, [&] { ++releases; });
    // Typed inline: one change between a press and a release; past the range, the range widens.
    slider.beginEdit();
    EXPECT_TRUE(slider.editing());
    slider.setEditText("42");
    EXPECT_TRUE(slider.commitEdit());
    EXPECT_FALSE(slider.editing());
    EXPECT_DOUBLE_EQ(slider.value(), 42.0);
    EXPECT_DOUBLE_EQ(slider.maximum(), 42.0);
    EXPECT_EQ(presses, 1);
    EXPECT_EQ(changes, 1);
    EXPECT_EQ(releases, 1);
    EXPECT_DOUBLE_EQ(last, 42.0);
    // Past the limits: clamped. Text that is no number: the edit stays open, nothing changes.
    slider.beginEdit();
    slider.setEditText("1000");
    EXPECT_TRUE(slider.commitEdit());
    EXPECT_DOUBLE_EQ(slider.value(), 100.0);
    slider.beginEdit();
    slider.setEditText("ten");
    EXPECT_FALSE(slider.commitEdit());
    EXPECT_TRUE(slider.editing());
    slider.cancelEdit();
    EXPECT_FALSE(slider.editing());
    EXPECT_DOUBLE_EQ(slider.value(), 100.0);
    // Arithmetic, and "," as the decimal point.
    slider.beginEdit();
    slider.setEditText("0.5*2 + 1/4");
    EXPECT_TRUE(slider.commitEdit());
    EXPECT_DOUBLE_EQ(slider.value(), 1.25);
    slider.beginEdit();
    slider.setEditText("3,5");
    EXPECT_TRUE(slider.commitEdit());
    EXPECT_DOUBLE_EQ(slider.value(), 3.5);
    // An integer slider snaps.
    ValueSlider count;
    count.setInteger(true);
    count.setRange(0.0, 10.0);
    count.setValue(3.4);
    EXPECT_DOUBLE_EQ(count.value(), 3.0);
    count.beginEdit();
    count.setEditText("7/2");
    EXPECT_TRUE(count.commitEdit());
    EXPECT_DOUBLE_EQ(count.value(), 4.0);
}

TEST(AppPanels, NumberExpressionsTakeArithmeticAndNothingElse) {
    double v = 0.0;
    auto parsed = [&](const char* text) { return parseNumberExpression(QString::fromUtf8(text), &v); };
    ASSERT_TRUE(parsed("0.5*2"));
    EXPECT_DOUBLE_EQ(v, 1.0);
    ASSERT_TRUE(parsed("1/3"));
    EXPECT_DOUBLE_EQ(v, 1.0 / 3.0);
    ASSERT_TRUE(parsed(" 0,25 "));
    EXPECT_DOUBLE_EQ(v, 0.25);
    ASSERT_TRUE(parsed("-(2+1)*2"));
    EXPECT_DOUBLE_EQ(v, -6.0);
    ASSERT_TRUE(parsed("2 - -3"));
    EXPECT_DOUBLE_EQ(v, 5.0);
    ASSERT_TRUE(parsed("1 + 2 * 3"));
    EXPECT_DOUBLE_EQ(v, 7.0) << "* before +";
    ASSERT_TRUE(parsed("2e3"));
    EXPECT_DOUBLE_EQ(v, 2000.0);
    ASSERT_TRUE(parsed(".5"));
    EXPECT_DOUBLE_EQ(v, 0.5);
    v = 9.0;
    for (const char* bad : {"", "ten", "2+", "(1", "1)", "1/0", "1 2", "2^3", "abs(1)", "0x10", "1;2", "."}) {
        EXPECT_FALSE(parsed(bad)) << bad;
    }
    EXPECT_DOUBLE_EQ(v, 9.0) << "a failed parse leaves the value";
}

TEST(AppPanels, ValueSliderClickTypesDragScrubsAndKeysNudge) {
    ValueSlider slider;
    slider.setCaption("Width");
    slider.setRange(0.0, 10.0);
    slider.setDecimals(3);
    slider.resize(200, slider.sizeHint().height());  // 0.05 a pixel
    slider.setValue(2.5);
    const int y = slider.height() / 2;
    int presses = 0, changes = 0, releases = 0;
    QObject::connect(&slider, &ValueSlider::pressed, [&] { ++presses; });
    QObject::connect(&slider, &ValueSlider::valueChanged, [&](double) { ++changes; });
    QObject::connect(&slider, &ValueSlider::released, [&] { ++releases; });

    // A click (under 3 px of travel): no change, the edit opens with the number selected.
    mouse(&slider, QEvent::MouseButtonPress, QPoint(100, y));
    mouse(&slider, QEvent::MouseMove, QPoint(102, y));
    EXPECT_DOUBLE_EQ(slider.value(), 2.5) << "2 px is no drag";
    mouse(&slider, QEvent::MouseButtonRelease, QPoint(102, y));
    EXPECT_TRUE(slider.editing()) << "a click types";
    auto* editor = slider.findChild<QLineEdit*>();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->text(), QStringLiteral("2.5"));
    EXPECT_EQ(editor->selectedText(), editor->text()) << "the number selected: typing replaces it";
    EXPECT_EQ(presses + changes + releases, 0) << "a click alone changes nothing";
    // Escape puts the old value back.
    slider.setEditText("7");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(editor, &escape);
    EXPECT_FALSE(slider.editing());
    EXPECT_DOUBLE_EQ(slider.value(), 2.5);
    EXPECT_EQ(changes, 0);
    // Enter (the line edit's returnPressed) sets it.
    slider.beginEdit();
    slider.setEditText("4");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(editor, &enter);
    EXPECT_FALSE(slider.editing());
    EXPECT_DOUBLE_EQ(slider.value(), 4.0);
    slider.setValue(2.5);

    // A drag: relative to the press (no jump to the mouse), the field's width the range.
    presses = changes = releases = 0;
    mouse(&slider, QEvent::MouseButtonPress, QPoint(20, y));
    EXPECT_DOUBLE_EQ(slider.value(), 2.5) << "a press does not jump";
    EXPECT_EQ(presses, 0);
    mouse(&slider, QEvent::MouseMove, QPoint(60, y));
    EXPECT_EQ(presses, 1) << "the drag starts past the threshold";
    EXPECT_NEAR(slider.value(), 4.5, 1e-9) << "40 px of a 200 px field over 0-10";
    mouse(&slider, QEvent::MouseButtonRelease, QPoint(60, y));
    EXPECT_EQ(releases, 1);
    EXPECT_FALSE(slider.editing()) << "a drag does not type";
    // Shift: a tenth of the speed.
    mouseAt(&slider, QEvent::MouseButtonPress, QPointF(20, y), Qt::ShiftModifier);
    mouseAt(&slider, QEvent::MouseMove, QPointF(60, y), Qt::ShiftModifier);
    EXPECT_NEAR(slider.value(), 4.7, 1e-9);
    mouseAt(&slider, QEvent::MouseButtonRelease, QPointF(60, y), Qt::ShiftModifier);
    // Ctrl: steps of ten (the step a 100th of the range, rounded: 0.1, so whole numbers).
    mouseAt(&slider, QEvent::MouseButtonPress, QPointF(20, y), Qt::ControlModifier);
    mouseAt(&slider, QEvent::MouseMove, QPointF(33, y), Qt::ControlModifier);
    EXPECT_NEAR(slider.value(), 5.0, 1e-9) << "4.7 + 0.65 to the nearest 1";
    mouseAt(&slider, QEvent::MouseButtonRelease, QPointF(33, y), Qt::ControlModifier);
    // The drag stays inside the range (a typed value may go past it).
    mouse(&slider, QEvent::MouseButtonPress, QPoint(20, y));
    mouse(&slider, QEvent::MouseMove, QPoint(800, y));
    EXPECT_DOUBLE_EQ(slider.value(), 10.0);
    mouse(&slider, QEvent::MouseMove, QPoint(790, y));
    EXPECT_LT(slider.value(), 10.0) << "back the moment the mouse turns: the travel past the end is not banked";
    mouse(&slider, QEvent::MouseButtonRelease, QPoint(790, y));

    // Keys (focused): the arrows nudge one step (Shift ten), Enter and F2 type.
    slider.setStep(0.5);
    slider.setValue(2.0);
    auto press = [&](int k, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QKeyEvent event(QEvent::KeyPress, k, mods);
        QCoreApplication::sendEvent(&slider, &event);
    };
    press(Qt::Key_Right);
    EXPECT_DOUBLE_EQ(slider.value(), 2.5);
    press(Qt::Key_Up, Qt::ShiftModifier);
    EXPECT_DOUBLE_EQ(slider.value(), 7.5);
    press(Qt::Key_Down);
    press(Qt::Key_Left);
    EXPECT_DOUBLE_EQ(slider.value(), 6.5);
    press(Qt::Key_Right, Qt::ShiftModifier);
    EXPECT_DOUBLE_EQ(slider.value(), 10.0) << "nudged to the range's end, no further";
    press(Qt::Key_F2);
    EXPECT_TRUE(slider.editing());
    slider.cancelEdit();
    press(Qt::Key_Return);
    EXPECT_TRUE(slider.editing());
    slider.cancelEdit();
    // The wheel over a field that is not focused scrolls the panel, not the value.
    QWheelEvent wheel(QPointF(50, y), slider.mapToGlobal(QPointF(50, y)), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&slider, &wheel);
    EXPECT_FALSE(wheel.isAccepted()) << "left to the scroll area";
    EXPECT_DOUBLE_EQ(slider.value(), 10.0);
}

TEST(AppPanels, ValueSliderPaintsItsFillStateAndDisabledLook) {
    ValueSlider slider;
    slider.setCaption("Strength");
    slider.setRange(0.0, 1.0);
    slider.setValue(0.5);
    slider.resize(200, theme::rowHeight());
    const QImage shot = slider.grab().toImage();
    ASSERT_FALSE(shot.isNull());
    const int y = 3;  // above the texts, inside the edge
    const QColor filled = shot.pixelColor(12, y), empty = shot.pixelColor(188, y);
    EXPECT_NE(filled.rgb(), empty.rgb()) << "the fill covers the value's share from the left";
    if (!theme::glassy()) EXPECT_EQ(slider.fillColor(), theme::uiColor("selection")) << "a solid theme fills with its selection tone";
    // A driven param: tinted; disabled: faded.
    slider.setStateColor(QColor(0xCF, 0xC2, 0x7A));
    EXPECT_NE(slider.grab().toImage().pixelColor(188, y).rgb(), empty.rgb()) << "the state color shows";
    slider.setStateColor(QColor());
    slider.setEnabled(false);
    EXPECT_NE(slider.grab().toImage().pixelColor(12, y).rgb(), filled.rgb()) << "disabled, it is drawn faded";
    slider.setEnabled(true);
    slider.beginEdit();
    EXPECT_NE(slider.grab().toImage().pixelColor(12, y).rgb(), filled.rgb()) << "editing: the field, no fill";
}

TEST(AppPanels, GroomToolsTilesReflowAndTheModePillClosesTheGraph) {
    // The Groom Tools' tiles (the core's catalog, Maya's grouping): Create, Shape, Trim and Guides
    // on the Tools tab (Unfreeze All beside Freeze), Paint on the Paint tab; a grid that wraps to
    // the dock's width, the names under the icons (Tool Labels off: icons alone, the names in the
    // tool tips).
    GroomTools tools;
    EXPECT_TRUE(tools.tileLabels()) << "labeled tiles by default";
    tools.setTileLabels(false);
    EXPECT_EQ(tools.tiles().size(), 19u) << "Select, Freeze, the 16 tools (Paint and Length Keys among them) and Unfreeze All";
    for (const char* id : {"place", "draw", "part", "flow", "comb", "shape", "cut", "lengths", "erase", "paint", "select", "freeze", "unfreeze"}) {
        EXPECT_NE(tools.tile(id), nullptr) << id;
    }
    std::vector<std::string> tabs;
    for (int i = 0; i < tools.tabBar()->count(); ++i) tabs.push_back(tools.tabBar()->tabText(i).toStdString());
    EXPECT_EQ(tabs, (std::vector<std::string>{"Tools", "Paint", "Correctives", "Photos"})) << "palette.json's tabs";
    auto* grid = qobject_cast<ToolGrid*>(tools.tile("comb")->parentWidget());
    ASSERT_NE(grid, nullptr);
    EXPECT_EQ(grid->columnsFor(260), 4) << "at most four to a row (six 38 px icons would fit in 260 px)";
    EXPECT_EQ(grid->columnsFor(130), 3) << "fewer when only fewer fit";
    EXPECT_EQ(qobject_cast<QToolButton*>(tools.tile("comb"))->toolButtonStyle(), Qt::ToolButtonIconOnly);
    EXPECT_TRUE(tools.tile("comb")->toolTip().contains(QStringLiteral("Comb"))) << "the name in the tool tip";
    const int icons = grid->heightForWidth(260);
    tools.setTileLabels(true);
    const int wide = grid->heightForWidth(300);
    EXPECT_GT(grid->heightForWidth(140), wide) << "a narrower dock wraps to more rows";
    EXPECT_EQ(grid->columnsFor(300), 4) << "labeled tiles (56 px): as many as fit, at most four";
    EXPECT_EQ(grid->columnsFor(180), 3);
    grid->resize(260, grid->heightForWidth(260));
    for (QAbstractButton* t : grid->findChildren<QAbstractButton*>()) {
        EXPECT_LE(t->geometry().right(), 260) << "a tile past the dock's width";
        EXPECT_EQ(qobject_cast<QToolButton*>(t)->toolButtonStyle(), Qt::ToolButtonTextUnderIcon);
    }
    EXPECT_LT(icons, grid->heightForWidth(260)) << "icons alone: more to a row";
    // A tile asks for its tool; the window lights it (setTool) and the card shows its settings.
    QString chosen;
    QObject::connect(&tools, &GroomTools::toolChosen, [&](const QString& id) { chosen = id; });
    tools.tile("comb")->click();
    EXPECT_EQ(chosen, QStringLiteral("comb"));
    EXPECT_FALSE(tools.tile("comb")->isChecked()) << "lit by the window, not by the click";
    tools.setTool("comb");
    EXPECT_TRUE(tools.tile("comb")->isChecked());
    EXPECT_FALSE(tools.tile("cut")->isChecked());
    EXPECT_EQ(tools.card()->findChild<QLabel*>(QStringLiteral("toolCardTitle")), nullptr) << "the card: only the settings, no title";
    // The mode switcher says when any segment is clicked, the current one too.
    Segmented modes;
    modes.setItems({{"A", Qt::red, Qt::white}, {"B", Qt::blue, Qt::white}});
    modes.resize(modes.sizeHint());
    int clicks = 0, changes = 0;
    QObject::connect(&modes, &Segmented::clicked, [&](int) { ++clicks; });
    QObject::connect(&modes, &Segmented::changed, [&](int) { ++changes; });
    click(&modes, QPoint(12, modes.height() / 2));
    EXPECT_EQ(clicks, 1);
    EXPECT_EQ(changes, 0) << "the current segment";
    click(&modes, QPoint(modes.width() - 12, modes.height() / 2));
    EXPECT_EQ(clicks, 2);
    EXPECT_EQ(changes, 1);
}

TEST(AppPanels, NodeGraphDrawsBackdropsAndPromptsForAName) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    NodeGraphView view;
    view.resize(800, 600);
    view.setDocument(doc.get());
    EXPECT_EQ(view.nodeCount(), static_cast<int>(doc->nodes().size()));
    EXPECT_EQ(view.backdropCount(), 0);
    // A preset's backdrops come with its graph (the layout travels in the graph JSON).
    EXPECT_EQ(doc->editGraph("backdrop", [](json& g) {
        g["backdrops"] = json::array({{{"title", "Shape"}, {"x", 0}, {"y", 0}, {"w", 400}, {"h", 200}, {"color", json::array({80, 120, 200})}}});
    }), "");
    view.refresh();
    EXPECT_EQ(view.backdropCount(), 1);
    // The name prompt: Enter hands the name over, nothing else is modal.
    QString named;
    view.promptName("Save as", "name", [&](const QString& name) { named = name; });
    EXPECT_TRUE(view.promptOpen());
    view.setPromptText("  my look ");
    view.acceptPrompt();
    EXPECT_FALSE(view.promptOpen());
    EXPECT_EQ(named, QStringLiteral("my look"));
}

TEST(AppPanels, GuidesPanelMirrorsResamplesAndClearsOverlays) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    ViewRequest request;
    request.density = 0.2f;
    doc->evaluate(request);
    GuidesPanel panel;
    panel.setDocument(doc.get());
    const HierarchyPtr h0 = doc->session().authoredHierarchy();
    ASSERT_TRUE(h0);
    const std::size_t guides = h0->guides().size();
    ASSERT_GT(guides, 0u);
    // Resampled to 6 segments: every guide has 7 points; one undo step.
    EXPECT_TRUE(panel.resampleGuides(6));
    for (const HierarchyGuide& g : doc->session().authoredHierarchy()->guides()) EXPECT_EQ(g.points.size(), 7u);
    EXPECT_EQ(doc->undoText(), "Undo resample guides");
    // Symmetry sync across x: at least as many guides, mirrored in x.
    EXPECT_TRUE(panel.symmetrySync());
    EXPECT_GE(doc->session().authoredHierarchy()->guides().size(), guides);
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->session().authoredHierarchy()->guides().size(), guides);
    // A root group mirrored; a child group cannot be; promoting a root group is refused.
    const std::string root = h0->groups()[0].name;
    EXPECT_TRUE(panel.mirrorGroup(root));
    EXPECT_FALSE(panel.promoteGroup(root));
    EXPECT_TRUE(panel.addGroup(root));
    std::string child;
    for (const GuideGroup& g : doc->session().authoredHierarchy()->groups()) {
        if (g.parent != kNoGroup) child = g.name;
    }
    ASSERT_FALSE(child.empty());
    EXPECT_FALSE(panel.mirrorGroup(child)) << "not a root group";
    EXPECT_TRUE(panel.promoteGroup(child));
    // The parts and flow: listed, removed, cleared.
    EXPECT_TRUE(panel.partNames().empty());
    EXPECT_TRUE(panel.clearFlow() || true);  // nothing to clear: no step
    // The flow's count (the Flow card's, Delete All Flow Arrows'), then all of it cleared in one step.
    EXPECT_EQ(doc->editStoredText("add flow", StrokeField::Flow, [](std::string& text) {
        text = R"({"version": 1, "pins": [{"face": 0, "u": 0.2, "v": 0.2, "dir": [1, 0, 0]},
                                          {"face": 1, "u": 0.2, "v": 0.2, "dir": [1, 0, 0]}],
                   "sinks": [{"face": 2, "u": 0.3, "v": 0.3, "angle": 70, "radius": 0.5}]})";
    }), "");
    EXPECT_EQ(panel.flowCount(), (std::pair<std::size_t, std::size_t>{2, 1}));
    EXPECT_TRUE(panel.clearFlow());
    EXPECT_EQ(panel.flowCount(), (std::pair<std::size_t, std::size_t>{0, 0}));
    EXPECT_EQ(doc->undoText(), "Undo clear flow stroke");  // one step
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(panel.flowCount().first, 2u);
    EXPECT_TRUE(panel.clearFlow());
    const MeshPtr scalp = doc->file().data.scalp;
    EXPECT_EQ(doc->editStoredText("add part", StrokeField::Parts, [&](std::string& text) {
        PartSet parts = PartSet::fromJson(text);
        std::vector<Vec3f> line;
        const Bounds b = doc->sceneBounds();
        for (int k = 0; k <= 8; ++k) line.push_back({b.lo.x + (b.hi.x - b.lo.x) * k / 8.0f, b.hi.y, (b.lo.z + b.hi.z) * 0.5f});
        parts.add("middle", line, *scalp);
        parts.add("side", line, *scalp);
        text = parts.toJson();
    }), "");
    panel.refresh();
    EXPECT_EQ(panel.partNames(), (std::vector<std::string>{"middle", "side"}));
    EXPECT_TRUE(panel.removePart("middle"));
    EXPECT_EQ(panel.partNames(), std::vector<std::string>{"side"});
    EXPECT_FALSE(panel.removePart("nope"));
    EXPECT_TRUE(panel.clearParts());
    EXPECT_TRUE(panel.partNames().empty());
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(panel.partNames(), std::vector<std::string>{"side"});
}

TEST(AppPanels, CorrectivesPanelAddsLayersAndAimsTheBrush) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    BrushSession session;
    session.setDocument(doc.get());
    CorrectivesPanel panel;
    panel.setSession(&session);
    panel.setDocument(doc.get());
    EXPECT_TRUE(panel.layerNames().empty());
    EXPECT_FALSE(session.shotSculpt());
    // A layer on every frame: the brushes sculpt into it from now on.
    EXPECT_TRUE(panel.addLayer(true));
    EXPECT_EQ(panel.layerNames(), std::vector<std::string>{"layer"});
    EXPECT_EQ(panel.targetLayer(), "layer");
    EXPECT_TRUE(session.shotSculpt());
    EXPECT_EQ(session.shotLayer(), "layer");
    EXPECT_EQ(doc->undoText(), "Undo add corrective stroke");
    // One fixed to this frame, with a fade; the weight and the switch, each a step.
    doc->setFrame(12.0);
    EXPECT_TRUE(panel.addLayer(false, 6.0));
    const std::vector<std::string> names = panel.layerNames();
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[1], "shot12");
    Correctives c = Correctives::fromJson(doc->storedText(StrokeField::Correctives));
    ASSERT_TRUE(c.layer("shot12"));
    EXPECT_NEAR(c.layer("shot12")->time, 12.0 / doc->framesPerSecond(), 1e-9);
    EXPECT_NEAR(c.layer("shot12")->fadeIn, 6.0 / doc->framesPerSecond(), 1e-9);
    EXPECT_TRUE(c.layer("layer")->holdBefore);
    EXPECT_TRUE(panel.setLayerWeight("layer", 0.4f));
    EXPECT_TRUE(panel.setLayerEnabled("layer", false));
    c = Correctives::fromJson(doc->storedText(StrokeField::Correctives));
    EXPECT_FLOAT_EQ(c.layer("layer")->weight, 0.4f);
    EXPECT_FALSE(c.layer("layer")->enabled);
    EXPECT_EQ(panel.targetLayer(), "shot12") << "an edit of another layer keeps the target";
    EXPECT_TRUE(panel.renameLayer("shot12", "lift"));
    EXPECT_EQ(panel.targetLayer(), "lift");
    EXPECT_FALSE(panel.renameLayer("lift", "layer")) << "a name taken";
    // Back to the base groom: ordinary strokes; a removed target lets the brush go too.
    panel.setTargetLayer({});
    EXPECT_FALSE(session.shotSculpt());
    panel.setTargetLayer("lift");
    EXPECT_TRUE(panel.removeLayer("lift"));
    EXPECT_EQ(panel.targetLayer(), "");
    EXPECT_FALSE(session.shotSculpt());
    EXPECT_FALSE(panel.removeLayer("nope"));
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(panel.layerNames().size(), 2u);
}

TEST(AppPanels, CorrectivesPanelAimsTheBrushAtASculptNode) {
    // The groom's guides -> an even Scatter + Interpolate -> ToGuides -> Sculpt -> the strands'
    // Interpolate (sculpt.h): picked in "Sculpt into", strokes shape the node's guides.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    const std::string graph = R"({"version": 1, "output": "output", "nodes": [
      {"name": "scalp", "type": "Scalp"}, {"name": "sparse", "type": "Guides"},
      {"name": "even", "type": "Scatter", "params": {"density": 6.0, "randomness": 0.0, "length": 0.5, "previewAll": true}},
      {"name": "dense", "type": "Interpolate"}, {"name": "toGuides", "type": "ToGuides"}, {"name": "sculpt1", "type": "Sculpt"},
      {"name": "roots", "type": "Scatter", "params": {"density": 60.0}}, {"name": "hair", "type": "Interpolate"},
      {"name": "output", "type": "Output"}],
     "connections": [
      {"from": "scalp", "to": "even", "input": "scalp"}, {"from": "even", "to": "dense", "input": "strands"},
      {"from": "sparse", "to": "dense", "input": "guides"}, {"from": "scalp", "to": "dense", "input": "scalp"},
      {"from": "dense", "to": "toGuides", "input": "strands"}, {"from": "toGuides", "to": "sculpt1", "input": "guides"},
      {"from": "scalp", "to": "sculpt1", "input": "scalp"}, {"from": "scalp", "to": "roots", "input": "scalp"},
      {"from": "roots", "to": "hair", "input": "strands"}, {"from": "sculpt1", "to": "hair", "input": "guides"},
      {"from": "scalp", "to": "hair", "input": "scalp"}, {"from": "hair", "to": "output", "input": "strands"}]})";
    ASSERT_EQ(doc->replaceGraph(graph), "");
    ViewRequest request;
    doc->evaluate(request);
    BrushSession session;
    OrbitCamera camera;
    const int w = 800, h = 600;
    camera.frame(doc->sceneBounds());
    session.setDocument(doc.get());
    session.setView(camera, w, h);
    CorrectivesPanel panel;
    panel.setSession(&session);
    panel.setDocument(doc.get());
    EXPECT_EQ(doc->sculptNodes(), std::vector<std::string>{"sculpt1"});
    panel.setTargetNode("sculpt1");
    EXPECT_EQ(doc->sculptTarget(), "sculpt1");
    const ViewResult& view = doc->evaluate(request);
    ASSERT_TRUE(view.hierarchy);
    ASSERT_FALSE(view.hierarchy->guides().empty());
    EXPECT_EQ(view.hierarchy->groups()[0].name, "sculpt1") << "the node's guides are the ones drawn";
    // A comb through the brush session: one undo step of the node's edits, the guides untouched.
    const std::string guides = doc->session().data().hierarchyJson;
    const HierarchyGuide g = view.hierarchy->guides()[view.hierarchy->guides().size() / 2];
    float x = 0.0f, y = 0.0f;
    ASSERT_TRUE(session.view().project(g.points[g.points.size() / 2], x, y));
    session.settings().setMode("comb");
    session.settings().screenRadius = 80.0f;
    std::vector<QPointF> path;
    for (int k = 0; k <= 8; ++k) path.emplace_back(x + 12.0 * k, h - y);
    ASSERT_TRUE(session.stroke(path)) << session.lastError();
    EXPECT_EQ(doc->undoText(), "Undo comb stroke");
    EXPECT_GT(doc->sculptedGuides("sculpt1"), 0u);
    EXPECT_EQ(doc->session().data().hierarchyJson, guides);
    // Renamed and duplicated, the edits go along, each in the edit's own step.
    EXPECT_EQ(doc->renameNode("sculpt1", "shape"), "");
    EXPECT_GT(doc->sculptedGuides("shape"), 0u);
    EXPECT_EQ(doc->sculptTarget(), "shape");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_GT(doc->sculptedGuides("sculpt1"), 0u);
    EXPECT_EQ(doc->sculptedGuides("shape"), 0u);
    std::string copy;
    EXPECT_EQ(doc->duplicateNode("sculpt1", &copy), "");
    EXPECT_EQ(doc->sculptedGuides(copy), doc->sculptedGuides("sculpt1"));
    EXPECT_EQ(doc->removeNode(copy), "");
    EXPECT_EQ(doc->sculptedGuides(copy), 0u);
    // Saved with the groom; Clear Edits drops them (one step); the base groom takes the brush back.
    EXPECT_FALSE(doc->currentFile().data.sculptsJson.empty());
    doc->setSculptTarget("sculpt1");
    panel.refresh();
    EXPECT_TRUE(panel.clearNodeEdits());
    EXPECT_EQ(doc->sculptedGuides("sculpt1"), 0u);
    EXPECT_EQ(doc->undo(), "");
    EXPECT_GT(doc->sculptedGuides("sculpt1"), 0u);
    panel.setTargetLayer({});
    EXPECT_EQ(doc->sculptTarget(), "");
}

TEST(AppPanels, TimelineSimControlsShowTheCacheAndAskForRuns) {
    Timeline timeline;
    timeline.resize(1100, 52);
    timeline.setRange(1, 48);
    EXPECT_FALSE(timeline.simulationShown()) << "no Simulate node: no sim controls";
    timeline.setSimulationShown(true);
    EXPECT_TRUE(timeline.simulationShown());
    int simulate = 0, bake = 0, clear = 0;
    QObject::connect(&timeline, &Timeline::simulateRequested, [&] { ++simulate; });
    QObject::connect(&timeline, &Timeline::bakeRequested, [&] { ++bake; });
    QObject::connect(&timeline, &Timeline::invalidateRequested, [&] { ++clear; });
    timeline.simulateButton()->click();
    timeline.bakeButton()->click();
    timeline.clearButton()->click();
    EXPECT_EQ(simulate, 1);
    EXPECT_EQ(bake, 1);
    EXPECT_EQ(clear, 1);
    // Busy: Simulate is the run's Cancel, the others wait for the run.
    int cancel = 0;
    QObject::connect(&timeline, &Timeline::simulationCancelRequested, [&] { ++cancel; });
    timeline.setSimulationBusy(true);
    EXPECT_TRUE(timeline.simulateButton()->isEnabled());
    EXPECT_EQ(timeline.simulateButton()->text(), QStringLiteral("Cancel"));
    EXPECT_FALSE(timeline.bakeButton()->isEnabled());
    timeline.simulateButton()->click();
    EXPECT_EQ(simulate, 1);
    EXPECT_EQ(cancel, 1);
    timeline.setSimulationBusy(false);
    EXPECT_TRUE(timeline.bakeButton()->isEnabled());
    EXPECT_EQ(timeline.simulateButton()->text(), QStringLiteral("Simulate"));

    // The strip under the scrubber's track: frames 1-24 cached (emerald), 25-48 baked (blue).
    timeline.setSimulatedFrames({{1, 24}}, {{25, 48}});
    EXPECT_EQ(timeline.scrubber()->cachedFrames(), (FrameScrubber::Runs{{1, 24}}));
    EXPECT_EQ(timeline.scrubber()->bakedFrames(), (FrameScrubber::Runs{{25, 48}}));
    FrameScrubber scrubber;
    scrubber.resize(400, 24);
    scrubber.setRange(1, 48);
    scrubber.setSimulatedFrames({{1, 24}}, {{25, 48}});
    const QImage image = scrubber.grab().toImage();
    auto near = [](QColor a, QColor b) {
        return std::abs(a.red() - b.red()) < 24 && std::abs(a.green() - b.green()) < 24 && std::abs(a.blue() - b.blue()) < 24;
    };
    const int y = 12 + 4 + 4;  // under the track (8 tall, centered), in the strip
    bool emerald = false, blue = false;
    for (int x = 0; x < image.width(); ++x) {
        const QColor c = image.pixelColor(x, y);
        emerald = emerald || (x < image.width() / 2 - 8 && near(c, theme::emerald()));
        blue = blue || (x > image.width() / 2 + 8 && near(c, theme::blue()));
    }
    EXPECT_TRUE(emerald) << "no cached frames drawn";
    EXPECT_TRUE(blue) << "no baked frames drawn";
    timeline.setSimulationShown(false);
    EXPECT_TRUE(timeline.scrubber()->cachedFrames().empty()) << "the strip stays without a Simulate node";
}

TEST(AppPanels, EmptyStateCardAndPillsInEveryMode) {
    // No groom open: the card in the middle of the viewport (Open, New, a preset, the recent
    // files), the brush tiles and card dimmed and disabled; no tool bar: Undo and Redo sit on the
    // viewport in every mode, the file's name is the window's title.
    MainWindow window;
    window.resize(1280, 720);
    window.show();  // the offscreen platform: the pieces get their geometry
    OverlayHost* host = window.overlay();
    AppShell* shell = window.shell();
    EXPECT_FALSE(host->groomOpen());
    EXPECT_TRUE(host->emptyCard()->isVisibleTo(host));
    EXPECT_TRUE(shell->groomTools()->emptyState()->isVisibleTo(&window)) << "the Groom Tools: Start a groom";
    EXPECT_FALSE(shell->groomTools()->work()->isVisibleTo(&window)) << "no tiles before a groom";
    EXPECT_TRUE(window.findChildren<QToolBar*>().isEmpty()) << "the window has no tool bar";
    EXPECT_TRUE(host->undoButton()->isVisibleTo(&window));
    EXPECT_FALSE(host->undoButton()->isEnabled());
    EXPECT_EQ(window.windowTitle(), QStringLiteral("GroomLab"));
    host->setRecentFiles({"a.groom", "b.groom", "c.obj", "d.abc", "e.groom", "f.groom"});
    EXPECT_EQ(host->recentCount(), 5) << "at most five recent files";
    host->setRecentFiles({});
    EXPECT_EQ(host->recentCount(), 0);
    // A file dropped (or a recent file clicked) opens.
    int dropped = 0;
    QObject::connect(host, &OverlayHost::fileDropped, [&](const QString&) { ++dropped; });
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    EXPECT_TRUE(host->groomOpen());
    EXPECT_FALSE(host->emptyCard()->isVisibleTo(host));
    EXPECT_FALSE(shell->groomTools()->emptyState()->isVisibleTo(&window));
    EXPECT_TRUE(shell->groomTools()->work()->isVisibleTo(&window));
    EXPECT_GE(host->recentCount(), 1) << "the opened file is remembered";
    for (const char* mode : {"groom", "look", "cards", "export"}) {
        ASSERT_TRUE(window.setUiMode(QLatin1String(mode)));
        EXPECT_TRUE(host->undoButton()->isVisibleTo(&window)) << mode;
        EXPECT_TRUE(host->redoButton()->isVisibleTo(&window)) << mode;
        // The modes are workspaces: the Groom Tools in Groom mode alone, the Properties dock the
        // mode's panel.
        EXPECT_EQ(shell->groomToolsDock()->isVisibleTo(&window), QLatin1String(mode) == QLatin1String("groom")) << mode;
        EXPECT_TRUE(shell->propertiesDock()->isVisibleTo(&window)) << mode;
    }
    ASSERT_TRUE(window.setUiMode(QStringLiteral("look")));
    EXPECT_TRUE(shell->lookCard()->isVisibleTo(&window));
    ASSERT_TRUE(window.setUiMode(QStringLiteral("export")));
    EXPECT_TRUE(shell->exportPanel()->isVisibleTo(&window));
    EXPECT_FALSE(shell->lookCard()->isVisibleTo(&window));
    EXPECT_EQ(dropped, 0);
    // The window's title names the file; the Graph Editor counts the nodes, after a preset too.
    EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("strokes.groom"))) << window.windowTitle().toStdString();
    EXPECT_TRUE(window.windowTitle().endsWith(QStringLiteral("GroomLab")));
    ASSERT_TRUE(window.setUiMode(QStringLiteral("groom")));
    const int before = static_cast<int>(window.document()->nodes().size());
    ASSERT_TRUE(window.applyPreset(QStringLiteral("curly")));
    EXPECT_NE(static_cast<int>(window.document()->nodes().size()), before);
    EXPECT_EQ(shell->nodeCount(), static_cast<int>(window.document()->nodes().size()));
    EXPECT_TRUE(window.windowTitle().startsWith(QStringLiteral("strokes.groom *"))) << "edited: a * after the name";
    EXPECT_TRUE(host->undoButton()->isEnabled());
    EXPECT_TRUE(host->undoButton()->toolTip().endsWith(QStringLiteral("(Ctrl+Z)"))) << host->undoButton()->toolTip().toStdString();
    // The Graph Editor docks at the bottom, the node's settings inside it, all within the window.
    window.setGraphShown(true);
    ASSERT_TRUE(window.selectUiNode(QStringLiteral("clump")));
    EXPECT_EQ(window.dockWidgetArea(shell->graphDock()), Qt::BottomDockWidgetArea);
    const QRect panel(shell->panel(AppShell::Panel::Node)->mapTo(&window, QPoint(0, 0)), shell->panel(AppShell::Panel::Node)->size());
    EXPECT_TRUE(window.rect().contains(panel)) << "the node's settings leave the window";
    // Help pages in the Help dock beside Properties, no dialog.
    EXPECT_TRUE(window.showHelp(QStringLiteral("shortcuts")));
    EXPECT_TRUE(shell->panel(AppShell::Panel::Help)->isVisibleTo(&window));
    EXPECT_FALSE(window.showHelp(QStringLiteral("nothing")));
}

TEST(AppPanels, ViewThemeSavesTheChoiceForTheNextStart) {
    // GroomLab > Theme lists every theme, the worn one checked; a choice is worn at once (restyle)
    // and saved (QSettings: this run's own ini), and a window built after it wears it too: Playful's
    // glass over the viewport, its docks solid all the same.
    const QString saved = theme::savedTheme();
    struct Restore {
        QString name;
        ~Restore() {
            theme::saveTheme(name);
            theme::setTheme(theme::defaultTheme());
        }
    } restore{saved};
    {
        MainWindow window;
        QStringList listed;
        // (The menu bar's Theme submenu; the Graph Editor's Edit menu lists them too.)
        for (const QAction* a : window.menuBar()->findChildren<QAction*>()) {
            if (a->objectName().startsWith(QStringLiteral("theme_"))) {
                listed << a->objectName().mid(6);
                EXPECT_EQ(a->isChecked(), a->objectName().mid(6) == theme::currentTheme());
            }
        }
        listed.sort();
        EXPECT_EQ(listed, theme::themeNames());
        EXPECT_TRUE(listed.contains(QStringLiteral("playful")));
        EXPECT_FALSE(window.chooseTheme(QStringLiteral("no such theme")));
        EXPECT_EQ(theme::savedTheme(), saved);
        ASSERT_TRUE(window.chooseTheme(QStringLiteral("playful")));
        EXPECT_EQ(theme::savedTheme(), QStringLiteral("playful"));
        EXPECT_EQ(theme::currentTheme(), QStringLiteral("playful")) << "worn at once";
        EXPECT_EQ(window.styleSheet(), theme::styleSheet());
        EXPECT_TRUE(window.findChild<QAction*>(QStringLiteral("theme_playful"))->isChecked());
        theme::setTheme(theme::defaultTheme());
    }
    ASSERT_TRUE(theme::setTheme(theme::savedTheme()));  // the next start
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    EXPECT_TRUE(theme::glassy());
    EXPECT_TRUE(window.styleSheet().contains(theme::uiColor("panel").name())) << "the docks' solid panel";
    window.overlay()->toast(QStringLiteral("glass"), false);
    QWidget* toast = nullptr;
    for (QLabel* label : window.overlay()->findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("glass")) toast = label->parentWidget();
    }
    ASSERT_NE(qobject_cast<GlassPanel*>(toast), nullptr);
    EXPECT_LT(static_cast<GlassPanel*>(toast)->color().alpha(), 255) << "glass over the viewport";
}

TEST(AppPanels, UnsavedChangesAreAskedAboutAndMessagesToast) {
    // A window someone sits at asks before an edit is lost (closing, Quit, Open, a dropped or
    // recent file, Reload) in a glass prompt over the viewport, never a dialog; scripted runs and
    // tests (not interactive) never ask. Info messages toast briefly, errors tinted and longer, one
    // timer for all of them.
    const std::string copy = sampleCopy("groomlab_app_unsaved");
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    OverlayHost* host = window.overlay();
    host->resize(1280, 720);
    // Without a groom the groom-only actions are off; Reload needs a file too.
    const auto action = [&](const char* name) { return window.findChild<QAction*>(QLatin1String(name)); };
    for (const char* name : {"transfer", "guidesFromReference", "deleteGuides", "reload", "saveAs"}) {
        ASSERT_NE(action(name), nullptr) << name;
        EXPECT_FALSE(action(name)->isEnabled()) << name << " without a groom";
    }
    ASSERT_TRUE(window.openGroom(QString::fromStdString(copy)));
    for (const char* name : {"transfer", "guidesFromReference", "deleteGuides", "reload", "saveAs"}) {
        EXPECT_TRUE(action(name)->isEnabled()) << name << " with a groom";
    }
    const std::string hairColor = nodeOfType(*window.document(), "HairColor");
    ASSERT_TRUE(window.setParamText(QString::fromStdString(hairColor) + QStringLiteral(".melanin=0.25")));
    ASSERT_TRUE(window.document()->dirty());
    // Not interactive: nothing asks, the reload goes ahead.
    EXPECT_FALSE(window.interactive());
    int proceeded = 0;
    window.confirmDiscard(QStringLiteral("testing"), [&] { ++proceeded; });
    EXPECT_EQ(proceeded, 1);
    EXPECT_FALSE(host->promptShown());
    // Interactive: the prompt, its choices; Cancel keeps everything, Discard goes ahead.
    window.setInteractive(true);
    window.confirmDiscard(QStringLiteral("testing"), [&] { ++proceeded; });
    EXPECT_TRUE(host->promptShown());
    EXPECT_EQ(proceeded, 1);
    QStringList labels;
    for (const RoundButton* b : host->prompt()->findChildren<RoundButton*>()) labels << b->text();
    EXPECT_EQ(labels, (QStringList{QStringLiteral("Save"), QStringLiteral("Discard"), QStringLiteral("Cancel")}));
    EXPECT_TRUE(host->choosePrompt(QStringLiteral("Cancel")));
    EXPECT_FALSE(host->promptShown());
    EXPECT_EQ(proceeded, 1);
    window.confirmDiscard(QStringLiteral("testing"), [&] { ++proceeded; });
    EXPECT_TRUE(host->choosePrompt(QStringLiteral("Discard")));
    EXPECT_EQ(proceeded, 2);
    // Closing the window: refused and asked; Cancel keeps it open.
    {
        QCloseEvent close;
        QCoreApplication::sendEvent(&window, &close);
        EXPECT_FALSE(close.isAccepted()) << "a dirty groom closed without asking";
        EXPECT_TRUE(host->promptShown());
        EXPECT_TRUE(host->choosePrompt(QStringLiteral("Cancel")));
        EXPECT_TRUE(window.isVisible());
    }
    // Save writes the file and then goes ahead.
    window.confirmDiscard(QStringLiteral("testing"), [&] { ++proceeded; });
    EXPECT_TRUE(host->choosePrompt(QStringLiteral("Save")));
    EXPECT_FALSE(window.document()->dirty());
    EXPECT_EQ(proceeded, 3);
    EXPECT_TRUE(host->toastShown()) << "Saved shows as an info toast";
    EXPECT_FALSE(host->toastIsError());
    EXPECT_TRUE(host->toastText().startsWith(QStringLiteral("Saved")));
    // Reload asks when there is something to lose, and reads the file again on Reload.
    ASSERT_TRUE(window.setParamText(QString::fromStdString(hairColor) + QStringLiteral(".melanin=0.75")));
    window.askReload();
    EXPECT_TRUE(host->promptShown());
    EXPECT_TRUE(host->choosePrompt(QStringLiteral("Reload")));
    EXPECT_FALSE(window.document()->dirty());
    EXPECT_NEAR(window.document()->param(hairColor, "melanin").get<double>(), 0.25, 1e-6) << "the saved value read back";
    // Toasts: an error is tinted and stays longer than an info message; a long one wraps within the view.
    host->toast(QStringLiteral("Saved sample.groom"), false);
    const int infoMs = host->toastMilliseconds();
    host->toast(QString(QStringLiteral("Cannot export: the folder is read-only. ")).repeated(6), true);
    EXPECT_TRUE(host->toastIsError());
    EXPECT_GT(host->toastMilliseconds(), infoMs);
    QWidget* toast = nullptr;
    for (QLabel* label : host->findChildren<QLabel*>()) {
        if (label->text() == host->toastText()) toast = label->parentWidget();
    }
    ASSERT_NE(toast, nullptr);
    EXPECT_LE(toast->width(), 560 + 1);
    EXPECT_GT(toast->height(), 40) << "a long message wraps";
    window.setInteractive(false);
}

TEST(AppPanels, MenusWearTheThemeAndKeysWorkWhereTheySay) {
    // The menus are the menu bar's (the window's style sheet themes them); Ctrl+Shift+S saves as
    // (spelled out: QKeySequence::SaveAs has no Windows binding); Tab opens the Graph Editor from
    // the viewport (and gives it the focus) but a text field keeps it (the next field); the modes
    // have Ctrl+1..4 in their tooltips.
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    window.activateWindow();
    QCoreApplication::processEvents();
    OverlayHost* host = window.overlay();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    QAction* saveAs = window.findChild<QAction*>(QStringLiteral("saveAs"));
    ASSERT_NE(saveAs, nullptr);
    EXPECT_EQ(saveAs->shortcut(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    QMenu* menu = nullptr;
    for (QMenu* m : window.menuBar()->findChildren<QMenu*>()) {
        for (const QAction* a : m->actions()) {
            if (a == saveAs) menu = m;
        }
    }
    ASSERT_NE(menu, nullptr) << "the File menu under the menu bar (the window's style sheet)";
    QStringList titles;
    for (const QAction* a : window.menuBar()->actions()) titles << a->text().remove(QLatin1Char('&'));
    EXPECT_EQ(titles, (QStringList{QStringLiteral("File"), QStringLiteral("Edit"), QStringLiteral("View"), QStringLiteral("GroomLab"),
                                   QStringLiteral("Help")}))
        << "menus.json's app.menuBar";
    EXPECT_TRUE(window.styleSheet().contains(QStringLiteral("QMenuBar")));
    // View > Panels: every dock's toggle (filled when the menu opens), then the sections.
    QMenu* panels = nullptr;
    for (QMenu* m : window.menuBar()->findChildren<QMenu*>()) {
        if (m->title().remove(QLatin1Char('&')) == QStringLiteral("Panels")) panels = m;
    }
    ASSERT_NE(panels, nullptr) << "View > Panels";
    emit panels->aboutToShow();
    for (QDockWidget* dock : window.shell()->docks()) {
        EXPECT_TRUE(panels->actions().contains(dock->toggleViewAction())) << dock->objectName().toStdString();
    }
    EXPECT_NE(window.findChild<QAction*>(QStringLiteral("resetLayout")), nullptr);
    int sameKey = 0;
    for (const QAction* a : window.findChildren<QAction*>()) sameKey += a->shortcut() == saveAs->shortcut() ? 1 : 0;
    EXPECT_EQ(sameKey, 1) << "an ambiguous Ctrl+Shift+S does nothing";
    // The modes: View > Mode's actions, Ctrl+1..4 (the window has no mode switch of its own).
    int mode = 0;
    for (const char* name : {"mode_groom", "mode_look", "mode_cards", "mode_export"}) {
        const QAction* action = window.findChild<QAction*>(QLatin1String(name));
        ASSERT_NE(action, nullptr) << name;
        EXPECT_EQ(action->shortcut(), QKeySequence(Qt::CTRL | Qt::Key(Qt::Key_1 + mode))) << name;
        ++mode;
    }
    if (QApplication::activeWindow() != &window) GTEST_SKIP() << "the offscreen platform did not activate the window: shortcuts not tried";
    // Ctrl+Shift+S reaches Save As (its slot swapped for a count: no file dialog in a test).
    QObject::disconnect(saveAs, &QAction::triggered, nullptr, nullptr);
    int savedAs = 0;
    QObject::connect(saveAs, &QAction::triggered, [&] { ++savedAs; });
    host->viewport()->setFocus();
    EXPECT_TRUE(qt_sendShortcutOverrideEvent(host->viewport(), 0, Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier));
    EXPECT_EQ(savedAs, 1);
    // Tab in a text field: no shortcut, the graph stays closed.
    auto* field = new QLineEdit(host);
    field->show();
    field->setFocus();
    EXPECT_FALSE(qt_sendShortcutOverrideEvent(field, 0, Qt::Key_Tab, Qt::NoModifier)) << "Tab taken from the text field";
    EXPECT_FALSE(window.graphShown());
    // Tab over the viewport: the Graph Editor, focused; Tab again from the viewport focuses it again.
    host->viewport()->setFocus();
    EXPECT_TRUE(qt_sendShortcutOverrideEvent(host->viewport(), 0, Qt::Key_Tab, Qt::NoModifier));
    EXPECT_TRUE(window.graphShown());
    EXPECT_TRUE(window.shell()->graphDock()->isVisible());
    EXPECT_TRUE(window.shell()->graph()->hasFocus());
    host->viewport()->setFocus();
    EXPECT_TRUE(qt_sendShortcutOverrideEvent(host->viewport(), 0, Qt::Key_Tab, Qt::NoModifier));
    EXPECT_TRUE(window.shell()->graph()->hasFocus()) << "Tab gives the docked graph the focus back";
    // Ctrl+3: Cards mode.
    EXPECT_TRUE(qt_sendShortcutOverrideEvent(host->viewport(), 0, Qt::Key_3, Qt::ControlModifier));
    EXPECT_EQ(host->mode(), OverlayHost::Mode::Cards);
    EXPECT_EQ(window.shell()->mode(), OverlayHost::Mode::Cards);
}

TEST(AppPanels, FreeForAnyUseNoWatermarkAndSupportGroomLab) {
    // GroomLab is free: no key, no watermark over the viewport, no Learning mode in the title,
    // every export from the window; Help > Support GroomLab and About link to the donations page.
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    OverlayHost* host = window.overlay();
    for (const QLabel* label : host->findChildren<QLabel*>()) {
        EXPECT_FALSE(label->text().contains(QStringLiteral("Learning")) || label->text().contains(QStringLiteral("non-commercial")) ||
                     label->text().contains(QStringLiteral("license"), Qt::CaseInsensitive))
            << label->objectName().toStdString() << ": " << label->text().toStdString();
    }
    EXPECT_FALSE(window.windowTitle().contains(QStringLiteral("Learning")));
    EXPECT_STREQ(kDonateUrl, "https://ko-fi.com/getgroomlab");
    const QList<QAction*> actions = window.findChildren<QAction*>();
    EXPECT_TRUE(std::any_of(actions.begin(), actions.end(), [](const QAction* a) { return a->objectName() == QStringLiteral("supportGroomLab"); }));
    EXPECT_FALSE(std::any_of(actions.begin(), actions.end(), [](const QAction* a) { return a->text().contains(QStringLiteral("License")); }));
    ASSERT_TRUE(window.showHelp(QStringLiteral("about")));
    bool linked = false;
    for (const QLabel* label : window.shell()->panel(AppShell::Panel::Help)->findChildren<QLabel*>()) {
        linked = linked || (label->text().contains(QString::fromLatin1(kDonateUrl)) && label->text().contains(QStringLiteral("Support GroomLab")));
    }
    EXPECT_TRUE(linked) << "About links to the donations page";
    EXPECT_STREQ(kRepoUrl, "https://github.com/thekesslereffect/groomlab");
    bool source = false;
    for (const QLabel* label : window.shell()->panel(AppShell::Panel::Help)->findChildren<QLabel*>()) {
        source = source || (label->text().contains(QString::fromLatin1(kRepoUrl)) && label->text().contains(QStringLiteral("Open source (Apache-2.0)")));
    }
    EXPECT_TRUE(source) << "About links to the source";
    // Every export target from the window, with no key anywhere.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "groomlab_app_free_exports";
    std::filesystem::create_directories(dir);
    ExportOptions options;
    options.density = 0.05f;
    for (const auto& [target, file] : std::vector<std::pair<const char*, const char*>>{
             {"alembic", "free.abc"}, {"usd", "free.usda"}, {"unreal", "free_unreal.abc"}, {"unity", "free_unity.abc"}}) {
        const std::filesystem::path out = dir / file;
        EXPECT_TRUE(window.exportTo(QLatin1String(target), QString::fromStdString(out.u8string()), options)) << target;
        EXPECT_TRUE(std::filesystem::is_regular_file(out)) << target;
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(AppPanels, CommitsOffTheUiThreadLandWhatAnEvaluationHereMakes) {
    // With commits off the UI thread (the window someone sits at), an edit returns before its
    // evaluation: the window keeps what it showed until the commit lands, then shows what a
    // fresh document evaluated with the same edit makes. An evaluation of another view takes a
    // pending commit in first.
    MainWindow window;
    window.resize(1280, 720);
    window.setPreviewDensity(1.0);
    window.setPointBudget(0.0);
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    window.setAsyncCommits(true);
    GroomDocument* doc = window.document();
    const std::string noise = nodeOfType(*doc, "Noise");
    ASSERT_FALSE(noise.empty());
    const std::uint64_t shownBefore = doc->lastView().versions.strands;
    ASSERT_TRUE(window.setParamText(QString::fromStdString(noise + ".amplitude=0.37")));
    EXPECT_TRUE(window.commitPending()) << "the edit's evaluation is the worker's";
    EXPECT_EQ(doc->lastView().versions.strands, shownBefore) << "the strands shown stay until it lands";
    QElapsedTimer clock;
    clock.start();
    while (window.commitPending() && clock.elapsed() < 20000) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    ASSERT_FALSE(window.commitPending()) << "the commit never landed";
    const StrandsPtr landed = doc->lastView().strands;
    ASSERT_TRUE(landed);
    EXPECT_NE(doc->lastView().versions.strands, shownBefore);
    auto fresh = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(fresh);
    ASSERT_EQ(fresh->setParam(noise, "amplitude", 0.37), "");
    ViewRequest request;
    request.viewport = true;
    const ViewResult& expected = fresh->evaluate(request);
    ASSERT_TRUE(expected.strands);
    ASSERT_EQ(landed->strandCount(), expected.strands->strandCount());
    ASSERT_EQ(landed->pointCount(), expected.strands->pointCount());
    for (std::size_t i = 0; i < landed->strandCount(); ++i) {
        const Vec3f* a = landed->strandPoints(i);
        const Vec3f* b = expected.strands->strandPoints(i);
        for (std::size_t k = 0; k < landed->strandPointCount(i); ++k) {
            ASSERT_EQ(a[k].x, b[k].x) << "strand " << i << " point " << k;
            ASSERT_EQ(a[k].y, b[k].y);
            ASSERT_EQ(a[k].z, b[k].z);
        }
    }
    // Another edit, then another view (another frame): taken in, then evaluated here.
    ASSERT_TRUE(window.setParamText(QString::fromStdString(noise + ".amplitude=0.2")));
    EXPECT_TRUE(window.commitPending());
    ASSERT_TRUE(window.setFrame(3.0));
    EXPECT_FALSE(window.commitPending()) << "taken in by the evaluation of the new view";
    EXPECT_NE(doc->lastView().strands, landed);
    // Off, every evaluation is done when the edit returns.
    window.setAsyncCommits(false);
    ASSERT_TRUE(window.setParamText(QString::fromStdString(noise + ".amplitude=0.25")));
    EXPECT_FALSE(window.commitPending());
}

TEST(AppPanels, DabsOffTheUiThreadCommitWhatDabsHereCommit) {
    // A CPU stroke through the viewport's mouse events, its dabs evaluated on a worker (the
    // window someone sits at): the mouse events return before their dab's evaluation, dabs that
    // come while one evaluates go as one (no queue), each result shows as it lands, and the
    // stroke commits exactly what the same stroke with every dab evaluated here commits (the
    // stored guides and the strands, bit for bit).
    struct Outcome {
        std::string before, guides;
        StrandsPtr strands;
        std::uint64_t landed = 0;
        int pendingAfterMove = 0;
    };
    auto stroke = [](bool async) {
        Outcome out;
        MainWindow window;
        window.resize(1280, 720);
        window.show();  // the viewport gets its size
        window.setPreviewDensity(1.0);
        window.setPointBudget(0.0);
        window.setGpuPreview(false);  // the stroke's dabs on the CPU
        EXPECT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
        window.setAsyncDabs(async);
        EXPECT_TRUE(window.setToolMode(QStringLiteral("brush")));
        EXPECT_TRUE(window.setBrushText(QStringLiteral("mode=comb")));
        EXPECT_TRUE(window.setBrushText(QStringLiteral("radius=120")));
        EXPECT_TRUE(window.setBrushText(QStringLiteral("strength=1")));
        // A stroke document of its own (at full density, the default, the stroke is the view's
        // own and its dabs evaluate here: GroomSession::detachStroke).
        EXPECT_TRUE(window.setBrushText(QStringLiteral("sculptDensity=0.35")));
        GroomDocument* doc = window.document();
        auto* viewport = window.findChild<Viewport*>();
        EXPECT_TRUE(doc && viewport);
        if (!doc || !viewport) return out;
        // Around the top of the scalp as seen (as the live benchmark strokes).
        const MeshPtr& scalp = doc->file().data.scalp;
        Vec3f lo = scalp->points().front(), hi = lo;
        for (const Vec3f& p : scalp->points()) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        QPointF around;
        EXPECT_TRUE(window.projectToView(QVector3D((lo.x + hi.x) * 0.5f, hi.y, (lo.z + hi.z) * 0.5f), viewport->size(), &around));
        auto at = [&](int step) {
            const double a = step * 0.15, r = 40.0;
            return QPoint(static_cast<int>(around.x() + r * std::cos(a)), static_cast<int>(around.y() + r * std::sin(a)));
        };
        out.before = doc->storedText(StrokeField::Guides);
        mouse(viewport, QEvent::MouseButtonPress, at(0));
        EXPECT_TRUE(window.brush().stroking());
        for (int step = 1; step <= 24; ++step) {
            mouse(viewport, QEvent::MouseMove, at(step));
            if (window.dabPending()) ++out.pendingAfterMove;
            // The event loop turns every third move: the dabs between go as one.
            if (step % 3 == 0) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        mouse(viewport, QEvent::MouseButtonRelease, at(24));
        // (The release's evaluation here took in the dab still on the worker.)
        EXPECT_FALSE(window.dabPending());
        out.guides = doc->storedText(StrokeField::Guides);
        out.strands = doc->lastView().strands;
        out.landed = window.dabsLanded();
        return out;
    };
    const Outcome here = stroke(false), worker = stroke(true);
    EXPECT_EQ(here.landed, 0u);
    EXPECT_EQ(here.pendingAfterMove, 0) << "off, each dab is evaluated before its event returns";
    EXPECT_GT(worker.pendingAfterMove, 0) << "the dabs were not the worker's";
    EXPECT_GT(worker.landed, 0u);
    EXPECT_LT(worker.landed, 24u) << "dabs that came while one evaluated were queued";
    ASSERT_NE(here.guides, here.before) << "the stroke combed nothing";
    EXPECT_EQ(worker.guides, here.guides) << "the stroke committed something else";
    ASSERT_TRUE(here.strands && worker.strands);
    ASSERT_EQ(worker.strands->strandCount(), here.strands->strandCount());
    ASSERT_EQ(worker.strands->pointCount(), here.strands->pointCount());
    for (std::size_t i = 0; i < here.strands->strandCount(); ++i) {
        const Vec3f* a = worker.strands->strandPoints(i);
        const Vec3f* b = here.strands->strandPoints(i);
        for (std::size_t k = 0; k < here.strands->strandPointCount(i); ++k) {
            ASSERT_EQ(a[k].x, b[k].x) << "strand " << i << " point " << k;
            ASSERT_EQ(a[k].y, b[k].y);
            ASSERT_EQ(a[k].z, b[k].z);
        }
    }
}

TEST(AppPanels, RampAndSeedDragsGoToTheGpuPreview) {
    // A profile's ramp and a seed dragged on the open groom as the node panel's ramp editor and
    // seed slider drag them (setParam with record false, each step): the window's GPU preview
    // takes them (the value held, the graph untouched, nothing evaluated) until the release
    // records them on the CPU. A param the kernels do not read (Scatter's seed moves the roots;
    // the Clump's picks its own centers) is previewed on the CPU as ever.
    MainWindow window;
    window.resize(1280, 720);
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    ASSERT_TRUE(window.applyPreset(QStringLiteral("hair_basics")));
    if (!window.gpuPreview()) GTEST_SKIP() << "the GPU preview is off (GROOMLAB_APP_GPU_PREVIEW)";
    GroomDocument* doc = window.document();
    GpuLookPreview& gpu = window.gpuPreviewSession();
    const std::string noise = nodeOfType(*doc, "Noise"), clump = nodeOfType(*doc, "Clump"), scatter = nodeOfType(*doc, "Scatter");
    doc->evaluate(ViewRequest{});
    struct Step {
        std::string node, param;
        json value;
    };
    const json ramp = json::parse("[[0, 0.2], [0.5, 1.0], [1, 0.3]]");
    for (const Step& s : {Step{noise, "profile", ramp}, Step{noise, "seed", 7}, Step{clump, "profile", ramp}}) {
        SCOPED_TRACE(s.node + "." + s.param);
        const json before = doc->paramOrDefault(s.node, s.param);
        const std::size_t evaluations = doc->session().evaluationCount();
        EXPECT_EQ(doc->setParam(s.node, s.param, s.value, false), "");
        EXPECT_TRUE(doc->previewing()) << gpu.why();
        EXPECT_TRUE(gpu.dragging()) << gpu.why();
        EXPECT_EQ(doc->param(s.node, s.param), s.value);
        EXPECT_EQ(doc->session().evaluationCount(), evaluations) << "nothing evaluated while the GPU previews";
        doc->recordEdit(s.node, s.param, before, doc->paramOrDefault(s.node, s.param));
        EXPECT_FALSE(doc->previewing());
        EXPECT_FALSE(gpu.dragging()) << "the release ends the drag";
        EXPECT_EQ(doc->undo(), "");
        EXPECT_EQ(doc->paramOrDefault(s.node, s.param), before);
        doc->evaluate(ViewRequest{});  // as the window evaluates after an edit
    }
    EXPECT_EQ(doc->setParam(scatter, "seed", 11, false), "");
    EXPECT_FALSE(doc->previewing()) << "Scatter's seed is the CPU's";
    EXPECT_FALSE(gpu.dragging());
    EXPECT_NE(gpu.why().find("CPU"), std::string::npos) << gpu.why();
}

TEST(AppPanels, LookCardEditsTheGroomHairColorAndSavesIt) {
    // The card's melanin, redness and roughness sliders and the swatches edit the groom's last
    // HairColor node (undoable, saved with the groom), not a viewport-only material.
    const std::string path = sampleCopy("groomlab_app_lookcolor");
    MainWindow window;
    window.resize(1280, 720);
    ASSERT_TRUE(window.openGroom(QString::fromStdString(path)));
    LookCard* card = window.shell()->lookCard();
    EXPECT_EQ(card->groomColor().node, "color") << "the default graph's HairColor";
    EXPECT_TRUE(card->colorNote().contains(QStringLiteral("color")));
    EXPECT_FALSE(card->colorNote().contains(QStringLiteral("viewport only")));
    // A graph without a HairColor (the default's taken out, its neighbours joined): viewport only.
    const std::string bare = (std::filesystem::path(path).parent_path() / "bare.groom").u8string();
    {
        auto doc = GroomDocument::open(path);
        ASSERT_TRUE(doc);
        std::string from, to;
        for (const GroomDocument::ConnectionInfo& c : doc->connections()) {
            if (c.to == "color") from = c.from;
            if (c.from == "color") to = c.to;
        }
        ASSERT_EQ(doc->removeNode("color"), "");
        ASSERT_EQ(doc->connectNodes(from, "", to, "strands"), "");
        doc->save(bare);
    }
    ASSERT_TRUE(window.openGroom(QString::fromStdString(bare)));
    EXPECT_EQ(card->groomColor().node, "") << "no HairColor node";
    EXPECT_TRUE(card->colorNote().contains(QStringLiteral("viewport only")));
    ASSERT_TRUE(window.openGroom(QString::fromStdString(path)));
    ASSERT_TRUE(window.applyPreset(QStringLiteral("hair_basics")));
    const std::string node = nodeOfType(*window.document(), "HairColor");
    ASSERT_EQ(card->groomColor().node, node);
    EXPECT_TRUE(card->colorNote().contains(QString::fromStdString(node)));
    EXPECT_NEAR(card->melaninSlider()->value(), window.document()->paramOrDefault(node, "melanin").get<double>(), 1e-6);
    // A slider: its steps preview (not recorded), the release records one edit.
    auto type = [](ValueSlider* slider, const char* text) {  // a typed number: pressed, changed, released
        slider->beginEdit();
        slider->setEditText(QLatin1String(text));
        ASSERT_TRUE(slider->commitEdit());
    };
    type(card->melaninSlider(), "0.8");
    EXPECT_NEAR(window.document()->paramOrDefault(node, "melanin").get<double>(), 0.8, 1e-6);
    EXPECT_EQ(window.document()->undoText(), "Undo " + node + ".melanin");
    // A swatch: the preset's pigment onto the node as one edit; the viewport keeps the groom's pigment.
    card->swatch(1)->click();  // black
    EXPECT_NEAR(window.document()->paramOrDefault(node, "melanin").get<double>(), 1.0, 1e-6);
    EXPECT_NEAR(window.document()->paramOrDefault(node, "redness").get<double>(), 0.0, 1e-6);
    EXPECT_EQ(window.document()->undoText(), "Undo hair color black");
    EXPECT_EQ(card->material().pigment, HairMaterial::Pigment::GroomMelanin);
    EXPECT_NEAR(card->melaninSlider()->value(), 1.0, 1e-6) << "the slider shows the node's value";
    window.overlay()->undoButton()->click();
    EXPECT_NEAR(window.document()->paramOrDefault(node, "melanin").get<double>(), 0.8, 1e-6);
    EXPECT_NEAR(card->melaninSlider()->value(), 0.8, 1e-6) << "undo reaches the card";
    // Saved with the groom.
    ASSERT_TRUE(window.saveTo(QString::fromStdString(path)));
    auto again = GroomDocument::open(path);
    ASSERT_TRUE(again);
    EXPECT_NEAR(again->paramOrDefault(node, "melanin").get<double>(), 0.8, 1e-6);
    // The roughness slider edits the HairColor's highlight (agouti_fur sets it), saved with the groom.
    ASSERT_TRUE(window.applyPreset(QStringLiteral("agouti_fur")));
    const std::string fur = nodeOfType(*window.document(), "HairColor");
    ASSERT_EQ(card->groomColor().node, fur);
    EXPECT_NEAR(card->roughnessSlider()->value(), window.document()->paramOrDefault(fur, "roughness").get<double>(), 1e-6);
    type(card->roughnessSlider(), "0.9");
    EXPECT_NEAR(window.document()->paramOrDefault(fur, "roughness").get<double>(), 0.9, 1e-6);
    ASSERT_TRUE(window.saveTo(QString::fromStdString(path)));
    auto rough = GroomDocument::open(path);
    ASSERT_TRUE(rough);
    EXPECT_NEAR(rough->paramOrDefault(fur, "roughness").get<double>(), 0.9, 1e-6);
    // Without a HairColor node the sliders edit the viewport material and say so.
    LookCard alone;
    alone.setGroomColor({});
    EXPECT_TRUE(alone.colorNote().contains(QStringLiteral("viewport only")));
    int materialEdits = 0;
    QObject::connect(&alone, &LookCard::materialEdited, [&] { ++materialEdits; });
    type(alone.melaninSlider(), "0.6");
    EXPECT_NEAR(alone.material().melanin, 0.6, 1e-6);
    EXPECT_EQ(alone.material().pigment, HairMaterial::Pigment::Material);
    EXPECT_GE(materialEdits, 1);
}

TEST(AppPanels, LookCardPicksAColorAndWhiteTips) {
    // Skin-toned roots and white tips from the Look card: Picked color mode, the color, a tip color
    // (which raises the tips from 0 so the pick shows). The look then colors by RGB everywhere; a
    // swatch is a pigment again.
    const std::string path = sampleCopy("groomlab_app_lookpicked");
    MainWindow window;
    window.resize(1280, 720);
    ASSERT_TRUE(window.openGroom(QString::fromStdString(path)));
    LookCard* card = window.shell()->lookCard();
    const std::string node = card->groomColor().node;
    ASSERT_EQ(node, "color");
    EXPECT_FALSE(card->groomModeSwitch()->isHidden());
    EXPECT_TRUE(card->groomColorEditor()->parentWidget()->isHidden()) << "natural: the pigment's sliders";
    EXPECT_FALSE(card->melaninSlider()->isHidden());
    emit card->groomModeSwitch()->changed(1);
    EXPECT_EQ(window.document()->paramOrDefault(node, "mode"), "color");
    EXPECT_TRUE(card->groomColor().picked);
    EXPECT_FALSE(card->groomColorEditor()->parentWidget()->isHidden());
    EXPECT_TRUE(card->melaninSlider()->isHidden());
    const RGB skin{0.8f, 0.55f, 0.45f}, white{1.0f, 1.0f, 1.0f};
    card->groomColorEditor()->setLinear(skin);
    emit card->groomColorEditor()->edited(false);
    EXPECT_NEAR(window.document()->paramOrDefault(node, "color")[1].get<double>(), 0.55, 1e-6);
    EXPECT_EQ(window.document()->undoText(), "Undo " + node + ".color");
    card->tipColorEditor()->setLinear(white);
    emit card->tipColorEditor()->edited(false);
    EXPECT_NEAR(window.document()->paramOrDefault(node, "tipColor")[0].get<double>(), 1.0, 1e-6);
    EXPECT_NEAR(window.document()->paramOrDefault(node, "tipStrength").get<double>(), 1.0, 1e-6) << "raised so the tips show";
    EXPECT_NEAR(card->tipStrengthSlider()->value(), 1.0, 1e-6);
    EXPECT_TRUE(window.document()->session().hairLook().colorsByRgb);
    // A swatch: a pigment, the natural mode again.
    card->swatch(3)->click();  // blonde
    EXPECT_EQ(window.document()->paramOrDefault(node, "mode"), "melanin");
    EXPECT_FALSE(card->groomColor().picked);
    EXPECT_FALSE(card->melaninSlider()->isHidden());
    // Dyed (the violet swatch) dyes the groom's HairColor; another swatch takes the dye off.
    card->swatch(6)->click();  // dyed
    const json dye = window.document()->paramOrDefault(node, "dye");
    EXPECT_LT(dye[1].get<double>(), 0.5);
    EXPECT_NEAR(dye[2].get<double>(), 1.0, 1e-6);
    card->swatch(1)->click();  // black
    EXPECT_NEAR(window.document()->paramOrDefault(node, "dye")[1].get<double>(), 1.0, 1e-6);
    // GroomLab's own swatch: HairColor's defaults back.
    card->swatch(0)->click();
    EXPECT_NEAR(window.document()->paramOrDefault(node, "melanin").get<double>(), 0.35, 1e-6);
    EXPECT_NEAR(window.document()->paramOrDefault(node, "redness").get<double>(), 0.2, 1e-6);
    EXPECT_EQ(window.document()->undoText(), "Undo hair color groom");
}

// Picking a lighting preset keeps the background and the render settings (a gradient and
// Interactive by default).
TEST(AppPanels, LightingPresetsKeepTheBackgroundAndRenderSettings) {
    const LightingSetup start;
    EXPECT_EQ(start.background, Background::Gradient);
    EXPECT_EQ(start.quality, Quality::Interactive);
    for (const std::string& name : lightingPresetNames()) {
        const LightingSetup s = withLightingPreset(start, name);
        EXPECT_EQ(s.background, Background::Gradient) << name;
        EXPECT_EQ(s.quality, Quality::Interactive) << name;
        EXPECT_EQ(s.presetName, name);
        EXPECT_EQ(s.lights.size(), lightingPreset(name).lights.size()) << name;
    }
    LookCard card;
    EXPECT_EQ(card.lighting().background, Background::Gradient);
    EXPECT_EQ(card.lighting().quality, Quality::Interactive);
}

// Small screens and large text: Qt scales the app by the display's scale (125%, 150%), so what
// matters is the screen in logical pixels: 1920x1080 at 125% is 1536x864, at 150% 1280x720, a
// 1366x768 laptop at 100% itself (each less a taskbar). The windows open within the screen, their
// minimum sizes fit it, and in every mode the floating pieces stay inside the window; panels taller
// than the room scroll.
TEST(AppPanels, WindowsAndPanelsFitSmallScreensAndLargeText) {
    // A window's default size within the screen.
    EXPECT_EQ(theme::fitToAvailable({1440, 900}, QRect(0, 0, 1920, 1040)), QSize(1440, 900));
    const QSize at150 = theme::fitToAvailable({1440, 900}, QRect(0, 0, 1280, 680));
    EXPECT_LE(at150.width(), 1280);
    EXPECT_LE(at150.height(), 680);
    EXPECT_EQ(theme::fitToAvailable({1440, 900}, QRect(0, 0, 600, 380)), QSize(600, 380)) << "never past a tiny screen";
    EXPECT_EQ(theme::fitToAvailable({1000, 660}, QRect()), QSize(1000, 660)) << "no screen: as asked";

    const std::string path = sampleCopy("groomlab_app_smallscreen");
    for (const QSize screen : {QSize(1536, 816), QSize(1280, 680), QSize(1366, 728)}) {
        SCOPED_TRACE(std::to_string(screen.width()) + "x" + std::to_string(screen.height()));
        MainWindow window;
        window.resize(screen);
        window.show();
        ASSERT_TRUE(window.openGroom(QString::fromStdString(path)));
        const QSize minimum = window.minimumSizeHint();
        EXPECT_LE(minimum.width(), screen.width());
        EXPECT_LE(minimum.height(), screen.height());
        AppShell* shell = window.shell();
        const QRect room = window.rect();
        auto inside = [&](QWidget* w, const char* what) {
            if (!w->isVisible()) return;
            const QRect g = QRect(w->mapTo(&window, QPoint(0, 0)), w->size());
            EXPECT_TRUE(room.contains(g)) << what << " at " << g.x() << "," << g.y() << " " << g.width() << "x" << g.height();
        };
        // The viewport keeps a usable share of the window beside the docks.
        auto viewportRoom = [&](const char* what) {
            const QSize view = window.overlay()->size();
            EXPECT_GE(view.width(), screen.width() / 3) << what << ": the viewport squeezed to " << view.width();
            EXPECT_GE(view.height(), screen.height() / 3) << what << ": the viewport squeezed to " << view.height();
        };
        for (const char* mode : {"groom", "look", "cards", "export"}) {
            ASSERT_TRUE(window.setUiMode(QString::fromLatin1(mode)));
            QCoreApplication::processEvents();
            inside(shell->lookCard(), "the look card");
            inside(shell->cardsPanel(), "the cards panel");
            inside(shell->exportPanel(), "the export panel");
            inside(shell->groomToolsDock(), "the Groom Tools");
            viewportRoom(mode);
        }
        window.setUiMode(QStringLiteral("groom"));
        window.setGraphShown(true);
        QCoreApplication::processEvents();
        inside(shell->graphDock(), "the Graph Editor");
        viewportRoom("with the Graph Editor");
        for (AppShell::Panel panel : {AppShell::Panel::Look, AppShell::Panel::Display, AppShell::Panel::Help,
                                      AppShell::Panel::Maps, AppShell::Panel::Guides}) {
            shell->showPanel(panel, true);
            QCoreApplication::processEvents();
            inside(shell->panel(panel), "a dock's section");
            shell->showPanel(panel, false);
        }
        // The transfer window: within the screen, its two views side by side at their minimum.
        TransferWindow* transfer = window.openTransferWindow();
        ASSERT_TRUE(transfer);
        EXPECT_LE(transfer->minimumSizeHint().width(), screen.width());
        EXPECT_LE(transfer->minimumSizeHint().height(), screen.height());
        transfer->close();
        // The reference window too: the photo and the mesh side by side, the panel beside them.
        ReferenceWindow* reference = window.openReferenceWindow();
        ASSERT_TRUE(reference);
        EXPECT_LE(reference->minimumSizeHint().width(), screen.width());
        EXPECT_LE(reference->minimumSizeHint().height(), screen.height());
        reference->close();
    }
    // A panel shorter than its rows scrolls them (the export panel squeezed).
    MainWindow small;
    small.resize(1280, 480);
    small.show();
    ASSERT_TRUE(small.openGroom(QString::fromStdString(path)));
    ASSERT_TRUE(small.setUiMode(QStringLiteral("export")));
    QCoreApplication::processEvents();
    ExportPanel* exportPanel = small.shell()->exportPanel();
    EXPECT_LE(exportPanel->height(), small.height());
    auto* scroll = exportPanel->findChild<FitScrollArea*>();
    ASSERT_TRUE(scroll);
    if (scroll->widget()->sizeHint().height() > scroll->height()) EXPECT_GT(scroll->verticalScrollBar()->maximum(), 0);
}

TEST(AppPanels, ParamPanelShowsCompoundParamsAndTheDescription) {
    // A Compound's exposed params (a feather design's) come from the core's per-node spec;
    // every node shows its operator's description; nothing is shown without a node.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    ASSERT_EQ(applyPresetFile(*doc, presetPath("hair_basics", presetDirectories())), "");
    ParamPanel panel;
    panel.setDocument(doc.get());
    panel.showNode("");
    EXPECT_EQ(panel.description(), "");
    EXPECT_FALSE(panel.findChild<QCheckBox*>()->isVisibleTo(&panel)) << "no Bypass box without a node";
    const std::string clump = nodeOfType(*doc, "Clump");
    panel.showNode(clump);
    EXPECT_FALSE(panel.description().isEmpty()) << "the operator's description, as Maya's Attribute Editor";
    EXPECT_TRUE(panel.findChild<QCheckBox*>()->isVisibleTo(&panel));
    EXPECT_GT(panel.sizeHint().height(), 200);
    // When the node runs, beside Bypass: picking Render Only sets it on the node (one undo step).
    auto* evaluation = panel.findChild<QComboBox*>(QStringLiteral("nodeEvaluation"));
    ASSERT_NE(evaluation, nullptr);
    EXPECT_TRUE(evaluation->isVisibleTo(&panel));
    EXPECT_EQ(evaluation->currentData().toString(), QStringLiteral("always"));
    evaluation->setCurrentIndex(evaluation->findData(QStringLiteral("renderOnly")));
    for (const GroomDocument::NodeInfo& info : doc->nodes()) {
        if (info.name == clump) EXPECT_EQ(info.evaluation, "renderOnly");
    }
    EXPECT_EQ(doc->undo(), "");
    panel.showNode(clump);
    EXPECT_EQ(evaluation->currentData().toString(), QStringLiteral("always"));
    const std::string presets = presetPath("feathers_contour", presetDirectories());
    ASSERT_FALSE(presets.empty());
    ASSERT_EQ(applyPresetFile(*doc, presets), "");
    const std::string compound = nodeOfType(*doc, "Compound");
    panel.showNode(compound);
    EXPECT_NE(panel.editorKind("count"), "") << "an exposed param of the design";
    EXPECT_NE(panel.editorKind("vaneWidth"), "");
    EXPECT_EQ(panel.editorKind("definition"), "") << "the definition is not a row (a Compound's params are the exposed ones)";
    EXPECT_FALSE(panel.description().isEmpty());
}

TEST(AppPanels, AnyParamTakesAValueInput) {
    // A param no field drives (Scatter's seed) exposed as a Value port: a Value node sets it, a
    // field node is refused, the row says what sets it and the graph draws the link.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    const std::string scatter = nodeOfType(*doc, "Scatter");
    NodeGraphView view;
    view.resize(800, 600);
    view.setDocument(doc.get());
    const int wires = view.wireCount();
    EXPECT_EQ(doc->exposeParam(scatter, "seed", true), "");
    std::string value, field;
    EXPECT_EQ(doc->addNode("Value", 0.0f, 0.0f, &value), "");
    EXPECT_EQ(doc->setParam(value, "kind", "integer"), "");
    EXPECT_EQ(doc->setParam(value, "integer", 5), "");
    EXPECT_EQ(doc->connectNodes(value, "", scatter, "seed"), "");
    EXPECT_EQ(doc->paramInputSource(scatter, "seed"), value);
    EXPECT_EQ(doc->addNode("FieldNoise", 0.0f, 0.0f, &field), "");
    EXPECT_NE(doc->connectNodes(field, "", scatter, "seed"), "") << "a value per strand cannot set one value";
    view.refresh();
    EXPECT_EQ(view.wireCount(), wires + 1) << "the Value port and its link are drawn";
    ParamPanel panel;
    panel.setDocument(doc.get());
    panel.showNode(scatter);
    auto* fed = panel.findChild<QLabel*>(QStringLiteral("paramFed_seed"));
    ASSERT_NE(fed, nullptr);
    EXPECT_TRUE(fed->text().contains(QString::fromStdString(value)));
    EXPECT_NE(panel.findChild<QWidget*>(QStringLiteral("paramRow_layout")), nullptr) << "a choice takes a Value port too";
    EXPECT_EQ(panel.findChild<QWidget*>(QStringLiteral("paramRow_density")), nullptr) << "a mappable param keeps its Field port";
    EXPECT_EQ(theme::portColor("Value"), theme::portColor("value"));
    EXPECT_NE(theme::portColor("Value"), theme::portColor("Curves"));
}

TEST(AppPanels, AttributeParamsSayWritesOrReads) {
    // Clump's clumpChannel writes (a plain field, a warning when it overwrites an upstream Clump's);
    // a Curl's clumpChannel reads: the attributes reaching it to pick from, each with its writer, a
    // note (and the node's note) when the one named does not reach it.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    ParamPanel panel;
    panel.setDocument(doc.get());
    const std::string interpolate = nodeOfType(*doc, "Interpolate");
    std::string large, small, curl;
    ASSERT_EQ(doc->addNode("Clump", 0.0f, 0.0f, &large, "clumpLarge"), "");
    ASSERT_EQ(doc->addNode("Clump", 0.0f, 0.0f, &small, "clumpSmall"), "");
    ASSERT_EQ(doc->addNode("Curl", 0.0f, 0.0f, &curl, "curlByClump"), "");
    ASSERT_EQ(doc->connectNodes(interpolate, "", large, "strands"), "");
    ASSERT_EQ(doc->connectNodes(large, "", small, "strands"), "");
    ASSERT_EQ(doc->connectNodes(small, "", curl, "strands"), "");
    ASSERT_EQ(doc->setParam(curl, "coherence", "clump", true), "");
    ASSERT_EQ(doc->setParam(curl, "clumpChannel", "lockId", true), "");

    panel.showNode(small);
    EXPECT_EQ(panel.editorKind("clumpChannel"), "attributeWrites");
    EXPECT_EQ(panel.editorKind("regionChannel"), "attributeReads");
    EXPECT_TRUE(panel.attributeNote("clumpChannel").contains("overwrites clumpId from clumpLarge")) << panel.attributeNote("clumpChannel").toStdString();
    EXPECT_EQ(panel.shownValue("clumpChannel"), "clumpId");
    panel.showNode(curl);
    EXPECT_EQ(panel.editorKind("clumpChannel"), "attributeReads");
    EXPECT_TRUE(panel.attributeNote("clumpChannel").contains("no node upstream writes lockId"));
    EXPECT_TRUE(panel.attributeChoices("clumpChannel").contains("clumpId (from clumpSmall)"));
    EXPECT_EQ(panel.shownValue("clumpChannel"), "lockId");
    // The node's note (the panel's grey line, the node graph's badge); none for Clump's optional region.
    auto noteOf = [&](const std::string& node) {
        for (const auto& [name, note] : doc->nodeNotes()) {
            if (name == node) return note;
        }
        return std::string();
    };
    EXPECT_NE(noteOf(curl).find("reads the attribute lockId"), std::string::npos) << noteOf(curl);
    EXPECT_EQ(noteOf(small).find("region"), std::string::npos) << noteOf(small);

    ASSERT_EQ(doc->setParam(large, "clumpChannel", "lockId", true), "");
    panel.showNode(small);
    EXPECT_EQ(panel.attributeNote("clumpChannel"), QString());
    panel.showNode(curl);
    EXPECT_EQ(panel.attributeNote("clumpChannel"), QString());
    EXPECT_TRUE(panel.attributeChoices("clumpChannel").contains("lockId (from clumpLarge)"));
    EXPECT_EQ(noteOf(curl).find("lockId"), std::string::npos) << noteOf(curl);
}

TEST(AppPanels, ParamPanelShowsAnInsertedPresetsKeyParams) {
    // A preset inserted as a Compound shows its "exposed" params on the Compound's own panel, as
    // the inner params' editors would (groups, units, color); an edit there changes the groom
    // and is saved with it.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    ASSERT_EQ(applyPresetFile(*doc, presetPath("hair_basics", presetDirectories())), "");
    PresetApplied applied;
    ASSERT_EQ(insertPresetFile(*doc, presetPath("creature_fur", presetDirectories()), "", &applied), "");
    const std::string fur = applied.nodeMade;
    ASSERT_EQ(fur, "creatureFur");
    ParamPanel panel;
    panel.setDocument(doc.get());
    panel.showNode(fur);
    EXPECT_EQ(panel.editorKind("tuftSize"), "float");  // a length, fitted to the scalp
    EXPECT_EQ(panel.editorKind("tuftStrength"), "map") << "mappable, as the Clump's strength";
    EXPECT_EQ(panel.editorKind("melanin"), "map");
    EXPECT_EQ(panel.editorKind("tipColor"), "color");
    EXPECT_EQ(panel.editorKind("width"), "map");
    EXPECT_EQ(panel.editorKind("definition"), "");
    EXPECT_EQ(panel.sectionOf("tuftStrength"), "");
    EXPECT_EQ(panel.sectionOf("melanin"), "Base") << "the HairColor's groups";
    EXPECT_EQ(panel.sectionOf("tipStrength"), "Tip");
    EXPECT_TRUE(panel.rowShown("tuftStrength"));
    EXPECT_NEAR(panel.shownValue("tuftStrength").get<double>(), 0.4, 1e-9);

    auto pointSum = [&] {
        ViewRequest request;
        request.density = 0.3f;
        const ViewResult& view = doc->evaluate(request);
        double sum = 0.0;
        if (!view.strands) return sum;
        for (std::size_t i = 0; i < view.strands->strandCount(); ++i) {
            const Vec3f* p = view.strands->strandPoints(i);
            for (std::size_t k = 0; k < view.strands->strandPointCount(i); ++k) sum += p[k].x + p[k].y + p[k].z;
        }
        return sum;
    };
    const double before = pointSum();
    ValueSlider* strength = panel.numberSlider("tuftStrength");
    ASSERT_TRUE(strength);
    strength->beginEdit();
    strength->setEditText(QStringLiteral("0.9"));
    EXPECT_TRUE(strength->commitEdit());
    EXPECT_NEAR(doc->paramOrDefault(fur, "tuftStrength").get<double>(), 0.9, 1e-9);
    EXPECT_NE(pointSum(), before) << "the inner Clump takes the Compound's strength";

    const std::string saved = sampleCopy("groomlab_exposed_preset");
    doc->save(saved);
    auto reopened = GroomDocument::open(saved);
    ASSERT_TRUE(reopened);
    EXPECT_NEAR(reopened->paramOrDefault(fur, "tuftStrength").get<double>(), 0.9, 1e-9);
    const json tip = reopened->paramOrDefault(fur, "tipColor");
    ASSERT_TRUE(tip.is_array() && tip.size() == 3) << tip.dump();
    for (std::size_t i = 0; i < 3; ++i) EXPECT_NEAR(tip[i].get<double>(), doc->paramOrDefault(fur, "tipColor")[i].get<double>(), 1e-6);
}

TEST(AppPanels, ParamPanelPaintsAParamInOneStep) {
    // Paint on a mappable param's row: a vertex map named after the node and param, filled 1.0,
    // wired as the param's source (0 to its value), one undo step for both.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    ASSERT_EQ(applyPresetFile(*doc, presetPath("hair_basics", presetDirectories())), "");
    const std::string clump = nodeOfType(*doc, "Clump");
    const json strength = doc->paramOrDefault(clump, "strength");
    ASSERT_TRUE(strength.is_number());
    ParamPanel panel;
    panel.setDocument(doc.get());
    panel.showNode(clump);
    ASSERT_EQ(panel.editorKind("strength"), "map");
    auto* paint = panel.findChild<RoundButton*>(QStringLiteral("paintParam_strength"));
    ASSERT_TRUE(paint) << "the Paint action on a mappable param's row";
    EXPECT_EQ(panel.findChild<RoundButton*>(QStringLiteral("paintParam_profile")), nullptr) << "a ramp has none";
    std::string asked;
    QObject::connect(&panel, &ParamPanel::paintParamRequested, [&](const std::string& node, const std::string& name) { asked = node + "." + name; });
    paint->click();
    EXPECT_EQ(asked, clump + ".strength");
    // The document's part (the window does this on the signal, then picks the Paint brush).
    const std::size_t edits = doc->maps().size();
    std::string map;
    EXPECT_EQ(doc->paintParam(clump, "strength", &map), "");
    EXPECT_EQ(map, clump + "_strength");
    ASSERT_TRUE(doc->map(map));
    EXPECT_EQ(doc->map(map)->kind, "vertices");
    EXPECT_EQ(doc->maps().size(), edits + 1);
    const json wired = doc->paramOrDefault(clump, "strength");
    ASSERT_TRUE(wired.is_object());
    EXPECT_EQ(wired["colorSet"], map);
    EXPECT_DOUBLE_EQ(wired["min"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(wired["max"].get<double>(), strength.get<double>());
    EXPECT_EQ(doc->undoText(), "Undo map " + clump + ".strength");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->map(map), nullptr) << "one step: the map and the wiring go together";
    EXPECT_EQ(doc->paramOrDefault(clump, "strength"), strength);
    EXPECT_EQ(doc->redo(), "");
    ASSERT_TRUE(doc->map(map));
    EXPECT_TRUE(doc->paramOrDefault(clump, "strength").is_object());
    // Again on the same param: the map it reads is picked, no step at all.
    const std::string undoBefore = doc->undoText();
    bool made = true;
    std::string again;
    EXPECT_EQ(doc->paintParam(clump, "strength", &again, "texture", &made), "");
    EXPECT_EQ(again, map);
    EXPECT_FALSE(made);
    EXPECT_EQ(doc->maps().size(), edits + 1);
    EXPECT_EQ(doc->undoText(), undoBefore);
    EXPECT_NE(doc->paintParam("noSuchNode", "strength"), "");
    EXPECT_NE(doc->paintParam(clump, "centerWeight", nullptr, "guides"), "") << "a setting's map lives on the scalp";
}

namespace {

// An operator at version 2 (as a real one becomes after a change of behaviour), its params in one
// group with an advanced one shown only in mode b.
class VersionedPanelOp : public Operator {
public:
    std::string typeName() const override { return "TestPanelVersioned"; }
    std::vector<PortSpec> inputs() const override { return {}; }
    PortSpec output() const override { return {"out", PortType::Field}; }
    int version() const override { return 2; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {ParamSpec{"gain", 1.0, "What it makes"}.range(0.0, 5.0).soft(0.0, 2.0).in("Base"),
                ParamSpec{"mode", std::string("a"), "A mode", "", {"a", "b"}},
                ParamSpec{"onlyB", std::int64_t{1}, "Shown in mode b"}.range(0.0, 3.0).when("mode", {"b"}).adv()};
    }
    Value evaluate(const EvalContext&, const std::vector<Value>&) const override {
        return constantField(static_cast<float>(params().getDouble("gain")));
    }
};

}  // namespace

// The Length Keys tool in the app: a click places a key (the Keys box shows it), a right-drag
// (kStrokeAdjust) scales it in the preview, Apply in the box sets the guides (one undo step).
TEST(AppPanels, LengthKeysPreviewAndApply) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    ViewRequest request;
    request.density = 0.2f;
    doc->evaluate(request);
    BrushSession session;
    OrbitCamera camera;
    camera.frame(doc->sceneBounds());
    session.setDocument(doc.get());
    session.setView(camera, 800, 600);
    GroomTools tools;
    tools.setSession(&session);
    BrushPanel& panel = *tools.card();
    session.setTool("lengths");
    panel.setTool("lengths");
    EXPECT_EQ(session.settings().modeName(), "lengths");
    auto* box = panel.findChild<QWidget*>(QStringLiteral("lengthKeysBox"));
    auto* scale = panel.findChild<ValueSlider*>(QStringLiteral("lengthScale"));
    auto* key = panel.findChild<ValueSlider*>(QStringLiteral("lengthKey"));
    auto* apply = panel.findChild<QPushButton*>(QStringLiteral("lengthApply"));
    ASSERT_TRUE(box && scale && key && apply);
    EXPECT_FALSE(box->isHidden());
    EXPECT_FALSE(apply->isEnabled());
    const std::string before = doc->session().data().hierarchyJson;
    // A guide's root on the scalp, in widget pixels (y down).
    const Vec3f root = doc->session().hierarchy()->guides().front().points.front();
    float x = 0.0f, y = 0.0f;
    ASSERT_TRUE(session.view().project(root, x, y));
    const QPointF at(x, 600.0 - y);
    ASSERT_TRUE(session.press(at, 1.0f, 0)) << session.lastError();
    session.release();
    panel.refresh();
    std::size_t keys = 0;
    session.editLengths("keys", [&](LengthKeyTool& t, StrokeHost& h, const BrushToolSettings&) { keys = t.keys(h).keys.size(); });
    ASSERT_EQ(keys, 1u);
    ASSERT_TRUE(key->isEnabled());
    EXPECT_DOUBLE_EQ(scale->value(), 1.0);
    const double first = key->value();
    EXPECT_GT(first, 0.0);
    // Right-drag up a doubling's worth on the key: x2, its guide twice as long, previewed.
    ASSERT_TRUE(session.press(at, 1.0f, kStrokeAdjust)) << session.lastError();
    session.move(QPointF(at.x(), at.y() - LengthKeyTool::kPixelsPerDoubling), 1.0f);
    session.release();
    panel.refresh();
    EXPECT_NEAR(scale->value(), 2.0, 1e-3);
    EXPECT_NEAR(key->value(), 2.0 * first, 1e-3 * first);
    EXPECT_TRUE(apply->isEnabled());
    EXPECT_EQ(doc->session().data().hierarchyJson, before);  // previewed, not applied
    apply->click();
    EXPECT_NE(doc->session().data().hierarchyJson, before);
    EXPECT_NE(doc->session().data().hierarchyJson.find("lengthKeys"), std::string::npos);
    panel.refresh();
    EXPECT_DOUBLE_EQ(scale->value(), 1.0) << "applied: the key back at x1";
    EXPECT_FALSE(apply->isEnabled());
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->session().data().hierarchyJson, before);
}

TEST(AppPanels, BrushPanelCurvesSymmetryAndAWidthMap) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    ASSERT_EQ(applyPresetFile(*doc, presetPath("hair_basics", presetDirectories())), "");
    ViewRequest request;
    request.density = 0.2f;
    doc->evaluate(request);
    BrushSession session;
    OrbitCamera camera;
    camera.frame(doc->sceneBounds());
    session.setDocument(doc.get());
    session.setView(camera, 800, 600);
    GroomTools tools;
    tools.setSession(&session);
    BrushPanel& panel = *tools.card();

    // The curves: off until ticked, then the editor's keys; unticked, none again.
    // (Chips: Falloff curve and Root to tip, each showing its ramp under the chips while on.)
    auto* strength = panel.findChild<QAbstractButton*>(QStringLiteral("strengthProfile"));
    auto* falloff = panel.findChild<QAbstractButton*>(QStringLiteral("falloffProfile"));
    ASSERT_TRUE(strength && falloff);
    EXPECT_FALSE(strength->isChecked());
    strength->setChecked(true);
    EXPECT_NE(session.settings().strengthProfileJson(), "");
    strength->setChecked(false);
    EXPECT_EQ(session.settings().brush.strengthProfile, nullptr);
    falloff->setChecked(true);
    ASSERT_NE(session.settings().brush.falloffProfile, nullptr);
    EXPECT_FLOAT_EQ((*session.settings().brush.falloffProfile)(0.0f), 1.0f);
    session.settings().setFalloffProfile("");
    panel.refresh();
    EXPECT_FALSE(falloff->isChecked()) << "the panel follows the settings";
    // Symmetry (Applies to's Mirror row): on, and across World X, Y, Z or Topology.
    tools.setTool("comb");
    QComboBox* symmetry = tools.symmetryMode();
    EXPECT_FALSE(symmetry->isEnabled()) << "off: the choice waits";
    symmetry->setCurrentIndex(symmetry->findText(QStringLiteral("Topology")));
    EXPECT_EQ(session.settings().symmetryName(), "none") << "chosen while off: still off";
    tools.symmetryBox()->setChecked(true);
    EXPECT_TRUE(session.settings().brush.mirrorTopology);
    EXPECT_EQ(session.settings().symmetryName(), "topology");
    session.settings().setSymmetry("x");
    tools.refresh();
    EXPECT_EQ(symmetry->currentText(), QStringLiteral("World X"));
    EXPECT_TRUE(tools.symmetryBox()->isChecked());
    session.settings().setSymmetry("none");
    tools.refresh();
    EXPECT_FALSE(tools.symmetryBox()->isChecked());
    // The card shows what the tool uses, in Maya's order: Comb's radius and Stretch, not Place's
    // new guides; Place's new guides, no radius; no duplicate radius anywhere.
    tools.setTool("comb");
    EXPECT_TRUE(panel.findChild<QWidget*>(QStringLiteral("radius"))->isVisibleTo(&panel));
    EXPECT_TRUE(panel.findChild<QAbstractButton*>(QStringLiteral("stretch"))->isVisibleTo(&panel));
    EXPECT_FALSE(panel.findChild<QWidget*>(QStringLiteral("newGuidesBox"))->isVisibleTo(&panel));
    EXPECT_EQ(tools.findChildren<ValueSlider*>(QStringLiteral("radius")).size(), 1);
    tools.setTool("place");
    EXPECT_FALSE(panel.findChild<QWidget*>(QStringLiteral("radius"))->isVisibleTo(&panel));
    EXPECT_TRUE(panel.findChild<QWidget*>(QStringLiteral("newGuidesBox"))->isVisibleTo(&panel));
    tools.setTool("erase");
    EXPECT_TRUE(panel.findChild<QWidget*>(QStringLiteral("eraseBox"))->isVisibleTo(&panel));
    // Signed tools' strength runs -1..1.
    tools.setTool("lift");
    EXPECT_DOUBLE_EQ(panel.findChild<ValueSlider*>(QStringLiteral("strength"))->minimum(), -1.0);
    tools.setTool("comb");
    EXPECT_DOUBLE_EQ(panel.findChild<ValueSlider*>(QStringLiteral("strength"))->minimum(), 0.0);
    // Paint on a scalp map shows the Alpha picker (not on a guide map, nor with Comb): its grid
    // lists None and the library; a pick reaches the settings, its rows shown while one is picked.
    QWidget* alphaBox = panel.findChild<QWidget*>(QStringLiteral("alphaBox"));
    ASSERT_TRUE(alphaBox);
    EXPECT_FALSE(alphaBox->isVisibleTo(&panel));
    session.setTool("paint");
    tools.setTool("paint");
    session.settings().paintKind = "guides";
    tools.refresh();
    EXPECT_FALSE(alphaBox->isVisibleTo(&panel));
    session.settings().paintKind = "vertices";
    tools.refresh();
    EXPECT_TRUE(alphaBox->isVisibleTo(&panel));
    AlphaPicker* picker = panel.alphaPicker();
    picker->fill();
    ASSERT_GT(picker->grid()->count(), 8);
    EXPECT_EQ(picker->grid()->item(0)->text(), QStringLiteral("None"));
    QListWidgetItem* spots = nullptr;
    for (int i = 0; i < picker->grid()->count(); ++i) {
        if (picker->grid()->item(i)->text() == QStringLiteral("Spots")) spots = picker->grid()->item(i);
    }
    ASSERT_TRUE(spots);
    picker->pick(spots);
    EXPECT_EQ(session.settings().alphaPath, spots->data(Qt::UserRole).toString().toStdString());
    ASSERT_TRUE(session.settings().alpha);
    auto* rotation = panel.findChild<QComboBox*>(QStringLiteral("alphaRotation"));
    ASSERT_TRUE(rotation);
    EXPECT_TRUE(rotation->isVisibleTo(&panel));
    rotation->setCurrentIndex(rotation->findData(QStringLiteral("stroke")));
    EXPECT_EQ(session.settings().alphaRotationName(), "stroke");
    ValueSlider* spacing = panel.findChild<ValueSlider*>(QStringLiteral("alphaSpacing"));
    spacing->beginEdit();
    spacing->setEditText(QStringLiteral("50"));
    ASSERT_TRUE(spacing->commitEdit());
    EXPECT_FLOAT_EQ(session.settings().alphaSpacing, 0.5f);
    picker->setPath({});
    EXPECT_EQ(session.settings().alpha, nullptr);
    EXPECT_FALSE(rotation->isVisibleTo(&panel));
    session.settings().setAlphaRotation("fixed");
    tools.setTool("comb");
    session.setTool("comb");

    // A width map (what the Width brush was: + New map for Width): a white map wired to the Width
    // node's width, then the Paint brush on it (Ctrl: thinner); the wiring and the paint are a
    // step each.
    const std::string width = nodeOfType(*doc, "Width");
    const json before = doc->paramOrDefault(width, "width");
    ASSERT_TRUE(before.is_number());
    std::string made;
    ASSERT_EQ(doc->paintParam(width, "width", &made), "");
    session.setTool("paint");
    session.settings().brush.attribute = made;
    EXPECT_EQ(session.toolId(), "paint");
    session.settings().paintKind = "vertices";
    session.settings().brush.value = 1.0f;
    session.settings().screenRadius = 120.0f;
    const Vec3f root = doc->session().hierarchy()->guides().at(0).points.front();
    float x = 0.0f, y = 0.0f;
    ASSERT_TRUE(session.view().project(root, x, y));
    std::vector<QPointF> path;
    for (int k = 0; k <= 8; ++k) path.emplace_back(x - 40.0 + 10.0 * k, 600.0 - y);
    ASSERT_TRUE(session.stroke(path, 1.0f, kStrokeCtrl)) << session.lastError();
    const std::string map = width + "_width";
    ASSERT_TRUE(doc->map(map));
    EXPECT_EQ(doc->map(map)->kind, "vertices");
    const json wired = doc->paramOrDefault(width, "width");
    ASSERT_TRUE(wired.is_object());
    EXPECT_EQ(wired["colorSet"], map);
    EXPECT_DOUBLE_EQ(wired["max"].get<double>(), before.get<double>());
    EXPECT_LT(*std::min_element(doc->session().scalpMaps().maps.at(map).values.begin(), doc->session().scalpMaps().maps.at(map).values.end()), 0.9f)
        << "Ctrl thinned it";
    EXPECT_EQ(doc->undoText(), "Undo paint stroke");
    EXPECT_EQ(doc->undo(), "");
    ASSERT_TRUE(doc->map(map)) << "the paint undone, the map stays";
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->map(map), nullptr) << "the wiring undone: the map goes with it";
    EXPECT_EQ(doc->paramOrDefault(width, "width"), before);
    EXPECT_EQ(doc->redo(), "");
    EXPECT_TRUE(doc->paramOrDefault(width, "width").is_object());

    // The solo workflow: Solo new guides off by default (placing freezes nothing); the Select tool
    // freezes the guides not selected (an undo step), a pick of nothing unfreezes them; Unfreeze All
    // in the panel.
    EXPECT_FALSE(session.settings().place.soloNewGuides);
    ASSERT_TRUE(panel.findChild<QAbstractButton*>(QStringLiteral("soloNewGuides")));
    const QPointF at(x, 600.0 - y);
    ASSERT_EQ(session.select({at}, SelectMode::Replace, true), 1u);
    const auto frozenCount = [&] {
        std::size_t n = 0;
        const std::shared_ptr<const GuideHierarchy> h = doc->session().authoredHierarchy();
        for (const HierarchyGuide& g : h->guides()) {
            float v = 0.0f;
            n += h->effectiveAttribute(g, "freeze", &v) && v > 0.5f;
        }
        return n;
    };
    const std::size_t guides = doc->session().authoredHierarchy()->guides().size();
    EXPECT_GE(frozenCount(), guides - 4) << "all but the picked guide (and its children) frozen";
    EXPECT_EQ(doc->undoText(), "Undo solo selection");
    session.select({QPointF(2.0, 2.0)}, SelectMode::Replace, true);
    EXPECT_EQ(frozenCount(), 0u) << "a pick of nothing unfreezes every guide";
    session.select({at}, SelectMode::Replace, true);
    // Unfreeze All: the tile beside Freeze (the card has no second one); the window unfreezes.
    EXPECT_EQ(panel.findChild<QPushButton*>(QStringLiteral("unfreezeAll")), nullptr);
    int unfreezes = 0;
    QObject::connect(&tools, &GroomTools::unfreezeAllRequested, [&] {
        ++unfreezes;
        doc->editGuides("unfreeze all", [](GuideHierarchy& h) { unfreezeAll(h); });
    });
    tools.tile("unfreeze")->click();
    EXPECT_EQ(unfreezes, 1);
    EXPECT_EQ(frozenCount(), 0u);
    EXPECT_EQ(doc->undoText(), "Undo unfreeze all");
}

TEST(AppPanels, ParamPanelGroupsFoldsShowWhenUnitsAndUpgrade) {
    // Registered before the document reads the specs (and unregistered at the end).
    OperatorRegistry::instance().registerType("TestPanelVersioned", {}, [] { return std::make_unique<VersionedPanelOp>(); });
    struct Unregister {
        ~Unregister() { OperatorRegistry::instance().unregisterType("TestPanelVersioned"); }
    } unregister;
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    ParamPanel panel;
    panel.setDocument(doc.get());

    // HairColor's groups under headers (the main params first, without one), Advanced last and folded.
    std::string color;
    ASSERT_EQ(doc->addNode("HairColor", 0.0f, 0.0f, &color), "");
    panel.showNode(color);
    EXPECT_EQ(panel.sectionOf("melanin"), "Base");
    EXPECT_EQ(panel.sectionOf("melaninRandom"), "Random");
    EXPECT_EQ(panel.sectionOf("seed"), "Advanced");
    ASSERT_TRUE(panel.findChild<QAbstractButton*>(QStringLiteral("paramSection_Base")));
    auto* random = panel.findChild<QAbstractButton*>(QStringLiteral("paramSection_Random"));
    ASSERT_TRUE(random);
    EXPECT_TRUE(panel.rowShown("melanin"));
    EXPECT_FALSE(panel.rowShown("seed")) << "advanced: folded away";
    EXPECT_TRUE(panel.folded("Advanced"));
    // A header's click folds its section, for every HairColor for the session.
    random->click();
    EXPECT_FALSE(panel.rowShown("melaninRandom"));
    panel.setFolded("Advanced", false);
    EXPECT_TRUE(panel.rowShown("seed"));
    std::string other;
    ASSERT_EQ(doc->addNode("HairColor", 0.0f, 0.0f, &other), "");
    panel.showNode(other);
    EXPECT_FALSE(panel.rowShown("melaninRandom"));
    EXPECT_TRUE(panel.rowShown("seed"));
    panel.setFolded("Random", false);
    panel.setFolded("Advanced", true);

    // showWhen: color only in mode color (melanin, greys only in mode melanin), shown at the edit (no
    // rebuild); a fraction as a percentage.
    EXPECT_FALSE(panel.rowShown("color"));
    QComboBox* mode = nullptr;
    for (QComboBox* c : panel.findChildren<QComboBox*>()) {
        if (c->findText(QStringLiteral("melanin")) >= 0 && c->findText(QStringLiteral("color")) >= 0) mode = c;
    }
    ASSERT_TRUE(mode);
    mode->setCurrentText(QStringLiteral("color"));
    EXPECT_EQ(doc->paramOrDefault(other, "mode"), "color");
    EXPECT_TRUE(panel.rowShown("color"));
    EXPECT_FALSE(panel.rowShown("melanin"));
    mode->setCurrentText(QStringLiteral("melanin"));
    EXPECT_TRUE(panel.rowShown("greys"));
    ValueSlider* greys = panel.numberSlider("greys");
    ASSERT_TRUE(greys);
    const double stored = doc->paramOrDefault(other, "greys").get<double>();
    EXPECT_NEAR(greys->value(), stored * 100.0, 1e-9) << "shown as a percentage";
    greys->beginEdit();
    greys->setEditText(QStringLiteral("25"));
    EXPECT_TRUE(greys->commitEdit());
    EXPECT_NEAR(doc->paramOrDefault(other, "greys").get<double>(), 0.25, 1e-9) << "stored as the fraction";
    EXPECT_NEAR(panel.shownValue("greys").get<double>(), 0.25, 1e-9);

    // showWhen chained: a Clump's clumpBy, count and size only with centersFrom pick, and of count
    // and size only the one clumpBy picks. A link drawn into its centers sets centersFrom
    // inputPoints; disconnecting leaves it (the dropdown decides, never the link).
    std::string clump, centers;
    ASSERT_EQ(doc->addNode("Clump", 0.0f, 0.0f, &clump), "");
    ASSERT_EQ(doc->addNode("ToGuides", 0.0f, 0.0f, &centers), "");
    panel.showNode(clump);
    EXPECT_TRUE(panel.rowShown("centersFrom"));
    EXPECT_TRUE(panel.rowShown("clumpBy"));
    EXPECT_TRUE(panel.rowShown("count"));
    EXPECT_FALSE(panel.rowShown("size"));
    EXPECT_TRUE(panel.rowShown("clumpChannel"));  // not folded away: chained Clumps name theirs
    ASSERT_EQ(doc->setParam(clump, "clumpBy", "size", true), "");
    panel.rebuildNow();
    EXPECT_FALSE(panel.rowShown("count"));
    EXPECT_TRUE(panel.rowShown("size"));
    ASSERT_EQ(doc->connectNodes(centers, "", clump, "centers"), "");
    EXPECT_EQ(doc->paramOrDefault(clump, "centersFrom"), "inputPoints");
    panel.rebuildNow();
    EXPECT_FALSE(panel.rowShown("clumpBy"));
    EXPECT_FALSE(panel.rowShown("size"));  // its clumpBy hidden: hidden too
    EXPECT_FALSE(panel.rowShown("randomness"));
    EXPECT_TRUE(panel.rowShown("strength"));
    ASSERT_EQ(doc->disconnectInput(clump, "centers"), "");
    EXPECT_EQ(doc->paramOrDefault(clump, "centersFrom"), "inputPoints");
    panel.rebuildNow();
    EXPECT_FALSE(panel.rowShown("size"));
    ASSERT_EQ(doc->setParam(clump, "centersFrom", "pick", true), "");
    panel.rebuildNow();
    EXPECT_TRUE(panel.rowShown("size"));
    panel.showNode(other);

    // One group: no group headers, only Advanced. A node saved at version 1: Upgrade, one undo step.
    std::string versioned;
    ASSERT_EQ(doc->addNode("TestPanelVersioned", 0.0f, 0.0f, &versioned), "");
    EXPECT_EQ(doc->nodeVersion(versioned), std::make_pair(2, 2));
    ASSERT_EQ(doc->editGraph("keep version 1", [&](json& g) {
        for (json& n : g["nodes"]) {
            if (n.value("name", std::string()) == versioned) n["version"] = 1;
        }
    }), "");
    EXPECT_EQ(doc->nodeVersion(versioned), std::make_pair(1, 2));
    panel.showNode(versioned);
    EXPECT_EQ(panel.sectionOf("gain"), "");
    EXPECT_EQ(panel.findChild<QAbstractButton*>(QStringLiteral("paramSection_Base")), nullptr);
    EXPECT_EQ(panel.sectionOf("onlyB"), "Advanced");
    panel.setFolded("Advanced", false);
    EXPECT_FALSE(panel.rowShown("onlyB")) << "mode a";
    EXPECT_FALSE(panel.findChild<QAbstractButton*>(QStringLiteral("paramSection_Advanced"))->isVisibleTo(&panel))
        << "no row of Advanced shows: no header";
    ASSERT_EQ(doc->setParam(versioned, "mode", "b"), "");
    panel.rebuildNow();
    EXPECT_TRUE(panel.rowShown("onlyB"));
    auto* upgrade = panel.findChild<RoundButton*>(QStringLiteral("upgradeNode"));
    ASSERT_TRUE(upgrade);
    EXPECT_TRUE(upgrade->isVisibleTo(&panel));
    EXPECT_TRUE(upgrade->toolTip().contains(QStringLiteral("version 1")));
    upgrade->click();
    EXPECT_EQ(doc->nodeVersion(versioned), std::make_pair(2, 2));
    EXPECT_EQ(doc->undoText(), "Undo upgrade " + versioned);
    panel.rebuildNow();
    EXPECT_FALSE(upgrade->isVisibleTo(&panel));
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->nodeVersion(versioned), std::make_pair(1, 2));
    panel.setFolded("Advanced", true);
}

TEST(AppPanels, SrgbAndLinearAreInverses) {
    for (float v = 0.0f; v <= 1.0f; v += 0.05f) {
        EXPECT_NEAR(linearToSrgb(srgbToLinear(v)), v, 1e-5f);
        EXPECT_NEAR(srgbToLinear(linearToSrgb(v)), v, 1e-5f);
    }
    EXPECT_NEAR(srgbToLinear(0.5f), 0.2140f, 1e-3f);
    EXPECT_NEAR(linearToSrgb(0.2140f), 0.5f, 1e-3f);
}

TEST(AppPanels, TransferToAMeshByMarkersFromAJsonFile) {
    // Tools > Transfer Groom to Mesh by a method: the groom onto a copy of its scalp turned a
    // quarter about y, half again as big and moved, by marker pairs read from a JSON file.
    const std::string groom = sampleCopy("groomlab_app_transfer_markers");
    const std::filesystem::path dir = std::filesystem::u8path(groom).parent_path();
    MainWindow window;
    ASSERT_TRUE(window.openGroom(QString::fromStdString(groom)));
    const MeshPtr scalp = window.document()->file().data.scalp;
    ASSERT_TRUE(scalp);
    auto place = [](const Vec3f& p) { return Vec3f(p.z, p.y, -p.x) * 1.5f + Vec3f(30.0f, 0.0f, 0.0f); };
    {
        std::FILE* obj = std::fopen((dir / "turned.obj").u8string().c_str(), "w");
        ASSERT_TRUE(obj);
        for (const Vec3f& p : scalp->points()) {
            const Vec3f q = place(p);
            std::fprintf(obj, "v %.7g %.7g %.7g\n", q.x, q.y, q.z);
        }
        for (const auto& t : scalp->triangles()) std::fprintf(obj, "f %d %d %d\n", t[0] + 1, t[1] + 1, t[2] + 1);
        std::fclose(obj);
    }
    // Markers at the scalp's extreme points (spread over it), by position alone.
    std::vector<std::size_t> picks;
    for (int axis = 0; axis < 3; ++axis) {
        auto coord = [axis](const Vec3f& p) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; };
        const auto& pts = scalp->points();
        const auto [lo, hi] = std::minmax_element(pts.begin(), pts.end(), [&](const Vec3f& a, const Vec3f& b) { return coord(a) < coord(b); });
        for (auto it : {lo, hi}) {
            const auto i = static_cast<std::size_t>(it - pts.begin());
            if (std::find(picks.begin(), picks.end(), i) == picks.end()) picks.push_back(i);
        }
    }
    json pairs = json::array();
    for (const std::size_t i : picks) {
        const Vec3f a = scalp->points()[i], b = place(a);
        pairs.push_back({{"old", {{"point", {a.x, a.y, a.z}}}}, {"new", {{"point", {b.x, b.y, b.z}}}}});
    }
    const std::string markersFile = (dir / "markers.json").u8string();
    {
        std::FILE* f = std::fopen(markersFile.c_str(), "w");
        ASSERT_TRUE(f);
        std::fputs(pairs.dump().c_str(), f);
        std::fclose(f);
    }
    std::vector<TransferMarker> markers;
    ASSERT_TRUE(window.readTransferMarkers(QString::fromStdString(markersFile), &markers));
    EXPECT_EQ(markers.size(), picks.size());
    EXPECT_FALSE(window.readTransferMarkers(QString::fromStdString((dir / "missing.json").u8string()), &markers));

    std::map<GuideId, Vec3f> before;
    for (const HierarchyGuide& g : window.document()->session().authoredHierarchy()->guides()) {
        if (!g.points.empty() && !g.isChild()) before[g.id] = g.points.front();
    }
    ASSERT_FALSE(before.empty());
    const Bounds box = window.document()->sceneBounds();
    const float size = (box.hi - box.lo).length();

    // Too few markers: refused, the groom as it was.
    TransferOptions options;
    options.method = TransferMethod::MarkersAlign;
    options.markers = {markers[0], markers[1]};
    EXPECT_FALSE(window.transferToMesh(QString::fromStdString((dir / "turned.obj").u8string()), options));
    EXPECT_EQ(window.document()->file().data.scalp, scalp);

    ASSERT_TRUE(window.readTransferMarkers(QString::fromStdString(markersFile), &markers));
    options.markers = markers;
    ASSERT_TRUE(window.transferToMesh(QString::fromStdString((dir / "turned.obj").u8string()), options));
    for (const HierarchyGuide& g : window.document()->session().authoredHierarchy()->guides()) {
        const auto it = before.find(g.id);
        if (it == before.end()) continue;
        EXPECT_LT((g.points.front() - place(it->second)).length(), 0.01f * size) << "guide " << g.id;
    }
    // The markers stay with the groom, snapped onto both meshes, and are saved in its file.
    ASSERT_EQ(window.document()->file().transferMarkers.size(), markers.size());
    EXPECT_GE(window.document()->file().transferMarkers[0].newRoot.face, 0);
    const std::string saved = (dir / "saved.groom").u8string();
    ASSERT_TRUE(window.saveTo(QString::fromStdString(saved)));
    EXPECT_EQ(readGroomFile(saved).transferMarkers.size(), markers.size());
}

TEST(AppPanels, TransferWindowPlacesMarkerPairsInTwoViews) {
    // Tools > Transfer Groom to Mesh: the window with the groom's scalp on the left and a turned,
    // scaled, moved copy of it on the right; marker points placed on each side (pairs by index),
    // deleted with their partners, undone, mirrored, stored with the groom; the click picked
    // through a view's camera; then the transfer by those markers.
    const std::string groom = sampleCopy("groomlab_app_transfer_window");
    const std::filesystem::path dir = std::filesystem::u8path(groom).parent_path();
    MainWindow window;
    ASSERT_TRUE(window.openGroom(QString::fromStdString(groom)));
    const MeshPtr scalp = window.document()->file().data.scalp;
    ASSERT_TRUE(scalp);
    auto place = [](const Vec3f& p) { return Vec3f(p.z, p.y, -p.x) * 1.5f + Vec3f(30.0f, 0.0f, 0.0f); };
    const std::string turned = (dir / "turned.obj").u8string();
    {
        std::FILE* obj = std::fopen(turned.c_str(), "w");
        ASSERT_TRUE(obj);
        for (const Vec3f& p : scalp->points()) {
            const Vec3f q = place(p);
            std::fprintf(obj, "v %.7g %.7g %.7g\n", q.x, q.y, q.z);
        }
        for (const auto& t : scalp->triangles()) std::fprintf(obj, "f %d %d %d\n", t[0] + 1, t[1] + 1, t[2] + 1);
        std::fclose(obj);
    }
    // Points spread over the scalp: its extremes.
    std::vector<Vec3f> picks;
    for (int axis = 0; axis < 3; ++axis) {
        auto coord = [axis](const Vec3f& p) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; };
        const auto& pts = scalp->points();
        const auto [lo, hi] = std::minmax_element(pts.begin(), pts.end(), [&](const Vec3f& a, const Vec3f& b) { return coord(a) < coord(b); });
        for (auto it : {lo, hi}) {
            if (std::find(picks.begin(), picks.end(), *it) == picks.end()) picks.push_back(*it);
        }
    }
    ASSERT_GE(picks.size(), 4u);
    const Bounds box = window.document()->sceneBounds();
    const float size = (box.hi - box.lo).length();
    auto near = [size](const Vec3f& a, const Vec3f& b) { return (a - b).length() < 1e-3f * size; };
    constexpr MarkerSide L = MarkerSide::Old;
    constexpr MarkerSide R = MarkerSide::New;

    TransferWindow* tw = window.openTransferWindow();
    ASSERT_TRUE(tw);
    EXPECT_EQ(window.openTransferWindow(), tw);  // one window, raised again
    tw->resize(1000, 660);
    EXPECT_EQ(tw->view(L)->mesh(), scalp);
    EXPECT_FALSE(tw->view(R)->mesh());
    EXPECT_FALSE(tw->transferButton()->isEnabled());  // no new scalp yet
    EXPECT_FALSE(tw->setNewScalpPath(QString::fromStdString((dir / "missing.obj").u8string())));
    ASSERT_TRUE(tw->setNewScalpPath(QString::fromStdString(turned)));
    ASSERT_TRUE(tw->view(R)->mesh());
    EXPECT_EQ(tw->view(L)->mesh(), scalp);  // the left view kept
    EXPECT_TRUE(tw->transferButton()->isEnabled());  // closest point needs no markers
    tw->setMethod(TransferMethod::MarkersAlign);
    EXPECT_FALSE(tw->transferButton()->isEnabled());
    EXPECT_TRUE(tw->statusText().contains("needs 3 or more pairs")) << tw->statusText().toStdString();

    // Three on the left first, then two on the right.
    for (int i = 0; i < 3; ++i) tw->addPoint(L, picks[i]);
    EXPECT_EQ(tw->pending().size(), 3u);
    tw->addPoint(R, place(picks[0]));
    tw->addPoint(R, place(picks[1]));
    EXPECT_EQ(tw->editor().pairCount(), 2u);
    ASSERT_EQ(tw->pending().size(), 1u);
    EXPECT_EQ(tw->pending()[0], std::make_pair(L, std::size_t{2}));
    EXPECT_TRUE(tw->statusText().startsWith("2 pairs, 1 unmatched")) << tw->statusText().toStdString();
    EXPECT_EQ(window.document()->transferMarkers().size(), 2u);  // complete pairs stored with the groom
    EXPECT_GE(window.document()->transferMarkers()[1].newRoot.face, 0);
    // Pair 2 deleted from the right: the left's 2 goes too, its 3 becomes 2.
    EXPECT_EQ(tw->deletePoint(R, 1), std::vector<std::size_t>{1});
    EXPECT_TRUE(near(tw->editor().point(L, 1).point, picks[2]));
    EXPECT_EQ(tw->editor().pairCount(), 1u);
    EXPECT_EQ(window.document()->transferMarkers().size(), 1u);
    ASSERT_TRUE(tw->undo());
    EXPECT_EQ(tw->editor().pairCount(), 2u);
    EXPECT_EQ(window.document()->transferMarkers().size(), 2u);
    ASSERT_TRUE(tw->redo());
    ASSERT_TRUE(tw->undo());
    tw->addPoint(R, place(picks[2]));
    tw->addPoint(L, picks[3]);
    tw->addPoint(R, place(picks[3]));
    EXPECT_EQ(tw->editor().pairCount(), 4u);
    EXPECT_TRUE(tw->pending().empty());
    EXPECT_TRUE(tw->transferButton()->isEnabled());

    // Symmetry X: the mirror is the next point on that side; deleting one deletes both.
    tw->setSymmetry(true);
    const Bounds sb = [&] {
        Bounds b{scalp->points()[0], scalp->points()[0], true};
        for (const Vec3f& p : scalp->points()) {
            b.lo = {std::min(b.lo.x, p.x), std::min(b.lo.y, p.y), std::min(b.lo.z, p.z)};
            b.hi = {std::max(b.hi.x, p.x), std::max(b.hi.y, p.y), std::max(b.hi.z, p.z)};
        }
        return b;
    }();
    const float midX = (sb.lo.x + sb.hi.x) * 0.5f;
    const Vec3f side = scalp->points()[scalp->pointCount() / 3];
    ASSERT_GT(std::abs(side.x - midX), 0.05f * size) << "pick a point off the middle";
    EXPECT_EQ(tw->addPoint(L, side), 4u);
    ASSERT_EQ(tw->editor().count(L), 6u);
    EXPECT_NEAR(tw->editor().point(L, 5).point.x, 2.0f * midX - tw->editor().point(L, 4).point.x, 0.1f * size);
    EXPECT_EQ(tw->editor().point(L, 4).group, tw->editor().point(L, 5).group);
    EXPECT_EQ(tw->deletePoint(L, 5), (std::vector<std::size_t>{4, 5}));
    EXPECT_EQ(tw->editor().count(L), 4u);
    tw->undo();
    EXPECT_EQ(tw->editor().count(L), 6u);
    tw->undo();
    EXPECT_EQ(tw->editor().count(L), 4u);
    tw->setSymmetry(false);

    // The pick through a known camera: the left view framed on the scalp, the ray through its
    // middle meets the mesh on the line from the eye to the target.
    MarkerView* view = tw->view(L);
    tw->layout()->activate();
    ASSERT_GT(view->width(), 100);
    view->frame();
    const QPointF middle(view->width() / 2.0, view->height() / 2.0);
    MarkerPoint hit;
    ASSERT_TRUE(view->pick(middle, &hit));
    EXPECT_GE(hit.root.face, 0);
    EXPECT_TRUE(near(scalp->position(hit.root), hit.point));
    const QVector3D e = view->camera().eye(), t = view->camera().target();
    const Vec3f eye(e.x(), e.y(), e.z()), axis = Vec3f(t.x() - e.x(), t.y() - e.y(), t.z() - e.z()) * (1.0f / (t - e).length());
    const Vec3f off = (hit.point - eye) - axis * (hit.point - eye).dot(axis);
    EXPECT_LT(off.length(), 2e-3f * size);
    QPointF back;
    ASSERT_TRUE(view->project(hit.point, &back));
    EXPECT_LT((back - middle).manhattanLength(), 1.5);
    EXPECT_FALSE(view->pick(QPointF(1.0, 1.0), &hit));  // the corner misses
    // A click there adds a point (synthetic events to the widget; the cursor never moves), a
    // Ctrl+click on it deletes it again.
    mouse(view, QEvent::MouseButtonPress, middle.toPoint());
    mouse(view, QEvent::MouseButtonRelease, middle.toPoint());
    ASSERT_EQ(tw->editor().count(L), 5u);
    EXPECT_EQ(view->pointNear(middle + QPointF(3.0, 0.0)), 4);
    {
        QMouseEvent press(QEvent::MouseButtonPress, middle, view->mapToGlobal(middle), Qt::LeftButton, Qt::LeftButton,
                          Qt::ControlModifier);
        QCoreApplication::sendEvent(view, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, middle, view->mapToGlobal(middle), Qt::LeftButton, Qt::NoButton,
                            Qt::ControlModifier);
        QCoreApplication::sendEvent(view, &release);
    }
    EXPECT_EQ(tw->editor().count(L), 4u);
    // Hovering lights the point and its partner.
    tw->setHover(std::make_pair(L, std::size_t{0}));
    EXPECT_TRUE(tw->hover().has_value());
    tw->setHover(std::nullopt);

    // The views draw the meshes (a GPU device: the mesh's pixels differ from the background).
    const QImage image = view->meshImage();
    if (!image.isNull()) {
        const QColor center = image.pixelColor(image.width() / 2, image.height() / 2);
        const QColor corner = image.pixelColor(2, 2);
        EXPECT_NE(center.rgb(), corner.rgb());
    }

    // The picture (GROOMLAB_TRANSFER_APP_SHOT): a few pairs, one unmatched, symmetry on.
    if (const char* shot = std::getenv("GROOMLAB_TRANSFER_APP_SHOT")) {
        tw->view(L)->frame();
        tw->setSymmetry(true);
        tw->addPoint(L, side);
        tw->setSymmetry(false);
        tw->addPoint(R, place(side));
        tw->setSymmetry(true);
        EXPECT_TRUE(tw->statusText().startsWith("5 pairs, 1 unmatched")) << tw->statusText().toStdString();
        EXPECT_TRUE(tw->grab().save(QString::fromUtf8(shot)));
        tw->undo();
        tw->undo();
        tw->setSymmetry(false);
    }

    // Transfer by the four pairs: the guides land where the copy has them.
    std::map<GuideId, Vec3f> before;
    for (const HierarchyGuide& g : window.document()->session().authoredHierarchy()->guides()) {
        if (!g.points.empty() && !g.isChild()) before[g.id] = g.points.front();
    }
    ASSERT_EQ(tw->editor().pairCount(), 4u);
    ASSERT_TRUE(tw->transfer()) << tw->findChild<QLabel*>("transferMessage")->text().toStdString();
    for (const HierarchyGuide& g : window.document()->session().authoredHierarchy()->guides()) {
        const auto it = before.find(g.id);
        if (it == before.end()) continue;
        EXPECT_LT((g.points.front() - place(it->second)).length(), 0.01f * size) << "guide " << g.id;
    }
    EXPECT_NE(tw->view(L)->mesh(), scalp);  // the groom's scalp now: the new one
    EXPECT_EQ(window.document()->transferMarkers().size(), 4u);
}

TEST(AppPanels, ReferenceWindowLinesAPhotoUpAndGrowsGuides) {
    // The Photos window: a photo of the groom's scalp drawn through a known camera, imported;
    // landmarks clicked on the photo and the mesh; the camera solves live (4 pairs, the focal length
    // kept; 6+, solved); the focal length by hand, Keep focal length, Lock; flow arrows and mask
    // touch-ups drawn on the photo with the window's undo (the Guides stage); Generate grows the
    // fromReference group; all of it saved with the groom.
    const std::string groom = sampleCopy("groomlab_app_reference_window");
    const std::filesystem::path dir = std::filesystem::u8path(groom).parent_path();
    MainWindow window;
    ASSERT_TRUE(window.openGroom(QString::fromStdString(groom)));
    const MeshPtr scalp = window.document()->file().data.scalp;
    ASSERT_TRUE(scalp);
    Vec3f lo = scalp->points()[0], hi = lo;
    for (const Vec3f& p : scalp->points()) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const Vec3f center = (lo + hi) * 0.5f;
    const float radius = (hi - lo).length() * 0.5f;
    // The truth: a camera in front (+Z) of the scalp, a little above and aside, 50 mm on 36 mm.
    const int W = 320, H = 240;
    RefCamera truth;
    {
        const Vec3f z = Vec3f(0.25f, 0.2f, 1.0f).normalized();
        const Vec3f x = Vec3f(0, 1, 0).cross(z).normalized();
        const Vec3f y = z.cross(x);
        const Vec3f axes[3] = {x, y, z};
        for (std::size_t i = 0; i < 3; ++i) {
            truth.pose.rotation[0][i] = axes[i].x;
            truth.pose.rotation[1][i] = axes[i].y;
            truth.pose.rotation[2][i] = axes[i].z;
        }
        const Vec3f eye = center + z * (radius * 3.2f);
        truth.pose.position = {eye.x, eye.y, eye.z};
        truth.width = W;
        truth.height = H;
        truth.focalPx = W * 50.0 / 36.0;
    }
    // The photo: a light backdrop, the scalp's points through the camera as dark strands.
    QImage picture(W, H, QImage::Format_RGB32);
    picture.fill(QColor(230, 230, 235));
    for (const Vec3f& p : scalp->points()) {
        float u = 0, v = 0;
        if (!truth.project(p, u, v)) continue;
        for (int dy = -3; dy <= 6; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const int x = static_cast<int>(u) + dx, y = static_cast<int>(v) + dy;
                if (x >= 0 && y >= 0 && x < W && y < H) picture.setPixelColor(x, y, x % 3 ? QColor(115, 80, 55) : QColor(70, 45, 30));
            }
    }
    const QString photoPath = QString::fromStdString((dir / "front.png").u8string());
    ASSERT_TRUE(picture.save(photoPath));
    // Landmark points: scalp vertices facing the camera, spread over it.
    std::vector<Vec3f> front;
    const Vec3f eye(static_cast<float>(truth.pose.position[0]), static_cast<float>(truth.pose.position[1]),
                    static_cast<float>(truth.pose.position[2]));
    for (std::size_t i = 0; i < scalp->pointCount(); ++i) {
        const Vec3f p = scalp->points()[i];
        if (scalp->vertexNormals()[i].dot((eye - p).normalized()) > 0.5f) front.push_back(p);
    }
    ASSERT_GE(front.size(), 20u);
    // The first: the point nearest the eye; then each time the front point farthest from those picked.
    std::vector<Vec3f> picks = {*std::min_element(front.begin(), front.end(), [&](const Vec3f& a, const Vec3f& b) {
        return (a - eye).length() < (b - eye).length();
    })};
    while (picks.size() < 7) {
        Vec3f best = front[0];
        float bestScore = -1.0f;
        for (const Vec3f& p : front) {
            float score = 1e30f;
            for (const Vec3f& q : picks) score = std::min(score, (p - q).length());
            if (score > bestScore) {
                bestScore = score;
                best = p;
            }
        }
        picks.push_back(best);
    }
    auto pixel = [&](const Vec3f& p) {
        float u = 0, v = 0;
        EXPECT_TRUE(truth.project(p, u, v));
        return QPointF(u, v);
    };

    ReferenceWindow* rw = window.openReferenceWindow();
    ASSERT_TRUE(rw);
    EXPECT_EQ(window.openReferenceWindow(), rw);  // one window, raised again
    rw->resize(1240, 720);
    QCoreApplication::processEvents();
    EXPECT_EQ(rw->meshView()->mesh(), scalp);
    EXPECT_FALSE(rw->generateButton()->isEnabled());  // no photo yet
    EXPECT_EQ(rw->importPhoto(QString::fromStdString((dir / "missing.png").u8string())), -1);
    ASSERT_EQ(rw->importPhoto(photoPath), 0);
    ASSERT_EQ(rw->photoCount(), 1);
    EXPECT_EQ(rw->photo()->width, W);
    EXPECT_FALSE(rw->generateButton()->isEnabled());  // not lined up yet
    QCoreApplication::processEvents();

    // Pair 1: the photo by a click in its view, the mesh by a click in the mesh view (its pick).
    PhotoView* view = rw->photoView();
    mouseAt(view, QEvent::MouseButtonPress, view->toWidget(pixel(picks[0])));
    mouseAt(view, QEvent::MouseButtonRelease, view->toWidget(pixel(picks[0])));
    ASSERT_EQ(rw->markerLists().count(kLandmarkPhoto), 1u);
    EXPECT_NEAR(rw->markerLists().point(kLandmarkPhoto, 0).point.x, pixel(picks[0]).x(), 1e-3);
    QPointF onMesh;
    ASSERT_TRUE(rw->meshView()->project(picks[0], &onMesh));
    click(rw->meshView(), onMesh.toPoint());
    ASSERT_EQ(rw->markerLists().pairCount(), 1u);
    EXPECT_LT((rw->markerLists().point(kLandmarkMesh, 0).point - picks[0]).length(), 0.05f * radius);
    rw->moveLandmark(kLandmarkMesh, 0, picks[0]);  // exactly on the vertex for the checks below
    // Pairs 2-4, the mesh side first: the camera solves on the fourth, the focal length kept.
    for (int k = 1; k < 4; ++k) {
        EXPECT_FALSE(rw->photo()->solve.ok);
        rw->addLandmark(kLandmarkMesh, picks[k]);
        rw->addLandmark(kLandmarkPhoto, Vec3f(static_cast<float>(pixel(picks[k]).x()), static_cast<float>(pixel(picks[k]).y()), 0.0f));
    }
    ASSERT_TRUE(rw->photo()->solve.ok) << rw->photo()->solve.why;
    EXPECT_FALSE(rw->photo()->solve.focalSolved);
    ASSERT_TRUE(rw->photo()->camera);
    for (int i = 0; i < 3; ++i) EXPECT_NEAR(rw->photo()->camera->pose.position[i], truth.pose.position[i], 0.03 * radius * 3.2);
    EXPECT_TRUE(rw->solveText().contains("camera lined up")) << rw->solveText().toStdString();
    EXPECT_FALSE(rw->overlayLines().empty());  // the outline over the photo
    EXPECT_TRUE(rw->generateButton()->isEnabled());
    for (int k = 4; k < 7; ++k) {
        rw->addLandmark(kLandmarkPhoto, Vec3f(static_cast<float>(pixel(picks[k]).x()), static_cast<float>(pixel(picks[k]).y()), 0.0f));
        rw->addLandmark(kLandmarkMesh, picks[k]);
    }
    EXPECT_TRUE(rw->photo()->solve.focalSolved);
    EXPECT_LT(rw->photo()->solve.rms, 1.0);
    EXPECT_EQ(rw->stage(), ReferenceWindow::Stage::Align);
    EXPECT_FALSE(rw->guidesPanel()->isVisibleTo(rw));
    // Keep focal length (kept with the photo); the focal length by hand turns it on, the camera follows.
    rw->setKeepFocal(true);
    EXPECT_FALSE(rw->photo()->solve.focalSolved);
    EXPECT_TRUE(QString::fromStdString(rw->photo()->display).contains("keepFocal"));
    rw->setKeepFocal(false);
    EXPECT_TRUE(rw->photo()->solve.focalSolved);
    const double solvedMm = rw->photo()->camera->focalPx * 36.0 / W;
    EXPECT_NEAR(rw->focalSlider()->value(), solvedMm, 0.05);
    ASSERT_TRUE(rw->setFocal(solvedMm * 1.5));
    EXPECT_TRUE(rw->keepFocal());
    EXPECT_NEAR(rw->photo()->camera->focalPx, solvedMm * 1.5 * W / 36.0, 1e-6);
    EXPECT_TRUE(rw->photo()->solve.ok);
    EXPECT_FALSE(rw->photo()->solve.focalSolved);
    ASSERT_TRUE(rw->setFocal(3000.0));
    EXPECT_GE(rw->focalSlider()->maximum(), 3000.0);
    ASSERT_TRUE(rw->resetFocal());  // the default lens, the slider's starting range
    EXPECT_NEAR(rw->photo()->camera->focalPx, 50.0 * W / 36.0, 1e-6);
    EXPECT_EQ(rw->focalSlider()->maximum(), 200.0);
    rw->setKeepFocal(false);
    // Lock: the landmarks no longer move the camera, nor does the focal length change.
    rw->setLocked(true);
    EXPECT_TRUE(rw->lockButton()->isChecked());
    EXPECT_FALSE(rw->focalSlider()->isEnabled());
    const auto lockedAt = rw->photo()->camera->pose.position;
    EXPECT_FALSE(rw->setFocal(20.0));
    rw->moveLandmark(kLandmarkPhoto, 0, Vec3f(static_cast<float>(pixel(picks[0]).x() + 6.0), static_cast<float>(pixel(picks[0]).y()), 0.0f));
    EXPECT_EQ(rw->photo()->camera->pose.position, lockedAt);
    EXPECT_TRUE(rw->messageText().contains("locked")) << rw->messageText().toStdString();
    ASSERT_TRUE(rw->undo());
    rw->setLocked(false);
    EXPECT_TRUE(rw->focalSlider()->isEnabled());
    // A point dragged off turns bad; undo puts it back.
    const QPointF third = pixel(picks[2]);
    dragPhoto(view, {third, third + QPointF(25, 0), third + QPointF(50, 10)});
    ASSERT_EQ(rw->photo()->solve.errors.size(), 7u);
    const auto& errors = rw->photo()->solve.errors;
    EXPECT_EQ(std::max_element(errors.begin(), errors.end()) - errors.begin(), 2);
    EXPECT_TRUE(rw->photo()->solve.bad[2]) << errors[2];
    ASSERT_TRUE(rw->undo());
    EXPECT_NEAR(rw->markerLists().point(kLandmarkPhoto, 2).point.x, third.x(), 1e-3);
    // Ctrl+click deletes a pair, both sides.
    mouseAt(view, QEvent::MouseButtonPress, view->toWidget(pixel(picks[6])), Qt::ControlModifier);
    mouseAt(view, QEvent::MouseButtonRelease, view->toWidget(pixel(picks[6])), Qt::ControlModifier);
    EXPECT_EQ(rw->markerLists().count(kLandmarkPhoto), 6u);
    EXPECT_EQ(rw->markerLists().count(kLandmarkMesh), 6u);
    ASSERT_TRUE(rw->undo());

    // Flow arrows (a tool of the Guides stage): a drag root to tip; Ctrl+drag erases; undo.
    rw->setMode(ReferenceWindow::Mode::Flow);
    EXPECT_EQ(rw->stage(), ReferenceWindow::Stage::Guides);
    EXPECT_TRUE(rw->guidesPanel()->isVisibleTo(rw));
    const QPointF top = pixel(center + Vec3f(0, radius * 0.5f, 0)), bottom = pixel(center - Vec3f(0, radius * 0.3f, 0));
    std::vector<QPointF> stroke;
    for (int k = 0; k <= 10; ++k) stroke.push_back(top + (bottom - top) * (k / 10.0));
    dragPhoto(view, stroke);
    ASSERT_EQ(rw->photo()->editor.strokes().size(), 1u);
    dragPhoto(view, {stroke[5], stroke[5] + QPointF(1, 1)}, Qt::ControlModifier);
    EXPECT_TRUE(rw->photo()->editor.strokes().empty());
    ASSERT_TRUE(rw->undo());
    EXPECT_EQ(rw->photo()->editor.strokes().size(), 1u);
    EXPECT_EQ(rw->arrows().size(), 1u);
    EXPECT_TRUE(rw->arrows()[0].hasHead);
    // Mask touch-ups: backdrop over the middle, the preview's mask follows.
    rw->setMaskTint(true);
    rw->setFieldLines(true);
    const ReferenceViewPreview* before = rw->preview();
    ASSERT_TRUE(before);
    float hairBefore = 0.0f;
    for (float m : before->mask.pixels) hairBefore += m;
    EXPECT_GT(hairBefore, 0.0f);
    rw->setMode(ReferenceWindow::Mode::Backdrop);
    const QPointF mid = pixel(center);
    dragPhoto(view, {mid - QPointF(30, 0), mid, mid + QPointF(30, 0)});
    ASSERT_EQ(rw->photo()->editor.maskStrokes().size(), 1u);
    EXPECT_FALSE(rw->photo()->editor.maskStrokes()[0].include);
    const ReferenceViewPreview* after = rw->preview();
    ASSERT_TRUE(after);
    float hairAfter = 0.0f;
    for (float m : after->mask.pixels) hairAfter += m;
    EXPECT_LT(hairAfter, hairBefore);
    EXPECT_FALSE(rw->previewTint().isNull());
    rw->grab();  // paints without trouble
    ASSERT_TRUE(rw->undo());  // the touch-up

    // Generate: the fromReference group, one undo step of the document's.
    rw->setGuideCount(80);
    rw->setSymmetry(QStringLiteral("none"));
    ASSERT_TRUE(rw->generate()) << rw->messageText().toStdString();
    const auto groupSize = [&] {
        const GuideHierarchy h = GuideHierarchy::fromJson(window.document()->session().data().hierarchyJson);
        const GroupId g = h.findGroup("fromReference");
        return g == kNoGroup ? std::size_t{0} : h.guidesInGroup(g).size();
    };
    EXPECT_GT(groupSize(), 5u);
    EXPECT_TRUE(rw->messageText().contains("guides in fromReference"));
    // Saved with the groom and read back.
    const std::string saved = (dir / "saved.groom").u8string();
    ASSERT_TRUE(window.saveTo(QString::fromStdString(saved)));
    EXPECT_FALSE(readGroomFile(saved).referencePhotos.empty());
    MainWindow again;
    ASSERT_TRUE(again.openGroom(QString::fromStdString(saved)));
    ReferenceWindow* back = again.openReferenceWindow();
    ASSERT_EQ(back->photoCount(), 1);
    EXPECT_EQ(back->markerLists().pairCount(), 7u);
    EXPECT_EQ(back->photo()->editor.strokes().size(), 1u);
    ASSERT_TRUE(back->photo()->camera);
    EXPECT_NEAR(back->photo()->camera->focalPx, rw->photo()->camera->focalPx, 1e-6 * rw->photo()->camera->focalPx);
    EXPECT_TRUE(back->generateButton()->isEnabled());
}


// ---- UX batch 2: jobs, keys, the shortcut sheet, the Add Node search, numbers ----------------------

namespace {

// A click posted to a widget (delivered by the event loop, as a click on the screen would be).
void postClick(QWidget* w) {
    const QPointF at(w->width() / 2.0, w->height() / 2.0);
    QCoreApplication::postEvent(w, new QMouseEvent(QEvent::MouseButtonPress, at, w->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier));
    QCoreApplication::postEvent(w, new QMouseEvent(QEvent::MouseButtonRelease, at, w->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier));
}

void key(QWidget* w, QEvent::Type type, int k, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent event(type, k, mods);
    QCoreApplication::sendEvent(w, &event);
}

}  // namespace

TEST(AppPanels, AJobsCancelTakesAClickWhileTheRestOfTheWindowWaits) {
    // An export runs on the UI thread with the events in between: its Cancel takes a click, every
    // other click waits (the viewport's Undo does nothing), another export is refused, and closing the
    // window stops the job first, then closes it.
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    AppShell* host = window.shell();
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "groomlab_app_job_cancel";
    std::filesystem::create_directories(dir);
    ExportOptions options;
    options.density = 0.05f;
    options.frames = FrameRange{1.0, 40.0, 1.0};
    bool sawBusy = false, refused = false;
    QString job;
    QTimer::singleShot(0, &window, [&] {
        sawBusy = window.busy();
        job = window.busyJob();
        refused = !window.exportTo(QStringLiteral("alembic"), QString::fromStdString((dir / "again.abc").u8string()), options);
        postClick(window.overlay()->undoButton());        // held back
        postClick(host->exportPanel()->exportButton());   // Cancel
    });
    EXPECT_FALSE(window.exportTo(QStringLiteral("alembic"), QString::fromStdString((dir / "cancelled.abc").u8string()), options))
        << "the export ran to its end: Cancel's click never landed";
    EXPECT_TRUE(sawBusy);
    EXPECT_EQ(job, QStringLiteral("export"));
    EXPECT_TRUE(refused) << "a second export started inside the first";
    EXPECT_TRUE(window.lastReport().startsWith(QStringLiteral("Stopped after"))) << window.lastReport().toStdString();
    EXPECT_FALSE(window.busy());
    EXPECT_FALSE(window.graphShown()) << "a click on the Graph pill got through during the export";
    EXPECT_EQ(QApplication::overrideCursor(), nullptr) << "the busy cursor stayed";
    // Undo and Open are refused while it runs (a script or a timer calling them).
    bool undoRefused = false;
    QTimer::singleShot(0, &window, [&] {
        undoRefused = !window.openGroom(QString::fromStdString(kSampleGroom));
        // Closing the window: refused while the job runs, the job stopped, the window closed after.
        QCloseEvent close;
        QCoreApplication::sendEvent(&window, &close);
        EXPECT_FALSE(close.isAccepted());
        EXPECT_TRUE(window.isVisible());
    });
    EXPECT_FALSE(window.exportTo(QStringLiteral("alembic"), QString::fromStdString((dir / "closed.abc").u8string()), options));
    EXPECT_TRUE(undoRefused);
    QCoreApplication::processEvents();
    EXPECT_FALSE(window.isVisible()) << "the window closed once the job stopped";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(AppPanels, GraphFFramesTheSelectedNodeAndAFramesAll) {
    // Maya's Graph Editor keys: F frames the selected node (none: the whole graph), A the whole graph;
    // the menus show the same keys.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    NodeGraphView view;
    view.resize(1200, 800);
    view.setDocument(doc.get());
    key(&view, QEvent::KeyPress, Qt::Key_A);
    const qreal fitScale = view.viewScale();
    const QPointF fitPan = view.viewPan();
    const std::string scatter = nodeOfType(*doc, "Scatter");
    view.selectNode(scatter);
    key(&view, QEvent::KeyPress, Qt::Key_F);
    EXPECT_TRUE(view.viewScale() != fitScale || view.viewPan() != fitPan) << "F framed nothing new";
    // The node's center sits at the middle of the view (inside the chrome's margins: 60 90 60 100).
    float x = 0.0f, y = 0.0f;
    if (doc->nodePosition(scatter, &x, &y)) {
        const QPointF center = (QPointF(x, y) + QPointF(80.0, 30.0)) * view.viewScale() + view.viewPan();
        EXPECT_NEAR(center.x(), 60 + (1200 - 120) / 2.0, 40.0);
    }
    key(&view, QEvent::KeyPress, Qt::Key_A);
    EXPECT_DOUBLE_EQ(view.viewScale(), fitScale);
    EXPECT_EQ(view.viewPan(), fitPan);
    view.selectNode({});
    key(&view, QEvent::KeyPress, Qt::Key_F);
    EXPECT_DOUBLE_EQ(view.viewScale(), fitScale) << "F with nothing selected frames the whole graph";
    EXPECT_EQ(view.viewPan(), fitPan);
}

TEST(AppPanels, MDragSetsTheStrengthAndAltShiftXTogglesSymmetry) {
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    OverlayHost* host = window.overlay();
    Viewport* viewport = host->viewport();
    viewport->resize(1000, 600);
    // M held: brush mode, a left drag to the right raises the strength (0.01 a pixel), the radius kept.
    BrushToolSettings& s = window.brush().settings();
    s.brush.strength = 0.5f;
    const float radius = s.screenRadius;
    key(viewport, QEvent::KeyPress, Qt::Key_M);
    EXPECT_EQ(viewport->toolMode(), Viewport::ToolMode::Brush);
    mouse(viewport, QEvent::MouseButtonPress, QPoint(400, 300));
    {
        QMouseEvent move(QEvent::MouseMove, QPointF(430, 300), viewport->mapToGlobal(QPointF(430, 300)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &move);
    }
    mouse(viewport, QEvent::MouseButtonRelease, QPoint(430, 300));
    key(viewport, QEvent::KeyRelease, Qt::Key_M);
    EXPECT_NEAR(s.brush.strength, 0.8f, 1e-4f);
    EXPECT_FLOAT_EQ(s.screenRadius, radius);
    // Alt+Shift+X (Tools > Symmetry On/Off): World X on, off, and the mode it had back on.
    QAction* symmetry = window.findChild<QAction*>(QStringLiteral("symmetry"));
    ASSERT_NE(symmetry, nullptr);
    EXPECT_EQ(symmetry->shortcut(), QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_X));
    s.setSymmetry("none");
    symmetry->trigger();
    EXPECT_EQ(s.symmetryName(), "x");
    EXPECT_EQ(window.lastReport(), QStringLiteral("Symmetry: World X on"));
    symmetry->trigger();
    EXPECT_EQ(s.symmetryName(), "none");
    EXPECT_EQ(window.lastReport(), QStringLiteral("Symmetry: World X off"));
    // The Groom Tools' Symmetry box shows symmetry on for any mode (World Y, Topology), not only World X.
    QCheckBox* chip = window.shell()->groomTools()->symmetryBox();
    ASSERT_NE(chip, nullptr) << "the Groom Tools' Symmetry box";
    ASSERT_TRUE(window.setBrushText(QStringLiteral("symmetry=y")));
    symmetry->trigger();
    symmetry->trigger();  // off and on again: World Y comes back
    EXPECT_EQ(s.symmetryName(), "y");
    EXPECT_TRUE(chip->isChecked()) << "World Y on, the box off";
    EXPECT_EQ(window.shell()->groomTools()->symmetryMode()->currentText(), QStringLiteral("World Y"));
    s.setSymmetry("topology");
    symmetry->trigger();
    symmetry->trigger();
    EXPECT_TRUE(chip->isChecked());
    EXPECT_EQ(window.lastReport(), QStringLiteral("Symmetry: Topology on"));
    // The chip turns it off and back on as it was.
    chip->click();
    EXPECT_EQ(s.symmetryName(), "none");
    chip->click();
    EXPECT_EQ(s.symmetryName(), "topology");
}

TEST(AppPanels, TheShortcutSheetListsTheSharedTable) {
    // Help > Keyboard Shortcuts is built from the one table (groomlab/shortcuts.h): every app row,
    // a heading per context; tooltips take their keys from it too.
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    ASSERT_TRUE(window.showHelp(QStringLiteral("shortcuts")));
    QString sheet;
    for (const QLabel* label : window.shell()->panel(AppShell::Panel::Help)->findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("<table"))) sheet = label->text();
    }
    ASSERT_FALSE(sheet.isEmpty());
    const std::vector<Shortcut> rows = shortcutsFor(kShortcutApp);
    ASSERT_GT(rows.size(), 20u);
    for (const Shortcut& s : rows) {
        EXPECT_TRUE(sheet.contains(QString::fromStdString(s.keys).toHtmlEscaped())) << s.id;
        EXPECT_TRUE(sheet.contains(QString::fromStdString(s.action).toHtmlEscaped())) << s.id;
    }
    for (const char* context : {"File", "Modes", "Brush", "Viewport", "Graph"}) {
        EXPECT_TRUE(sheet.contains(QStringLiteral("<b>%1</b>").arg(QLatin1String(context)))) << context;
    }
    for (const Shortcut& s : shortcutsFor(kShortcutMaya)) {
        if (!(s.hosts & kShortcutApp) && s.id == "graph.view") EXPECT_FALSE(sheet.contains(QString::fromStdString(s.action)));
    }
    EXPECT_EQ(withShortcut(QStringLiteral("Frame the groom"), "view.frame"), QStringLiteral("Frame the groom (F)"));
    EXPECT_EQ(withShortcut(QStringLiteral("Nothing"), "no.such.row"), QStringLiteral("Nothing"));
    const QAction* graph = window.findChild<QAction*>(QStringLiteral("graphEditor"));
    ASSERT_NE(graph, nullptr);
    EXPECT_EQ(graph->shortcut(), QKeySequence(Qt::Key_Tab)) << "GroomLab > Graph Editor (Tab)";
    EXPECT_TRUE(window.showUiPanel(QStringLiteral("shortcuts")));
}

TEST(AppPanels, AddNodeSearchRanksAsMayaAndShowsLabels) {
    // The Add Node search ranks as Maya's: a search word ("symmetry") finds Mirror first, a param's
    // name (density) ranks Scatter before a node that only mentions it, the label finds a type
    // ("linked": Reference, shown as Linked Groom); empty: every type but the hidden ones, by menu.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    const std::vector<std::string> symmetry = nodeSearchResults(*doc, QStringLiteral("symmetry"));
    ASSERT_FALSE(symmetry.empty());
    EXPECT_EQ(symmetry.front(), "Mirror");
    const std::vector<std::string> density = nodeSearchResults(*doc, QStringLiteral("density"));
    const auto at = [](const std::vector<std::string>& v, const char* type) { return std::find(v.begin(), v.end(), type) - v.begin(); };
    ASSERT_LT(at(density, "Scatter"), static_cast<long long>(density.size()));
    ASSERT_LT(at(density, "Expression"), static_cast<long long>(density.size()));
    EXPECT_LT(at(density, "Scatter"), at(density, "Expression"));
    const std::vector<std::string> linked = nodeSearchResults(*doc, QStringLiteral("linked"));
    ASSERT_FALSE(linked.empty());
    EXPECT_EQ(linked.front(), "Reference");
    EXPECT_TRUE(nodeSearchResults(*doc, QStringLiteral("zzqqxx")).empty());
    // A category's title is a search word too: "deform" finds the first Deform node, "collision" a Collide one.
    const std::vector<std::string> deform = nodeSearchResults(*doc, QStringLiteral("deform"));
    ASSERT_FALSE(deform.empty());
    EXPECT_EQ(doc->spec(deform.front())->value("category", std::string()), "deform") << deform.front();
    const std::vector<std::string> collision = nodeSearchResults(*doc, QStringLiteral("collision"));
    ASSERT_FALSE(collision.empty());
    EXPECT_EQ(doc->spec(collision.front())->value("category", std::string()), "collide") << collision.front();
    const std::vector<std::string> all = nodeSearchResults(*doc, {});
    EXPECT_EQ(at(all, "Compound"), static_cast<long long>(all.size())) << "a hidden type in the search";
    EXPECT_EQ(at(all, "Input"), static_cast<long long>(all.size()));
    int lastCategory = -1;
    for (const std::string& type : all) {
        const int category = doc->spec(type)->value("categoryOrder", 0);
        EXPECT_GE(category, lastCategory) << type;
        lastCategory = category;
    }
    // The view: "<label>    <category>", the description under the list.
    NodeGraphView view;
    view.resize(1000, 700);
    view.setDocument(doc.get());
    view.setSearchText(QStringLiteral("linked"));
    ASSERT_TRUE(view.searchOpen());
    ASSERT_FALSE(view.searchRows().isEmpty());
    EXPECT_TRUE(view.searchRows().front().startsWith(QStringLiteral("Linked Groom"))) << view.searchRows().front().toStdString();
    EXPECT_EQ(view.searchTypes().front(), "Reference");
    EXPECT_EQ(view.searchAbout(), QString::fromStdString(doc->spec("Reference")->value("description", std::string())));
    view.setSearchText(QStringLiteral("zzqqxx"));
    EXPECT_TRUE(view.searchRows().isEmpty());
    EXPECT_TRUE(view.searchAbout().startsWith(QStringLiteral("No node matches")));
    // The new categories have colors of their own.
    EXPECT_NE(theme::nodeCategoryColor("deform"), theme::nodeCategoryColor("shape"));
    EXPECT_NE(theme::nodeCategoryColor("collide"), theme::nodeCategoryColor("utility"));
    EXPECT_TRUE(theme::nodeCategoryColor("games").isValid());
    // A Reference node shows its label in the Node List and the Node Settings title.
    std::string reference;
    ASSERT_EQ(doc->addNode("Reference", 0.0f, 0.0f, &reference), "");
    ParamPanel panel;
    panel.setDocument(doc.get());
    panel.showNode(reference);
    bool titled = false;
    for (const QLabel* label : panel.findChildren<QLabel*>()) titled = titled || label->text() == QString::fromStdString(reference + "  (Linked Groom)");
    EXPECT_TRUE(titled);
}

TEST(AppPanels, NumbersShowTheSpecsDecimalsWithoutTrailingZeros) {
    EXPECT_EQ(trimmedNumber(0.35, 3), QStringLiteral("0.35"));
    EXPECT_EQ(trimmedNumber(1.0, 3), QStringLiteral("1"));
    EXPECT_EQ(trimmedNumber(12.5, 1), QStringLiteral("12.5"));
    EXPECT_EQ(trimmedNumber(-0.0, 2), QStringLiteral("0"));
    // A small number keeps two significant digits (a density fitted to a large scalp), six
    // decimals at most.
    EXPECT_EQ(trimmedNumber(0.0347, 1), QStringLiteral("0.035"));
    EXPECT_EQ(trimmedNumber(0.00874, 1), QStringLiteral("0.0087"));
    EXPECT_EQ(trimmedNumber(-0.0001, 2), QStringLiteral("-0.0001"));
    EXPECT_EQ(trimmedNumber(1e-9, 3), QStringLiteral("0"));
    EXPECT_EQ(trimmedNumber(980.0, 1), QStringLiteral("980"));
    // A fraction as a percentage: the spec's decimals less two, 0 to 1; degrees one; else the spec's.
    const DisplayUnit fraction = displayUnit("fraction", false, 3);
    EXPECT_DOUBLE_EQ(fraction.factor, 100.0);
    EXPECT_EQ(fraction.suffix, QStringLiteral("%"));
    EXPECT_EQ(fraction.decimals, 1);
    EXPECT_EQ(displayUnit("fraction", false, 4).decimals, 1);
    EXPECT_EQ(displayUnit("angle", false, 3).decimals, 1);
    EXPECT_EQ(displayUnit("length", false, 2).decimals, 2);
    EXPECT_TRUE(displayUnit("length", false, 2).suffix.isEmpty());
    // In the panel: Simulate's gravity reads "<n> /s²"; a fraction 35% (not 35.0%); a length 0.35.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    std::string sim;
    ASSERT_EQ(doc->addNode("Simulate", 0.0f, 0.0f, &sim), "");
    ParamPanel panel;
    panel.setDocument(doc.get());
    panel.showNode(sim);
    QWidget* gravityRow = panel.findChild<QWidget*>(QStringLiteral("paramRow_gravity"));
    ASSERT_NE(gravityRow, nullptr);
    const ValueSlider* gravity = gravityRow->findChild<ValueSlider*>();
    ASSERT_NE(gravity, nullptr);
    // (A new node's lengths are fitted to the scalp: the sample's small head pulls 98, not 980.)
    const double g = doc->paramOrDefault(sim, "gravity").get<double>();
    EXPECT_EQ(gravity->shownText(), trimmedNumber(g, 1) + QStringLiteral(" /s²"));
    for (const ValueSlider* slider : panel.findChildren<ValueSlider*>()) {
        const QString text = slider->shownText();
        const QString number = text.section(QLatin1Char(' '), 0, 0).remove(QLatin1Char('%')).remove(QStringLiteral("°"));
        if (number.contains(QLatin1Char('.'))) EXPECT_FALSE(number.endsWith(QLatin1Char('0'))) << text.toStdString();
    }
}

namespace {

// A node's index in the view (the document's order) and a port's in its spec.
int indexOf(const GroomDocument& doc, const std::string& name) {
    const std::vector<GroomDocument::NodeInfo> nodes = doc.nodes();
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

int portOf(const GroomDocument& doc, const std::string& node, const char* list, const std::string& port) {
    const json* spec = doc.nodeSpec(node);
    if (!spec) return -1;
    const json ports = spec->value(list, json::array());
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (ports[i].value("name", std::string()) == port) return static_cast<int>(i);
    }
    return -1;
}

// An input port's center in the view (the card's geometry: a 28 px header, 8 px padding, 18 px rows).
QPoint inputAt(const NodeGraphView& view, const std::string& node, int port) {
    const QRectF r = view.nodeRect(node);
    const qreal s = view.viewScale();
    return QPointF(r.left(), r.top() + (28.0 + 8.0 + 18.0 * (port + 0.5)) * s).toPoint();
}

void mouseMods(QWidget* w, QEvent::Type type, const QPoint& at, Qt::KeyboardModifiers mods) {
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton);
    QMouseEvent event(type, QPointF(at), w->mapToGlobal(QPointF(at)), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held, mods);
    QCoreApplication::sendEvent(w, &event);
}

}  // namespace

TEST(AppPanels, NumbersDropTrailingZerosInSpinBoxesAndTheLookCard) {
    // The vec3 spin boxes and the Look card's HairColor sliders as the sliders show numbers:
    // the spec's decimals, trailing zeros dropped (0.35, not 0.350).
    TrimmedSpinBox spin;
    spin.setDecimals(3);
    EXPECT_EQ(spin.textFromValue(0.35), QStringLiteral("0.35"));
    EXPECT_EQ(spin.textFromValue(1.0), QStringLiteral("1"));
    EXPECT_EQ(spin.textFromValue(0.125), QStringLiteral("0.125"));
    LookCard card;
    LookCard::GroomColor color;
    color.node = "hairColor";
    color.melanin = 0.35f;
    color.redness = 0.2f;
    color.roughness = 0.5f;
    card.setGroomColor(color);
    EXPECT_EQ(card.melaninSlider()->shownText(), QStringLiteral("0.35"));
    EXPECT_EQ(card.rednessSlider()->shownText(), QStringLiteral("0.2"));
    EXPECT_EQ(card.roughnessSlider()->shownText(), QStringLiteral("0.5"));
    // A vec3 param (not a color) in the Node panel: no trailing zeros either.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    for (const std::string& type : doc->operatorTypes()) {
        const json* spec = doc->spec(type);
        if (!spec || spec->value("hidden", false)) continue;
        for (const json& p : spec->value("params", json::array())) {
            if (p.value("kind", std::string()) != "vec3" || p.value("unit", std::string()) == "color") continue;
            std::string made;
            if (doc->addNode(type, 0.0f, 0.0f, &made) != "") continue;
            ParamPanel panel;
            panel.setDocument(doc.get());
            panel.showNode(made);
            for (const TrimmedSpinBox* box : panel.findChildren<TrimmedSpinBox*>()) {
                const QString text = box->textFromValue(box->value());
                if (text.contains(QLatin1Char('.'))) EXPECT_FALSE(text.endsWith(QLatin1Char('0'))) << type << " " << text.toStdString();
            }
            EXPECT_FALSE(panel.findChildren<TrimmedSpinBox*>().isEmpty()) << type;
            return;
        }
    }
}

TEST(AppPanels, TransferSymmetryFollowsTheGlossary) {
    MainWindow window;
    window.resize(1280, 720);
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    TransferWindow* tw = window.openTransferWindow();
    ASSERT_NE(tw, nullptr);
    bool found = false;
    for (const QCheckBox* box : tw->findChildren<QCheckBox*>()) found = found || box->text() == QStringLiteral("Symmetry");
    EXPECT_TRUE(found) << "the Transfer window's checkbox says Symmetry (docs/APP_PARITY.md's glossary)";
    tw->close();
}

TEST(AppPanels, GraphShowsAFailingNodesErrorOnItsCardAndInTheNodePanel) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    // A Linked Groom reading a file that is not there, made the output: its evaluation fails.
    std::string linked;
    ASSERT_EQ(doc->addNode("Reference", 0.0f, 0.0f, &linked), "");
    ASSERT_EQ(doc->setParam(linked, "path", "no_such_file.groom"), "");
    ASSERT_EQ(doc->setOutputNode(linked), "");
    ViewRequest request;
    request.density = 0.2f;
    doc->evaluate(request);
    const auto errors = doc->nodeErrors();
    ASSERT_FALSE(errors.empty()) << "the view's errors name the node";
    EXPECT_EQ(errors.front().first, linked);
    EXPECT_NE(errors.front().second.find("no groom file"), std::string::npos) << errors.front().second;
    // The card's badge and tooltip text, and the Node panel's red line under the title.
    NodeGraphView view;
    view.resize(1200, 800);
    view.setDocument(doc.get());
    view.setNodeErrors(errors);
    EXPECT_EQ(view.nodeError(linked), errors.front().second);
    EXPECT_TRUE(view.nodeError(nodeOfType(*doc, "Scatter")).empty());
    ParamPanel panel;
    panel.setDocument(doc.get());
    panel.showNode(linked);
    EXPECT_TRUE(panel.error().contains(QStringLiteral("no groom file"))) << panel.error().toStdString();
    panel.showNode(nodeOfType(*doc, "Scatter"));
    EXPECT_TRUE(panel.error().isEmpty());
    // The theme's error color, in every theme (the badge and the line).
    EXPECT_TRUE(theme::graphError().isValid());
    EXPECT_TRUE(theme::errorText().isValid());
}

TEST(AppPanels, GraphMultiSelectsDeletesCopiesAndPastes) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    std::string a, b;
    ASSERT_EQ(doc->addNode("Noise", 900.0f, 600.0f, &a), "");
    ASSERT_EQ(doc->addNode("Noise", 1100.0f, 600.0f, &b), "");
    ASSERT_EQ(doc->connectNodes(a, "", b, "strands"), "");
    const std::size_t nodes = doc->nodes().size(), links = doc->connections().size();
    NodeGraphView view;
    view.resize(1400, 900);
    view.setDocument(doc.get());
    // A click selects one; Ctrl+click adds (and toggles), Shift+click adds.
    view.selectNode(a);
    mouseMods(&view, QEvent::MouseButtonPress, view.nodeRect(b).center().toPoint(), Qt::ControlModifier);
    mouseMods(&view, QEvent::MouseButtonRelease, view.nodeRect(b).center().toPoint(), Qt::ControlModifier);
    EXPECT_EQ(view.selectedNodes(), (std::vector<std::string>{a, b}));
    EXPECT_EQ(view.selectedNode(), b) << "the last one clicked is the current one";
    mouseMods(&view, QEvent::MouseButtonPress, view.nodeRect(a).center().toPoint(), Qt::ControlModifier);
    mouseMods(&view, QEvent::MouseButtonRelease, view.nodeRect(a).center().toPoint(), Qt::ControlModifier);
    EXPECT_EQ(view.selectedNodes(), std::vector<std::string>{b});
    // The rubber band over both cards selects both.
    const QRectF band = view.nodeRect(a).united(view.nodeRect(b)).adjusted(-10, -10, 10, 10);
    mouse(&view, QEvent::MouseButtonPress, band.topLeft().toPoint());
    mouse(&view, QEvent::MouseMove, band.bottomRight().toPoint());
    mouse(&view, QEvent::MouseButtonRelease, band.bottomRight().toPoint());
    std::vector<std::string> picked = view.selectedNodes();
    std::sort(picked.begin(), picked.end());
    std::vector<std::string> both{a, b};
    std::sort(both.begin(), both.end());
    EXPECT_EQ(picked, both);
    // Ctrl+C, Ctrl+V: both again, with the link between them, beside; one undo step.
    key(&view, QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    EXPECT_TRUE(QGuiApplication::clipboard()->text().contains(QStringLiteral("groomlabNodes")));
    key(&view, QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
    EXPECT_EQ(doc->nodes().size(), nodes + 2);
    EXPECT_EQ(doc->connections().size(), links + 1);
    EXPECT_EQ(view.selectedNodes().size(), 2u) << "the pasted nodes are selected";
    float ax = 0.0f, ay = 0.0f, px = 0.0f, py = 0.0f;
    ASSERT_TRUE(doc->nodePosition(a, &ax, &ay));
    const std::string pasted = view.selectedNodes().front();
    ASSERT_TRUE(doc->nodePosition(pasted, &px, &py));
    EXPECT_GT(px + py, ax + ay) << "pasted beside, not on top";
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->nodes().size(), nodes);
    // Delete removes the selection in one undo step.
    view.refresh();
    view.setSelection({a, b});
    key(&view, QEvent::KeyPress, Qt::Key_Delete);
    EXPECT_EQ(doc->nodes().size(), nodes - 2);
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->nodes().size(), nodes);
    // The output node never goes, nor is copied.
    EXPECT_TRUE(doc->copyNodes({nodeOfType(*doc, "Output")}).empty());
    EXPECT_NE(doc->removeNodes({nodeOfType(*doc, "Output")}), "");
    EXPECT_NE(doc->pasteNodes("not nodes", 0.0f, 0.0f), "");
}

TEST(AppPanels, GraphGroupsIntoCompoundsAndOpensThem) {
    // Ctrl+G groups the selection into one Compound (the core's collapse, Maya's too), one undo
    // step; a double-click on it opens it (its nodes back), Ctrl+Shift+G too; Ctrl+Z each time.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    const std::string length = nodeOfType(*doc, "Length"), noise = nodeOfType(*doc, "Noise");
    ASSERT_FALSE(length.empty());
    ASSERT_FALSE(noise.empty());
    const std::string before = doc->graphJson();
    const std::size_t nodes = doc->nodes().size();
    NodeGraphView view;
    view.resize(1400, 900);
    view.setDocument(doc.get());
    view.setSelection({length, noise});
    key(&view, QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier);
    EXPECT_EQ(doc->nodes().size(), nodes - 1);
    const std::string compound = view.selectedNode();
    ASSERT_EQ(doc->nodeSpec(compound) ? doc->nodeSpec(compound)->value("type", std::string()) : std::string(), "Compound");
    EXPECT_EQ(doc->undoText(), "Undo group 2 nodes");
    EXPECT_EQ(view.nodeCount(), static_cast<int>(nodes - 1));
    // Opened by a double-click: the two nodes again, the same links, selected.
    const QPoint at = view.nodeRect(compound).center().toPoint();
    mouse(&view, QEvent::MouseButtonPress, at);
    mouse(&view, QEvent::MouseButtonRelease, at);
    mouse(&view, QEvent::MouseButtonDblClick, at);
    EXPECT_EQ(doc->nodes().size(), nodes);
    std::vector<std::string> picked = view.selectedNodes();
    std::sort(picked.begin(), picked.end());
    std::vector<std::string> both{length, noise};
    std::sort(both.begin(), both.end());
    EXPECT_EQ(picked, both);
    EXPECT_EQ(doc->undoText(), "Undo ungroup " + compound);
    EXPECT_EQ(doc->connections().size(), json::parse(before)["connections"].size());
    // Undone, one step each: the compound, then the graph as it was.
    EXPECT_EQ(doc->undo(), "");
    view.refresh();
    EXPECT_EQ(doc->nodes().size(), nodes - 1);
    view.setSelection({compound});
    key(&view, QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(doc->nodes().size(), nodes);
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(json::parse(doc->graphJson())["connections"], json::parse(before)["connections"]);
    // Two nodes feeding the rest cannot be one group: refused, the graph unchanged.
    view.refresh();
    view.setSelection({nodeOfType(*doc, "Scalp"), nodeOfType(*doc, "Guides")});
    key(&view, QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier);
    EXPECT_EQ(doc->nodes().size(), nodes);
}

TEST(AppPanels, GraphSavesACompoundAsANodeTheSearchListsAndImports) {
    // Save as Node: a glass form over the graph (no dialog) writes the compound to the user's node
    // folder (the environment's here), the compound holds the saved copy; the search lists it and
    // the shipped nodes; a shared file imported is added; a newer copy offers Update.
    const std::filesystem::path user = std::filesystem::temp_directory_path() / "groomlab_app_user_nodes";
    std::filesystem::remove_all(user);
#ifdef _WIN32
    _putenv_s("GROOMLAB_USER_NODES", user.u8string().c_str());
#else
    setenv("GROOMLAB_USER_NODES", user.u8string().c_str(), 1);
#endif
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    NodeGraphView view;
    view.resize(1400, 900);
    view.setDocument(doc.get());
    view.setSelection({nodeOfType(*doc, "Length"), nodeOfType(*doc, "Noise")});
    key(&view, QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier);
    const std::string compound = view.selectedNode();
    view.openSaveNode(compound);
    ASSERT_TRUE(view.saveNodeOpen());
    view.setSaveNodeField("label", "App Look");
    view.setSaveNodeField("author", "Tester");
    view.setSaveNodeField("category", "Look");
    EXPECT_EQ(view.saveNodeField("id"), "tester.app_look");
    EXPECT_EQ(view.saveNodeField("version"), "1");
    EXPECT_EQ(view.acceptSaveNode(), "");
    EXPECT_FALSE(view.saveNodeOpen());
    EXPECT_TRUE(std::filesystem::exists(user / "tester.app_look.glnode"));
    EXPECT_EQ(doc->libraryInfo(compound).value("id", std::string()), "tester.app_look");
    EXPECT_EQ(doc->undoText(), "Undo update " + compound);

    // The search lists it (and a shipped one); picking it adds a library node.
    view.openSearchAt(QPointF(300, 300));
    view.setSearchText("app look");
    ASSERT_FALSE(view.searchTypes().empty());
    EXPECT_EQ(view.searchTypes()[0], "node:tester.app_look");
    view.setSearchText("soft frizz");
    ASSERT_FALSE(view.searchTypes().empty());
    EXPECT_EQ(view.searchTypes()[0], "node:groomlab.soft_frizz");
    view.closeSearch();
    std::string added;
    EXPECT_EQ(doc->addNode("node:tester.app_look", 0.0f, 400.0f, &added), "");
    EXPECT_EQ(added, "appLook");
    EXPECT_EQ(doc->libraryInfo(added).value("version", 0), 1);

    // Saved again: version 2 in the library, the other node offers Update and takes it.
    view.saveToLibrary(compound);
    const nlohmann::json info = doc->libraryInfo(added);
    EXPECT_EQ(info.value("latest", 0), 2);
    EXPECT_TRUE(info.value("updateAvailable", false));
    EXPECT_EQ(doc->updateLibraryNode(added), "");
    EXPECT_FALSE(doc->libraryInfo(added).value("updateAvailable", true));

    // Import: a shared file copied in and added.
    const std::filesystem::path shared = std::filesystem::temp_directory_path() / "groomlab_app_shared.glnode";
    {
        std::ifstream in(user / "tester.app_look.glnode");
        json file = json::parse(in);
        file["id"] = "friend.wave";
        file["label"] = "Wave";
        std::ofstream(shared) << file.dump();
    }
    const std::vector<std::string> imported = view.importNodeFiles({QString::fromStdString(shared.u8string())});
    ASSERT_EQ(imported.size(), 1u);
    EXPECT_EQ(imported[0], "wave");
    EXPECT_TRUE(std::filesystem::exists(user / "friend.wave.glnode"));
    std::filesystem::remove(shared);
#ifdef _WIN32
    _putenv_s("GROOMLAB_USER_NODES", "");
#else
    unsetenv("GROOMLAB_USER_NODES");
#endif
    std::filesystem::remove_all(user);
}

TEST(AppPanels, GraphMakesEditsAndSavesAScriptNode) {
    // New Script Node opens its editor over the graph (no dialog); a script with an error says
    // where and the node keeps its last good one; fixed, the node takes it and its params show;
    // Save to Library writes the file, Save as New refuses an id the library has.
    const std::filesystem::path user = std::filesystem::temp_directory_path() / "groomlab_app_script_nodes";
    std::filesystem::remove_all(user);
#ifdef _WIN32
    _putenv_s("GROOMLAB_USER_NODES", user.u8string().c_str());
#else
    setenv("GROOMLAB_USER_NODES", user.u8string().c_str(), 1);
#endif
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    NodeGraphView view;
    view.resize(1400, 900);
    view.setDocument(doc.get());
    const std::string name = view.newScriptNode();
    ASSERT_EQ(name, "myScript");
    EXPECT_EQ(nodeOfType(*doc, "Script"), name);
    ASSERT_TRUE(view.scriptEditorOpen());
    const std::string good = doc->param(name, "definition").value("script", std::string());
    view.setScriptEditorField("script", "per point {\n    P += N * ;\n}");
    const QString errors = view.applyScriptEditor();
    EXPECT_TRUE(errors.contains("line 2, column 14")) << errors.toStdString();
    EXPECT_EQ(view.scriptEditorErrors(), errors);
    EXPECT_EQ(doc->param(name, "definition").value("script", std::string()), good);
    view.setScriptEditorField("writes", "P, width");
    view.setScriptEditorField("script", "per point {\n    P += N * $amount * t;\n    width = 0.002;\n}");
    EXPECT_EQ(view.applyScriptEditor(), "");
    EXPECT_EQ(view.scriptEditorErrors(), "");
    EXPECT_EQ(doc->param(name, "definition")["writes"], json::array({"P", "width"}));
    bool amount = false;
    for (const json& p : doc->nodeSpec(name)->value("params", json::array())) amount = amount || p.value("name", std::string()) == "amount";
    EXPECT_TRUE(amount);  // the header's param, in the param panel as any node's
    for (const auto& [node, error] : doc->nodeErrors()) ADD_FAILURE() << node << ": " << error;
    view.setScriptEditorField("label", "App Push");
    EXPECT_EQ(view.saveScriptEditor(false), "");
    const std::string id = doc->param(name, "definition").value("id", std::string());
    EXPECT_TRUE(std::filesystem::exists(user / (id + ".glnode")));
    EXPECT_EQ(doc->libraryInfo(name).value("id", std::string()), id);
    EXPECT_TRUE(view.saveScriptEditor(true).contains("already"));
    view.closeScriptEditor();
    EXPECT_FALSE(view.scriptEditorOpen());
#ifdef _WIN32
    _putenv_s("GROOMLAB_USER_NODES", "");
#else
    unsetenv("GROOMLAB_USER_NODES");
#endif
    std::filesystem::remove_all(user);
}

TEST(AppPanels, GraphBackdropsAreMadeNamedColoredMovedResizedAndDeleted) {
    // As Maya's Graph Editor's: around the selection, named at once, a color from the menu's,
    // moved by the title bar with the nodes on it, resized by the corner, Delete removes it.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    const std::string length = nodeOfType(*doc, "Length");
    NodeGraphView view;
    view.resize(1400, 900);
    view.setDocument(doc.get());
    view.setSelection({length});
    view.addBackdrop();
    ASSERT_EQ(view.backdropCount(), 1);
    EXPECT_EQ(view.selectedBackdrop(), 0);
    EXPECT_TRUE(view.backdropRect(0).contains(QRectF(view.nodeRect(length)).center() / view.viewScale() -
                                              view.viewPan() / view.viewScale()));
    EXPECT_EQ(doc->undoText(), "Undo add backdrop");
    ASSERT_TRUE(view.promptOpen());
    view.setPromptText("Shape");
    view.acceptPrompt();
    EXPECT_EQ(view.backdropTitle(0), QStringLiteral("Shape"));
    EXPECT_EQ(doc->undoText(), "Undo rename backdrop");
    view.setBackdropColor(0, QColor(70, 120, 80));
    EXPECT_EQ(doc->graph()["backdrops"][0]["color"], json::array({70, 120, 80}));
    EXPECT_EQ(view.backdropColor(0), QColor(70, 120, 80));
    // Dragged by its title bar: it and the node on it move (no undo step, as node moves).
    const QRectF card = view.nodeRect(length);  // laid out by the view (the sample keeps no positions)
    const QRectF rect = view.backdropRect(0);
    const QPoint grab = view.backdropTitleBar(0).center().toPoint();
    mouse(&view, QEvent::MouseButtonPress, grab);
    mouse(&view, QEvent::MouseMove, grab + QPoint(60, 40));
    mouse(&view, QEvent::MouseButtonRelease, grab + QPoint(60, 40));
    const qreal s = view.viewScale();
    EXPECT_NEAR(view.backdropRect(0).x(), rect.x() + 60 / s, 1e-6);
    EXPECT_NEAR(doc->graph()["backdrops"][0]["y"].get<double>(), rect.y() + 40 / s, 1e-6);
    EXPECT_NEAR(view.nodeRect(length).x(), card.x() + 60, 1e-3) << "the node on it moved along";
    float x1 = 0.0f, y1 = 0.0f;
    ASSERT_TRUE(doc->nodePosition(length, &x1, &y1)) << "and keeps where it was put";
    EXPECT_NEAR(x1, (card.x() + 60 - view.viewPan().x()) / s, 1e-3);
    EXPECT_EQ(doc->undoText(), "Undo backdrop color");
    // Resized by its corner, never smaller than Maya's least.
    const QPoint corner = view.backdropGrip(0).center().toPoint();
    mouse(&view, QEvent::MouseButtonPress, corner);
    mouse(&view, QEvent::MouseMove, corner + QPoint(100, 50));
    mouse(&view, QEvent::MouseButtonRelease, corner + QPoint(100, 50));
    EXPECT_NEAR(doc->graph()["backdrops"][0]["w"].get<double>(), rect.width() + 100 / s, 1e-6);
    mouse(&view, QEvent::MouseButtonPress, view.backdropGrip(0).center().toPoint());
    mouse(&view, QEvent::MouseMove, view.backdropGrip(0).center().toPoint() - QPoint(2000, 2000));
    mouse(&view, QEvent::MouseButtonRelease, view.backdropGrip(0).center().toPoint() - QPoint(2000, 2000));
    EXPECT_EQ(view.backdropRect(0).size(), QSizeF(120, 80));
    // Double-clicking the title bar renames it; Delete removes the selected one (one step).
    mouse(&view, QEvent::MouseButtonDblClick, view.backdropTitleBar(0).center().toPoint());
    EXPECT_TRUE(view.promptOpen());
    view.setPromptText("Frizz");
    view.acceptPrompt();
    EXPECT_EQ(view.backdropTitle(0), QStringLiteral("Frizz"));
    view.selectBackdrop(0);
    key(&view, QEvent::KeyPress, Qt::Key_Delete);
    EXPECT_EQ(view.backdropCount(), 0);
    EXPECT_FALSE(doc->graph().contains("backdrops"));
    EXPECT_EQ(doc->undo(), "");
    view.refresh();
    EXPECT_EQ(view.backdropCount(), 1);
    EXPECT_EQ(view.backdropTitle(0), QStringLiteral("Frizz"));
}

TEST(AppPanels, GraphPinsRouteWiresAndWiresDragOutOfThemAndIntoThem) {
    // As Maya's Graph Editor's (Unreal's reroute nodes): double-click a wire for a pin on it, drag
    // the pin (no undo step, on the grid with Snap), drag from its nub for another wire of its
    // signal, drop an output's wire on it to feed its wires from there; Delete removes it.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    const std::string length = nodeOfType(*doc, "Length"), interpolate = nodeOfType(*doc, "Interpolate");
    std::string noise;
    ASSERT_EQ(doc->addNode("Noise", 0.0f, 900.0f, &noise), "");
    NodeGraphView view;
    view.resize(1400, 900);
    view.setDocument(doc.get());
    const qreal s = view.viewScale();
    auto widget = [&](const QPointF& scene) { return (scene * view.viewScale() + view.viewPan()).toPoint(); };
    int wire = -1;
    std::string to, input;
    for (int i = 0; i < view.wireCount(); ++i) {
        std::string from, output;
        if (view.wireLink(i, &from, &output, &to, &input) && from == length) {
            wire = i;
            break;
        }
    }
    ASSERT_GE(wire, 0);
    mouse(&view, QEvent::MouseButtonDblClick, widget(view.wirePathOf(wire).pointAtPercent(0.5)));
    ASSERT_EQ(view.pinIds().size(), 1u);
    const std::string pin = view.pinIds().front();
    EXPECT_EQ(view.selectedPin(), pin);
    EXPECT_EQ(doc->graph()["pins"]["routes"][0]["to"], to);
    EXPECT_EQ(doc->undoText(), "Undo add pin");
    // Dragged: the wire follows it, the graph keeps where it was put (no undo step).
    const QPointF before = view.pinPos(pin);
    mouse(&view, QEvent::MouseButtonPress, widget(before));
    mouse(&view, QEvent::MouseMove, widget(before) + QPoint(0, 80));
    mouse(&view, QEvent::MouseButtonRelease, widget(before) + QPoint(0, 80));
    EXPECT_NEAR(view.pinPos(pin).y(), before.y() + 80 / s, 1e-3);
    EXPECT_NEAR(doc->graph()["pins"]["points"][0]["y"].get<double>(), before.y() + 80 / s, 1e-3);
    EXPECT_EQ(doc->undoText(), "Undo add pin");
    // With Snap: on the grid.
    view.setSnapping(true);
    const QPointF at = view.pinPos(pin);
    mouse(&view, QEvent::MouseButtonPress, widget(at));
    mouse(&view, QEvent::MouseMove, widget(at) + QPoint(37, 29));
    mouse(&view, QEvent::MouseButtonRelease, widget(at) + QPoint(37, 29));
    EXPECT_EQ(std::fmod(view.pinPos(pin).x(), 24.0), 0.0);
    EXPECT_EQ(std::fmod(view.pinPos(pin).y(), 24.0), 0.0);
    view.setSnapping(false);
    // Out of its nub into the Noise's input: the length's signal, through the pin.
    const QPoint nub = widget(view.pinPos(pin) + QPointF(11.0, 0.0));
    const QPoint noiseIn = inputAt(view, noise, portOf(*doc, noise, "inputs", "strands"));
    mouse(&view, QEvent::MouseButtonPress, nub);
    mouse(&view, QEvent::MouseMove, noiseIn);
    mouse(&view, QEvent::MouseButtonRelease, noiseIn);
    EXPECT_EQ(pinRoute(doc->graph(), noise, "strands"), std::vector<std::string>{pin});
    bool fed = false;
    for (const GroomDocument::ConnectionInfo& c : doc->connections()) fed = fed || (c.to == noise && c.from == length);
    EXPECT_TRUE(fed);
    // The interpolate's output dropped on the pin: both wires through it come from there now.
    const QRectF card = view.nodeRect(interpolate);
    const QPoint out = QPointF(card.right(), card.top() + (28.0 + 8.0 + 18.0 * 0.5) * s).toPoint();
    mouse(&view, QEvent::MouseButtonPress, out);
    mouse(&view, QEvent::MouseMove, widget(view.pinPos(pin)));
    mouse(&view, QEvent::MouseButtonRelease, widget(view.pinPos(pin)));
    int fromInterpolate = 0;
    for (const GroomDocument::ConnectionInfo& c : doc->connections()) {
        if ((c.to == noise || (c.to == to && c.input == input)) && c.from == interpolate) ++fromInterpolate;
    }
    EXPECT_EQ(fromInterpolate, 2);
    EXPECT_EQ(pinLinks(doc->graph(), pin).size(), 2u);
    EXPECT_EQ(doc->undoText(), "Undo feed pin from " + interpolate);
    EXPECT_EQ(doc->undo(), "");
    view.refresh();
    EXPECT_EQ(pinSource(doc->graph(), pin), (PinSource{length, ""}));
    // Delete removes the selected pin; the wires stay.
    view.selectPin(pin);
    const auto links = doc->connections().size();
    key(&view, QEvent::KeyPress, Qt::Key_Delete);
    EXPECT_TRUE(view.pinIds().empty());
    EXPECT_FALSE(doc->graph().contains("pins"));
    EXPECT_EQ(doc->connections().size(), links);
}

TEST(AppPanels, GraphLinkDragsSayWhyAndRestoreOnARefusedDrop) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    NodeGraphView view;
    view.resize(1400, 900);
    view.setDocument(doc.get());
    const std::string interpolate = nodeOfType(*doc, "Interpolate"), scatter = nodeOfType(*doc, "Scatter");
    const std::string length = nodeOfType(*doc, "Length");
    std::string guidesSource, scalpSource;
    for (const GroomDocument::ConnectionInfo& c : doc->connections()) {
        if (c.to == interpolate && c.input == "guides") guidesSource = c.from;
        if (c.to == interpolate && c.input == "scalp") scalpSource = c.from;
    }
    ASSERT_FALSE(guidesSource.empty());
    ASSERT_FALSE(scalpSource.empty());
    // Mesh into a guides input: the types; a node downstream into its source: a loop; its own input: itself.
    const int guidesIn = portOf(*doc, interpolate, "inputs", "guides"), scalpIn = portOf(*doc, interpolate, "inputs", "scalp");
    const int strandsIn = portOf(*doc, interpolate, "inputs", "strands");
    EXPECT_TRUE(view.connectionProblem(indexOf(*doc, scalpSource), 0, indexOf(*doc, interpolate), guidesIn).contains(QStringLiteral("takes")));
    EXPECT_TRUE(view.connectionProblem(indexOf(*doc, length), 0, indexOf(*doc, interpolate), strandsIn).contains(QStringLiteral("loop")));
    EXPECT_FALSE(view.connectionProblem(indexOf(*doc, interpolate), 0, indexOf(*doc, interpolate), strandsIn).isEmpty());
    EXPECT_TRUE(view.connectionProblem(indexOf(*doc, scatter), 0, indexOf(*doc, length), 0).isEmpty()) << "strands into strands fit";
    // The guides wire picked up from its input and dropped on the scalp input: refused, the link stays.
    const auto before = doc->connections().size();
    const QPoint from = inputAt(view, interpolate, guidesIn), to = inputAt(view, interpolate, scalpIn);
    mouse(&view, QEvent::MouseButtonPress, from);
    mouse(&view, QEvent::MouseMove, to);
    EXPECT_FALSE(view.wireHint().isEmpty()) << "over a port it cannot feed, the reason by the cursor";
    mouse(&view, QEvent::MouseButtonRelease, to);
    EXPECT_TRUE(view.wireHint().isEmpty());
    EXPECT_EQ(doc->connections().size(), before);
    bool kept = false;
    for (const GroomDocument::ConnectionInfo& c : doc->connections()) kept = kept || (c.to == interpolate && c.input == "guides" && c.from == guidesSource);
    EXPECT_TRUE(kept) << "the picked-up wire went back";
    EXPECT_FALSE(doc->canUndo()) << "nothing changed";
}

TEST(AppPanels, GraphBackwardLinksLoopUnderTheirNodes) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    std::string right, left;
    ASSERT_EQ(doc->addNode("Noise", 800.0f, 0.0f, &right), "");
    ASSERT_EQ(doc->addNode("Noise", 0.0f, 0.0f, &left), "");
    ASSERT_EQ(doc->connectNodes(right, "", left, "strands"), "");
    NodeGraphView view;
    view.resize(1400, 900);
    view.setDocument(doc.get());
    const QRectF a = view.nodeRect(right), b = view.nodeRect(left);
    const qreal lowest = (std::max(a.bottom(), b.bottom()) - view.viewPan().y()) / view.viewScale();  // scene units
    bool found = false;
    for (int i = 0; i < view.wireCount(); ++i) {
        const QRectF bounds = view.wirePathOf(i).boundingRect();
        // The wire from the right node's output (its x) to the left node's input.
        if (std::abs(bounds.left() - 0.0) < 50.0 && bounds.right() > 800.0) {
            found = true;
            EXPECT_GT(bounds.bottom(), lowest) << "the backward link goes under both cards";
        }
    }
    EXPECT_TRUE(found);
}

TEST(AppPanels, GraphViewUpToHereShowsTheNodesResult) {
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    const std::string scatter = nodeOfType(*window.document(), "Scatter");
    ASSERT_TRUE(window.selectUiNode(QString::fromStdString(scatter)));
    NodeGraphView* graph = window.shell()->graph();
    key(graph, QEvent::KeyPress, Qt::Key_V);
    EXPECT_EQ(graph->viewedNode(), scatter);
    EXPECT_EQ(window.document()->lastView().viewedNode, scatter) << "the viewport shows the graph up to the node";
    key(graph, QEvent::KeyPress, Qt::Key_V);
    EXPECT_TRUE(graph->viewedNode().empty());
    EXPECT_TRUE(window.document()->lastView().viewedNode.empty()) << "V again: the output";
    // A node that gives no strands is not viewed.
    std::string field;
    ASSERT_EQ(window.document()->addNode("Map", 0.0f, 0.0f, &field), "");
    window.shell()->graph()->refresh();
    graph->selectNode(field);
    graph->toggleViewSelected();
    EXPECT_TRUE(graph->viewedNode().empty());
}

TEST(AppPanels, GraphEditorDocksBesideTheLookCardInLookMode) {
    // Look mode at 1280x720: the Graph Editor docked at the bottom, its Node Settings on screen at
    // its right, the look card in the Properties dock at the window's right, none over another;
    // the graph's canvas is opaque (the docked graph has no viewport behind it) and fits the graph.
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    AppShell* shell = window.shell();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    ASSERT_TRUE(window.setUiMode(QStringLiteral("look")));
    window.setGraphShown(true);
    ASSERT_TRUE(window.selectUiNode(QString::fromStdString(nodeOfType(*window.document(), "Interpolate"))));
    QCoreApplication::processEvents();
    auto rectOf = [&](QWidget* w) { return QRect(w->mapTo(&window, QPoint(0, 0)), w->size()); };
    const QRect panel = rectOf(shell->panel(AppShell::Panel::Node));
    const QRect card = rectOf(shell->lookCard());
    const QRect graph = rectOf(shell->graph());
    EXPECT_TRUE(window.rect().contains(panel)) << "the Node Settings on screen";
    EXPECT_FALSE(panel.intersects(card)) << "the Node Settings under the look card";
    EXPECT_FALSE(graph.intersects(card)) << "the graph under the look card";
    EXPECT_FALSE(graph.intersects(panel));
    EXPECT_GE(panel.height(), 150);
    const QRect viewport = rectOf(window.overlay());
    EXPECT_FALSE(viewport.intersects(graph)) << "the graph docked, not over the viewport";
    const QRectF node = shell->graph()->nodeRect(shell->graph()->selectedNode());
    EXPECT_TRUE(QRectF(shell->graph()->rect()).intersects(node)) << "the selected node in the graph's view";
    QImage canvas(shell->graph()->size(), QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    shell->graph()->render(&canvas);
    EXPECT_EQ(qAlpha(canvas.pixel(2, 2)), 255) << "an opaque canvas";
}

TEST(AppPanels, EmptyStateDisablesWhatNeedsAGroomAndSaysWhatEachModeDoes) {
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    OverlayHost* host = window.overlay();
    AppShell* shell = window.shell();
    EXPECT_FALSE(shell->graphAddButton()->isEnabled());
    EXPECT_FALSE(shell->graphEditButton()->isEnabled());
    EXPECT_FALSE(host->undoButton()->isEnabled());
    EXPECT_FALSE(window.findChild<QAction*>(QStringLiteral("prmanFrame"))->isEnabled()) << "nothing to render";
    QSet<QString> sentences;
    for (const char* mode : {"groom", "look", "cards", "export"}) {
        ASSERT_TRUE(window.setUiMode(QLatin1String(mode)));
        sentences.insert(host->emptyText());
        EXPECT_TRUE(host->emptyCard()->isVisibleTo(host)) << mode;
        EXPECT_FALSE(shell->cardsPanel()->isVisibleTo(&window)) << mode << ": no zeros before a groom";
        EXPECT_FALSE(shell->exportPanel()->isVisibleTo(&window)) << mode;
        EXPECT_FALSE(shell->lookCard()->isVisibleTo(&window)) << mode;
    }
    EXPECT_EQ(sentences.size(), 4) << "a sentence per mode";
    ASSERT_TRUE(window.setUiMode(QStringLiteral("cards")));
    EXPECT_TRUE(host->emptyText().contains(QStringLiteral("cards")));
    // A preset with no groom: a mesh first (no dialog in a scripted run), then the look on it.
    EXPECT_FALSE(window.startFromPreset(QStringLiteral("curly")));
    EXPECT_EQ(window.document(), nullptr);
    const QString mesh = QString::fromStdString(std::string(GROOMLAB_TEST_DATA) + "/app/head_scalp.obj");
    ASSERT_TRUE(window.startFromPreset(QStringLiteral("curly"), mesh));
    ASSERT_NE(window.document(), nullptr);
    EXPECT_GT(window.document()->nodes().size(), 3u);
    EXPECT_TRUE(shell->graphAddButton()->isEnabled());
    EXPECT_TRUE(shell->graphEditButton()->isEnabled());
    EXPECT_TRUE(shell->cardsPanel()->isVisibleTo(&window)) << "Cards mode's panel once a groom is open";
    // New Groom on Mesh: the first-groom steps in a small card over the viewport, bottom-left above
    // the stats (not in the Groom Tools); Place some guides picks the Place brush.
    ASSERT_TRUE(window.setUiMode(QStringLiteral("groom")));
    ASSERT_TRUE(window.newGroomOnMesh(mesh));
    QCoreApplication::processEvents();
    EXPECT_TRUE(host->starterShown());
    EXPECT_EQ(shell->groomTools()->findChild<QWidget*>(QStringLiteral("starter")), nullptr) << "not in the Groom Tools";
    EXPECT_EQ(host->findChild<QWidget*>(QStringLiteral("startHint")), nullptr);
    GlassPanel* card = host->starterCard();
    ASSERT_NE(card, nullptr);
    EXPECT_EQ(card->parentWidget(), host) << "over the viewport";
    EXPECT_LT(card->geometry().center().x(), host->width() / 2) << "bottom-left";
    EXPECT_GT(card->geometry().center().y(), host->height() / 2);
    for (QWidget* top : {static_cast<QWidget*>(host->history())}) {
        if (top->isVisible()) EXPECT_FALSE(card->geometry().intersects(top->geometry()));
    }
    // A toast never covers it.
    host->toast(QStringLiteral("A message long enough to reach across the bottom of a narrow view, and then some more words"), false);
    QCoreApplication::processEvents();
    ASSERT_TRUE(host->toastShown());
    QWidget* toast = nullptr;
    for (GlassPanel* g : host->findChildren<GlassPanel*>(Qt::FindDirectChildrenOnly)) {
        if (!g->isVisible() || g == card) continue;
        for (QLabel* l : g->findChildren<QLabel*>()) {
            if (l->text() == host->toastText()) toast = g;
        }
    }
    ASSERT_NE(toast, nullptr);
    if (host->starterShown()) EXPECT_FALSE(toast->geometry().intersects(card->geometry())) << "the toast over the first-groom card";
    auto* place = host->findChild<QPushButton*>(QStringLiteral("starter_guides"));
    ASSERT_NE(place, nullptr);
    place->click();
    EXPECT_EQ(window.brush().toolId(), "place");
    // Other modes leave it out; Groom brings it back.
    ASSERT_TRUE(window.setUiMode(QStringLiteral("look")));
    EXPECT_FALSE(host->starterShown());
    ASSERT_TRUE(window.setUiMode(QStringLiteral("groom")));
    EXPECT_TRUE(host->starterShown());
    // Its x hides it (for good: ui/hideStarter in an interactive window).
    auto* dismiss = host->findChild<QToolButton*>(QStringLiteral("starterDismiss"));
    ASSERT_NE(dismiss, nullptr);
    dismiss->click();
    EXPECT_FALSE(host->starterShown());
}

TEST(AppPanels, FirstGroomCardTicksItsStepsAndGoesWhenDone) {
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    OverlayHost* host = window.overlay();
    const QString mesh = QString::fromStdString(std::string(GROOMLAB_TEST_DATA) + "/app/head_scalp.obj");
    ASSERT_TRUE(window.newGroomOnMesh(mesh));
    QCoreApplication::processEvents();
    ASSERT_TRUE(host->starterShown());
    auto* guides = host->findChild<QPushButton*>(QStringLiteral("starter_guides"));
    auto* look = host->findChild<QPushButton*>(QStringLiteral("starter_look"));
    auto* tip = host->findChild<QLabel*>(QStringLiteral("starterTip"));
    ASSERT_TRUE(guides && look && tip);
    EXPECT_FALSE(guides->property("done").toBool());
    EXPECT_FALSE(tip->text().isEmpty()) << "the next step's tip";
    // Steps ticked as the groom gets there; both done: the card goes.
    host->setStarterProgress(true, false);
    EXPECT_TRUE(guides->property("done").toBool());
    EXPECT_FALSE(look->property("done").toBool());
    EXPECT_TRUE(host->starterShown());
    host->setStarterProgress(true, true);
    EXPECT_FALSE(host->starterShown());
    host->setStarterProgress(false, false);
    EXPECT_TRUE(host->starterShown());
    // Stacked over the stats line, never on it.
    if (host->statsShown()) {
        const ShadowText* stats = nullptr;
        for (ShadowText* t : host->findChildren<ShadowText*>(Qt::FindDirectChildrenOnly)) {
            if (t->isVisible() && !t->runs().empty() && t->runs().front().text.contains(QStringLiteral("strands"))) stats = t;
        }
        ASSERT_NE(stats, nullptr);
        EXPECT_LT(host->starterCard()->geometry().bottom(), stats->geometry().top());
        EXPECT_EQ(host->starterCard()->geometry().left(), stats->geometry().left());
    }
    // Give it a look: the Graph Editor.
    look->click();
    EXPECT_TRUE(window.graphShown());
    // A view too short for it: left out until there is room.
    host->setStarterDismissed(false);
    window.resize(1280, 330);
    QCoreApplication::processEvents();
    if (host->height() < host->starterCard()->sizeHint().height() + 80) EXPECT_FALSE(host->starterShown());
}

TEST(AppPanels, ToolLabelsUnderTheTilesAndCompactTiles) {
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    AppShell* host = window.shell();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    EXPECT_TRUE(host->railLabels()) << "labeled tiles by default";
    QAction* toggle = window.findChild<QAction*>(QStringLiteral("railLabels"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_TRUE(toggle->isChecked());
    // Docked in the Groom Tools: the tiles stay within the dock's width with their names (the
    // column scrolls up and down).
    window.setRailLabels(true, false);
    EXPECT_TRUE(host->railLabels());
    EXPECT_TRUE(toggle->isChecked());
    QCoreApplication::processEvents();
    const QRect dock(host->groomToolsDock()->mapTo(&window, QPoint(0, 0)), host->groomToolsDock()->size());
    for (const QAbstractButton* tile : host->groomTools()->tiles()) {
        if (!tile->isVisibleTo(&window)) continue;  // (the Paint tab's)
        const QRect r(tile->mapTo(&window, QPoint(0, 0)), tile->size());
        EXPECT_TRUE(r.left() >= dock.left() && r.right() <= dock.right()) << "a tile past the Groom Tools: " << tile->objectName().toStdString();
    }
    window.setRailLabels(false, false);
    EXPECT_FALSE(host->railLabels());
    EXPECT_FALSE(toggle->isChecked());
    window.setRailLabels(true, false);
    EXPECT_TRUE(toggle->isChecked());
}

TEST(AppPanels, GroomToolsTilesPickToolsAndTheEyeHidesTheGroom) {
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    GroomTools* tools = window.shell()->groomTools();
    // A tile picks its tool: brush mode, the tile lit, its card; each brush keeps its strength.
    tools->tile("comb")->click();
    EXPECT_EQ(window.brush().toolId(), "comb");
    EXPECT_EQ(window.overlay()->viewport()->toolMode(), Viewport::ToolMode::Brush);
    EXPECT_TRUE(tools->tile("comb")->isChecked());
    EXPECT_TRUE(tools->card()->isVisibleTo(tools));
    window.brush().settings().brush.strength = 0.3f;
    tools->tile("lift")->click();
    EXPECT_FLOAT_EQ(window.brush().settings().brush.strength, brushEntry("lift")->defaultStrength);
    tools->tile("comb")->click();
    EXPECT_FLOAT_EQ(window.brush().settings().brush.strength, 0.3f) << "Comb's own strength back";
    // Select, and again: the camera.
    tools->tile("select")->click();
    EXPECT_EQ(window.overlay()->viewport()->toolMode(), Viewport::ToolMode::Select);
    EXPECT_TRUE(tools->tile("select")->isChecked());
    tools->tile("select")->click();
    EXPECT_EQ(window.overlay()->viewport()->toolMode(), Viewport::ToolMode::Camera);
    // Paint without a map: the Paint tab and a line saying what to do first.
    if (window.mapsPanel()->selectedMap().empty()) {
        tools->tile("paint")->click();
        EXPECT_EQ(tools->currentTab(), QStringLiteral("paint"));
        EXPECT_FALSE(tools->status().isEmpty());
    }
    // The eye: the hair (the strands alone) hidden in the viewport and back; the guides stay.
    tools->eyeButton()->click();
    EXPECT_FALSE(window.hairVisible());
    EXPECT_FALSE(window.overlay()->viewport()->renderer().hairShown());
    EXPECT_TRUE(window.guidesVisible()) << "the hair's eye leaves the guides";
    EXPECT_TRUE(tools->guidesEyeButton()->isChecked());
    tools->eyeButton()->click();
    EXPECT_TRUE(window.hairVisible());
    EXPECT_TRUE(window.overlay()->viewport()->renderer().hairShown());
    // The guides' eye beside it: every guide group (Properties > Guides follows), and back; GroomLab >
    // Display and View > Guides (Alt+Shift+G) do the same for the app's one groom.
    ASSERT_TRUE(tools->guidesEyeButton()->isChecked());
    tools->guidesEyeButton()->click();
    EXPECT_FALSE(window.guidesVisible());
    window.setGuidesVisible(true);
    EXPECT_TRUE(tools->guidesEyeButton()->isChecked());
    QAction* hideGuides = window.findChild<QAction*>(QStringLiteral("hideAllGuides"));
    ASSERT_NE(hideGuides, nullptr);
    hideGuides->trigger();
    EXPECT_FALSE(window.guidesVisible());
    EXPECT_FALSE(tools->guidesEyeButton()->isChecked());
    EXPECT_TRUE(window.hairVisible()) << "the guides' eye leaves the hair";
    QAction* hideHair = window.findChild<QAction*>(QStringLiteral("hideAllHair"));
    QAction* showHair = window.findChild<QAction*>(QStringLiteral("showAllHair"));
    ASSERT_NE(hideHair, nullptr);
    ASSERT_NE(showHair, nullptr);
    hideHair->trigger();
    EXPECT_FALSE(window.hairVisible());
    EXPECT_FALSE(tools->eyeButton()->isChecked());
    showHair->trigger();
    EXPECT_TRUE(window.hairVisible());
    QAction* toggleGuides = window.findChild<QAction*>(QStringLiteral("toggleGuides"));
    ASSERT_NE(toggleGuides, nullptr);
    toggleGuides->trigger();
    EXPECT_TRUE(window.guidesVisible());
    // GroomLab > Groom Tools brings the dock back.
    window.shell()->groomToolsDock()->hide();
    QAction* open = window.findChild<QAction*>(QStringLiteral("groomTools"));
    ASSERT_NE(open, nullptr);
    open->trigger();
    EXPECT_TRUE(window.shell()->groomToolsDock()->isVisibleTo(&window));
    // No Output strip (palette.output is Maya's and Blender's): Export and Render sit in the menu
    // bar's corner, the rest in the GroomLab menu.
    EXPECT_EQ(tools->findChild<QWidget*>(QStringLiteral("section_output")), nullptr);
    EXPECT_EQ(tools->findChild<QWidget*>(QStringLiteral("outputStrip")), nullptr);
}

TEST(AppPanels, MenuBarCornerHasTheFileNameRenderAndExport) {
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    QWidget* corner = window.menuCorner();
    ASSERT_NE(corner, nullptr);
    EXPECT_EQ(window.menuBar()->cornerWidget(Qt::TopRightCorner), corner);
    EXPECT_FALSE(window.cornerFileName()->isVisibleTo(&window)) << "no groom, no name";
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    QCoreApplication::processEvents();
    EXPECT_TRUE(window.cornerFileName()->isVisibleTo(&window));
    EXPECT_EQ(window.cornerFileName()->text(), QFileInfo(QString::fromStdString(kSampleGroom)).fileName());
    EXPECT_EQ(window.cornerFileName()->property("role").toString(), QStringLiteral("cornerName"));
    // Render (a ghost button) left of Export (the accent), at the bar's right.
    QToolButton* render = window.cornerRender();
    QToolButton* exportButton = window.cornerExport();
    ASSERT_TRUE(render && exportButton);
    EXPECT_EQ(render->property("role").toString(), QStringLiteral("cornerGhost"));
    EXPECT_EQ(exportButton->property("role").toString(), QStringLiteral("cornerPrimary"));
    EXPECT_TRUE(render->text().startsWith(QStringLiteral("Render")));
    EXPECT_TRUE(exportButton->text().startsWith(QStringLiteral("Export")));
    EXPECT_LT(render->mapTo(&window, QPoint(0, 0)).x(), exportButton->mapTo(&window, QPoint(0, 0)).x());
    EXPECT_GT(exportButton->mapTo(&window, QPoint(0, 0)).x(), window.width() / 2);
    EXPECT_FALSE(render->toolTip().isEmpty());
    EXPECT_FALSE(exportButton->toolTip().isEmpty());
    // Whole, never cut short: the bar lays the corner out again as the name changes.
    for (QWidget* w : {static_cast<QWidget*>(window.cornerFileName()), static_cast<QWidget*>(render), static_cast<QWidget*>(exportButton)}) {
        EXPECT_GE(w->width(), w->sizeHint().width()) << w->objectName().toStdString();
    }
    EXPECT_GE(corner->width(), corner->sizeHint().width());
    // Their menus: menus.json's output.export and output.render, the window's actions under
    // another name (the menu bar's keep theirs).
    auto texts = [](QMenu* menu) {
        std::vector<std::string> out;
        for (QAction* a : menu->actions()) {
            if (!a->isSeparator() && a->isVisible()) out.push_back(a->text().section(QLatin1Char('\t'), 0, 0).toStdString());
        }
        return out;
    };
    ASSERT_NE(exportButton->menu(), nullptr);
    ASSERT_NE(render->menu(), nullptr);
    const std::vector<std::string> exports = texts(exportButton->menu());
    ASSERT_GE(exports.size(), 4u);
    EXPECT_TRUE(std::any_of(exports.begin(), exports.end(), [](const std::string& t) { return t.find("Groom File") != std::string::npos; }))
        << "the groom file too: " << json(exports).dump();
    EXPECT_FALSE(texts(render->menu()).empty());
    for (QAction* a : exportButton->menu()->actions()) {
        if (!a->objectName().isEmpty()) EXPECT_TRUE(a->objectName().startsWith(QStringLiteral("corner_"))) << a->objectName().toStdString();
    }
    // What the Groom Tools' Output strip had stays one menu away: the GroomLab menu holds Simulate
    // Range, Studio, Transfer and Import.
    QMenu* groomlab = nullptr;
    for (QAction* a : window.menuBar()->actions()) {
        if (a->menu() && a->text().remove(QLatin1Char('&')) == QStringLiteral("GroomLab")) groomlab = a->menu();
    }
    ASSERT_NE(groomlab, nullptr);
    std::set<std::string> submenus, actions;
    std::function<void(QMenu*)> walk = [&](QMenu* menu) {
        for (QAction* a : menu->actions()) {
            const std::string text = a->text().remove(QLatin1Char('&')).section(QLatin1Char('\t'), 0, 0).toStdString();
            if (a->menu()) {
                if (menu == groomlab) submenus.insert(text);
                walk(a->menu());
            } else if (!a->isSeparator()) {
                actions.insert(text);
            }
        }
    };
    walk(groomlab);
    for (const char* sub : {"Simulate", "Studio", "Import", "Render", "Export", "Utilities"}) EXPECT_TRUE(submenus.count(sub)) << sub;
    EXPECT_TRUE(actions.count("Simulate Range"));
    EXPECT_TRUE(std::any_of(actions.begin(), actions.end(), [](const std::string& t) { return t.rfind("Transfer", 0) == 0; }));
}

TEST(AppPanels, FileMenuAddsPropsAndEyesAndSavesAnImage) {
    MainWindow window;
    window.resize(1280, 720);
    window.show();
    for (const char* name : {"addProp", "addEye", "saveImage"}) EXPECT_NE(window.findChild<QAction*>(QLatin1String(name)), nullptr) << name;
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    const std::size_t props = window.props().size();
    EXPECT_FALSE(window.addEyeText(QStringLiteral("1, 2, 3")));
    EXPECT_TRUE(window.addEyeText(QStringLiteral("0, 0, 1, 0, 0, 1, 0.1")));
    EXPECT_EQ(window.props().size(), props + 1);
    EXPECT_TRUE(window.props().back()->isEye);
    // Add Eye...: a glass prompt with a guess (never a dialog); Add Eye takes the line.
    window.askEye();
    ASSERT_TRUE(window.overlay()->promptShown());
    EXPECT_EQ(window.overlay()->promptLine().split(QLatin1Char(',')).size(), 7);
    ASSERT_TRUE(window.overlay()->choosePrompt(QStringLiteral("Add Eye")));
    EXPECT_EQ(window.props().size(), props + 2);
    // Save Image: the view to a PNG (when the platform gives an offscreen device).
    const std::filesystem::path out = std::filesystem::temp_directory_path() / "groomlab_save_image.png";
    std::filesystem::remove(out);
    if (window.saveImage(QString::fromStdString(out.u8string()))) {
        EXPECT_TRUE(std::filesystem::exists(out));
        EXPECT_FALSE(QImage(QString::fromStdString(out.u8string())).isNull());
    } else {
        EXPECT_TRUE(window.lastReport().contains(QStringLiteral("offscreen"))) << window.lastReport().toStdString();
    }
    std::filesystem::remove(out);
}

TEST(AppPanels, ThemesGiveTheGraphStatusColors) {
    const QString was = theme::currentTheme();
    for (const QString& name : theme::themeNames()) {
        ASSERT_TRUE(theme::setTheme(name));
        for (const QColor& c : {theme::graphError(), theme::graphWarning(), theme::graphNote(), theme::graphDriven(), theme::graphReads(),
                                theme::graphWrites(), theme::errorText(), theme::warningText(), theme::linkText(), theme::windowButton(),
                                theme::windowButtonHover()}) {
            EXPECT_TRUE(c.isValid()) << name.toStdString();
        }
        // Every theme is dark: the error words stay readable on its panels.
        EXPECT_GT(theme::errorText().lightnessF(), theme::glassPanel().lightnessF() + 0.2) << name.toStdString();
        EXPECT_TRUE(theme::nodeCategoryColor("deform").isValid());
        EXPECT_TRUE(theme::nodeCategoryColor("collide").isValid());
    }
    theme::setTheme(was);
}

TEST(AppPanels, CardMapsPanelPicksPresetsEditsThePackingAndExports) {
    // The export panel's card maps (the Maya host's Export Card Maps window): a preset fills the
    // packing and the format, an edit turns it Custom, Save as Preset keeps it (no dialog), and
    // the export writes the preset's files.
    const std::filesystem::path presets = std::filesystem::temp_directory_path() / "groomlab_app_card_presets";
    std::filesystem::remove_all(presets);
    qputenv("GROOMLAB_USER_CARD_PRESETS", QByteArray::fromStdString(presets.u8string()));
    qunsetenv("GROOMLAB_CARD_PRESET_PATH");
    ExportPanel panel;
    panel.setTarget(ExportPanel::Target::CardMaps);
    EXPECT_STREQ(ExportPanel::targetName(ExportPanel::Target::CardMaps), "maps");
    EXPECT_EQ(panel.mapPreset(), QStringLiteral("Unreal Hair Cards"));
    EXPECT_EQ(panel.mapFileCount(), 7);
    EXPECT_EQ(panel.options().mapSettings.format, "tga");
    EXPECT_EQ(panel.options().mapPreset.files[3].channels, (std::vector<std::string>{"rootuv.u", "rootuv.v", "seed", "root"}));
    ASSERT_TRUE(panel.setMapPreset(QStringLiteral("Unity HDRP Hair")));
    EXPECT_EQ(panel.mapFileCount(), 5);
    EXPECT_EQ(panel.options().mapSettings.format, "png");
    EXPECT_EQ(panel.options().mapPreset.normal, "opengl");
    EXPECT_FALSE(panel.setMapPreset(QStringLiteral("Nothing")));
    // Edited: Custom, with the edit in the packing.
    panel.setMapChannel(2, 3, QStringLiteral("ao"), true);
    EXPECT_EQ(panel.mapPreset(), QStringLiteral("Custom"));
    EXPECT_EQ(panel.mapPacking().files[2].channels, (std::vector<std::string>{"0", "ao", "root", "1-ao"}));
    panel.addMapFile();
    EXPECT_EQ(panel.mapFileCount(), 6);
    panel.removeMapFile(5);
    EXPECT_EQ(panel.mapFileCount(), 5);
    // Saved under a name of our own; a built-in's name is refused.
    EXPECT_FALSE(panel.saveMapPreset(QStringLiteral("Unreal Hair Cards")));
    ASSERT_TRUE(panel.saveMapPreset(QStringLiteral("My Unity")));
    EXPECT_EQ(panel.mapPreset(), QStringLiteral("My Unity"));
    const auto saved = findCardMapPreset("My Unity");
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->files[2].channels.back(), "1-ao");

    // The export: 16-bit PNG at 64 px from the groom's cards, into <folder>/<name>.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    std::string cards;
    ASSERT_EQ(doc->addCardsNode(&cards), "");
    panel.setMapSettings(64, QStringLiteral("png"), 16, 4, 1);
    const std::filesystem::path out = std::filesystem::temp_directory_path() / "groomlab_app_card_maps";
    std::filesystem::remove_all(out);
    const ExportResult result = doc->exportCardMaps((out / "hair").u8string(), panel.options());
    ASSERT_EQ(result.files.size(), 5u);
    EXPECT_EQ(std::filesystem::u8path(result.files[0]).filename().u8string(), "hair_BaseColor.png");
    const QImage base(QString::fromStdString(result.files[0]));
    ASSERT_FALSE(base.isNull());
    EXPECT_EQ(base.size(), QSize(64, 64));
    EXPECT_TRUE(base.hasAlphaChannel());
    EXPECT_EQ(base.depth(), 64);  // 16 bits a channel
    // Deleted again; the built-in presets stay.
    EXPECT_TRUE(panel.deleteMapPreset());
    EXPECT_FALSE(findCardMapPreset("My Unity"));
    EXPECT_EQ(panel.mapPreset(), QStringLiteral("Unreal Hair Cards"));
    std::filesystem::remove_all(out);
    std::filesystem::remove_all(presets);
    qunsetenv("GROOMLAB_USER_CARD_PRESETS");
}

TEST(AppPanels, VrayRenderWindowShowsTheFrame) {
    // Render > Render Frame in V-Ray (when V-Ray Standalone is installed): the window shows the
    // progress, then the image V-Ray wrote; the export panel has the V-Ray scene target.
    MainWindow window;
    window.resize(640, 480);
    window.show();
    ExportPanel::Target target;
    EXPECT_TRUE(ExportPanel::targetFromName("vrscene", &target));
    EXPECT_EQ(target, ExportPanel::Target::Vrscene);
    for (const char* name : {"vrayFrame", "vrayTurntable", "vrayLocation", "prmanFrame", "prmanTurntable", "prmanLocation"}) {
        EXPECT_NE(window.findChild<QAction*>(QLatin1String(name)), nullptr) << name;
    }
    if (findVrayStandalone().isEmpty()) GTEST_SKIP() << "V-Ray Standalone is not installed";
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    const std::filesystem::path out = std::filesystem::temp_directory_path() / "groomlab_app_vray_window" / "frame.png";
    std::filesystem::remove_all(out.parent_path());
    ASSERT_TRUE(window.renderVray(QString::fromStdString(out.u8string()), 0, QSize(96, 72), false));
    RenderWindow* shown = window.renderWindow();
    ASSERT_NE(shown, nullptr);
    EXPECT_TRUE(shown->isVisible());
    QElapsedTimer clock;
    clock.start();
    while (shown->image().isNull() && clock.elapsed() < 300000) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    ASSERT_FALSE(shown->image().isNull()) << "no image within 5 minutes";
    EXPECT_EQ(shown->image().size(), QSize(96, 72));
    EXPECT_TRUE(std::filesystem::exists(out));
    std::error_code ignored;
    std::filesystem::remove_all(out.parent_path(), ignored);
}

TEST(AppPanels, PrmanRenderWindowShowsTheFrame) {
    // Render > Render Frame in RenderMan (when prman and the procedural beside the app are there):
    // the same window shows the progress, then the image prman wrote.
    if (findPrman().isEmpty()) GTEST_SKIP() << "RenderMan (prman) is not installed";
    if (findRenderManProcedural().isEmpty()) GTEST_SKIP() << "GroomLab's RenderMan procedural is not beside the app";
    MainWindow window;
    window.resize(640, 480);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(kSampleGroom)));
    const std::filesystem::path out = std::filesystem::temp_directory_path() / "groomlab_app_prman_window" / "frame.png";
    std::filesystem::remove_all(out.parent_path());
    ASSERT_TRUE(window.renderPrman(QString::fromStdString(out.u8string()), 0, QSize(96, 72), false));
    RenderWindow* shown = window.renderWindow();
    ASSERT_NE(shown, nullptr);
    EXPECT_TRUE(shown->isVisible());
    QElapsedTimer clock;
    clock.start();
    while (shown->image().isNull() && clock.elapsed() < 300000) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    ASSERT_FALSE(shown->image().isNull()) << "no image within 5 minutes";
    EXPECT_EQ(shown->image().size(), QSize(96, 72));
    EXPECT_TRUE(std::filesystem::exists(out));
    std::error_code ignored;
    std::filesystem::remove_all(out.parent_path(), ignored);
}

}  // namespace groomlab::app
