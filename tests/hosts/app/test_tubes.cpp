// SPDX-License-Identifier: Apache-2.0
// Groom Tools' Tubes (tube_panel.h, tube_shape_editor.h) and the document's tube edits
// (groom_document.h "Tubes") driven without a window: each edit one undo step, the outline
// editor's mirror and its mouse edits, the panel's shape picker, library and Fill Tubes, the Shape
// brush's Paints chips. Maya's palette.py TubePanel and tube_shape_editor.py are the reference.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QResizeEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "brush_panel.h"
#include "brush_session.h"
#include "glass.h"
#include "groom_document.h"
#include "card_parts.h"
#include "groom_tools.h"
#include "guides_panel.h"
#include "list_bar.h"
#include "maps_panel.h"
#include "groomlab/brush.h"
#include "groomlab/brush_catalog.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/tube.h"
#include "param_panel.h"
#include "tube_panel.h"
#include "tube_shape_editor.h"

namespace groomlab::app {
namespace {

using nlohmann::json;

const std::string kSampleGroom = std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom";

void mouse(QWidget* w, QEvent::Type type, const QPointF& at, Qt::MouseButton button = Qt::LeftButton) {
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent event(type, at, w->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : button, held, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &event);
}

std::vector<GuideId> primaryGuides(GroomDocument& doc, std::size_t count) {
    std::vector<GuideId> out;
    for (const HierarchyGuide& g : doc.session().authoredHierarchy()->guides()) {
        if (!g.isChild() && out.size() < count) out.push_back(g.id);
    }
    return out;
}

const HierarchyGuide& guideOf(GroomDocument& doc, GuideId id) { return *doc.session().authoredHierarchy()->guide(id); }

bool symmetricAboutV(const std::vector<SectionPoint>& points) {
    for (const int mate : tube_editor::partners(points)) {
        if (mate < 0) return false;
    }
    return true;
}

}  // namespace

TEST(AppTubes, DocumentEditsTubesAndTheLibraryEachOneUndoStep) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    // No selection, no solo: no targets (the sample has more than one guide).
    EXPECT_EQ(doc->tubeTargets().source, "none");
    const std::vector<GuideId> ids = primaryGuides(*doc, 3);
    ASSERT_EQ(ids.size(), 3u);
    doc->setSelection(ids);
    GroomDocument::TubeTargets targets = doc->tubeTargets();
    EXPECT_EQ(targets.source, "selection");
    EXPECT_EQ(targets.guides, ids);
    EXPECT_EQ(targets.shapes, std::vector<std::string>{"round"});
    ASSERT_TRUE(targets.tube.has_value());
    // The built-ins first, then the library (empty).
    const std::vector<GroomDocument::TubeShapeInfo> shapes = doc->tubeShapes();
    ASSERT_EQ(shapes.size(), tubeShapeNames().size());
    EXPECT_TRUE(shapes.front().builtin);
    EXPECT_GE(shapes.front().points.size(), 3u);

