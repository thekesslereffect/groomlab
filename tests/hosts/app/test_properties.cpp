// SPDX-License-Identifier: Apache-2.0
// The Properties dock's Groom workspace from properties.json (Maya's Attribute Editor's sections,
// order, labels and tips: properties_panel.h), what its controls edit, the Groom Tools' Photos tab
// (photos_panel.h: Look Through, Lock, Opacity, In front / Behind, Align Photo...) and what the
// groom file keeps of the guide groups' display (their colors, the hidden ones). The offscreen
// platform; the prompts answered by their labels.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include <QCheckBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "app_actions.h"
#include "glass.h"
#include "groom_document.h"
#include "list_bar.h"
#include "main_window.h"
#include "overlay.h"
#include "photos_panel.h"
#include "reference_window.h"
#include "prman_render.h"
#include "properties_panel.h"
#include "render_job.h"
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

json readJson(const std::string& path) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

// A slider's value set as typed (pressed, valueChanged, released: one edit).
void type(ValueSlider* s, double value) {
    ASSERT_TRUE(s);
    s->beginEdit();
    s->setEditText(QString::number(value));
    ASSERT_TRUE(s->commitEdit());
}

void runFor(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

// Items properties.json shows for the app that it does not do yet: hidden in the Properties dock
// (the parity checklist with Maya's Attribute Editor; give one a control and take it off here).
const std::set<std::string> ALLOWED_MISSING_PROPERTIES = {
    "displayOverlaysOnTop",    // the lookdev path draws the overlays over everything, the line path depth-tests them
    "displaySelfShadowing",    // the lookdev's own shadows and the look card's lighting stand for these
    "displayStrandVariation",
    "displayShadows",
    "useMaterialColor",        // the look card's material colors the strands
    "displayColor",
    "displayRibbons",          // Lookdev draws ribbons; no ribbons on the line path
    "softSelectRadius",        // no soft selection in the app yet
    "displayCardTextures",     // Cards mode shows the bake when there is one
    "groomButtons",            // Maya's two window buttons: the app's docks are the windows (View > Panels)
};

TEST(AppProperties, TheDockFollowsTheDefinitionsInMayasOrder) {
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(sampleCopy("groomlab_app_properties_order"))));
    auto* panel = window.findChild<PropertiesPanel*>();
    ASSERT_TRUE(panel);

    // Every item the definitions show for the app has its control (labelled as the definitions say),
    // or is on the checklist above; the checklist holds nothing the app does.
    std::vector<std::string> unbound, stale;
    for (const PropertiesPanel::Item& item : PropertiesPanel::appItems()) {
        const bool allowed = ALLOWED_MISSING_PROPERTIES.count(item.id) > 0;
        QWidget* control = panel->control(item.key);
        if (!control && !allowed) unbound.push_back(item.id + " (" + item.key + ")");
        if (control && allowed) stale.push_back(item.id);
        if (control && !item.custom) {
            ASSERT_TRUE(panel->label(item.key)) << item.id;
            EXPECT_EQ(panel->label(item.key)->text().toStdString(), item.label) << item.id;
            EXPECT_EQ(control->toolTip().toStdString(), item.tip) << item.id;
            if (auto* field = qobject_cast<ValueSlider*>(control)) {
                // A number field: the label inside it, the row's own label hidden.
                EXPECT_EQ(field->caption().toStdString(), item.label) << item.id;
                EXPECT_TRUE(panel->label(item.key)->isHidden()) << item.id;
            }
        }
        if (!control) EXPECT_FALSE(panel->shown(item.key)) << item.id << ": hidden";
    }
    EXPECT_TRUE(unbound.empty()) << "items with no control in the app (bind them, or list them in ALLOWED_MISSING_PROPERTIES): "
                                 << json(unbound).dump();
    EXPECT_TRUE(stale.empty()) << "ALLOWED_MISSING_PROPERTIES holds items the app shows: " << json(stale).dump();

    // The sections in Maya's order (nested ones after their parent), each with something shown.
    std::vector<std::string> ids;
    for (PropertySection* s : panel->sections()) {
        ids.push_back(s->objectName().toStdString());
        EXPECT_FALSE(s->isHidden()) << s->objectName().toStdString();
    }
    EXPECT_EQ(ids, (std::vector<std::string>{"propSection_groom", "propSection_inputs", "propSection_controls", "propSection_hairColor",
                                             "propSection_display", "propSection_guidesDisplay", "propSection_strandShading",
                                             "propSection_sculpting", "propSection_gameCards", "propSection_rendering",
                                             "propSection_renderOverrides", "propSection_lod"}));
    // Open as Maya's AE opens them (Groom, Inputs, Hair Color, Display), the rest folded.
    EXPECT_TRUE(panel->section("groom")->expanded());
    EXPECT_TRUE(panel->section("display")->expanded());
    EXPECT_FALSE(panel->section("guidesDisplay")->expanded());
    EXPECT_FALSE(panel->section("lod")->expanded());
    // Source and the range apply to the color modes that read them.
    EXPECT_FALSE(panel->shown("colorSource"));
    EXPECT_FALSE(panel->shown("colorMin"));
    ASSERT_TRUE(window.setDisplayText(QStringLiteral("colorMode=attribute")));
    EXPECT_TRUE(panel->shown("colorSource"));
    EXPECT_TRUE(panel->shown("colorMax"));

    // The inputs as the groom has them.
    auto* scalp = panel->findChild<QLabel*>(QStringLiteral("inputs_scalp"));
    ASSERT_TRUE(scalp);
    EXPECT_TRUE(scalp->text().contains(QStringLiteral("vertices"))) << scalp->text().toStdString();
    auto* guides = panel->findChild<QLabel*>(QStringLiteral("inputs_guides"));
    ASSERT_TRUE(guides);
    EXPECT_TRUE(guides->text().contains(QStringLiteral("guides in"))) << guides->text().toStdString();
    EXPECT_EQ(panel->findChild<QLabel*>(QStringLiteral("inputs_file"))->text(), QStringLiteral("sample.groom"));
}

