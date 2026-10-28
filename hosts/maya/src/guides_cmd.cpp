// SPDX-License-Identifier: Apache-2.0
#include "guides_cmd.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include <maya/M3dView.h>
#include <maya/MArgDatabase.h>
#include <maya/MIntArray.h>
#include <maya/MArgList.h>
#include <maya/MDagPath.h>
#include <maya/MDoubleArray.h>
#include <maya/MFnStringArrayData.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnGeometryData.h>
#include <maya/MFnNurbsCurve.h>
#include <maya/MMatrix.h>
#include <maya/MPoint.h>
#include <maya/MGlobal.h>
#include <maya/MPlug.h>
#include <maya/MSelectionList.h>
#include <maya/MStringArray.h>

#include "command_args.h"
#include "guarded.h"
#include "groom_shape.h"
#include "groomlab/curve.h"
#include "groomlab/display.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/guide_set.h"
#include "groomlab/guide_selection.h"
#include "maya_inputs.h"

namespace groomlab::maya {

namespace {

// Flags: short, long, argument type.
const char* kFromCurves[] = {"-fc", "-fromCurves"};
const char* kFromNode[] = {"-fnd", "-fromNode"};
const char* kDensity[] = {"-dn", "-density"};
const char* kGroup[] = {"-g", "-group"};
const char* kTubeRadius[] = {"-tr", "-tubeRadius"};
const char* kTubeTipRadius[] = {"-ttr", "-tubeTipRadius"};
const char* kGenerate[] = {"-gc", "-generateChildren"};
const char* kNameFlag[] = {"-n", "-name"};
const char* kCount[] = {"-c", "-count"};
const char* kRootSpread[] = {"-rs", "-rootSpread"};
const char* kTipSpread[] = {"-ts", "-tipSpread"};
const char* kTwist[] = {"-tw", "-twist"};
const char* kJitter[] = {"-j", "-jitter"};
const char* kTubeScale[] = {"-tsc", "-tubeScale"};
const char* kSeed[] = {"-sd", "-seed"};
const char* kRemoveGroup[] = {"-rg", "-removeGroup"};
const char* kRenameGroup[] = {"-rng", "-renameGroup"};
const char* kAddGroup[] = {"-adg", "-addGroup"};
const char* kLock[] = {"-lk", "-lock"};
const char* kUnlock[] = {"-ul", "-unlock"};
const char* kGuide[] = {"-gid", "-guide"};
const char* kListGroups[] = {"-lg", "-listGroups"};
const char* kListGuides[] = {"-lsg", "-listGuides"};
const char* kGuidePoints[] = {"-gp", "-guidePoints"};
const char* kEvaluated[] = {"-ev", "-evaluated"};  // with -guidePoints: as shown (posed, shot sculpts applied)
const char* kClear[] = {"-cl", "-clear"};
const char* kLevel[] = {"-lv", "-level"};
const char* kSelect[] = {"-sel", "-select"};
const char* kAdd[] = {"-ap", "-append"};  // "-add" is refused by MSyntax
const char* kDeselect[] = {"-ds", "-deselect"};
const char* kToggle[] = {"-tgl", "-toggle"};
const char* kMirrorSelection[] = {"-msl", "-mirrorSelection"};
const char* kListSelected[] = {"-lss", "-listSelected"};
const char* kClearSelection[] = {"-csl", "-clearSelection"};
const char* kReparent[] = {"-rp", "-reparent"};
const char* kPromote[] = {"-pro", "-promote"};
const char* kDemote[] = {"-dem", "-demote"};
const char* kUnder[] = {"-u", "-under"};
const char* kMirror[] = {"-mir", "-mirror"};
const char* kAxis[] = {"-ax", "-axis"};
const char* kCenter[] = {"-ctr", "-center"};
const char* kFromNegative[] = {"-fn", "-fromNegative"};
const char* kSetAttribute[] = {"-sat", "-setAttribute"};
const char* kValue[] = {"-v", "-value"};
const char* kListAttributes[] = {"-lat", "-listAttributes"};
const char* kRemoveAttribute[] = {"-rat", "-removeAttribute"};
const char* kRenameAttribute[] = {"-rna", "-renameAttribute"};
const char* kResample[] = {"-rsm", "-resample"};
// Live Resample (Groom Tools' slider): the guides resampled shown as the shape's preview, from the
// stored guides each time (so dragging back and forth never resamples a resample), no undo step.
const char* kPreviewResample[] = {"-prs", "-previewResample"};
const char* kEndPreview[] = {"-epv", "-endPreview"};
// Each target guide's segment count (points - 1): the slider's value.
const char* kPointCounts[] = {"-pcn", "-pointCounts"};
const char* kOnlySelected[] = {"-osl", "-onlySelected"};
const char* kFreezeOthers[] = {"-fzo", "-freezeOthers"};
const char* kReshape[] = {"-rsh", "-reshape"};  // <guide id> <curve>, multi-use
// Tube guides (tube.h): a tube edit as JSON on the targets, and one guide's tube as JSON.
const char* kSetTube[] = {"-stb", "-setTube"};
const char* kTube[] = {"-tb", "-tube"};
// The groom's tube shape library (GuideHierarchy::tubeShapes): listed as JSON; edited by a JSON
// op (one undo step, every guide taking the shape following); a shape's new points shown live
// without changing the groom or making an undo step (endPreview drops it).
const char* kTubeShapes[] = {"-tss", "-tubeShapes"};
const char* kEditTubeShape[] = {"-ets", "-editTubeShape"};
const char* kPreviewTubeShape[] = {"-pts", "-previewTubeShape"};
// The guides the Tubes panel edits, as JSON: the selected ones, else the soloed ones (a brush's
// Shift+click: the guides left unfrozen while others are frozen, not those under them), with the
// first one's tube and every shape among them.
const char* kTubeTargets[] = {"-ttg", "-tubeTargets"};

// A tube shape op's points: [[u, v], ...].
std::vector<SectionPoint> shapePoints(const nlohmann::json& j) {
    if (!j.is_array()) throw std::invalid_argument("tube shape points: a list of [u, v] pairs");
    std::vector<SectionPoint> out;
    for (const nlohmann::json& p : j) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) {
            throw std::invalid_argument("tube shape points: a list of [u, v] pairs");
        }
        out.push_back({p[0].get<float>(), p[1].get<float>()});
    }
    return out;
}