    // A shape, an aspect, a twist: one step each.
    EXPECT_EQ(doc->editTubes(ids, json{{"shape", "ribbon"}}), "");
    EXPECT_EQ(doc->undoText(), "Undo set tube");
    for (const GuideId id : ids) EXPECT_EQ(guideOf(*doc, id).tube.shapeName(), "ribbon");
    EXPECT_EQ(doc->editTubes(ids, json{{"aspect", 0.4}}), "");
    EXPECT_NEAR(guideOf(*doc, ids[0]).tube.aspectAt(0.5f), 0.4f, 1e-5f);
    // Scaled: the widest section doubles, the taper kept.
    const std::vector<float> before = guideOf(*doc, ids[0]).tube.width;
    EXPECT_EQ(doc->scaleTubes(ids, 2.0f), "");
    EXPECT_EQ(doc->undoText(), "Undo scale tube");
    const std::vector<float> after = guideOf(*doc, ids[0]).tube.width;
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < before.size(); ++i) EXPECT_NEAR(after[i], before[i] * 2.0f, 1e-5f);
    EXPECT_EQ(doc->smoothTubes(ids, "flatness"), "");
    EXPECT_EQ(doc->undoText(), "Undo smooth tube");
    EXPECT_NE(doc->smoothTubes(ids, "colour"), "") << "not a channel";
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(guideOf(*doc, ids[0]).tube.width, before) << "undone";
    EXPECT_NE(doc->editTubes({}, json{{"shape", "blade"}}), "") << "no targets";

    // The library: Edit as Custom (a built-in's copy, the targets taking it, one step).
    bool smooth = true;
    const std::vector<SectionPoint> teardrop = builtinTubeShapePoints(TubeShape::Teardrop, &smooth);
    EXPECT_EQ(doc->addTubeShape("lock 1", teardrop, smooth, {ids[0], ids[1]}), "");
    EXPECT_EQ(doc->undoText(), "Undo tube shape");
    EXPECT_EQ(guideOf(*doc, ids[0]).tube.shapeName(), "lock 1");
    EXPECT_EQ(guideOf(*doc, ids[2]).tube.shapeName(), "ribbon");
    targets = doc->tubeTargets();
    EXPECT_EQ(targets.shapes, (std::vector<std::string>{"lock 1", "ribbon"})) << "mixed";
    EXPECT_NE(doc->addTubeShape("lock 1", teardrop, true), "") << "the name is taken";
    EXPECT_NE(doc->addTubeShape("round", teardrop, true), "") << "a built-in's name";
    const std::vector<GroomDocument::TubeShapeInfo> library = doc->tubeShapes();
    ASSERT_EQ(library.size(), tubeShapeNames().size() + 1);
    EXPECT_FALSE(library.back().builtin);
    EXPECT_EQ(library.back().users, 2u);
    // New points: every guide taking it follows; a shape crossing itself is refused.
    const std::vector<SectionPoint> diamond = {{0.0f, 1.0f}, {-1.0f, 0.0f}, {0.0f, -1.0f}, {1.0f, 0.0f}};
    EXPECT_EQ(doc->setTubeShape("lock 1", &diamond, false), "");
    EXPECT_EQ(doc->undoText(), "Undo edit tube shape");
    EXPECT_FALSE(guideOf(*doc, ids[0]).tube.custom->smooth);
    const std::vector<SectionPoint> bow = {{-1.0f, -1.0f}, {1.0f, 1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}};
    EXPECT_NE(doc->setTubeShape("lock 1", &bow, std::nullopt), "") << "crosses itself";
    EXPECT_EQ(doc->setTubeShape("lock 1", nullptr, true), "") << "smooth alone";
    EXPECT_TRUE(guideOf(*doc, ids[0]).tube.custom->smooth);
    // The preview: shown, not stored, no step; any edit ends it.
    const std::string undoBefore = doc->undoText();
    EXPECT_EQ(doc->previewTubeShape("lock 1", teardrop, true), "");
    EXPECT_TRUE(doc->tubePreviewing());
    EXPECT_EQ(doc->undoText(), undoBefore);
    doc->endTubePreview();
    EXPECT_FALSE(doc->tubePreviewing());
    // Renamed (its guides keep it), removed (its guides go round).
    EXPECT_EQ(doc->renameTubeShape("lock 1", "lock A"), "");
    EXPECT_EQ(guideOf(*doc, ids[1]).tube.shapeName(), "lock A");
    EXPECT_NE(doc->renameTubeShape("nope", "x"), "");
    std::size_t users = 0;
    EXPECT_EQ(doc->removeTubeShape("lock A", &users), "");
    EXPECT_EQ(users, 2u);
    EXPECT_EQ(guideOf(*doc, ids[0]).tube.shapeName(), "round");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(guideOf(*doc, ids[0]).tube.shapeName(), "lock A") << "the removal undone";

    // Soloed guides are the targets without a selection.
    doc->setSelection({});
    EXPECT_EQ(doc->editGuides("solo", [&](GuideHierarchy& h) { soloGuides(h, {ids[2]}); }), "");
    targets = doc->tubeTargets();
    EXPECT_EQ(targets.source, "solo");
    EXPECT_EQ(targets.guides, std::vector<GuideId>{ids[2]});

    // Fill Tubes and Tube Blend: every Interpolate node, one step each.
    const std::optional<GroomDocument::FillTubes> fill = doc->fillTubes();
    ASSERT_TRUE(fill.has_value()) << "the default graph has an Interpolate node";
    EXPECT_EQ(doc->setFillTubes(!fill->fill, std::nullopt), "");
    EXPECT_EQ(doc->undoText(), "Undo fill tubes");
    EXPECT_EQ(doc->fillTubes()->fill, !fill->fill);
    EXPECT_EQ(doc->setFillTubes(std::nullopt, 0.6f), "");
    EXPECT_NEAR(doc->fillTubes()->blend, 0.6f, 1e-6f);
    EXPECT_EQ(doc->setFillTubes(std::nullopt, 7.0f), "");
    EXPECT_NEAR(doc->fillTubes()->blend, 1.0f, 1e-6f) << "clamped";
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->undo(), "");
    EXPECT_EQ(doc->fillTubes()->fill, fill->fill);
}

