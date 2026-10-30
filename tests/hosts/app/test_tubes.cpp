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
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QToolButton>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "brush_panel.h"
#include "brush_session.h"
#include "glass.h"
#include "groom_document.h"
#include "groom_tools.h"
#include "groomlab/brush.h"
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
    int guideEdits = 0, graphEdits = 0;
    QString said;
    QObject::connect(&panel, &TubePanel::guidesEdited, [&] { ++guideEdits; });
    QObject::connect(&panel, &TubePanel::graphEdited, [&] { ++graphEdits; });
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

    // Fill Tubes and Tube Blend: the graph's (one step each).
    const bool filled = doc->fillTubes()->fill;
    panel.fillBox()->setChecked(!filled);
    EXPECT_EQ(graphEdits, 1);
    EXPECT_EQ(doc->fillTubes()->fill, !filled);
    EXPECT_EQ(panel.blendSlider()->isEnabled(), !filled);
    panel.setBlend(40.0);
    EXPECT_EQ(graphEdits, 2);
    EXPECT_NEAR(doc->fillTubes()->blend, 0.4f, 1e-6f);
}

TEST(AppTubes, ShapeBrushCardPaintsAChannelAndOpensTheTubesSection) {
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    BrushSession session;
    session.setDocument(doc.get());
    GroomTools tools;
    tools.setSession(&session);
    tools.setGroomName(QStringLiteral("sample"));
    ASSERT_NE(tools.tubePanel(), nullptr);
    Section* tubes = tools.section("tubes");
    ASSERT_NE(tubes, nullptr);
    EXPECT_FALSE(tubes->expanded());
    session.setTool("shape");
    tools.setTool("shape");
    EXPECT_TRUE(tubes->expanded()) << "the Shape brush opens Tubes (Maya's)";
    BrushPanel* card = tools.card();
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
    // Folded by hand, Tubes stays folded while the tool does; another tool hides the chips.
    tubes->setExpanded(false);
    tools.setTool("shape");
    EXPECT_FALSE(tubes->expanded());
    tools.setTool("comb");
    EXPECT_TRUE(box->isHidden());
    EXPECT_TRUE(tubes->isHidden()) << "Tubes only with Place, Draw, Shape and Select (palette.json's tools)";
    tools.setTool("place");
    EXPECT_FALSE(tubes->isHidden());
    EXPECT_TRUE(tubes->expanded()) << "Place opens it too";
}

TEST(AppGroomTools, SectionsFollowTheToolAndTheTab) {
    // palette.json's sections: Tubes with Place, Draw, Shape and Select; While sculpting with the
    // brushes that stroke; the guide groups on the Tools tab; Output always (Maya's and Blender's rule).
    auto doc = GroomDocument::open(kSampleGroom);
    ASSERT_TRUE(doc);
    BrushSession session;
    session.setDocument(doc.get());
    GroomTools tools;
    tools.setSession(&session);
    tools.setGroomName(QStringLiteral("sample"));
    Section* tubes = tools.section("tubes");
    Section* sculpting = tools.section("sculptDisplay");
    Section* groups = tools.section("groups");
    Section* output = tools.section("output");
    ASSERT_TRUE(tubes && sculpting && groups && output);
    tools.setTool({});
    EXPECT_TRUE(tubes->isHidden());
    EXPECT_TRUE(sculpting->isHidden());
    EXPECT_FALSE(groups->isHidden());
    EXPECT_FALSE(output->isHidden());
    tools.setTool("part");
    EXPECT_TRUE(tubes->isHidden());
    EXPECT_TRUE(sculpting->isHidden());
    tools.setTool("comb");
    EXPECT_TRUE(tubes->isHidden());
    EXPECT_FALSE(sculpting->isHidden());
    tools.setTool("select");
    EXPECT_FALSE(tubes->isHidden()) << "the selected guides' tubes";
    EXPECT_TRUE(sculpting->isHidden());
    ASSERT_TRUE(tools.showTab(QStringLiteral("paint")));
    EXPECT_TRUE(groups->isHidden()) << "the guide groups: the Tools tab's";
    EXPECT_FALSE(output->isHidden());
    ASSERT_TRUE(tools.showTab(QStringLiteral("tools")));
    EXPECT_FALSE(groups->isHidden());
    const ui::json rule = {{"id", "x"}, {"tools", {"place"}}, {"tabs", {"tools"}}};
    EXPECT_TRUE(GroomTools::sectionShown(rule, "place", "tools"));
    EXPECT_FALSE(GroomTools::sectionShown(rule, "place", "paint"));
    EXPECT_FALSE(GroomTools::sectionShown(rule, "", "tools"));
    EXPECT_TRUE(GroomTools::sectionShown({{"id", "output"}}, "", "photos"));
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