TEST(AppProperties, InputsRowsHaveAnIconTheValueAndOneGhostAction) {
    // The design's Inputs: a muted icon, the name, what it is, and its action as a ghost icon button
    // with a tip (the File row's two: Reload and Save); no Groom Tools / Graph Editor buttons.
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(sampleCopy("groomlab_app_properties_inputs"))));
    auto* panel = window.findChild<PropertiesPanel*>();
    ASSERT_TRUE(panel);
    EXPECT_EQ(panel->findChild<QWidget*>(QStringLiteral("propGroomTools")), nullptr);
    EXPECT_EQ(panel->findChild<QWidget*>(QStringLiteral("propGraphEditor")), nullptr);
    EXPECT_FALSE(panel->shown("groomButtons"));
    const std::vector<std::pair<const char*, std::vector<const char*>>> rows = {
        {"scalp", {"transfer"}}, {"rest", {}}, {"scalps", {}}, {"guides", {"edit"}}, {"colliders", {"add"}}, {"file", {"reload", "save"}}};
    for (const auto& [part, actions] : rows) {
        SCOPED_TRACE(part);
        auto* icon = panel->findChild<QLabel*>(QStringLiteral("inputsIcon_") + QLatin1String(part));
        ASSERT_TRUE(icon);
        EXPECT_FALSE(icon->pixmap().isNull());
        ASSERT_TRUE(panel->findChild<QLabel*>(QStringLiteral("inputs_") + QLatin1String(part)));
        auto* holder = panel->findChild<QWidget*>(QStringLiteral("inputsActions_") + QLatin1String(part));
        ASSERT_TRUE(holder);
        const QList<QToolButton*> buttons = holder->findChildren<QToolButton*>();
        ASSERT_EQ(static_cast<std::size_t>(buttons.size()), actions.size());
        for (std::size_t i = 0; i < actions.size(); ++i) {
            auto* b = panel->findChild<QToolButton*>(QStringLiteral("inputs_%1_%2").arg(QLatin1String(part), QLatin1String(actions[i])));
            ASSERT_TRUE(b) << actions[i];
            EXPECT_EQ(b->property("role").toString(), QStringLiteral("glyph")) << "a ghost icon button";
            EXPECT_TRUE(b->text().isEmpty());
            EXPECT_FALSE(b->icon().isNull());
            EXPECT_FALSE(b->toolTip().isEmpty());
            EXPECT_TRUE(b->isEnabled());
        }
    }
    // Edit: the guides' panel in the Groom Tools.
    panel->findChild<QToolButton*>(QStringLiteral("inputs_guides_edit"))->click();
    QCoreApplication::processEvents();
    // Save: the groom written (no unsaved edits after).
    panel->findChild<QToolButton*>(QStringLiteral("inputs_file_save"))->click();
    EXPECT_FALSE(window.document()->dirty());
}