TEST(AppTubes, OutlineEditorMirrorsAddsRemovesAndDrags) {
    // Mirror: a lopsided outline made symmetric from its right side.
    const std::vector<SectionPoint> lopsided = {{0.0f, 1.0f}, {-0.6f, 0.2f}, {0.0f, -1.0f}, {0.9f, 0.1f}};
    const std::vector<SectionPoint> made = tube_editor::symmetric(lopsided);
    EXPECT_TRUE(symmetricAboutV(made));
    EXPECT_EQ(made.size(), 4u);
    EXPECT_TRUE(std::any_of(made.begin(), made.end(), [](const SectionPoint& p) { return std::fabs(p.u + 0.9f) < 1e-6f; }))
        << "the right point's mirror image";
    const std::vector<int> mates = tube_editor::partners(made);
    for (std::size_t i = 0; i < made.size(); ++i) {
        if (std::fabs(made[i].u) <= tube_editor::kAxis) EXPECT_EQ(mates[i], static_cast<int>(i)) << "on the axis: its own";
    }

    TubeShapeEditor editor;
    editor.resize(220, 190);
    bool smooth = true;
    const std::vector<SectionPoint> round = builtinTubeShapePoints(TubeShape::Round, &smooth);
    // A built-in: read-only, its note said; a press does nothing.
    editor.setShape(round, smooth, false, QStringLiteral("Built in"));
    int commits = 0, drags = 0;
    QObject::connect(&editor, &TubeShapeEditor::committed, [&](const std::vector<SectionPoint>&) { ++commits; });
    QObject::connect(&editor, &TubeShapeEditor::dragged, [&](const std::vector<SectionPoint>&) { ++drags; });
    const QPointF first = editor.toScreen(round[0].u, round[0].v);
    mouse(&editor, QEvent::MouseButtonPress, first);
    mouse(&editor, QEvent::MouseButtonRelease, first);
    EXPECT_EQ(commits, 0);
    EXPECT_FALSE(editor.busy());
    // A library shape: lopsided, mirror on makes it symmetric.
    editor.setShape(lopsided, true, true);
    EXPECT_TRUE(editor.problem().isEmpty()) << editor.problem().toStdString();
    EXPECT_TRUE(editor.setMirror(true));
    EXPECT_TRUE(symmetricAboutV(editor.points()));
    EXPECT_FALSE(editor.setMirror(true)) << "already symmetric";
    // Screen and section coordinates round-trip.
    const SectionPoint back = editor.fromScreen(editor.toScreen(0.5f, -0.25f));
    EXPECT_NEAR(back.u, 0.5f, 1e-4f);
    EXPECT_NEAR(back.v, -0.25f, 1e-4f);
    // Dragging the right point moves its mirror image with it; the release commits once.
    int right = -1;
    for (std::size_t i = 0; i < editor.points().size(); ++i) {
        if (editor.points()[i].u > 0.5f) right = static_cast<int>(i);
    }
    ASSERT_GE(right, 0);
    const QPointF from = editor.toScreen(editor.points()[static_cast<std::size_t>(right)].u, editor.points()[static_cast<std::size_t>(right)].v);
    const QPointF to = editor.toScreen(0.7f, 0.4f);
    mouse(&editor, QEvent::MouseButtonPress, from);
    EXPECT_TRUE(editor.busy());
    mouse(&editor, QEvent::MouseMove, (from + to) / 2.0);
    mouse(&editor, QEvent::MouseMove, to);
    mouse(&editor, QEvent::MouseButtonRelease, to);
    EXPECT_EQ(drags, 2);
    EXPECT_EQ(commits, 1);
    EXPECT_NEAR(editor.points()[static_cast<std::size_t>(right)].u, 0.7f, 0.02f);
    EXPECT_NEAR(editor.points()[static_cast<std::size_t>(right)].v, 0.4f, 0.02f);
    EXPECT_TRUE(symmetricAboutV(editor.points())) << "the mirror image followed";
    // A click on the outline between points adds one (its mirror image too, or one on the axis).
    const std::size_t count = editor.points().size();
    editor.addPoint(0, {0.4f, 0.8f});
    EXPECT_EQ(editor.points().size(), count + 2) << "a point and its mirror image";
    EXPECT_TRUE(symmetricAboutV(editor.points()));
    // Right-click removes a point with its mirror image; three stay at least.
    EXPECT_TRUE(editor.removePoint(1));
    EXPECT_EQ(editor.points().size(), count);
    editor.setMirror(false);
    while (editor.points().size() > 3) EXPECT_TRUE(editor.removePoint(0));
    EXPECT_FALSE(editor.removePoint(0)) << "three stay";
    // A self-crossing outline is said (drawn red), not refused here.
    editor.setShape({{-1.0f, -1.0f}, {1.0f, 1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}}, false, true);
    EXPECT_FALSE(editor.problem().isEmpty());
}