nlohmann::json shapeOpJson(const MString& text) {
    try {
        nlohmann::json j = nlohmann::json::parse(text.asChar());
        if (!j.is_object()) throw std::invalid_argument("tube shape op: a JSON object");
        return j;
    } catch (const nlohmann::json::exception& e) {
        throw std::invalid_argument(std::string("tube shape op: ") + e.what());
    }
}

// The library as JSON: [{"name", "points": [[u, v], ...], "smooth", "users", "outline"}, ...].
std::string tubeShapesJson(const GuideHierarchy& h) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& [name, shape] : h.tubeShapes()) {
        nlohmann::json points = nlohmann::json::array(), outline = nlohmann::json::array();
        for (const SectionPoint& p : shape->points) points.push_back({p.u, p.v});
        for (const SectionPoint& p : shape->outline.points) outline.push_back({p.u, p.v});
        out.push_back({{"name", name}, {"points", points}, {"smooth", shape->smooth}, {"users", h.tubeShapeUsers(name)},
                       {"outline", outline}});
    }
    return out.dump();
}

// A curve's edit points in world space: its points at its distinct knots (a degree 1 curve's
// CVs; a cubic made through points, those points).
std::vector<Vec3f> curveEditPoints(const MObject& data) {
    MStatus status;
    MFnNurbsCurve fn(data, &status);
    if (!status) return {};
    MMatrix m;
    if (MFnGeometryData(data).getMatrix(m) != MS::kSuccess) m = MMatrix::identity;
    double start = 0.0, end = 0.0;
    fn.getKnotDomain(start, end);
    MDoubleArray knots;
    fn.getKnots(knots);
    std::vector<Vec3f> points;
    double last = start - 1.0;
    for (unsigned i = 0; i < knots.length(); ++i) {
        const double k = knots[i];
        if (k < start - 1e-9 || k > end + 1e-9 || k - last <= 1e-9) continue;
        last = k;
        MPoint p;
        fn.getPointAtParam(k, p, MSpace::kObject);
        p *= m;
        points.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
    }
    return points;
}

bool samePoints(const std::vector<Vec3f>& a, const std::vector<Vec3f>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if ((a[i] - b[i]).length() > 1e-4f) return false;
    }
    return true;
}

std::vector<MString> multiStrings(const MArgDatabase& db, const char* flag) {
    std::vector<MString> values;
    const unsigned uses = db.numberOfFlagUses(flag);
    for (unsigned i = 0; i < uses; ++i) {
        MArgList args;
        db.getFlagArgumentList(flag, i, args);
        values.push_back(args.asString(0));
    }
    return values;
}

// A row per group: name|parent|depth|guides|color, the color the group draws in when the groom's
// guideGroupColors gives it none (its own, a ToGuides node's, else its automatic one), "#rrggbb".
void describeGroups(const GuideHierarchy& h, const std::string& colorSeed, MStringArray& out) {
    const std::map<GroupId, std::array<float, 3>> automatic = autoGuideColors(h, guideHueSeed(colorSeed));
    for (const GuideGroup& g : h.groups()) {
        const GuideGroup* parent = h.group(g.parent);
        const auto shown = automatic.find(g.id);
        const std::array<float, 3> drawn = g.hasColor ? g.color : shown != automatic.end() ? shown->second : std::array<float, 3>{};
        const MString row = MString(g.name.c_str()) + "|" + (parent ? parent->name.c_str() : "") + "|" +
                            h.groupDepth(g.id) + "|" + static_cast<unsigned>(h.guidesInGroup(g.id).size()) + "|" +
                            guideColorHex(drawn).c_str();
        out.append(row);
    }
}

int axisIndex(MString text) {
    const std::string a = text.toLowerCase().asChar();
    if (a == "x" || a == "0") return 0;
    if (a == "y" || a == "1") return 1;
    if (a == "z" || a == "2") return 2;
    return -1;
}

std::vector<GuideId> parseIds(const std::vector<MString>& texts) {
    std::vector<GuideId> ids;
    for (const MString& t : texts) ids.push_back(std::strtoull(t.asChar(), nullptr, 10));
    return ids;
}

MObject selectionData(const std::vector<GuideId>& ids) {
    MStringArray values;
    for (const GuideId id : ids) values.append(std::to_string(id).c_str());
    MFnStringArrayData fn;
    return fn.create(values);
}

}  // namespace

const char* GuidesCmd::kName = "groomLabGuides";

void* GuidesCmd::creator() { return new GuidesCmd(); }