TEST(AppProperties, ControlsHairColorLockedAndRenderOverrides) {
    const std::string path = sampleCopy("groomlab_app_properties_edits");
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(path)));
    GroomDocument* doc = window.document();
    auto* panel = window.findChild<PropertiesPanel*>();
    ASSERT_TRUE(panel);

    // Controls: none yet, then a Control node's slider (Drive with Slider), its value saved with the groom.
    EXPECT_TRUE(doc->controlSliders().empty());
    std::string node, param;
    for (const json& n : doc->graph().at("nodes")) {
        if (!node.empty() || n.value("type", std::string()) == "Output") continue;
        const json params = n.value("params", json::object());
        for (const auto& [name, value] : params.items()) {
            if (value.is_number() && name != "seed") {
                node = n.value("name", std::string());
                param = name;
                break;
            }
        }
    }
    ASSERT_FALSE(node.empty()) << "a node with a number to drive";
    std::string made;
    ASSERT_EQ(doc->addControl(node, param, &made), "");
    ASSERT_TRUE(window.saveTo(QString::fromStdString(path)));
    ASSERT_TRUE(window.reloadGroom());
    doc = window.document();
    ASSERT_EQ(doc->controlSliders().size(), 1u);
    const std::string control = doc->controlSliders()[0].name;
    auto* slider = panel->findChild<ValueSlider*>(QStringLiteral("control_") + QString::fromStdString(control));
    ASSERT_TRUE(slider) << "the Controls section's slider";
    type(slider, 0.25);
    EXPECT_NEAR(doc->controlSliders()[0].value, 0.25, 1e-6);
    EXPECT_NEAR(doc->session().data().controls.at(control), 0.25f, 1e-6);
    ASSERT_TRUE(window.saveTo(QString::fromStdString(path)));
    EXPECT_NEAR(readJson(path).at("controls").at(control).get<double>(), 0.25, 1e-6);

    // Hair Color: the HairColor node's rows; a slider's edit is one undoable edit of the node.
    std::string hair;
    for (const GroomDocument::NodeInfo& n : doc->nodes()) {
        if (n.type == "HairColor") hair = n.name;
    }
    if (!hair.empty()) {
        auto* melanin = panel->findChild<ValueSlider*>(QStringLiteral("hairColor_melanin"));
        if (doc->paramOrDefault(hair, "mode") != "color") {
            ASSERT_TRUE(melanin) << "the Hair Color rows";
            type(melanin, 0.8);
            EXPECT_NEAR(doc->paramOrDefault(hair, "melanin").get<double>(), 0.8, 1e-6);
            EXPECT_EQ(doc->undoText().rfind("Undo", 0), 0u);
        }
    } else {
        EXPECT_TRUE(panel->findChild<QPushButton*>() != nullptr);
    }

    // Locked: brushes refused (the camera stays), Unlocked: they work again.
    auto* locked = qobject_cast<QCheckBox*>(panel->control("locked"));
    ASSERT_TRUE(locked);
    locked->setChecked(true);
    auto* viewport = window.findChild<Viewport*>();
    ASSERT_TRUE(viewport);
    window.setToolMode(QStringLiteral("brush"));
    EXPECT_EQ(viewport->toolMode(), Viewport::ToolMode::Camera) << "a locked groom ignores brushes";
    locked->setChecked(false);
    window.setToolMode(QStringLiteral("brush"));
    EXPECT_EQ(viewport->toolMode(), Viewport::ToolMode::Brush);
    window.setToolMode(QStringLiteral("camera"));

    // Render overrides: the document's exports and renders take them; RenderMan's procedural gets them.
    type(qobject_cast<ValueSlider*>(panel->control("renderDensity")), 0.5);
    type(qobject_cast<ValueSlider*>(panel->control("renderWidthScale")), 2.0);
    EXPECT_FLOAT_EQ(doc->renderOverrides().density, 0.5f);
    EXPECT_FLOAT_EQ(doc->renderOverrides().widthScale, 2.0f);
    PrmanScene scene;
    scene.groomPath = "groom.groom";
    scene.density = 0.5f;
    scene.widthScale = 2.0f;
    RenderStage stage;
    const std::string rib = (std::filesystem::u8path(path).parent_path() / "scene.rib").u8string();
    writePrmanScene(rib, scene, stage);
    std::ifstream in(std::filesystem::u8path(rib));
    std::stringstream text;
    text << in.rdbuf();
    EXPECT_NE(text.str().find("\"float density\" [0.5]"), std::string::npos) << text.str();
    EXPECT_NE(text.str().find("\"float widthScale\" [2]"), std::string::npos);
    EXPECT_EQ(text.str().find("lengthScale"), std::string::npos) << "1 is left out";
}