TEST(AppTubes, TubesSectionPicksShapesEditsTheLibraryAndFillsTubes) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    BrushSession session;
    session.setDocument(doc.get());
    TubePanel panel;
    panel.resize(300, 800);
    panel.setSession(&session);
    int guideEdits = 0;
    QString said;
    QObject::connect(&panel, &TubePanel::guidesEdited, [&] { ++guideEdits; });
    QObject::connect(&panel, &TubePanel::message, [&](const QString& text) { said = text; });
    // No targets: the picker sets what Place and Draw make.
    EXPECT_EQ(panel.source(), "none");
    EXPECT_FALSE(panel.applyButton()->isEnabled());
    panel.pick("blade");
    EXPECT_EQ(guideEdits, 0);
    EXPECT_EQ(json::parse(placementTubeJson(session.settings().place))["shape"], "blade") << "the placement tube";
    EXPECT_FALSE(panel.editor()->editable()) << "a built-in shows read-only";
    EXPECT_TRUE(panel.editAsCustomButton()->isVisibleTo(&panel));

    // Targets: the selection; the shape picked goes to them (one step).
    const std::vector<GuideId> ids = primaryGuides(*doc, 2);
    doc->setSelection(ids);
    panel.refresh();
    EXPECT_EQ(panel.source(), "selection");
    EXPECT_EQ(panel.choice(), "round") << "the choice follows the targets";
    EXPECT_TRUE(panel.targetsLabel()->text().startsWith(QStringLiteral("2 guides selected")));
    EXPECT_TRUE(panel.widthSlider()->toolTip().contains(QStringLiteral("2 guides selected"))) << "what the fields change: their tips";
    panel.pick("ribbon");
    EXPECT_EQ(guideEdits, 1);
    EXPECT_EQ(guideOf(*doc, ids[1]).tube.shapeName(), "ribbon");
    panel.setFlatness(0.5);
    EXPECT_NEAR(guideOf(*doc, ids[0]).tube.aspectAt(0.0f), 0.5f, 1e-5f);
    EXPECT_NEAR(panel.flatnessSlider()->value(), 0.5, 1e-5);
    panel.setTwist(90.0);
    EXPECT_NEAR(guideOf(*doc, ids[0]).tube.twistAt(1.0f), 3.14159265f / 2.0f, 1e-4f);
    const double width = panel.widthSlider()->value();
    ASSERT_GT(width, 0.0);
    panel.setWidth(width * 3.0);
    EXPECT_NEAR(panel.widthSlider()->value(), width * 3.0, width * 1e-3);
    // Smooth: the current channel (the Shape brush's) or all.
    session.settings().brush.tubeChannel = TubeChannel::Flatness;
    panel.smoothTargets();
    EXPECT_EQ(doc->undoText(), "Undo smooth tube");

    // Edit as Custom: a library copy the targets take, editable; Rename in place; Duplicate.
    panel.editAsCustom();
    EXPECT_EQ(panel.choice(), "ribbon 1");
    EXPECT_TRUE(panel.libraryShape("ribbon 1"));
    EXPECT_TRUE(panel.editor()->editable());
    EXPECT_EQ(guideOf(*doc, ids[0]).tube.shapeName(), "ribbon 1");
    EXPECT_FALSE(panel.editAsCustomButton()->isVisibleTo(&panel));
    EXPECT_TRUE(panel.renameButton()->isEnabled());
    panel.startRename();
    EXPECT_TRUE(panel.renameField()->isVisibleTo(&panel));
    EXPECT_TRUE(panel.busy()) << "a refresh waits while the name is typed";
    panel.renameField()->setText(QStringLiteral("flat lock"));
    panel.finishRename();
    EXPECT_FALSE(panel.renameField()->isVisibleTo(&panel));
    EXPECT_EQ(panel.choice(), "flat lock");
    EXPECT_EQ(guideOf(*doc, ids[0]).tube.shapeName(), "flat lock");
    EXPECT_GE(panel.shapeBox()->findData(QStringLiteral("flat lock")), 0) << "listed in the picker";
    panel.duplicateShape();
    EXPECT_EQ(panel.choice(), "flat lock 1");
    EXPECT_EQ(guideOf(*doc, ids[0]).tube.shapeName(), "flat lock") << "a copy: the guides keep theirs";
    // The editor's edit commits to the library shape (one step); the guides taking it follow.
    panel.pick("flat lock");
    EXPECT_EQ(panel.choice(), "flat lock");
    const std::vector<SectionPoint> diamond = {{0.0f, 1.0f}, {-1.0f, 0.0f}, {0.0f, -1.0f}, {1.0f, 0.0f}};
    panel.editor()->setShape(diamond, false, true);
    emit panel.editor()->committed(panel.editor()->points());
    EXPECT_EQ(doc->undoText(), "Undo edit tube shape");
    EXPECT_FALSE(guideOf(*doc, ids[0]).tube.custom->smooth);
    EXPECT_EQ(guideOf(*doc, ids[0]).tube.custom->points.size(), 4u);
    // Delete: the first click arms (it says how many go round), the second deletes.
    panel.deleteShape();
    EXPECT_TRUE(panel.deleteButton()->text().startsWith(QStringLiteral("Delete? 2")));
    EXPECT_TRUE(panel.libraryShape("flat lock"));
    panel.deleteShape();
    EXPECT_FALSE(panel.libraryShape("flat lock"));
    EXPECT_EQ(panel.deleteButton()->text(), QStringLiteral("Delete"));
    EXPECT_EQ(guideOf(*doc, ids[0]).tube.shapeName(), "round");
    // Mixed: the targets' shapes differ.
    EXPECT_EQ(doc->editTubes({ids[1]}, json{{"shape", "teardrop"}}), "");
    panel.refresh();
    EXPECT_EQ(panel.shapeBox()->currentData().toString(), QString()) << "Mixed";
    // A refused edit is said; nothing changes.
    said.clear();
    panel.renameField()->setText(QStringLiteral("round"));
    panel.pick("flat lock 1");
    panel.startRename();
    panel.renameField()->setText(QStringLiteral("round"));
    panel.finishRename();
    EXPECT_FALSE(said.isEmpty()) << "a built-in's name";
    EXPECT_TRUE(panel.libraryShape("flat lock 1"));

    // Fill Tubes and Tube Blend are not the panel's: the Interpolate node's settings (the Graph
    // Editor's node settings show them).
    EXPECT_EQ(panel.findChild<QWidget*>(QStringLiteral("fillTubes")), nullptr);
    EXPECT_EQ(panel.findChild<QWidget*>(QStringLiteral("tubeBlend")), nullptr);
    ASSERT_TRUE(doc->fillTubes());
    std::string interpolate;
    for (const GroomDocument::NodeInfo& node : doc->nodes()) {
        if (node.type == "Interpolate") interpolate = node.name;
    }
    ASSERT_FALSE(interpolate.empty());
    ASSERT_TRUE(doc->nodeSpec(interpolate));
    std::vector<std::string> params;
    for (const json& p : doc->nodeSpec(interpolate)->value("params", json::array())) params.push_back(p.value("name", std::string()));
    EXPECT_NE(std::find(params.begin(), params.end(), "fillTubes"), params.end());
    EXPECT_NE(std::find(params.begin(), params.end(), "tubeBlend"), params.end());
}