MSyntax GuidesCmd::newSyntax() {
    MSyntax s;
    s.addFlag(kFromCurves[0], kFromCurves[1], MSyntax::kString);
    s.makeFlagMultiUse(kFromCurves[0]);
    s.addFlag(kFromNode[0], kFromNode[1], MSyntax::kString);
    s.addFlag(kDensity[0], kDensity[1], MSyntax::kDouble);
    s.addFlag(kGroup[0], kGroup[1], MSyntax::kString);
    s.addFlag(kTubeRadius[0], kTubeRadius[1], MSyntax::kDouble);
    s.addFlag(kTubeTipRadius[0], kTubeTipRadius[1], MSyntax::kDouble);
    s.addFlag(kGenerate[0], kGenerate[1], MSyntax::kString);
    s.addFlag(kNameFlag[0], kNameFlag[1], MSyntax::kString);
    s.addFlag(kCount[0], kCount[1], MSyntax::kLong);
    s.addFlag(kRootSpread[0], kRootSpread[1], MSyntax::kDouble);
    s.addFlag(kTipSpread[0], kTipSpread[1], MSyntax::kDouble);
    s.addFlag(kTwist[0], kTwist[1], MSyntax::kDouble);
    s.addFlag(kJitter[0], kJitter[1], MSyntax::kDouble);
    s.addFlag(kTubeScale[0], kTubeScale[1], MSyntax::kDouble);
    s.addFlag(kSeed[0], kSeed[1], MSyntax::kLong);
    s.addFlag(kRemoveGroup[0], kRemoveGroup[1], MSyntax::kString);
    s.addFlag(kRenameGroup[0], kRenameGroup[1], MSyntax::kString, MSyntax::kString);
    s.addFlag(kAddGroup[0], kAddGroup[1], MSyntax::kString);
    s.addFlag(kLock[0], kLock[1]);
    s.addFlag(kUnlock[0], kUnlock[1]);
    s.addFlag(kGuide[0], kGuide[1], MSyntax::kString);
    s.makeFlagMultiUse(kGuide[0]);
    s.addFlag(kListGroups[0], kListGroups[1]);
    s.addFlag(kListGuides[0], kListGuides[1], MSyntax::kString);
    s.addFlag(kGuidePoints[0], kGuidePoints[1], MSyntax::kString);
    s.addFlag(kEvaluated[0], kEvaluated[1]);
    s.addFlag(kClear[0], kClear[1]);
    s.addFlag(kLevel[0], kLevel[1], MSyntax::kLong);
    s.addFlag(kSelect[0], kSelect[1]);
    s.addFlag(kAdd[0], kAdd[1]);
    s.addFlag(kDeselect[0], kDeselect[1]);
    s.addFlag(kToggle[0], kToggle[1]);
    s.addFlag(kMirrorSelection[0], kMirrorSelection[1], MSyntax::kString);
    s.addFlag(kListSelected[0], kListSelected[1]);
    s.addFlag(kClearSelection[0], kClearSelection[1]);
    s.addFlag(kReparent[0], kReparent[1], MSyntax::kString);
    s.addFlag(kPromote[0], kPromote[1], MSyntax::kString);
    s.addFlag(kDemote[0], kDemote[1], MSyntax::kString);
    s.addFlag(kUnder[0], kUnder[1], MSyntax::kString);
    s.addFlag(kMirror[0], kMirror[1], MSyntax::kString);
    s.addFlag(kAxis[0], kAxis[1], MSyntax::kString);
    s.addFlag(kCenter[0], kCenter[1], MSyntax::kDouble);
    s.addFlag(kFromNegative[0], kFromNegative[1]);
    s.addFlag(kSetAttribute[0], kSetAttribute[1], MSyntax::kString);
    s.addFlag(kValue[0], kValue[1], MSyntax::kDouble);
    s.addFlag(kListAttributes[0], kListAttributes[1]);
    s.addFlag(kRemoveAttribute[0], kRemoveAttribute[1], MSyntax::kString);
    s.addFlag(kRenameAttribute[0], kRenameAttribute[1], MSyntax::kString, MSyntax::kString);
    s.addFlag(kResample[0], kResample[1], MSyntax::kLong);
    s.addFlag(kPreviewResample[0], kPreviewResample[1], MSyntax::kLong);
    s.addFlag(kEndPreview[0], kEndPreview[1]);
    s.addFlag(kPointCounts[0], kPointCounts[1]);
    s.addFlag(kOnlySelected[0], kOnlySelected[1]);
    s.addFlag(kFreezeOthers[0], kFreezeOthers[1]);
    s.addFlag(kSetTube[0], kSetTube[1], MSyntax::kString);
    s.addFlag(kTube[0], kTube[1], MSyntax::kString);
    s.addFlag(kTubeShapes[0], kTubeShapes[1]);
    s.addFlag(kEditTubeShape[0], kEditTubeShape[1], MSyntax::kString);
    s.addFlag(kPreviewTubeShape[0], kPreviewTubeShape[1], MSyntax::kString);
    s.addFlag(kTubeTargets[0], kTubeTargets[1]);
    s.addFlag(kReshape[0], kReshape[1], MSyntax::kString, MSyntax::kString);
    s.makeFlagMultiUse(kReshape[0]);
    s.setObjectType(MSyntax::kSelectionList, 1, 1);
    s.useSelectionAsDefault(true);
    s.enableQuery(false);
    s.enableEdit(false);
    return s;
}