TEST(AppProperties, GroupColorsAndHiddenGroupsSaveWithTheGroom) {
    // Maya keeps them on the groom (guideGroupColors "name:#rrggbb ...", hiddenGuideGroups "name
    // ..."): the groom file holds the same text.
    const std::string path = sampleCopy("groomlab_app_group_display");
    std::string root;
    {
        MainWindow window;
        ASSERT_TRUE(window.openGroom(QString::fromStdString(path)));
        GroomDocument* doc = window.document();
        const HierarchyPtr h = doc->session().authoredHierarchy();
        ASSERT_TRUE(h && !h->groups().empty());
        root = h->groups().front().name;
        ASSERT_EQ(doc->setGroupColor(root, "#FF8800"), "");
        doc->setGroupHidden(root, true);
        EXPECT_TRUE(doc->dirty());
        ASSERT_TRUE(window.saveTo(QString::fromStdString(path)));
    }
    const json file = readJson(path);
    EXPECT_EQ(file.value("guideGroupColors", std::string()), root + ":#ff8800");
    EXPECT_EQ(file.value("hiddenGuideGroups", std::string()), root);
    MainWindow window;
    ASSERT_TRUE(window.openGroom(QString::fromStdString(path)));
    EXPECT_EQ(window.document()->groupColors().at(root), "#ff8800");
    EXPECT_EQ(window.document()->hiddenGroups(), (std::set<std::string>{root}));
}