TEST(AppTubes, ShapeBrushCardPaintsAChannelAndHoldsTheProfile) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    BrushSession session;
    session.setDocument(doc.get());
    GroomTools tools;
    tools.setSession(&session);
    tools.setGroomName(QStringLiteral("sample"));
    TubePanel* tubes = tools.tubePanel();
    ASSERT_NE(tubes, nullptr);
    session.setTool("shape");
    tools.setTool("shape");
    BrushPanel* card = tools.card();
    EXPECT_TRUE(card->isAncestorOf(tubes)) << "the tubes' blocks are the card's";
    EXPECT_FALSE(tubes->isHidden());
    EXPECT_EQ(tubes->mode(), TubePanel::Mode::Edit);
    // Shape's Profile block: the picker with Apply beside it, the outline editor (at most 200 px
    // square), Edit as custom, Mirror and Smooth; the library's glyphs in its header.
    CardBlock* profile = tubes->profileBlock();
    EXPECT_FALSE(profile->isHidden());
    EXPECT_TRUE(profile->isAncestorOf(tubes->editor()));
    EXPECT_TRUE(profile->isAncestorOf(tubes->applyButton()));
    EXPECT_TRUE(profile->isAncestorOf(tubes->shapeBox()));
    EXPECT_EQ(tubes->shapeBox()->parentWidget(), tubes->applyButton()->parentWidget()) << "Apply beside the picker";
    EXPECT_LE(tubes->editor()->maximumWidth(), 200);
    EXPECT_LE(tubes->editor()->height(), 200);
    for (QWidget* w : {static_cast<QWidget*>(tubes->newButton()), static_cast<QWidget*>(tubes->renameButton()),
                       static_cast<QWidget*>(tubes->duplicateButton()), static_cast<QWidget*>(tubes->deleteButton())}) {
        EXPECT_TRUE(profile->isAncestorOf(w));
        EXPECT_EQ(qobject_cast<QToolButton*>(w)->property("role").toString(), QStringLiteral("glyph"));
    }
    EXPECT_TRUE(tubes->selectedBlock()->isAncestorOf(tubes->widthSlider())) << "Selected guides: Width, Flatness, Twist";
    EXPECT_TRUE(tubes->selectedBlock()->isAncestorOf(tubes->smoothButton()));
    EXPECT_TRUE(tubes->tubeBlock()->isHidden());
    auto* box = card->findChild<QWidget*>(QStringLiteral("tubeChannelBox"));
    ASSERT_NE(box, nullptr);
    EXPECT_FALSE(box->isHidden());
    std::vector<QString> labels;
    for (const char* name : {"tubeChannel_width", "tubeChannel_flatness", "tubeChannel_twist"}) {
        auto* chip = card->findChild<QToolButton*>(QLatin1String(name));
        ASSERT_NE(chip, nullptr) << name;
        labels.push_back(chip->text());
    }
    EXPECT_EQ(labels, (std::vector<QString>{QStringLiteral("Width"), QStringLiteral("Flatness"), QStringLiteral("Twist")}));
    EXPECT_TRUE(card->findChild<QToolButton*>(QStringLiteral("tubeChannel_width"))->isChecked());
    card->findChild<QToolButton*>(QStringLiteral("tubeChannel_twist"))->click();
    EXPECT_EQ(session.settings().brush.tubeChannel, TubeChannel::Twist);
    EXPECT_TRUE(card->findChild<QToolButton*>(QStringLiteral("tubeChannel_twist"))->isChecked());
    EXPECT_FALSE(card->findChild<QToolButton*>(QStringLiteral("tubeChannel_width"))->isChecked()) << "one at a time";
    // Another tool hides the chips and the tubes; Place shows the Tube block: the new guides'.
    tools.setTool("comb");
    EXPECT_TRUE(box->isHidden());
    EXPECT_TRUE(tubes->isHidden()) << "the tubes only with Place, Draw, Shape and Select";
    tools.setTool("place");
    EXPECT_FALSE(tubes->isHidden());
    EXPECT_EQ(tubes->mode(), TubePanel::Mode::New);
    EXPECT_FALSE(tubes->tubeBlock()->isHidden());
    EXPECT_TRUE(tubes->profileBlock()->isHidden());
    EXPECT_TRUE(tubes->tubeBlock()->isAncestorOf(tubes->shapeBox())) << "the picker moves into the Tube block";
    EXPECT_TRUE(tubes->tubeBlock()->isAncestorOf(tubes->widthSlider()));
    EXPECT_TRUE(tubes->tubeBlock()->isAncestorOf(tubes->preview()));
    EXPECT_FALSE(tubes->preview()->editable()) << "a small read-only preview";
    tools.setTool("select");
    EXPECT_EQ(tubes->mode(), TubePanel::Mode::Edit) << "Select: the selected guides' profile too";
    EXPECT_TRUE(tubes->profileBlock()->isAncestorOf(tubes->shapeBox()));
}