MStatus GuidesCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("GuidesCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
    
        // The groom.
        MDagPath groomPath;
        if (!groomArgument(kName, db, &groomPath)) return MS::kFailure;
        MFnDagNode groomFn(groomPath);
    
        const int actions = int(db.isFlagSet(kFromCurves[0])) + int(db.isFlagSet(kFromNode[0])) + int(db.isFlagSet(kGenerate[0])) +
                            int(db.isFlagSet(kRemoveGroup[0])) + int(db.isFlagSet(kRenameGroup[0])) + int(db.isFlagSet(kAddGroup[0])) + int(db.isFlagSet(kLock[0])) +
                            int(db.isFlagSet(kUnlock[0])) + int(db.isFlagSet(kListGroups[0])) +
                            int(db.isFlagSet(kListGuides[0])) + int(db.isFlagSet(kGuidePoints[0])) +
                            int(db.isFlagSet(kClear[0])) + int(db.isFlagSet(kSelect[0])) +
                            int(db.isFlagSet(kMirrorSelection[0])) + int(db.isFlagSet(kListSelected[0])) +
                            int(db.isFlagSet(kClearSelection[0])) + int(db.isFlagSet(kReparent[0])) +
                            int(db.isFlagSet(kPromote[0])) + int(db.isFlagSet(kDemote[0])) + int(db.isFlagSet(kMirror[0])) +
                            int(db.isFlagSet(kSetAttribute[0])) + int(db.isFlagSet(kListAttributes[0])) +
                            int(db.isFlagSet(kRemoveAttribute[0])) + int(db.isFlagSet(kRenameAttribute[0])) +
                            int(db.isFlagSet(kResample[0])) + int(db.isFlagSet(kReshape[0])) +
                            int(db.isFlagSet(kPreviewResample[0])) + int(db.isFlagSet(kEndPreview[0])) +
                            int(db.isFlagSet(kPointCounts[0])) + int(db.isFlagSet(kSetTube[0])) + int(db.isFlagSet(kTube[0])) +
                            int(db.isFlagSet(kTubeShapes[0])) + int(db.isFlagSet(kEditTubeShape[0])) +
                            int(db.isFlagSet(kPreviewTubeShape[0])) + int(db.isFlagSet(kTubeTargets[0]));
        if (actions != 1) return fail(kName, "give exactly one action flag");

        // The groups need no rooting: the shape's parse of guideData (kept while it is unchanged).
        if (db.isFlagSet(kListGroups[0])) {
            if (auto* shape = dynamic_cast<GroomShape*>(groomFn.userNode())) {
                std::string error;
                const std::shared_ptr<const GuideHierarchy> stored = shape->storedHierarchy(&error);
                if (!stored) return fail(kName, error.c_str());
                MStringArray rows;
                describeGroups(*stored, guideColorSeedOf(groomFn.object()), rows);
                setResult(rows);
                return MS::kSuccess;
            }
        }
    
        // Resample's targets on a hierarchy: -guide, -group or -level, else the selection, else
        // every guide (Resample with nothing selected does them all).
        auto resampleTargets = [&](const GuideHierarchy& on) -> std::vector<GuideId> {
            std::vector<GuideId> ids;
            if (db.isFlagSet(kGuide[0])) {
                ids = parseIds(multiStrings(db, kGuide[0]));
            } else if (db.isFlagSet(kGroup[0])) {
                ids = guidesAt(on, -1, flagString(db, kGroup[0], "").asChar());
            } else if (db.isFlagSet(kLevel[0])) {
                int level = 0;
                db.getFlagArgument(kLevel[0], 0, level);
                ids = guidesAt(on, level);
            } else {
                ids = readGuideSelection(groomPath.node());
                if (ids.empty()) {
                    for (const HierarchyGuide& g : on.guides()) ids.push_back(g.id);
                }
            }
            return ids;
        };
        if (db.isFlagSet(kPreviewTubeShape[0])) {
            // A library shape's new points shown on the groom (the stored guides, the shape
            // replaced): no change, no undo step; editTubeShape commits, endPreview drops it.
            auto* shape = dynamic_cast<GroomShape*>(groomFn.userNode());
            if (!shape) return fail(kName, "not a groom");
            const HierarchyPtr authored = shape->authoredHierarchy();
            if (!authored) return fail(kName, "the groom has no guides");
            try {
                const nlohmann::json op = shapeOpJson(flagString(db, kPreviewTubeShape[0], ""));
                auto preview = std::make_shared<GuideHierarchy>(*authored);
                preview->setTubeShape(op.value("name", std::string()), shapePoints(op.value("points", nlohmann::json::array())),
                                      op.value("smooth", true));
                preview->evaluate(joinedScalpFromNode(groomPath.node()).get());
                shape->setPreviewHierarchy(preview);
            } catch (const std::exception& e) {
                return fail(kName, e.what());
            }
            return MS::kSuccess;
        }
        if (db.isFlagSet(kEndPreview[0]) || db.isFlagSet(kPreviewResample[0]) || db.isFlagSet(kPointCounts[0])) {
            auto* shape = dynamic_cast<GroomShape*>(groomFn.userNode());
            if (!shape) return fail(kName, "not a groom");
            if (db.isFlagSet(kEndPreview[0])) {
                shape->setPreviewHierarchy(nullptr);
                return MS::kSuccess;
            }
            if (db.isFlagSet(kPointCounts[0])) {
                std::string error;
                const std::shared_ptr<const GuideHierarchy> stored = shape->storedHierarchy(&error);
                if (!stored) return fail(kName, error.c_str());
                MIntArray counts;
                for (const GuideId id : resampleTargets(*stored)) {
                    if (const HierarchyGuide* g = stored->guide(id)) {
                        counts.append(std::max(static_cast<int>(g->points.size()) - 1, 0));
                    }
                }
                setResult(counts);
                return MS::kSuccess;
            }
            int segments = 0;
            db.getFlagArgument(kPreviewResample[0], 0, segments);
            if (segments < 1 || segments > 256) return fail(kName, "-previewResample takes 1 to 256 segments");
            const HierarchyPtr authored = shape->authoredHierarchy();  // as stored, rooted: never a preview
            if (!authored) return fail(kName, "the groom has no guides");
            auto preview = std::make_shared<GuideHierarchy>(*authored);
            int count = 0;
            for (const GuideId id : resampleTargets(*preview)) {
                const HierarchyGuide* g = preview->guide(id);
                if (!g || g->points.size() < 2) continue;
                preview->setPoints(id, resampleSmoothCurve(g->points.data(), g->points.size(),
                                                           static_cast<std::size_t>(segments) + 1));
                ++count;
            }
            shape->setPreviewHierarchy(preview);
            setResult(count);
            return MS::kSuccess;
        }

        // Current hierarchy, rooted on the groom's scalp.
        MPlug dataPlug = groomFn.findPlug("guideData", true);
        const MeshPtr scalp = joinedScalpFromNode(groomPath.node());  // with the groom's other scalps
        std::string error;
        std::shared_ptr<GuideHierarchy> h = toHierarchy(dataPlug.asString().asChar(), scalp.get(), &error, groomScaleOf(groomPath.node()));
        if (!h) return fail(kName, error.c_str());
    
        // Targets: -guide, else -group, else -level, else the selection.
        const std::vector<GuideId> selectedIds = readGuideSelection(groomPath.node());
        auto targets = [&]() -> std::vector<GuideId> {
            if (db.isFlagSet(kGuide[0])) return parseIds(multiStrings(db, kGuide[0]));
            if (db.isFlagSet(kGroup[0])) return guidesAt(*h, -1, flagString(db, kGroup[0], "").asChar());
            if (db.isFlagSet(kLevel[0])) {
                int level = 0;
                db.getFlagArgument(kLevel[0], 0, level);
                return guidesAt(*h, level);
            }
            return selectedIds;
        };
    
        // Selection edits change only guideSelection.
        const bool selecting = db.isFlagSet(kSelect[0]) || db.isFlagSet(kMirrorSelection[0]) || db.isFlagSet(kClearSelection[0]);
        if (db.isFlagSet(kListSelected[0])) {
            MStringArray ids;
            for (const GuideId id : selectedIds) ids.append(std::to_string(id).c_str());
            setResult(ids);
            return MS::kSuccess;
        }
        if (selecting) {
            std::vector<GuideId> result;
            std::vector<GuideId> picked;
            if (db.isFlagSet(kMirrorSelection[0])) {
                const int axis = axisIndex(flagString(db, kMirrorSelection[0], "x"));
                if (axis < 0) return fail(kName, "-mirrorSelection takes x, y or z");
                const float center = static_cast<float>(flagDouble(db, kCenter[0], 0.0));
                picked = mirrorSelection(*h, targets(), axis, center, 1e30f);
            } else if (db.isFlagSet(kSelect[0])) {
                picked = targets();
            }
            for (const GuideId id : picked) {
                if (!h->guide(id)) return fail(kName, MString("no guide ") + std::to_string(id).c_str());
            }
            if (!db.isFlagSet(kClearSelection[0])) {
                const SelectMode mode = db.isFlagSet(kAdd[0])        ? SelectMode::Add
                                        : db.isFlagSet(kDeselect[0]) ? SelectMode::Deselect
                                        : db.isFlagSet(kToggle[0])   ? SelectMode::Toggle
                                                                     : SelectMode::Replace;
                result = combineSelection(selectedIds, picked, mode);
            }
            MObject data = selectionData(result);
            modifier_.newPlugValue(groomFn.findPlug("guideSelection", true), data);
            // The Select tool's solo: every guide not selected frozen (none selected: all unfrozen),
            // in the same undo step as the selection.
            if (db.isFlagSet(kFreezeOthers[0]) && freezeToSelection(*h, result)) {
                h->evaluate(scalp.get());
                modifier_.newPlugValueString(dataPlug, MString(h->toJson().c_str()));
            }
            MStringArray ids;
            for (const GuideId id : result) ids.append(std::to_string(id).c_str());
            setResult(ids);
            undoable_ = true;
            return redoIt();
        }
    
        // Queries.
        if (db.isFlagSet(kListAttributes[0])) {
            MStringArray names;
            for (const std::string& n : h->attributeNames()) names.append(n.c_str());
            setResult(names);
            return MS::kSuccess;
        }
        if (db.isFlagSet(kListGroups[0])) {  // a groom node that is not loaded
            MStringArray rows;
            describeGroups(*h, guideColorSeedOf(groomFn.object()), rows);
            setResult(rows);
            return MS::kSuccess;
        }
        if (db.isFlagSet(kListGuides[0])) {
            const MString groupName = flagString(db, kListGuides[0], "");
            const GroupId gid = h->findGroup(groupName.asChar());
            if (gid == kNoGroup) return fail(kName, "no guide group named " + groupName);
            MStringArray ids;
            for (const GuideId id : h->guidesInGroup(gid)) ids.append(std::to_string(id).c_str());
            setResult(ids);
            return MS::kSuccess;
        }
        if (db.isFlagSet(kTubeShapes[0])) {
            setResult(MString(tubeShapesJson(*h).c_str()));
            return MS::kSuccess;
        }
        if (db.isFlagSet(kTubeTargets[0])) {
            std::vector<GuideId> ids;
            std::string source = "none";
            for (const GuideId id : selectedIds) {
                if (h->guide(id)) ids.push_back(id);
            }
            if (!ids.empty()) {
                source = "selection";
            } else {
                // Soloed: some guides frozen, and these not, nor their parents (the solo's own).
                auto frozen = [&](const HierarchyGuide& g) {
                    float value = 0.0f;
                    return h->effectiveAttribute(g, "freeze", &value) && value >= 0.5f;
                };
                bool any = false;
                for (const HierarchyGuide& g : h->guides()) any = any || frozen(g);
                if (any) {
                    for (const HierarchyGuide& g : h->guides()) {
                        if (frozen(g)) continue;
                        const HierarchyGuide* parent = g.isChild() ? h->guide(g.parent) : nullptr;
                        if (!parent || frozen(*parent)) ids.push_back(g.id);
                    }
                    if (!ids.empty()) source = "solo";
                }
                // A groom of one guide: that one (soloing it freezes nothing, so it shows as no solo).
                if (ids.empty() && h->guides().size() == 1) {
                    ids.push_back(h->guides().front().id);
                    source = "solo";
                }
            }
            nlohmann::json out = {{"source", source}, {"guides", nlohmann::json::array()}, {"shapes", nlohmann::json::array()}};
            std::vector<std::string> shapes;
            for (const GuideId id : ids) {
                out["guides"].push_back(std::to_string(id));
                const std::string name = h->guide(id)->tube.shapeName();
                if (std::find(shapes.begin(), shapes.end(), name) == shapes.end()) shapes.push_back(name);
            }
            out["shapes"] = shapes;
            out["tube"] = ids.empty() ? nlohmann::json() : nlohmann::json::parse(tubeToJson(h->guide(ids.front())->tube, h->scale()));
            setResult(MString(out.dump().c_str()));
            return MS::kSuccess;
        }
        if (db.isFlagSet(kTube[0])) {
            // The tube at the size shown (GuideHierarchy::scale): {"shape", "width", "aspect", "twist"}.
            const MString idText = flagString(db, kTube[0], "");
            const HierarchyGuide* g = h->guide(std::strtoull(idText.asChar(), nullptr, 10));
            if (!g) return fail(kName, "no guide " + idText);
            setResult(MString(tubeToJson(g->tube, h->scale()).c_str()));
            return MS::kSuccess;
        }
        if (db.isFlagSet(kGuidePoints[0])) {
            const MString idText = flagString(db, kGuidePoints[0], "");
            HierarchyPtr shown = h;
            if (db.isFlagSet(kEvaluated[0])) {
                auto* shape = dynamic_cast<GroomShape*>(groomFn.userNode());
                if (shape && shape->evaluatedHierarchy()) shown = shape->evaluatedHierarchy();
            }
            const HierarchyGuide* g = shown->guide(std::strtoull(idText.asChar(), nullptr, 10));
            if (!g) return fail(kName, "no guide " + idText);
            MDoubleArray xyz;
            for (const Vec3f& p : g->points) {
                xyz.append(p.x);
                xyz.append(p.y);
                xyz.append(p.z);
            }
            setResult(xyz);
            return MS::kSuccess;
        }
    
        // The root group new primaries go into (-group, default "primary"), made when missing.
        auto rootGroup = [&](GroupId* gid) -> MStatus {
            const MString groupName = flagString(db, kGroup[0], "primary");
            *gid = h->findGroup(groupName.asChar());
            if (*gid == kNoGroup) {
                *gid = h->addGroup(groupName.asChar());
            } else if (h->groupDepth(*gid) != 0) {
                return fail(kName, groupName + " is not a root group");
            }
            return MS::kSuccess;
        };
        auto tubeFlags = [&]() {
            const double rootRadius = flagDouble(db, kTubeRadius[0], 0.1);
            TubeProfile tube;
            tube.width = {static_cast<float>(rootRadius), static_cast<float>(flagDouble(db, kTubeTipRadius[0], rootRadius))};
            return tube;
        };

        // Edits.
        try {
            if (db.isFlagSet(kFromCurves[0])) {
                GroupId gid = kNoGroup;
                if (!rootGroup(&gid)) return MS::kFailure;
                const TubeProfile tube = tubeFlags();

                MStringArray added;
                for (const MString& curve : multiStrings(db, kFromCurves[0])) {
                    MObject data;
                    if (!curveWorldData(curve, data)) return fail(kName, curve + " is not a nurbsCurve");
                    std::vector<Vec3f> pts = sampleCurveData(data);
                    if (pts.size() < 2) return fail(kName, curve + " is too short");
                    added.append(std::to_string(h->addGuide(gid, std::move(pts), tube)).c_str());
                }
                setResult(added);
            } else if (db.isFlagSet(kFromNode[0])) {
                // Bake a node's output to guides: evaluated as the groom evaluates it (its
                // inputs as they are now), then added as primaries the artist can brush.
                auto* shape = dynamic_cast<GroomShape*>(groomFn.userNode());
                if (!shape) return fail(kName, "the groom node is not loaded");
                const MString nodeName = flagString(db, kFromNode[0], "");
                if (nodeName.length() == 0) return fail(kName, "-fromNode needs a graph node name");
                GroupId gid = kNoGroup;
                if (!rootGroup(&gid)) return MS::kFailure;
                const Value value = shape->evaluateNode(nodeName.asChar(), static_cast<float>(flagDouble(db, kDensity[0], 1.0)));
                GuideSet baked;
                if (const GuidesPtr* guides = std::get_if<GuidesPtr>(&value); guides && *guides) {
                    baked = **guides;
                } else if (const StrandsPtr* strands = std::get_if<StrandsPtr>(&value); strands && *strands) {
                    for (std::size_t i = 0; i < (*strands)->strandCount(); ++i) {  // one guide per strand
                        const Vec3f* p = (*strands)->strandPoints(i);
                        baked.add((*strands)->ids()[i], std::vector<Vec3f>(p, p + (*strands)->strandPointCount(i)));
                    }
                } else {
                    return fail(kName, "node '" + nodeName + "' does not output guides or strands (nothing to bake)");
                }
                if (baked.empty()) return fail(kName, "node '" + nodeName + "' made no guides");
                MStringArray added;
                for (const GuideId id : h->addGuides(gid, baked, tubeFlags())) added.append(std::to_string(id).c_str());
                setResult(added);
            } else if (db.isFlagSet(kGenerate[0])) {
                // Parents: -guide ids, or the selection with -onlySelected, else the whole group.
                std::vector<GuideId> only;
                const bool restricted = db.isFlagSet(kGuide[0]) || db.isFlagSet(kOnlySelected[0]);
                if (db.isFlagSet(kGuide[0])) {
                    only = parseIds(multiStrings(db, kGuide[0]));
                } else if (db.isFlagSet(kOnlySelected[0])) {
                    only = selectedIds;
                }
                if (restricted && only.empty()) return fail(kName, "select the guides that should get children");
                MString parentName = flagString(db, kGenerate[0], "");
                if (parentName.length() == 0 && restricted) {
                    const HierarchyGuide* first = h->guide(only.front());
                    if (!first) return fail(kName, MString("no guide ") + std::to_string(only.front()).c_str());
                    parentName = h->group(first->group)->name.c_str();
                }
                const GroupId parent = h->findGroup(parentName.asChar());
                if (parent == kNoGroup) return fail(kName, "no guide group named " + parentName);
                for (const GuideId id : only) {
                    const HierarchyGuide* g = h->guide(id);
                    if (!g || g->group != parent) {
                        return fail(kName, "the chosen parents must all be in one group (" + parentName + ")");
                    }
                }
    
                ChildGenerationParams p;
                int count = static_cast<int>(p.count);
                if (db.isFlagSet(kCount[0])) db.getFlagArgument(kCount[0], 0, count);
                if (count < 1 || count > 1000) return fail(kName, "-count must be between 1 and 1000");
                p.count = static_cast<std::size_t>(count);
                p.rootSpread = static_cast<float>(flagDouble(db, kRootSpread[0], p.rootSpread));
                p.tipSpread = static_cast<float>(flagDouble(db, kTipSpread[0], p.tipSpread));
                p.twist = static_cast<float>(flagDouble(db, kTwist[0], p.twist));
                p.jitter = static_cast<float>(flagDouble(db, kJitter[0], p.jitter));
                p.childTubeScale = static_cast<float>(flagDouble(db, kTubeScale[0], p.childTubeScale));
                int seed = 0;
                if (db.isFlagSet(kSeed[0])) db.getFlagArgument(kSeed[0], 0, seed);
                p.seed = static_cast<std::uint64_t>(seed);
    
                const MString childName = flagString(db, kNameFlag[0], parentName + "_children");
                const GroupId child = h->generateChildren(parent, childName.asChar(), p, restricted ? &only : nullptr);
                setResult(static_cast<int>(h->guidesInGroup(child).size()));
            } else if (db.isFlagSet(kRemoveGroup[0])) {
                const MString groupName = flagString(db, kRemoveGroup[0], "");
                const GroupId gid = h->findGroup(groupName.asChar());
                if (gid == kNoGroup) return fail(kName, "no guide group named " + groupName);
                h->removeGroup(gid);
            } else if (db.isFlagSet(kAddGroup[0])) {
                // An empty group: a root group, or with -under a child group of that one.
                const MString name = flagString(db, kAddGroup[0], "");
                if (name.length() == 0) return fail(kName, "a guide group needs a name");
                if (h->findGroup(name.asChar()) != kNoGroup) return fail(kName, "a guide group named " + name + " exists");
                GroupId parent = kNoGroup;
                if (db.isFlagSet(kUnder[0])) {
                    const MString under = flagString(db, kUnder[0], "");
                    parent = h->findGroup(under.asChar());
                    if (parent == kNoGroup) return fail(kName, "no guide group named " + under);
                }
                h->addGroup(name.asChar(), parent);
            } else if (db.isFlagSet(kRenameGroup[0])) {
                MString from, to;
                db.getFlagArgument(kRenameGroup[0], 0, from);
                db.getFlagArgument(kRenameGroup[0], 1, to);
                const GroupId gid = h->findGroup(from.asChar());
                if (gid == kNoGroup) return fail(kName, "no guide group named " + from);
                try {
                    h->renameGroup(gid, to.asChar());
                } catch (const std::invalid_argument& e) {
                    return fail(kName, e.what());
                }
            } else if (db.isFlagSet(kLock[0]) || db.isFlagSet(kUnlock[0])) {
                const bool lock = db.isFlagSet(kLock[0]);
                for (const MString& idText : multiStrings(db, kGuide[0])) {
                    const GuideId id = std::strtoull(idText.asChar(), nullptr, 10);
                    const HierarchyGuide* g = h->guide(id);
                    if (!g) return fail(kName, "no guide " + idText);
                    if (!g->isChild()) return fail(kName, "guide " + idText + " is a primary guide; only children lock");
                    lock ? h->lock(id) : h->unlock(id);
                }
            } else if (db.isFlagSet(kClear[0])) {
                *h = GuideHierarchy{};
            } else if (db.isFlagSet(kReparent[0])) {
                const GuideId parent = std::strtoull(flagString(db, kReparent[0], "").asChar(), nullptr, 10);
                const std::vector<GuideId> ids = targets();
                if (ids.empty()) return fail(kName, "-reparent: give -guide IDs or select guides");
                h->reparent(ids, parent);
                setResult(static_cast<int>(ids.size()));
            } else if (db.isFlagSet(kPromote[0])) {
                const MString name = flagString(db, kPromote[0], "");
                const GroupId gid = h->findGroup(name.asChar());
                if (gid == kNoGroup) return fail(kName, "no guide group named " + name);
                h->promoteGroup(gid);
            } else if (db.isFlagSet(kDemote[0])) {
                const MString name = flagString(db, kDemote[0], "");
                const MString under = flagString(db, kUnder[0], "");
                const GroupId gid = h->findGroup(name.asChar());
                const GroupId parent = h->findGroup(under.asChar());
                if (gid == kNoGroup) return fail(kName, "no guide group named " + name);
                if (parent == kNoGroup) return fail(kName, "-demote needs -under <group>; no group named " + under);
                h->demoteGroup(gid, parent);
            } else if (db.isFlagSet(kMirror[0])) {
                // A root group's name, or "*": symmetry sync of the whole groom, children included.
                const MString name = flagString(db, kMirror[0], "");
                const GroupId gid = name == "*" ? kNoGroup : h->findGroup(name.asChar());
                if (name != "*" && gid == kNoGroup) return fail(kName, "no guide group named " + name);
                const int axis = axisIndex(flagString(db, kAxis[0], "x"));
                if (axis < 0) return fail(kName, "-axis takes x, y or z");
                const float center = static_cast<float>(flagDouble(db, kCenter[0], 0.0));
                const bool fromPositive = !db.isFlagSet(kFromNegative[0]);
                const std::vector<GuideId> created = name == "*" ? h->mirrorAll(axis, center, fromPositive)
                                                                 : h->mirrorGroup(gid, axis, center, fromPositive);
                MStringArray ids;
                for (const GuideId id : created) ids.append(std::to_string(id).c_str());
                setResult(ids);
            } else if (db.isFlagSet(kRemoveAttribute[0])) {
                const MString name = flagString(db, kRemoveAttribute[0], "");
                const auto names = h->attributeNames();
                if (std::find(names.begin(), names.end(), std::string(name.asChar())) == names.end()) {
                    return fail(kName, "no guide attribute named " + name);
                }
                h->removeAttribute(name.asChar());
            } else if (db.isFlagSet(kResample[0])) {
                int segments = 0;
                db.getFlagArgument(kResample[0], 0, segments);
                if (segments < 1 || segments > 256) return fail(kName, "-resample takes 1 to 256 segments");
                const std::vector<GuideId> ids = resampleTargets(*h);
                if (ids.empty()) return fail(kName, "the groom has no guides to resample");
                for (const GuideId id : ids) {
                    const HierarchyGuide* g = h->guide(id);
                    if (!g) return fail(kName, MString("no guide ") + std::to_string(id).c_str());
                    if (g->points.size() < 2) continue;
                    // Along the smooth curve through its points, as the viewport draws it.
                    h->setPoints(id, resampleSmoothCurve(g->points.data(), g->points.size(),
                                                         static_cast<std::size_t>(segments) + 1));
                }
                setResult(static_cast<int>(ids.size()));
            } else if (db.isFlagSet(kReshape[0])) {
                // Guides take the shapes of curves (Curves to Guides: edited copies coming back).
                // Only guides whose curve differs from their shape change; parents first, so an
                // edited child keeps its new shape under its edited parent and an untouched child
                // follows its parent.
                struct Reshape {
                    GuideId id;
                    std::vector<Vec3f> points;
                    int depth;
                };
                std::vector<Reshape> reshapes;
                for (unsigned i = 0; i < db.numberOfFlagUses(kReshape[0]); ++i) {
                    MArgList a;
                    db.getFlagArgumentList(kReshape[0], i, a);
                    const MString idText = a.asString(0), curve = a.asString(1);
                    const GuideId id = std::strtoull(idText.asChar(), nullptr, 10);
                    const HierarchyGuide* g = h->guide(id);
                    if (!g) return fail(kName, "no guide " + idText);
                    MObject data;
                    if (!curveWorldData(curve, data)) return fail(kName, curve + " is not a nurbsCurve");
                    std::vector<Vec3f> pts = curveEditPoints(data);
                    if (pts.size() < 2) return fail(kName, curve + " is too short");
                    if (samePoints(pts, g->points)) continue;
                    reshapes.push_back({id, std::move(pts), h->groupDepth(g->group)});
                }
                std::stable_sort(reshapes.begin(), reshapes.end(),
                                 [](const Reshape& a, const Reshape& b) { return a.depth < b.depth; });
                MStringArray changed;
                for (Reshape& r : reshapes) {
                    h->setPoints(r.id, std::move(r.points));
                    changed.append(std::to_string(r.id).c_str());
                }
                setResult(changed);
                if (reshapes.empty()) return MS::kSuccess;  // nothing changed: no undo step
            } else if (db.isFlagSet(kRenameAttribute[0])) {
                MString from, to;
                db.getFlagArgument(kRenameAttribute[0], 0, from);
                db.getFlagArgument(kRenameAttribute[0], 1, to);
                const auto names = h->attributeNames();
                if (std::find(names.begin(), names.end(), std::string(from.asChar())) == names.end()) {
                    return fail(kName, "no guide attribute named " + from);
                }
                if (to.length() == 0) return fail(kName, "-renameAttribute needs a new name");
                h->renameAttribute(from.asChar(), to.asChar());
            } else if (db.isFlagSet(kSetTube[0])) {
                // Each target's tube edited by the JSON (editedTube: the keys given replace theirs;
                // widths at the size shown).
                const std::string edit = flagString(db, kSetTube[0], "").asChar();
                const std::vector<GuideId> ids = targets();
                if (ids.empty()) return fail(kName, "-setTube: no guides (give -guide, -group or -level, or select some)");
                for (const GuideId id : ids) {
                    const HierarchyGuide* g = h->guide(id);
                    if (!g) return fail(kName, MString("no guide ") + std::to_string(id).c_str());
                    h->setTube(id, editedTube(g->tube, edit, h->scale(), g->points.size(), &h->tubeShapes()));
                }
                setResult(static_cast<int>(ids.size()));
            } else if (db.isFlagSet(kEditTubeShape[0])) {
                // {"op": "add" | "set" | "rename" | "remove", "name", "points", "smooth", "to"}: the
                // library edited, every guide taking the shape following (remove: round).
                const nlohmann::json op = shapeOpJson(flagString(db, kEditTubeShape[0], ""));
                const std::string kind = op.value("op", std::string());
                const std::string name = op.value("name", std::string());
                const bool exists = h->tubeShape(name) != nullptr;
                if (kind == "add" || kind == "set") {
                    if (kind == "add" && exists) return fail(kName, ("a tube shape named '" + name + "' exists").c_str());
                    if (kind == "set" && !exists) return fail(kName, ("no tube shape named '" + name + "'").c_str());
                    const CustomTubeShapePtr old = h->tubeShape(name);
                    const bool smooth = op.contains("smooth") ? op["smooth"].get<bool>() : (old ? old->smooth : true);
                    const std::vector<SectionPoint> points =
                        op.contains("points") ? shapePoints(op["points"]) : (old ? old->points : std::vector<SectionPoint>{});
                    h->setTubeShape(name, points, smooth);
                    setResult(static_cast<int>(h->tubeShapeUsers(name)));
                } else if (kind == "rename") {
                    h->renameTubeShape(name, op.value("to", std::string()));
                    setResult(static_cast<int>(h->tubeShapeUsers(op.value("to", std::string()))));
                } else if (kind == "remove") {
                    setResult(static_cast<int>(h->removeTubeShape(name)));
                } else {
                    return fail(kName, "-editTubeShape: op is add, set, rename or remove");
                }
                auto* shape = dynamic_cast<GroomShape*>(groomFn.userNode());
                if (shape) shape->setPreviewHierarchy(nullptr);  // a live preview of the shape ends with its commit
            } else if (db.isFlagSet(kSetAttribute[0])) {
                const std::string name = flagString(db, kSetAttribute[0], "").asChar();
                if (name.empty() || !db.isFlagSet(kValue[0])) return fail(kName, "-setAttribute needs a name and -value");
                const float value = static_cast<float>(flagDouble(db, kValue[0], 0.0));
                const std::vector<GuideId> ids = targets();
                for (const GuideId id : ids) {
                    if (!h->guide(id)) return fail(kName, MString("no guide ") + std::to_string(id).c_str());
                    h->setAttribute(id, name, value);
                }
                setResult(static_cast<int>(ids.size()));
            }
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }
    
        h->evaluate(scalp.get());
        const MString json = h->guides().empty() && h->groups().empty() ? MString("") : MString(h->toJson().c_str());
        modifier_.newPlugValueString(dataPlug, json);
        undoable_ = true;
        return redoIt();
    });
}

namespace {

// The viewports draw the change now (a guide selection's highlight, a group's color), not at the
// next camera move: testers saw selection colors wait for one.
MStatus redrawn(MStatus status) {
    if (status && MGlobal::mayaState() == MGlobal::kInteractive) M3dView::scheduleRefreshAllViews();
    return status;
}

}  // namespace

MStatus GuidesCmd::redoIt() {
    return groomlab::maya::guarded("GuidesCmd::redoIt", [&]() -> MStatus { return redrawn(modifier_.doIt());
    });
}

MStatus GuidesCmd::undoIt() {
    return groomlab::maya::guarded("GuidesCmd::undoIt", [&]() -> MStatus { return redrawn(modifier_.undoIt());
    });
}

}  // namespace groomlab::maya