TEST(AppPhotos, LookThroughLockOpacityDepthAndAlignPhoto) {
    const std::string path = sampleCopy("groomlab_app_photos");
    const std::filesystem::path dir = std::filesystem::u8path(path).parent_path();
    MainWindow window;
    window.resize(1280, 800);
    window.show();
    ASSERT_TRUE(window.openGroom(QString::fromStdString(path)));
    GroomDocument* doc = window.document();
    auto* photos = window.findChild<PhotosPanel*>();
    auto* viewport = window.findChild<Viewport*>();
    ASSERT_TRUE(photos && viewport);
    const ui::Bindings bindings = AppActions::bindings(window);
    EXPECT_EQ(photos->bar()->ids(), (std::vector<std::string>{"photo.import", "photo.rename", "photo.remove"}));

    // Import: the photo as a camera in front of the groom, 50 mm on 36, shown at 60%, in front.
    const int W = 400, H = 300;
    QImage image(W, H, QImage::Format_RGB32);
    image.fill(QColor(200, 120, 60));
    const std::string file = (dir / "front.png").u8string();
    ASSERT_TRUE(image.save(QString::fromStdString(file)));
    ASSERT_EQ(photos->importPhoto(QString::fromStdString(file)), 0);
    EXPECT_EQ(photos->count(), 1);
    EXPECT_EQ(photos->name(0), QStringLiteral("front"));
    const std::optional<RefCamera> placed = photos->camera(0);
    ASSERT_TRUE(placed);
    EXPECT_NEAR(placed->focalPx, W * 50.0 / 36.0, 1e-6);
    PhotosPanel::Display d = photos->display(0);
    EXPECT_NEAR(d.opacity, 0.6, 1e-9);
    EXPECT_TRUE(d.front && d.visible && !d.locked);

    // Look Through: the viewport's camera is the photo's (its field of view the photo's, fitted),
    // the photo over the frame; again: back to the camera before.
    OrbitCamera& camera = viewport->camera();
    const QVector3D before = camera.eye();
    bindings.at("photo.lookThrough").trigger(nullptr, false);
    ASSERT_TRUE(camera.lookingThrough());
    EXPECT_TRUE(photos->lookButton()->isChecked());
    EXPECT_EQ(photos->lookButton()->text(), QStringLiteral("Back to Perspective"));
    EXPECT_NEAR(camera.eye().z(), placed->pose.position[2], 1e-3);
    camera.projection(4.0f / 3.0f);
    EXPECT_NEAR(camera.fovDegrees(), 2.0 * std::atan(0.5 * H / placed->focalPx) * 180.0 / 3.14159265358979, 1e-3);
    camera.projection(2.0f);  // a wider frame: the photo as tall as it
    EXPECT_NEAR(camera.fovDegrees(), 2.0 * std::atan(0.5 * H / placed->focalPx) * 180.0 / 3.14159265358979, 1e-3);
    camera.projection(1.0f);  // a narrower one: as wide as it
    EXPECT_NEAR(camera.fovDegrees(), 2.0 * std::atan(0.5 * W / placed->focalPx) * 180.0 / 3.14159265358979, 1e-3);
    // The focal length by hand (85 mm on 36): the photo's camera takes it, Keep focal length turns on
    // (a solve keeps it), and the view through it follows.
    EXPECT_NEAR(photos->focalSlider()->value(), 50.0, 1e-6);
    EXPECT_FALSE(photos->keepFocal());
    emit photos->focalSlider()->valueChanged(85.0);
    ASSERT_TRUE(photos->camera(0));
    EXPECT_NEAR(photos->camera(0)->focalPx, 85.0 * W / 36.0, 1e-6);
    EXPECT_TRUE(photos->keepFocal());
    camera.projection(1.0f);
    EXPECT_NEAR(camera.fovDegrees(), 2.0 * std::atan(0.5 * W / (85.0 * W / 36.0)) * 180.0 / 3.14159265358979, 1e-3);
    photos->focalSlider()->setValue(3000.0);  // (as a value typed past the range would)
    emit photos->focalSlider()->valueChanged(3000.0);
    EXPECT_GE(photos->focalSlider()->maximum(), 3000.0);
    photos->resetFocal();  // (back to the import's 50 mm and the starting range, for what follows)
    EXPECT_NEAR(photos->camera(0)->focalPx, 50.0 * W / 36.0, 1e-6);
    EXPECT_EQ(photos->focalSlider()->maximum(), 200.0);
    EXPECT_TRUE(photos->display(0).keepFocal);
    d.keepFocal = false;
    GroomRenderer& renderer = viewport->renderer();
    ASSERT_TRUE(renderer.photoShown());
    EXPECT_NEAR(renderer.photo().opacity, 0.6f, 1e-6);
    EXPECT_TRUE(renderer.photo().front);
    EXPECT_EQ(GroomRenderer::photoRect(QSize(800, 300), 4.0 / 3.0), QRectF(200.0, 0.0, 400.0, 300.0));
    EXPECT_EQ(GroomRenderer::photoRect(QSize(400, 600), 4.0 / 3.0), QRectF(0.0, 150.0, 400.0, 300.0));
    // The scene offscreen through the photo: the photo's color over the frame's middle.
    const QImage frame = window.renderOffscreen(QSize(320, 240), 1);
    if (!frame.isNull()) {
        const QColor middle = frame.pixelColor(4, 120);  // (the groom draws at the center: the edge is the photo)
        EXPECT_GT(middle.red(), middle.blue()) << "the photo's orange at 60% over the backdrop";
    }

    // Opacity, Behind, the eye: the renderer follows; saved with the groom.
    d.opacity = 0.3;
    d.front = false;
    photos->setDisplay(0, d);
    EXPECT_NEAR(renderer.photo().opacity, 0.3f, 1e-6);
    EXPECT_FALSE(renderer.photo().front);
    d.visible = false;
    photos->setDisplay(0, d);
    EXPECT_FALSE(renderer.photoShown()) << "hidden: the camera stays";
    EXPECT_TRUE(camera.lookingThrough());
    d.visible = true;
    photos->setDisplay(0, d);

    // Unlocked: tumbling through it lines the camera up by eye (stored once it stops); Lock keeps it.
    camera.orbit(8.0f, 0.0f);
    emit viewport->cameraChanged();
    runFor(600);
    const std::optional<RefCamera> moved = photos->camera(0);
    ASSERT_TRUE(moved);
    EXPECT_GT(std::abs(moved->pose.position[0] - placed->pose.position[0]), 1e-4) << "the orbit was stored";
    d.locked = true;
    photos->setDisplay(0, d);
    EXPECT_TRUE(camera.lookThroughLocked());
    EXPECT_TRUE(photos->lockButton()->isChecked());
    const QVector3D lockedEye = camera.eye();
    camera.orbit(20.0f, 10.0f);
    camera.pan(30.0f, 10.0f, 600);
    camera.dolly(0.5f);
    EXPECT_LT((camera.eye() - lockedEye).length(), 1e-5f) << "a locked camera does not move";
    bindings.at("photo.lookThrough").trigger(nullptr, false);  // (again: still through it)
    // Looking through one, picking another looks through it; Back to Perspective still goes back.
    ASSERT_EQ(photos->importPhoto(QString::fromStdString(file)), 1);
    ASSERT_TRUE(camera.lookingThrough());
    EXPECT_EQ(photos->lookingThrough(), photos->name(1));
    EXPECT_NEAR(camera.eye().z(), photos->camera(1)->pose.position[2], 1e-3);
    photos->select(0);
    EXPECT_EQ(photos->lookingThrough(), photos->name(0));
    EXPECT_TRUE(camera.lookThroughLocked());  // photo 0's lock
    photos->lookButton()->click();  // Back to Perspective
    EXPECT_FALSE(camera.lookingThrough());
    photos->select(1);  // not looking through: picking only picks
    EXPECT_FALSE(camera.lookingThrough());
    photos->remove(1);
    photos->select(0);
    EXPECT_FALSE(camera.lookingThrough());
    EXPECT_LT((camera.eye() - before).length(), 1e-4f) << "the camera before it";
    EXPECT_FALSE(renderer.photoShown());

    // Where the alignment stands: pairs whose mesh points the true camera puts on their photo points
    // (made in the Photos window); Align Photo... opens it on the photo, its Align stage.
    RefCamera truth = *placed;
    truth.width = W;
    truth.height = H;
    const MeshPtr scalp = doc->file().data.scalp;
    ASSERT_TRUE(scalp);
    EXPECT_TRUE(photos->alignText().startsWith(QStringLiteral("Not lined up yet"))) << photos->alignText().toStdString();
    json landmarks = json::array();
    const std::size_t n = scalp->pointCount();
    for (std::size_t k = 0; k < 8; ++k) {
        const Vec3f p = scalp->points()[(k * 7919) % n];
        float u = 0.0f, v = 0.0f;
        ASSERT_TRUE(truth.project(p, u, v));
        landmarks.push_back({{"image", {u, v}}, {"mesh", {p.x, p.y, p.z}}});
    }
    json all = json::parse(doc->referencePhotos());
    all[0]["state"]["landmarks"] = json::array({landmarks[0], landmarks[1], landmarks[2]});
    doc->setReferencePhotos(all.dump());
    photos->refresh();
    photos->select(0);
    EXPECT_TRUE(photos->alignText().startsWith(QStringLiteral("3 of 6 pairs"))) << photos->alignText().toStdString();
    all[0]["state"]["landmarks"] = landmarks;
    doc->setReferencePhotos(all.dump());
    photos->refresh();
    photos->select(0);
    EXPECT_TRUE(photos->alignText().startsWith(QStringLiteral("Lined up by 8 pairs"))) << photos->alignText().toStdString();
    photos->alignButton()->click();
    ReferenceWindow* align = window.openReferenceWindow();
    ASSERT_TRUE(align && align->isVisible());
    EXPECT_EQ(align->stage(), ReferenceWindow::Stage::Align);
    EXPECT_EQ(align->currentIndex(), 0);
    EXPECT_EQ(align->markerLists().pairCount(), 8u);
    EXPECT_FALSE(align->guidesPanel()->isVisibleTo(align));
    EXPECT_FALSE(align->modeButton(ReferenceWindow::Mode::Flow)->isVisibleTo(align));
    EXPECT_TRUE(align->locked());  // the Photos tab's Lock (above)
    bindings.at("photos.guides").trigger(nullptr, false);
    EXPECT_EQ(align->stage(), ReferenceWindow::Stage::Guides);
    EXPECT_TRUE(align->guidesPanel()->isVisibleTo(align));
    align->close();

    // Rename over the viewport's prompt; saved with the groom (display and all); Remove.
    bindings.at("photo.rename").trigger(nullptr, false);
    ASSERT_TRUE(window.overlay()->promptShown());
    window.overlay()->setPromptLine(QStringLiteral("frontView"));
    ASSERT_TRUE(window.overlay()->choosePrompt(QStringLiteral("Rename")));
    EXPECT_EQ(photos->name(0), QStringLiteral("frontView"));
    ASSERT_TRUE(window.saveTo(QString::fromStdString(path)));
    const json saved = readJson(path).at("referencePhotos");
    ASSERT_EQ(saved.size(), 1u);
    EXPECT_EQ(saved[0].value("name", std::string()), "frontView");
    EXPECT_EQ(saved[0]["display"].value("depth", std::string()), "behind");
    EXPECT_TRUE(saved[0]["display"].value("locked", false));
    EXPECT_NEAR(saved[0]["display"].value("opacity", 0.0), 0.3, 1e-9);
    bindings.at("photo.remove").trigger(nullptr, false);
    EXPECT_EQ(photos->count(), 0);
}

}  // namespace
}  // namespace groomlab::app