TEST(AppGroomTools, TilesKeepTheirSizeAndTheColumnsFollowTheWidth) {
    // The grid: every tile one fixed size (labeled 56 x 50, icons alone 34 x 34; 20 px
    // icons), left-aligned, never stretched; as many to a row as (width + spacing) / (tile + spacing),
    // at most four.
    GroomTools tools;
    tools.setGroomName(QStringLiteral("sample"));
    EXPECT_TRUE(tools.topPart()->isAncestorOf(tools.tabBar()));
    EXPECT_TRUE(tools.topPart()->isAncestorOf(tools.tile("comb")));
    EXPECT_TRUE(tools.scrollArea()->isAncestorOf(tools.topPart())) << "the whole panel scrolls: the header, the tabs and the grid too";
    EXPECT_TRUE(tools.scrollArea()->isAncestorOf(tools.card()));
    auto* grid = qobject_cast<ToolGrid*>(tools.tile("comb")->parentWidget());
    ASSERT_NE(grid, nullptr);
    for (const bool labels : {true, false}) {
        tools.setTileLabels(labels);
        const QSize tile = labels ? QSize(56, 50) : QSize(34, 34);
        EXPECT_EQ(grid->tile(), tile) << "labels " << labels;
        for (const int width : {120, 200, 252, 320, 480}) {
            const int columns = grid->columnsFor(width);
            EXPECT_EQ(columns, std::min(4, (width + grid->spacing()) / (tile.width() + grid->spacing()))) << width << ": as many as fit, at most four";
            grid->resize(width, grid->heightForWidth(width));
            QResizeEvent resized(grid->size(), QSize());  // a hidden widget gets its resize event on show
            QCoreApplication::sendEvent(grid, &resized);
            int lefts = 0;
            for (QAbstractButton* t : grid->findChildren<QAbstractButton*>()) {
                EXPECT_EQ(t->size(), tile) << "a tile stretched at " << width;
                EXPECT_EQ((t->x()) % (tile.width() + grid->spacing()), 0) << "on the grid's columns, from the left";
                EXPECT_LE(t->geometry().right(), width);
                if (t->x() == 0) ++lefts;
            }
            EXPECT_GT(lefts, 0);
        }
        EXPECT_GT(grid->heightForWidth(120), grid->heightForWidth(480)) << "narrower: more rows";
    }
    QAbstractButton* comb = tools.tile("comb");
    EXPECT_EQ(qobject_cast<QToolButton*>(comb)->iconSize(), QSize(20, 20));
    // The Paint tab: Paint is one full-width button over the maps, not a tile in a grid.
    auto* paint = qobject_cast<QToolButton*>(tools.tile("paint"));
    ASSERT_NE(paint, nullptr);
    EXPECT_EQ(qobject_cast<ToolGrid*>(paint->parentWidget()), nullptr);
    EXPECT_EQ(paint->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
    EXPECT_EQ(paint->sizePolicy().horizontalPolicy(), QSizePolicy::Expanding);
    EXPECT_EQ(paint->maximumWidth(), QWIDGETSIZE_MAX);
}

TEST(AppGroomTools, TheCardsBlocksInOrderPerTool) {
    // Every card: the title (the ? holds the hint and the gestures, then the short line), Brush,
    // the tool's own block(s), Applies to; a block only with rows.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    BrushSession session;
    session.setDocument(doc.get());
    GroomTools tools;
    tools.setSession(&session);
    tools.setGroomName(QStringLiteral("sample"));
    BrushPanel* card = tools.card();
    using Titles = std::vector<std::string>;
    const std::vector<std::pair<std::string, Titles>> cases = {
        {"comb", {"Brush", "Applies to"}},  // Stretch: a Brush chip
        {"smooth", {"Brush", "Applies to"}},
        {"flow", {"Brush", "Flow"}},
        {"place", {"New guides", "Tube", "Applies to"}},
        {"draw", {"New guides", "Tube", "Applies to"}},
        {"shape", {"Brush", "Paints", "Profile", "Selected guides", "Applies to"}},
        {"select", {"Profile", "Selected guides", "Applies to"}},
        {"erase", {"Brush", "Erases", "Applies to"}},
        {"lengths", {"Keys", "Applies to"}},
        {"part", {"Parts", "Applies to"}},
        {"freeze", {"Brush", "Applies to"}},
    };
    for (const auto& [tool, want] : cases) {
        tools.setTool(tool);
        EXPECT_EQ(card->blockTitles(), want) << tool;
        EXPECT_FALSE(card->isHidden()) << tool;
        EXPECT_TRUE(tools.noToolLabel()->isHidden()) << tool;
        const groomlab::BrushEntry* e = brushEntry(tool);
        EXPECT_TRUE(tools.tile(tool)->toolTip().contains(QString::fromStdString(e->hint).left(20).toHtmlEscaped())) << tool << ": the tile has the hint";
    }
    // The card is only the settings: no title, no hint line; the gestures are a brush tile's tip.
    tools.setTool("comb");
    EXPECT_EQ(card->findChild<QLabel*>(QStringLiteral("toolCardTitle")), nullptr);
    EXPECT_EQ(card->findChild<QLabel*>(QStringLiteral("toolCardShort")), nullptr);
    EXPECT_TRUE(tools.tile("comb")->toolTip().contains(QStringLiteral("Page Up / Down")));
    EXPECT_FALSE(tools.tile("part")->toolTip().contains(QStringLiteral("Page Up / Down"))) << "not a brush";
    EXPECT_EQ(card->findChild<QLabel*>(QStringLiteral("gestures")), nullptr);
    // Mirror: Symmetry and its mode inside Applies to (not for Flow).
    auto* applies = card->findChild<CardBlock*>(QStringLiteral("appliesBlock"));
    ASSERT_NE(applies, nullptr);
    EXPECT_TRUE(applies->isAncestorOf(tools.symmetryBox()));
    EXPECT_TRUE(applies->isAncestorOf(tools.symmetryMode()));
    // No tool: one muted line, no card.
    tools.setTool({});
    EXPECT_TRUE(card->isHidden());
    EXPECT_FALSE(tools.noToolLabel()->isHidden());
    EXPECT_EQ(tools.findChild<QLabel*>(QStringLiteral("toolHeading")), nullptr) << "no TOOL heading";
}

TEST(AppGroomTools, NoSelectedOnlyOrFrontOnlyInThePanel) {
    // The brushes keep their defaults (off and on); nothing in the panel sets them.
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    BrushSession session;
    session.setDocument(doc.get());
    GroomTools tools;
    tools.setSession(&session);
    tools.setGroomName(QStringLiteral("sample"));
    for (const char* tool : {"comb", "smooth", "shape", "paint", "freeze", "flow", "erase", "cut"}) {
        tools.setTool(tool);
        for (QAbstractButton* b : tools.findChildren<QAbstractButton*>()) {
            const QString text = b->text();
            EXPECT_FALSE(text.contains(QStringLiteral("Selected only"), Qt::CaseInsensitive)) << tool;
            EXPECT_FALSE(text.contains(QStringLiteral("Front"), Qt::CaseInsensitive) && text.contains(QStringLiteral("only"), Qt::CaseInsensitive)) << tool;
        }
        EXPECT_EQ(tools.findChild<QWidget*>(QStringLiteral("selectedOnly")), nullptr);
        EXPECT_EQ(tools.findChild<QWidget*>(QStringLiteral("frontFacingOnly")), nullptr);
    }
    EXPECT_FALSE(session.settings().selectedOnly);
    EXPECT_TRUE(session.settings().maskByScalp);
    // Gone from the top too: the group combo, Isolate's check box, the Symmetry row.
    EXPECT_EQ(tools.findChild<QWidget*>(QStringLiteral("targetRow")), nullptr);
    EXPECT_EQ(tools.findChild<QComboBox*>(QStringLiteral("brushGroup")), nullptr);
    EXPECT_EQ(tools.findChild<QWidget*>(QStringLiteral("section_tubes")), nullptr);
    EXPECT_EQ(tools.findChild<QWidget*>(QStringLiteral("section_sculptDisplay")), nullptr);
}

TEST(AppGroomTools, PaintsValueAndFillAreInThePaintCard) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    BrushSession session;
    session.setDocument(doc.get());
    GroomTools tools;
    tools.setSession(&session);
    tools.setGroomName(QStringLiteral("sample"));
    MapsPanel maps;
    maps.setSession(&session);
    maps.setDocument(doc.get());
    maps.setActions({{"map.new", {[](const ui::json&, bool) {}}}, {"map.import", {[](const ui::json&, bool) {}}},
                     {"map.rename", {[](const ui::json&, bool) {}}}},
                    {});
    tools.setPaintContent(&maps);
    ASSERT_TRUE(tools.showTab(QStringLiteral("paint")));
    BrushPanel* card = tools.card();
    EXPECT_EQ(card->tool(), "paint") << "the Paint tab's card is Paint's";
    EXPECT_FALSE(card->isHidden());
    EXPECT_EQ(card->blockTitles(), (std::vector<std::string>{"Brush", "Paint", "Applies to"}));
    auto* block = card->findChild<CardBlock*>(QStringLiteral("paintBlock"));
    ASSERT_NE(block, nullptr);
    EXPECT_TRUE(block->isAncestorOf(maps.paintControls())) << "Mode, Value + Fill, the color: the card's";
    EXPECT_FALSE(maps.isAncestorOf(maps.paintControls()));
    auto* fill = card->findChild<QPushButton*>(QStringLiteral("paintFill"));
    ASSERT_NE(fill, nullptr);
    EXPECT_TRUE(block->isAncestorOf(fill));
    bool value = false;
    for (ValueSlider* s : block->findChildren<ValueSlider*>()) value = value || s->caption() == QStringLiteral("Value");
    EXPECT_TRUE(value);
    // The Maps list over the card, right under the grid: the header's + New Map and Import glyphs,
    // the picked map's box.
    EXPECT_EQ(maps.bar()->ids(), (std::vector<std::string>{"map.new", "map.import"}));
    QLayout* work = tools.card()->parentWidget()->layout();
    ASSERT_NE(work, nullptr);
    for (const char* tab : {"tools", "paint", "correctives", "photos"})
        EXPECT_LT(work->indexOf(tools.findChild<QWidget*>(QStringLiteral("list_") + QLatin1String(tab))), work->indexOf(tools.card())) << tab;
    // Photos: no card.
    ASSERT_TRUE(tools.showTab(QStringLiteral("photos")));
    EXPECT_TRUE(card->isHidden());
    EXPECT_TRUE(tools.noToolLabel()->isHidden());
}

TEST(AppGroomTools, GuideGroupsAlwaysOpenAndAPickedRowSetsTheGroup) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    BrushSession session;
    session.setDocument(doc.get());
    GroomTools tools;
    tools.setSession(&session);
    tools.setGroomName(QStringLiteral("sample"));
    GuidesPanel guides;
    guides.setDocument(doc.get());
    tools.setGroupsContent(&guides);
    tools.resize(300, 900);
    tools.show();
    QCoreApplication::processEvents();
    guides.refresh();
    // Always open: no folding header; Isolate's eye and + New Group in its header.
    EXPECT_TRUE(guides.findChildren<Section*>().empty());
    EXPECT_TRUE(guides.header()->isAncestorOf(tools.isolateButton()));
    EXPECT_TRUE(guides.isVisible());
    QTreeWidget* tree = guides.tree();
    ASSERT_GT(tree->topLevelItemCount(), 0);
    EXPECT_TRUE(tree->isHeaderHidden()) << "plain rows";
    EXPECT_EQ(tree->columnCount(), 3) << "the dot and name, the count, the eye";
    EXPECT_EQ(guides.header()->countLabel()->text(), QString::number(doc->session().authoredHierarchy()->groups().size()));
    EXPECT_TRUE(tree->isVisible());
    EXPECT_TRUE(guides.groupBox()->isVisible()) << "the picked group's box";
    // A click on a row: the brushes' group (and level).
    QTreeWidgetItem* row = tree->topLevelItem(tree->topLevelItemCount() - 1);
    const std::string name = row->data(0, Qt::UserRole).toString().toStdString();
    session.settings().brush.group.clear();
    session.settings().brush.level = -1;
    int changed = 0;
    QObject::connect(&tools, &GroomTools::settingsChanged, [&] { ++changed; });
    emit tree->itemClicked(row, 0);
    EXPECT_EQ(session.settings().brush.group, name);
    EXPECT_EQ(session.settings().brush.level, 0);
    EXPECT_EQ(changed, 1);
    EXPECT_EQ(tools.currentGroup(), name);
    // Isolate: the eye; the picked group then.
    EXPECT_EQ(tools.isolatedGroup(), "");
    tools.isolateButton()->setChecked(true);
    EXPECT_EQ(tools.isolatedGroup(), name);
    // The eye column hides a group (not a pick).
    session.settings().brush.group = "elsewhere";
    emit tree->itemClicked(row, 2);
    EXPECT_EQ(session.settings().brush.group, "elsewhere");
    EXPECT_EQ(doc->hiddenGroups().count(name), 1u);
}

TEST(AppGroomTools, TheFlowCardCountsTheArrows) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    BrushSession session;
    session.setDocument(doc.get());
    GroomTools tools;
    tools.setSession(&session);
    tools.setGroomName(QStringLiteral("sample"));
    session.setTool("flow");
    tools.setTool("flow");
    BrushPanel* card = tools.card();
    auto* count = card->findChild<QLabel*>(QStringLiteral("flowCount"));
    auto* clear = card->findChild<QPushButton*>(QStringLiteral("clearFlow"));
    ASSERT_TRUE(count && clear);
    tools.setFlowCount(34, 2);
    EXPECT_EQ(count->text(), QStringLiteral("34 arrows \u00b7 2 sinks"));
    EXPECT_TRUE(clear->isEnabled());
    EXPECT_EQ(BrushPanel::flowCountText(1, 1), QStringLiteral("1 arrow \u00b7 1 sink"));
    tools.setFlowCount(0, 0);
    EXPECT_EQ(count->text(), QStringLiteral("No flow arrows yet"));
    EXPECT_FALSE(clear->isEnabled());
    EXPECT_NE(clear->toolTip().indexOf(QStringLiteral("one undo step")), -1) << "the tip of GroomLab > Utilities > Delete All Flow Arrows";
}

TEST(AppTubes, WidthFollowsTheLengthUnit) {
    // Edit > Units' lengths (as Maya's View > Units): "Width (mm)", the slider in mm; the tube Place
    // and Draw give new guides stays in scene units.
    const std::string before = unitChoice("length");
    setUnitChoice("length", "scene");
    TubePanel panel;
    EXPECT_EQ(panel.widthCaption(), QStringLiteral("Width (units)"));
    EXPECT_DOUBLE_EQ(panel.widthSlider()->value(), 0.1);
    setUnitChoice("length", "mm");
    panel.refreshUnits();
    EXPECT_EQ(panel.widthCaption(), QStringLiteral("Width (mm)"));
    EXPECT_NEAR(panel.widthSlider()->value(), 1.0, 1e-9) << "0.1 cm shown as 1 mm";
    EXPECT_NEAR(json::parse(panel.placement()).at("width").get<double>(), 0.1, 1e-9);
    setUnitChoice("length", "cm");
    panel.refreshUnits();
    EXPECT_EQ(panel.widthCaption(), QStringLiteral("Width (cm)"));
    EXPECT_NEAR(panel.widthSlider()->value(), 0.1, 1e-9);
    setUnitChoice("length", before);
}

}  // namespace groomlab::app
