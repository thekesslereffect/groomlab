// SPDX-License-Identifier: Apache-2.0
#include "export_cmds.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include <maya/MAnimControl.h>
#include <maya/MArgDatabase.h>
#include <maya/MArgList.h>
#include <maya/MDGContext.h>
#include <maya/MDGContextGuard.h>
#include <maya/MDagModifier.h>
#include <maya/MDagPath.h>
#include <maya/MFileIO.h>
#include <maya/MFloatArray.h>
#include <maya/MFloatPointArray.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnMesh.h>
#include <maya/MFnTransform.h>
#include <maya/MTransformationMatrix.h>
#include <maya/MIntArray.h>
#include <maya/MStringArray.h>
#include <maya/MVectorArray.h>
#include <maya/MColorArray.h>
#include <maya/MComputation.h>
#include <maya/MGlobal.h>
#include <maya/MSelectionList.h>
#include <maya/MTime.h>

#include <nlohmann/json.hpp>

#include "asset_files.h"
#include "command_args.h"
#include "guarded.h"
#include "groom_shape.h"
#include "groomlab/card_files.h"
#include "groomlab/display.h"
#include "groomlab/export_view.h"
#include "groomlab/feathers.h"
#include "groomlab/gltf_writer.h"
#include "groomlab/asset_files.h"
#include "groomlab/groom_file.h"
#include "groomlab/groom_package.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/render_overrides.h"
#include "groomlab/follicle_map.h"
#include "groomlab/scale_maps.h"
#include "groomlab/scales.h"
#include "groomlab/usd_writer.h"
#include "groomlab/vrscene_writer.h"
#include "hair_shading.h"
#include "maya_inputs.h"

#if GROOMLAB_HAS_ALEMBIC
#include "groomlab/alembic_io.h"
#include "groomlab/engine_export.h"
#endif

namespace groomlab::maya {

namespace {

namespace fs = std::filesystem;

const char* kFileFlag = "-f";
const char* kFileFlagLong = "-file";
const char* kStartFlag = "-sf";
const char* kStartFlagLong = "-startFrame";
const char* kEndFlag = "-ef";
const char* kEndFlagLong = "-endFrame";
const char* kStepFlag = "-s";
const char* kStepFlagLong = "-step";
const char* kDensityFlag = "-d";
const char* kDensityFlagLong = "-density";
const char* kScalpCacheFlag = "-sc";
const char* kScalpCacheFlagLong = "-scalpCache";
const char* kScalpObjectFlag = "-so";
const char* kScalpObjectFlagLong = "-scalpObject";
const char* kGuideCacheFlag = "-gc";
const char* kGuideCacheFlagLong = "-guideCache";
const char* kGuideObjectFlag = "-go";
const char* kGuideObjectFlagLong = "-guideObject";
const char* kGuideGroupFlag = "-gg";
const char* kGuideGroupFlagLong = "-guideGroup";
const char* kGuidePosesFlag = "-gps";
const char* kGuidePosesFlagLong = "-guidePoses";
const char* kDriverCurvesFlag = "-drv";
const char* kDriverCurvesFlagLong = "-driverCurves";
const char* kDriverCacheFlag = "-dc";
const char* kDriverCacheFlagLong = "-driverCache";
const char* kDriverObjectFlag = "-do";
const char* kDriverObjectFlagLong = "-driverObject";
const char* kRenderOverridesFlag = "-ro";
const char* kRenderOverridesFlagLong = "-renderOverrides";

// Object names in Alembic and USD: the groom's transform, namespaces flattened.
std::string exportName(const MDagPath& shapePath) {
    std::string name = MFnDagNode(shapePath.transform()).name().asChar();
    for (char& c : name) {
        if (c == ':' || c == '/' || c == '|') c = '_';
    }
    return name;
}

MSyntax curveSyntax() {
    MSyntax syntax;
    syntax.addFlag(kFileFlag, kFileFlagLong, MSyntax::kString);
    syntax.addFlag(kStartFlag, kStartFlagLong, MSyntax::kDouble);
    syntax.addFlag(kEndFlag, kEndFlagLong, MSyntax::kDouble);
    syntax.addFlag(kStepFlag, kStepFlagLong, MSyntax::kDouble);
    syntax.addFlag(kDensityFlag, kDensityFlagLong, MSyntax::kDouble);
    syntax.addFlag(kRenderOverridesFlag, kRenderOverridesFlagLong, MSyntax::kBoolean);
    syntax.setObjectType(MSyntax::kSelectionList, 1, 1);
    syntax.useSelectionAsDefault(true);
    syntax.enableQuery(false);
    syntax.enableEdit(false);
    return syntax;
}

struct FrameRange {
    double start = 0.0;
    double end = 0.0;
    double step = 1.0;
    double density = 1.0;
    // The groom's render overrides too (render_overrides.h: its renderDensity times the density,
    // its width and length scales), as a render has them.
    bool renderOverrides = false;
    // A game engine's export (-unreal, -unity): the Output's rootDepth left off (groom_session.h ViewRequest::gameExport).
    bool gameExport = false;
    unsigned count() const { return static_cast<unsigned>(std::floor((end - start) / step + 1e-6)) + 1; }
};

MStatus frameRange(const char* command, const MArgDatabase& db, FrameRange* r) {
    const double current = MAnimControl::currentTime().as(MTime::uiUnit());
    r->start = r->end = current;
    if (db.isFlagSet(kStartFlag)) db.getFlagArgument(kStartFlag, 0, r->start);
    if (db.isFlagSet(kEndFlag)) db.getFlagArgument(kEndFlag, 0, r->end);
    if (db.isFlagSet(kStepFlag)) db.getFlagArgument(kStepFlag, 0, r->step);
    if (db.isFlagSet(kDensityFlag)) db.getFlagArgument(kDensityFlag, 0, r->density);
    if (db.isFlagSet(kRenderOverridesFlag)) db.getFlagArgument(kRenderOverridesFlag, 0, r->renderOverrides);
    if (r->step <= 0.0) return fail(command, "-step must be positive");
    if (r->end < r->start) return fail(command, "-endFrame is before -startFrame");
    if (r->density < 0.0 || r->density > 1.0) return fail(command, "-density must be between 0 and 1");
    return MS::kSuccess;
}

// Maya's main progress bar over a frame loop, with the wait cursor; Esc cancels after the frame being
// written (the command then fails with "cancelled", the caller removing the partial file). One
// frame: no bar. Ends with the scope.
class FrameProgress {
public:
    FrameProgress(const char* command, unsigned frames) : command_(command), frames_(frames) {
        if (frames_ < 2) return;
        computation_.beginComputation(true, true, true);
        computation_.setProgressRange(0, static_cast<int>(frames_));
        status(0);
        active_ = true;
    }
    ~FrameProgress() {
        if (active_) computation_.endComputation();
    }
    FrameProgress(const FrameProgress&) = delete;
    FrameProgress& operator=(const FrameProgress&) = delete;

    // Frame `done` (0-based count of frames written) reached; false when Esc asked to stop.
    bool step(unsigned done) {
        if (!active_) return true;
        computation_.setProgress(static_cast<int>(done));
        status(done);
        return !computation_.isInterruptRequested();
    }
    MStatus cancelled(unsigned done) const {
        return fail(command_, MString("cancelled (Esc) after ") + done + " of " + frames_ + " frames");
    }

private:
    void status(unsigned done) { computation_.setProgressStatus(MString("GroomLab: writing frame ") + (done + 1) + " of " + frames_ + " (Esc cancels)"); }

    const char* command_;
    unsigned frames_;
    bool active_ = false;
    MComputation computation_;
};

// The context an export reads `frame` in: the normal one at the current frame. Maya keeps what a
// timed context computed for a frame, so a rig posed again without keys (or any edit upstream of
// the scalp) read through MDGContext(current frame) gives the scalp as it was before the edit;
// the normal context follows every edit.
MDGContext exportContext(double frame, MTime::Unit unit) {
    const MTime time(frame, unit);
    return time == MAnimControl::currentTime() ? MDGContext(MDGContext::fsNormal) : MDGContext(time);
}

// Evaluates the groom at each frame of the range and hands the strands (and
// the scalp they grew on) to `write`. Returns the total strand count. Maya's progress bar counts
// the frames; Esc stops after the current one (the command fails with "cancelled").
MStatus forEachFrame(const char* command, const MDagPath& path, const FrameRange& range,
                     const std::function<void(double frame, const StrandSet&, const MeshPtr&, NodeEvaluator&)>& write,
                     std::size_t* strandTotal, bool fromView = false) {
    const MTime::Unit unit = MTime::uiUnit();
    *strandTotal = 0;
    NodeEvaluator evaluator(path.node(), range.count() > 1);  // parsed once; the cache carries between frames
    RenderOverrides overrides;  // none unless asked for
    if (range.renderOverrides) {
        const MFnDependencyNode fn(path.node());
        overrides.density = fn.findPlug("renderDensity", true).asFloat();
        overrides.widthScale = fn.findPlug("renderWidthScale", true).asFloat();
        overrides.lengthScale = fn.findPlug("renderLengthScale", true).asFloat();
    }
    const float density = overrides.densityTimes(static_cast<float>(range.density));
    // One frame, the one the viewport shows, at its density (`fromView`: a writer reading nothing
    // of the evaluator): the viewport's evaluation, when it evaluated the groom as an export does
    // (GroomSession::viewedAsRendered), instead of a second one of the whole groom.
    auto* shape = fromView && range.count() == 1 ? dynamic_cast<GroomShape*>(MFnDagNode(path).userNode()) : nullptr;
    if (shape && range.start == MAnimControl::currentTime().as(unit)) {
        shape->ensureEvaluated();
        const GroomSession& session = shape->session();
        if (const StrandsPtr viewed = session.viewedAsRendered(density, mayaSeconds(MTime(range.start, unit)))) {
            const StrandsPtr strands = applyRenderOverrides(viewed, overrides);
            write(range.start, *strands, session.last().scalp, evaluator);
            *strandTotal = strands->strandCount();
            return MS::kSuccess;
        }
    }
    FrameProgress progress(command, range.count());
    for (unsigned i = 0; i < range.count(); ++i) {
        if (!progress.step(i)) return progress.cancelled(i);
        const double frame = range.start + i * range.step;
        MDGContext context = exportContext(frame, unit);
        MDGContextGuard guard(context);

        std::string error;
        MeshPtr scalp;
        const StrandsPtr strands = applyRenderOverrides(evaluator.evaluate(density, &error, &scalp, false, range.gameExport), overrides);
        if (!error.empty()) return fail(command, MString("frame ") + frame + ": " + error.c_str());
        write(frame, strands ? *strands : StrandSet{}, scalp, evaluator);
        *strandTotal += strands ? strands->strandCount() : 0;
    }
    return MS::kSuccess;
}

void reportWritten(const char* command, unsigned frames, std::size_t strandTotal, const MString& file) {
    MGlobal::displayInfo(MString(command) + ": wrote " + frames + " frame(s), " +
                         static_cast<unsigned>(strandTotal / frames) + " strands per frame on average, to " + file);
}

double framesPerSecond() { return mayaFramesPerSecond(); }  // NTSC rates exact (frame_time.h)

// The scales of an export, frame after frame: bent onto each frame's scalp with the scene's
// prototype meshes (read once each), a frame's scales bending only what moved since the last.
struct SceneScales {
    ScaleMeshBuilder builder;
    std::map<std::string, InstancePrototype> scene;
    PrototypeLookup lookup() {
        return [this](const std::string& name) -> const InstancePrototype* {
            auto it = scene.find(name);
            if (it == scene.end()) {
                it = scene.emplace(name, InstancePrototype{}).first;
                readInstancePrototype(name, it->second);
            }
            return it->second.found ? &it->second : nullptr;
        };
    }
    // Null without scales.
    std::shared_ptr<const ScaleMesh> build(const StrandSet& strands, const MeshPtr& scalp) {
        if (!strands.instances() || !strands.instances()->hasMeshes()) return nullptr;
        return builder.build(strands, scalp, lookup());
    }
};

// -scales: how the scales go into the file (mesh, instancer, both, none).
bool scalesMode(const MArgDatabase& db, UsdCurvesWriter::Options::Scales* out) {
    using S = UsdCurvesWriter::Options::Scales;
    const std::string mode = db.isFlagSet("-scl") ? flagString(db, "-scl", "mesh").asChar() : "mesh";
    if (mode == "mesh") *out = S::Mesh;
    else if (mode == "instancer") *out = S::Instancer;
    else if (mode == "both") *out = S::Both;
    else if (mode == "none") *out = S::None;
    else return false;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Alembic

#if GROOMLAB_HAS_ALEMBIC

const char* ExportAlembicCmd::kName = "groomLabExportAlembic";

void* ExportAlembicCmd::creator() { return new ExportAlembicCmd(); }

MSyntax ExportAlembicCmd::newSyntax() {
    MSyntax syntax = curveSyntax();
    syntax.addFlag(kGuideGroupFlag, kGuideGroupFlagLong, MSyntax::kString);
    syntax.addFlag(kGuidePosesFlag, kGuidePosesFlagLong);
    syntax.addFlag(kDriverCurvesFlag, kDriverCurvesFlagLong);  // the driver curves (DriveGuides), as the groom reads them
    syntax.addFlag("-ue", "-unreal");
    // Unreal only: groom groups ("": one object, groom_group_id from the groupId channel;
    // "auto": by groupId, an object each; else by that channel), the feathers' shafts as
    // strands (default: left out when the feathers have a rachis mesh), the shafts as the
    // feather groups' guides, and a lighter groom (barbs thinned, widened).
    syntax.addFlag("-grp", "-groups", MSyntax::kString);
    syntax.addFlag("-sh", "-shafts", MSyntax::kBoolean);
    syntax.addFlag("-fg", "-featherGuides", MSyntax::kBoolean);
    // FeatherColor's iridescence as groom_ao too (what Unreal's Hair Attributes node gives a material).
    syntax.addFlag("-iao", "-iridescenceAo", MSyntax::kBoolean);
    syntax.addFlag("-bd", "-barbDensity", MSyntax::kDouble);
    syntax.addFlag("-bws", "-barbWidthScale", MSyntax::kDouble);
    // For an engine's importer (Unity's hair): points and widths scaled, strands resampled to
    // a uniform point count, root UVs per point.
    syntax.addFlag("-sca", "-scale", MSyntax::kDouble);
    syntax.addFlag("-pps", "-pointsPerStrand", MSyntax::kLong);
    syntax.addFlag("-vuv", "-vertexUVs");
    // Unity's hair package: root UVs per point, root widths above 0, and with -groups a curves
    // object (a Unity strand group) per groom group, -barbPoints points on the feather groups'
    // strands, groups past -maxStrandsPerGroup split in parts; -shafts, -barbDensity and
    // -barbWidthScale as for Unreal. -report returns JSON of what was written instead of the frames.
    syntax.addFlag("-uty", "-unity");
    syntax.addFlag("-bpp", "-barbPoints", MSyntax::kLong);
    syntax.addFlag("-msg", "-maxStrandsPerGroup", MSyntax::kLong);
    syntax.addFlag("-rep", "-report");
    // The scales: their bent mesh beside the curves (mesh, the default) or none.
    syntax.addFlag("-scl", "-scales", MSyntax::kString);
    return syntax;
}

namespace {

StrandSet toCurves(const GuideSet& guides) {
    StrandSet out;
    for (const Guide& g : guides.guides()) {
        if (g.points.size() >= 2) out.addStrand(g.id, g.points, {}, g.root);
    }
    return out;
}

// What the guide export keeps from frame to frame: the inputs converted, and the groom (its
// hierarchy parsed once; each frame gives it the scalp and poses of its time).
struct GuideFrames {
    NodeInputCache inputs;
    std::unique_ptr<GroomSession> groom;
};

// The groom's guides at the context's time: a group of the posed hierarchy,
// or the poses alone (only they are read).
MStatus guidesAtFrame(const char* command, const MObject& node, const MString& group, bool poses, bool drivers,
                      GuideFrames& frames, StrandSet* out) {
    try {
        if (drivers) {  // the curves exactly as the groom samples them (driverCurvesFromNode): the farm drives the same
            const GuidesPtr curves = driverCurvesFromNode(node);
            *out = curves ? toCurves(*curves) : StrandSet{};
            return MS::kSuccess;
        }
        if (poses) {
            const GuidesPtr posed = guidePosesFromNode(node, &frames.inputs);
            *out = posed ? toCurves(*posed) : StrandSet{};
            return MS::kSuccess;
        }
        if (!frames.groom) {
            GroomData data = groomDataFromNode(node, &frames.inputs);
            data.correctivesJson.clear();  // posed guides feed sim caches: shot sculpts apply on top of those
            frames.groom = std::make_unique<GroomSession>(std::move(data));
        } else {  // another time: only the scalp and the poses change what the hierarchy is
            frames.groom->setScalp(scalpFromNode(node, &frames.inputs, nullptr, false));  // the maps as first read
            frames.groom->setGuidePoses(guidePosesFromNode(node, &frames.inputs));
        }
        // "*": every guide the Guides node feeds (what a GuideCache node poses), else one group's.
        *out = toCurves(guidesToSimulate(frames.groom->hierarchy(), frames.groom->data().guides, group.asChar()));
        return MS::kSuccess;
    } catch (const std::exception& e) {
        return fail(command, e.what());
    }
}

}  // namespace

MStatus ExportAlembicCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("ExportAlembicCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        if (!db.isFlagSet(kFileFlag)) return fail(kName, "-file is required");
        MString file;
        db.getFlagArgument(kFileFlag, 0, file);
        MDagPath path;
        FrameRange range;
        if (!groomArgument(kName, db, &path) || !frameRange(kName, db, &range)) return MS::kFailure;
    
        const MTime::Unit unit = MTime::uiUnit();
        const bool poses = db.isFlagSet(kGuidePosesFlag);
        const bool drivers = db.isFlagSet(kDriverCurvesFlag);
        MString group;
        if (db.isFlagSet(kGuideGroupFlag)) db.getFlagArgument(kGuideGroupFlag, 0, group);
        if (int(poses) + int(drivers) + int(group.length() > 0) > 1) {
            return fail(kName, "use one of -guideGroup, -guidePoses and -driverCurves");
        }
        const bool guides = poses || drivers || group.length() > 0;
    
        AlembicCurvesWriter::Options options;
        if (db.isFlagSet("-sca")) {
            double scale = 1.0;
            db.getFlagArgument("-sca", 0, scale);
            if (!(scale > 0.0)) return fail(kName, "-scale must be positive");
            options.scale = static_cast<float>(scale);
        }
        if (db.isFlagSet("-pps")) {
            int points = 0;
            db.getFlagArgument("-pps", 0, points);
            if (points != 0 && (points < 2 || points > 4096)) return fail(kName, "-pointsPerStrand must be 0 or 2 to 4096");
            options.pointsPerStrand = static_cast<std::size_t>(points);
        }
        options.vertexUVs = db.isFlagSet("-vuv");
        const bool adjusted = options.scale != 1.0f || options.pointsPerStrand != 0 || options.vertexUVs;

        std::size_t strandTotal = 0;
        const bool unreal = db.isFlagSet("-ue");
        const bool unity = db.isFlagSet("-uty");
        if (unreal && unity) return fail(kName, "use -unreal or -unity, not both");
        for (const char* flag : {"-grp", "-sh", "-bd", "-bws"}) {
            if (!unreal && !unity && db.isFlagSet(flag)) return fail(kName, MString(flag) + " goes with -unreal or -unity");
        }
        if (!unreal && db.isFlagSet("-fg")) return fail(kName, "-fg goes with -unreal");
        for (const char* flag : {"-bpp", "-msg", "-rep"}) {
            if (!unity && db.isFlagSet(flag)) return fail(kName, MString(flag) + " goes with -unity");
        }
        // Either engine's groom as every host writes it (engine_export.h): groom groups, the
        // feathers' shafts and lighter barbs; Unity's curves fitted to its importer.
        if (unreal || unity) {
            range.gameExport = true;
            EngineExportOptions engine;
            engine.engine = unreal ? EngineExportOptions::Engine::Unreal : EngineExportOptions::Engine::Unity;
            const std::string groupsBy = flagString(db, "-grp", "").asChar();
            engine.groups = groupsBy == "auto" ? std::string(kGroupIdChannel) : groupsBy;
            if (db.isFlagSet("-sh")) {
                bool keepShafts = true;
                db.getFlagArgument("-sh", 0, keepShafts);
                engine.shafts = keepShafts;
            }
            engine.barbDensity = static_cast<float>(flagDouble(db, "-bd", 1.0));
            engine.barbWidthScale = static_cast<float>(flagDouble(db, "-bws", 1.0));
            if (!(engine.barbDensity > 0.0f && engine.barbDensity <= 1.0f)) return fail(kName, "-barbDensity must be above 0 and at most 1");
            if (!(engine.barbWidthScale > 0.0f)) return fail(kName, "-barbWidthScale must be positive");
            if (unreal) {
                // Unreal's groom importer: strands plus the primary guides (what Unreal simulates).
                if (guides) return fail(kName, "-unreal writes its own guides; leave out -guideGroup and -guidePoses");
                if (adjusted) return fail(kName, "-unreal writes Unreal's units and layout; leave out -scale, -pointsPerStrand and -vertexUVs");
                if (db.isFlagSet("-fg")) db.getFlagArgument("-fg", 0, engine.featherGuides);
                if (db.isFlagSet("-iao")) db.getFlagArgument("-iao", 0, engine.iridescenceAo);
                // Written in Unreal's space from the scene's (world space, Maya's internal
                // centimeters whatever the UI unit), as Unreal's FBX importer turns this scene's
                // meshes: the importer's default conversion then lines the groom up with them.
                engine.upAxis = MGlobal::isZAxisUp() ? "Z" : "Y";
                engine.centimetersPerUnit = 1.0;
            } else {
                if (guides) return fail(kName, "-unity writes the strands; leave out -guideGroup and -guidePoses");
                engine.scale = options.scale;
                engine.pointsPerStrand = options.pointsPerStrand;
                int barbPoints = 0;
                if (db.isFlagSet("-bpp")) db.getFlagArgument("-bpp", 0, barbPoints);
                if (barbPoints != 0 && (barbPoints < 2 || barbPoints > 4096)) return fail(kName, "-barbPoints must be 0 or 2 to 4096");
                int maxStrands = static_cast<int>(kUnityMaxStrandsPerGroup);
                if (db.isFlagSet("-msg")) db.getFlagArgument("-msg", 0, maxStrands);
                if (maxStrands < 0) return fail(kName, "-maxStrandsPerGroup must be 0 (no limit) or more");
                if (engine.groups.empty() && (barbPoints != 0 || db.isFlagSet("-msg"))) {
                    return fail(kName, "-barbPoints and -maxStrandsPerGroup go with -groups");
                }
                engine.barbPoints = static_cast<std::size_t>(barbPoints);
                engine.maxStrandsPerGroup = static_cast<std::size_t>(maxStrands);
            }
            nlohmann::json objects = nlohmann::json::array();
            try {
                EngineGroomWriter writer(file.asChar(), exportName(path), mayaSeconds(MTime(range.start, unit)),
                                         frameToSeconds(range.step, mayaFramesPerSecond()), engine);
                status = forEachFrame(
                    kName, path, range,
                    [&](double, const StrandSet& evaluated, const MeshPtr& scalp, NodeEvaluator& evaluator) {
                        StrandSet primaries;  // the hierarchy's level 0: what Unreal simulates
                        if (unreal) {
                            if (const HierarchyPtr h = evaluator.hierarchy()) primaries = toCurves(h->flatten(0));
                        }
                        writer.writeFrame(evaluated, unreal ? &primaries : nullptr, scalp.get());
                    },
                    &strandTotal);
                if (!status) return status;
                writer.close();
                for (const EngineGroomWriter::Object& o : writer.objects()) {  // what the first frame wrote
                    objects.push_back({{"object", o.path}, {"group", o.group}, {"groupId", o.groupId}, {"feathers", o.feathers},
                                       {"strands", o.strands},
                                       {"pointsPerStrand", o.pointsPerStrand ? nlohmann::json(o.pointsPerStrand) : nlohmann::json()},
                                       {"minPoints", o.minPoints}, {"maxPoints", o.maxPoints}});
                }
            } catch (const std::invalid_argument& e) {
                return fail(kName, e.what());
            } catch (const AlembicError& e) {
                return fail(kName, e.what());
            }
            reportWritten(kName, range.count(), strandTotal, file);
            if (unity && db.isFlagSet("-rep")) {
                const nlohmann::json out = {{"frames", range.count()}, {"objects", objects}};
                setResult(MString(out.dump().c_str()));
            } else {
                setResult(static_cast<int>(range.count()));
            }
            return MS::kSuccess;
        }
        UsdCurvesWriter::Options::Scales scaleMode = UsdCurvesWriter::Options::Scales::Mesh;
        if (!scalesMode(db, &scaleMode) || scaleMode == UsdCurvesWriter::Options::Scales::Instancer ||
            scaleMode == UsdCurvesWriter::Options::Scales::Both) {
            return fail(kName, "-scales must be mesh or none (an instancer goes to USD)");
        }
        try {
            AlembicCurvesWriter writer(file.asChar(), exportName(path) + (drivers ? "Drivers" : guides ? "Guides" : ""),
                                       mayaSeconds(MTime(range.start, unit)), frameToSeconds(range.step, mayaFramesPerSecond()),
                                       options);
            if (guides) {
                GuideFrames frames;
                FrameProgress progress(kName, range.count());
                for (unsigned i = 0; i < range.count(); ++i) {
                    if (!progress.step(i)) return progress.cancelled(i);
                    MDGContext context = exportContext(range.start + i * range.step, unit);
                    MDGContextGuard guard(context);
                    StrandSet curves;
                    status = guidesAtFrame(kName, path.node(), group, poses, drivers, frames, &curves);
                    if (!status) return status;
                    writer.writeFrame(curves);
                    strandTotal += curves.strandCount();
                }
            } else {
                SceneScales scales;
                const bool withScales = scaleMode == UsdCurvesWriter::Options::Scales::Mesh;
                status = forEachFrame(
                    kName, path, range,
                    [&](double, const StrandSet& strands, const MeshPtr& scalp, NodeEvaluator&) {
                        const auto mesh = withScales ? scales.build(strands, scalp) : nullptr;
                        writer.writeFrame(strands, scalp.get(), mesh.get());
                    },
                    &strandTotal, true);
                if (!status) return status;
            }
            writer.close();
        } catch (const AlembicError& e) {
            return fail(kName, e.what());
        }
        reportWritten(kName, range.count(), strandTotal, file);
        setResult(static_cast<int>(range.count()));
        return MS::kSuccess;
    });
}

#endif

// ---------------------------------------------------------------------------
// USD

const char* ExportUsdCmd::kName = "groomLabExportUsd";

void* ExportUsdCmd::creator() { return new ExportUsdCmd(); }

MSyntax ExportUsdCmd::newSyntax() {
    MSyntax syntax = curveSyntax();
    syntax.addFlag("-scl", "-scales", MSyntax::kString);  // mesh (default), instancer, both, none
    return syntax;
}

MStatus ExportUsdCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("ExportUsdCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        if (!db.isFlagSet(kFileFlag)) return fail(kName, "-file is required");
        MString file;
        db.getFlagArgument(kFileFlag, 0, file);
        MDagPath path;
        FrameRange range;
        if (!groomArgument(kName, db, &path) || !frameRange(kName, db, &range)) return MS::kFailure;
    
        std::size_t strandTotal = 0;
        try {
            UsdCurvesWriter::Options options;
            options.framesPerSecond = framesPerSecond();
            options.comment = std::string(MFileIO::currentFile().asChar()) + " " + path.fullPathName().asChar();
            options.metersPerUnit = 0.01;  // Maya's internal unit is the centimeter, whatever the UI unit
            options.upAxis = MGlobal::isZAxisUp() ? "Z" : "Y";
            if (!scalesMode(db, &options.scales)) return fail(kName, "-scales must be mesh, instancer, both or none");
            SceneScales scales;
            options.prototypes = scales.lookup();  // the instancers' prototypes, as the scene has them
            UsdCurvesWriter writer(file.asChar(), exportName(path), options);
            const bool withScales = options.scales != UsdCurvesWriter::Options::Scales::None;
            status = forEachFrame(
                kName, path, range,
                [&](double frame, const StrandSet& strands, const MeshPtr& scalp, NodeEvaluator&) {
                    const auto mesh = withScales ? scales.build(strands, scalp) : nullptr;
                    writer.writeFrame(frame, strands, scalp.get(), mesh.get());
                },
                &strandTotal, true);
            if (!status) return status;
            writer.close();
        } catch (const UsdError& e) {
            return fail(kName, e.what());
        }
        reportWritten(kName, range.count(), strandTotal, file);
        setResult(static_cast<int>(range.count()));
        return MS::kSuccess;
    });
}

// ---------------------------------------------------------------------------
// V-Ray scene

const char* ExportVrsceneCmd::kName = "groomLabExportVrscene";

void* ExportVrsceneCmd::creator() { return new ExportVrsceneCmd(); }

MSyntax ExportVrsceneCmd::newSyntax() {
    MSyntax syntax = curveSyntax();
    syntax.addFlag("-mpw", "-minPixelWidth", MSyntax::kDouble);
    return syntax;
}

MStatus ExportVrsceneCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("ExportVrsceneCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        if (!db.isFlagSet(kFileFlag)) return fail(kName, "-file is required");
        MString file;
        db.getFlagArgument(kFileFlag, 0, file);
        MDagPath path;
        FrameRange range;
        if (!groomArgument(kName, db, &path) || !frameRange(kName, db, &range)) return MS::kFailure;

        std::size_t strandTotal = 0;
        try {
            VrsceneWriter::Options options;
            options.framesPerSecond = framesPerSecond();
            options.comment = std::string(MFileIO::currentFile().asChar()) + " " + path.fullPathName().asChar();
            options.metersPerUnit = 0.01;  // Maya's internal unit is the centimeter, whatever the UI unit
            options.upAxis = MGlobal::isZAxisUp() ? "Z" : "Y";
            options.minPixelWidth = static_cast<float>(flagDouble(db, "-mpw", 0.0));
            if (!(options.minPixelWidth >= 0.0f)) return fail(kName, "-minPixelWidth must be 0 or more");
            auto* shape = dynamic_cast<GroomShape*>(MFnDagNode(path).userNode());
            VrsceneWriter writer(file.asChar(), exportName(path), options);
            bool looked = false;
            status = forEachFrame(
                kName, path, range,
                [&](double frame, const StrandSet& strands, const MeshPtr& scalp, NodeEvaluator& evaluator) {
                    if (!looked) {  // the look (HairColor's highlight), as of the first frame
                        HairLook look;
                        if (evaluator.hairLook(&look)) {
                            writer.setLobes(hairLobes(look));
                        } else if (shape) {
                            writer.setLobes(hairLobes(shape->hairLook()));
                        }
                        looked = true;
                    }
                    // The groom's world matrix at this frame (in the frame's context).
                    double m[16];
                    const MMatrix matrix = path.inclusiveMatrix();
                    for (int r = 0; r < 4; ++r) {
                        for (int c = 0; c < 4; ++c) m[4 * r + c] = matrix(r, c);
                    }
                    writer.writeFrame(frame, strands, scalp.get(), m);
                },
                &strandTotal, true);
            if (!status) return status;
            writer.close();
        } catch (const VrsceneError& e) {
            return fail(kName, e.what());
        }
        reportWritten(kName, range.count(), strandTotal, file);
        setResult(static_cast<int>(range.count()));
        return MS::kSuccess;
    });
}

// ---------------------------------------------------------------------------
// Groom file

const char* ExportGroomCmd::kName = "groomLabExportGroom";

void* ExportGroomCmd::creator() { return new ExportGroomCmd(); }

MSyntax ExportGroomCmd::newSyntax() {
    MSyntax syntax;
    syntax.addFlag(kFileFlag, kFileFlagLong, MSyntax::kString);
    syntax.addFlag(kScalpCacheFlag, kScalpCacheFlagLong, MSyntax::kString);
    syntax.addFlag(kScalpObjectFlag, kScalpObjectFlagLong, MSyntax::kString);
    syntax.addFlag(kGuideCacheFlag, kGuideCacheFlagLong, MSyntax::kString);
    syntax.addFlag(kGuideObjectFlag, kGuideObjectFlagLong, MSyntax::kString);
    syntax.addFlag(kDriverCacheFlag, kDriverCacheFlagLong, MSyntax::kString);
    syntax.addFlag(kDriverObjectFlag, kDriverObjectFlagLong, MSyntax::kString);
    syntax.addFlag("-n", "-name", MSyntax::kString);     // the groom's name in the file (its asset name)
    syntax.addFlag("-mg", "-merge", MSyntax::kBoolean);  // keep the file's other grooms
    syntax.setObjectType(MSyntax::kSelectionList, 1, 1);
    syntax.useSelectionAsDefault(true);
    syntax.enableQuery(false);
    syntax.enableEdit(false);
    return syntax;
}

MStatus ExportGroomCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("ExportGroomCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        if (!db.isFlagSet(kFileFlag)) return fail(kName, "-file is required");
        MString file;
        db.getFlagArgument(kFileFlag, 0, file);
        MDagPath path;
        if (!groomArgument(kName, db, &path)) return MS::kFailure;
        auto stringFlag = [&db](const char* flag) {
            MString value;
            if (db.isFlagSet(flag)) db.getFlagArgument(flag, 0, value);
            return std::string(value.asChar());
        };
    
        GroomFile groom;
        groom.data = groomDataFromNode(path.node());
        if (!groom.data.scalp) return fail(kName, path.partialPathName() + " has no scalp mesh connected");
        groom.scalpCache = {stringFlag(kScalpCacheFlag), stringFlag(kScalpObjectFlag)};
        groom.guideCache = {stringFlag(kGuideCacheFlag), stringFlag(kGuideObjectFlag)};
        groom.driverCache = {stringFlag(kDriverCacheFlag), stringFlag(kDriverObjectFlag)};
        groom.framesPerSecond = framesPerSecond();
        groom.upAxis = MGlobal::isZAxisUp() ? "Z" : "Y";
        refreshMayaPaths();
        groom.searchPaths = nodeSearchPaths(path.node());  // relative map paths resolve against the project on the farm too
        groom.source = std::string(MFileIO::currentFile().asChar()) + " " + path.fullPathName().asChar();
        const fs::path target = fs::absolute(fs::u8path(file.asChar()));
        groom.name = fileSafeName(stringFlag("-n").empty() ? assetName(path) : stringFlag("-n"));
        // Every file the groom names, absolute: the core's writer (groom_package.h, every host's)
        // copies the ones the groom owns into the file's own folder, groomlab/<file>/<kind>/<groom>
        // (the scene's own when the file is named after the scene: nothing copied), and names
        // them all relative to the file.
        for (auto& [name, info] : groom.data.maps) {
            if (info.kind == "texture" && !info.texture.empty()) info.texture = resolveNodePath(path.node(), info.texture);
        }
        groom.data.graphJson = followGraphFiles(groom.data.graphJson, [&](const std::string& stored) {
            return stored.find_first_of("$%") == std::string::npos ? resolveNodePath(path.node(), stored) : stored;
        });
        {
            const std::string stored = cardMapsPrefix(MFnDependencyNode(path.node()).findPlug("cardMaps", true).asString().asChar());
            if (!stored.empty()) {
                const std::string placeholder = resolveNodePath(path.node(), stored + "x");
                const auto maps = parseCardMaps(MFnDependencyNode(path.node()).findPlug("cardMaps", true).asString().asChar());
                const auto strands = maps.find("Strands");
                groom.cardBake = {placeholder.substr(0, placeholder.size() - 1), strands == maps.end() ? std::string() : strands->second};
            }
        }
        // The caches named for the file (relative to it: the scalp's and the guides' it was written with).
        for (CacheRef* cache : {&groom.scalpCache, &groom.guideCache, &groom.driverCache}) {
            if (!cache->file.empty() && !isAbsolutePath(cache->file)) cache->file = (target.parent_path() / fs::u8path(cache->file)).generic_u8string();
        }
        // The scene meshes the Instance nodes name (scales of the artist's own plates): stored,
        // so the procedural and the farm build them without the scene.
        for (const std::string& name : graphMeshPrototypes(groom.data.graphJson)) {
            InstancePrototype mesh;
            readInstancePrototype(name, mesh);
            if (mesh.found) {
                groom.prototypes[name] = std::move(mesh);
            } else {
                MGlobal::displayWarning(MString(kName) + ": no mesh named " + name.c_str() +
                                        " for the Instance node; the groom file does not hold it");
            }
        }
        // The last transfer's marker pairs (the next transfer reuses them).
        try {
            const MPlug markers = MFnDependencyNode(path.node()).findPlug("transferMarkers", true);
            groom.transferMarkers = transferMarkersFromJson(markers.isNull() ? std::string() : markers.asString().asChar());
        } catch (const std::exception& e) {
            MGlobal::displayWarning(MString("GroomLab: ") + e.what() + " (the groom file holds no transfer markers)");
        }
        // The Simulate nodes' bakes (simCaches): renders read them.
        try {
            const MPlug sims = MFnDependencyNode(path.node()).findPlug("simCaches", true);
            for (auto& [node, ref] : simCachesFromJson(sims.isNull() ? std::string() : sims.asString().asChar())) {
                ref.file = resolveNodePath(path.node(), ref.file);
                groom.simCaches[node] = ref;
            }
        } catch (const std::exception& e) {
            MGlobal::displayWarning(MString("GroomLab: ") + e.what() + " (the groom file names no simulation bakes)");
        }
        bool merge = false;
        if (db.isFlagSet("-mg")) db.getFlagArgument("-mg", 0, merge);
        try {
            GroomSession check(groom.data);  // refuse to write a groom that does not load
            (void)check;
            GroomWriteOptions options;
            options.merge = merge;
            const GroomWriteReport report = writeGroomsToFile(target.u8string(), {groom}, options);
            for (const std::string& w : report.warnings) MGlobal::displayWarning(MString(kName) + ": " + w.c_str());
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }
        MGlobal::displayInfo(MString(kName) + ": wrote " + file);
        setResult(file);
        return MS::kSuccess;
    });
}

// ---------------------------------------------------------------------------
// glTF cards

const char* ExportGltfCmd::kName = "groomLabExportGltf";

void* ExportGltfCmd::creator() { return new ExportGltfCmd(); }

MSyntax ExportGltfCmd::newSyntax() {
    MSyntax s;
    s.addFlag(kFileFlag, kFileFlagLong, MSyntax::kString);
    s.addFlag("-n", "-name", MSyntax::kString);
    s.addFlag("-r", "-resolution", MSyntax::kLong);
    s.addFlag("-ao", "-occlusion", MSyntax::kDouble);
    s.addFlag("-bin", "-binary", MSyntax::kBoolean);
    s.addFlag("-cap", "-scalpCap", MSyntax::kBoolean);
    s.addFlag("-cr", "-capResolution", MSyntax::kLong);
    s.addFlag("-sca", "-scale", MSyntax::kDouble);
    s.addFlag("-am", "-alphaMode", MSyntax::kString);
    s.addFlag("-ac", "-alphaCutoff", MSyntax::kDouble);
    s.addFlag("-rtt", "-rootToTip", MSyntax::kBoolean);
    s.addFlag("-tan", "-tangents", MSyntax::kBoolean);
    s.addFlag("-lod", "-levelOfDetail", MSyntax::kString);
    s.makeFlagMultiUse("-lod");
    s.addFlag("-lg", "-lodGraph", MSyntax::kString);
    s.makeFlagMultiUse("-lg");
    s.setObjectType(MSyntax::kSelectionList, 1, 1);
    s.useSelectionAsDefault(true);
    s.enableQuery(false);
    s.enableEdit(false);
    return s;
}

MStatus ExportGltfCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("ExportGltfCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        if (!db.isFlagSet(kFileFlag)) return fail(kName, "-file is required");
        const MString file = flagString(db, kFileFlag);
        MDagPath path;
        if (!groomArgument(kName, db, &path)) return MS::kFailure;
        auto* shape = dynamic_cast<GroomShape*>(MFnDagNode(path).userNode());
        if (!shape) return fail(kName, path.partialPathName() + " is not a groomLabShape");

        const std::string name = fileSafeName(flagString(db, "-n", assetName(path).c_str()).asChar());
        int resolution = 0, capResolution = 0;
        if (db.isFlagSet("-r")) db.getFlagArgument("-r", 0, resolution);
        if (db.isFlagSet("-cr")) db.getFlagArgument("-cr", 0, capResolution);
        resolution = resolution > 0 ? std::clamp(resolution, 16, 16384) : 0;  // 0: the Cards nodes' atlasSize
        bool binary = true, cap = true, rootToTip = true, tangents = true;
        if (db.isFlagSet("-bin")) db.getFlagArgument("-bin", 0, binary);
        if (db.isFlagSet("-cap")) db.getFlagArgument("-cap", 0, cap);
        if (db.isFlagSet("-rtt")) db.getFlagArgument("-rtt", 0, rootToTip);
        if (db.isFlagSet("-tan")) db.getFlagArgument("-tan", 0, tangents);
        const double scale = flagDouble(db, "-sca", 0.01);  // Maya's centimeters to glTF's meters
        if (!(scale > 0.0)) return fail(kName, "-scale must be positive");
        const MString alpha = flagString(db, "-am", "mask");
        CardGltfSettings settings;
        if (alpha == "mask") settings.alphaMode = GltfMaterial::AlphaMode::Mask;
        else if (alpha == "blend") settings.alphaMode = GltfMaterial::AlphaMode::Blend;
        else if (alpha == "opaque") settings.alphaMode = GltfMaterial::AlphaMode::Opaque;
        else return fail(kName, "-alphaMode is mask, blend or opaque");
        settings.alphaCutoff = static_cast<float>(flagDouble(db, "-ac", 0.5));
        settings.scale = static_cast<float>(scale);
        settings.zUp = MGlobal::isZAxisUp();
        settings.rootToTip = rootToTip;
        settings.colorToRec709 = renderingSpace().toRec709;  // (the cards' bake: bakeCardsInMemory)
        settings.tangents = tangents;
        const GroomShape::HairLook look = shape->hairLook();
        settings.roughness = look.roughness;
        settings.specular = look.specular;

        // Levels of detail: a name and a graph each (the groom's graph with that LOD's
        // Cards settings), or the groom's own cards as they are.
        std::vector<std::pair<std::string, std::string>> lods;
        const unsigned lodCount = db.numberOfFlagUses("-lod");
        if (lodCount != db.numberOfFlagUses("-lg")) return fail(kName, "give one -lodGraph per -levelOfDetail");
        for (unsigned i = 0; i < lodCount; ++i) {
            MArgList lodArgs, graphArgs;
            db.getFlagArgumentList("-lod", i, lodArgs);
            db.getFlagArgumentList("-lg", i, graphArgs);
            lods.emplace_back(fileSafeName(lodArgs.asString(0).asChar()), graphArgs.asString(0).asChar());
        }

        const double occlusion = flagDouble(db, "-ao", 1.0);
        GltfDocument doc;
        doc.rootName = name;
        doc.generator = std::string("GroomLab ") + path.fullPathName().asChar();
        std::vector<std::string> names;
        try {
            if (lods.empty()) lods.emplace_back("", "");
            // One groom for every level of detail and the hair cap, caching on: each LOD's graph
            // is swapped in (only its Cards nodes and what follows re-run), and the cap's
            // evaluation reuses what the cards' made (the same density: everything upstream).
            NodeEvaluator evaluator(path.node(), true);
            for (const auto& [lod, graph] : lods) {
                const CardBake bake = shape->bakeCardsInMemory(resolution, static_cast<float>(occlusion), graph, &evaluator);
                settings.name = name + (lod.empty() ? "_cards" : "_" + lod);
                addCardsToGltf(doc, *bake.cards, bake.textures, settings);
                names.push_back(settings.name);
            }
            if (cap) {
                try {
                    // The groom's own graph at full density; the cap as every host writes it
                    // (gltf_writer.h: its resolution, just above the skin).
                    evaluator.setGraphJson({});
                    std::string error;
                    MeshPtr scalp;
                    const StrandsPtr strands = evaluator.evaluate(1.0f, &error, &scalp, false, true);  // (cards: a game asset)
                    if (!strands) throw std::runtime_error(error.empty() ? "the groom has no strands" : error);
                    if (!scalp || scalp->triangleCount() == 0) throw std::runtime_error("the groom has no scalp");
                    CardGltfSettings capSettings = settings;
                    capSettings.name = name + "_scalpCap";
                    addScalpCapToGltf(doc, *strands, *scalp, capResolution, resolution, capSettings);
                    names.push_back(capSettings.name);
                } catch (const std::exception& e) {  // a scalp without UVs: the cards still go
                    MGlobal::displayWarning(MString(kName) + ": no hair cap (" + e.what() + ")");
                }
            }
            const std::vector<std::string> written = writeGltf(file.asChar(), doc, binary);
            MStringArray result;
            for (const std::string& w : written) result.append(w.c_str());
            MGlobal::displayInfo(MString(kName) + ": wrote " + file);
            setResult(result);
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }
        return MS::kSuccess;
    });
}

// ---------------------------------------------------------------------------
// Scales: a Maya mesh of them (FBX), and their maps baked onto the skin

namespace {

// The groom's scales at the current time (render density, the scene's prototype meshes), and the
// scalp they lie on. Fails (with the command's message) without scales.
MStatus scalesNow(const char* command, const MDagPath& path, std::shared_ptr<const ScaleMesh>* mesh, MeshPtr* scalp) {
    NodeEvaluator evaluator(path.node(), false);
    std::string error;
    const StrandsPtr strands = evaluator.evaluate(1.0f, &error, scalp, false, true);  // (scales: game meshes and maps)
    if (!error.empty()) return fail(command, error.c_str());
    SceneScales scales;
    *mesh = strands ? scales.build(*strands, *scalp) : nullptr;
    if (!*mesh || (*mesh)->triangles.empty()) return fail(command, path.partialPathName() + " has no scales (a Scales node)");
    return MS::kSuccess;
}

}  // namespace

const char* ScaleMeshCmd::kName = "groomLabScaleMesh";

void* ScaleMeshCmd::creator() { return new ScaleMeshCmd(); }

MSyntax ScaleMeshCmd::newSyntax() {
    MSyntax s;
    s.addFlag("-n", "-name", MSyntax::kString);
    s.setObjectType(MSyntax::kSelectionList, 1, 1);
    s.useSelectionAsDefault(true);
    s.enableQuery(false);
    s.enableEdit(false);
    return s;
}

MStatus ScaleMeshCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("ScaleMeshCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        MDagPath path;
        if (!groomArgument(kName, db, &path)) return MS::kFailure;
        MeshPtr scalp;
        status = scalesNow(kName, path, &mesh_, &scalp);
        if (!status) return status;
        matrix_ = path.inclusiveMatrix();  // the points are the groom's (its transform places them, as in a render)
        name_ = flagString(db, "-n", MFnDagNode(path.transform()).name() + "Scales");
        return redoIt();
    });
}

MStatus ScaleMeshCmd::redoIt() {
    return groomlab::maya::guarded("ScaleMeshCmd::redoIt", [&]() -> MStatus {
        const ScaleMesh& m = *mesh_;
        const auto vertices = static_cast<unsigned>(m.vertexCount());
        const auto faces = static_cast<unsigned>(m.triangleCount());
        MFloatPointArray points(vertices);
        MFloatArray us(vertices), vs(vertices);
        for (unsigned i = 0; i < vertices; ++i) {
            points.set(i, m.points[i].x, m.points[i].y, m.points[i].z);
            us[i] = m.uvs[i][0];
            vs[i] = m.uvs[i][1];
        }
        MIntArray counts(faces, 3), connects(faces * 3);
        for (unsigned i = 0; i < faces * 3; ++i) connects[i] = static_cast<int>(m.triangles[i]);
        MStatus status;
        MFnDagNode transformFn;
        transform_ = transformFn.create("transform", name_, MObject::kNullObj, &status);
        if (!status) return fail(kName, "cannot make the scales' transform");
        MFnTransform(transform_).set(MTransformationMatrix(matrix_));
        MFnMesh fn;
        const MObject shape = fn.create(static_cast<int>(vertices), static_cast<int>(faces), points, counts, connects, us, vs, transform_, &status);
        if (!status) return fail(kName, "cannot make the scales' mesh");
        fn.setName(MFnDagNode(transform_).name() + "Shape");
        fn.assignUVs(counts, connects);
        // Per face: a scale's data in two more uv sets and a color set, every corner the same.
        std::vector<std::uint32_t> owner(faces, 0);
        for (std::size_t k = 0; k < m.scales.size(); ++k) {
            const ScaleMesh::Scale& s = m.scales[k];
            for (std::uint32_t f = s.firstTriangle; f < s.firstTriangle + s.triangleCount && f < faces; ++f) owner[f] = static_cast<std::uint32_t>(k);
        }
        MIntArray perFace(faces * 3);
        for (unsigned i = 0; i < faces * 3; ++i) perFace[i] = static_cast<int>(owner[i / 3]);
        const auto scaleCount = static_cast<unsigned>(m.scales.size());
        for (const char* set : {"scaleUV", "scaleData"}) {
            MString name(set);
            fn.createUVSetWithName(name);
            MFloatArray su(scaleCount), sv(scaleCount);
            for (unsigned k = 0; k < scaleCount; ++k) {
                const ScaleMesh::Scale& s = m.scales[k];
                su[k] = name == "scaleUV" ? s.rootUv[0] : s.random;
                sv[k] = name == "scaleUV" ? s.rootUv[1] : static_cast<float>(s.variant);
            }
            fn.setUVs(su, sv, &name);
            fn.assignUVs(counts, perFace, &name);
        }
        MColorArray vertexColors(vertices), rootColors(scaleCount);
        for (unsigned i = 0; i < vertices; ++i) vertexColors.set(i, m.colors[i][0], m.colors[i][1], m.colors[i][2]);
        for (unsigned k = 0; k < scaleCount; ++k) rootColors.set(k, m.scales[k].color[0], m.scales[k].color[1], m.scales[k].color[2]);
        MString colorSet("scaleColor"), rootSet("scaleRootColor");
        fn.createColorSetWithName(colorSet);
        fn.setColors(vertexColors, &colorSet);
        fn.assignColors(connects, &colorSet);
        fn.createColorSetWithName(rootSet);
        fn.setColors(rootColors, &rootSet);
        fn.assignColors(perFace, &rootSet);
        fn.setCurrentColorSetName(colorSet);
        MString map1("map1");
        fn.setCurrentUVSetName(map1);
        // The scales' own normals (their plates' hard edges, the bend), locked.
        MVectorArray normals(vertices);
        MIntArray ids(vertices);
        for (unsigned i = 0; i < vertices; ++i) {
            normals.set(MVector(m.normals[i].x, m.normals[i].y, m.normals[i].z), i);
            ids[i] = static_cast<int>(i);
        }
        fn.setVertexNormals(normals, ids);
        MGlobal::executeCommand("sets -edit -forceElement initialShadingGroup " + fn.fullPathName());
        (void)shape;
        clearResult();
        setResult(MFnDagNode(transform_).fullPathName());
        return MS::kSuccess;
    });
}

MStatus ScaleMeshCmd::undoIt() {
    if (!transform_.isNull()) MGlobal::deleteNode(transform_);
    transform_ = MObject::kNullObj;
    return MS::kSuccess;
}

const char* BakeScaleMapsCmd::kName = "groomLabBakeScaleMaps";

void* BakeScaleMapsCmd::creator() { return new BakeScaleMapsCmd(); }

MSyntax BakeScaleMapsCmd::newSyntax() {
    MSyntax s;
    s.addFlag("-d", "-directory", MSyntax::kString);
    s.addFlag("-n", "-name", MSyntax::kString);
    s.addFlag("-r", "-resolution", MSyntax::kLong);
    s.addFlag("-mh", "-maxHeight", MSyntax::kDouble);
    s.addFlag("-p", "-padding", MSyntax::kLong);
    s.setObjectType(MSyntax::kSelectionList, 1, 1);
    s.useSelectionAsDefault(true);
    s.enableQuery(false);
    s.enableEdit(false);
    return s;
}

MStatus BakeScaleMapsCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("BakeScaleMapsCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        MDagPath path;
        if (!groomArgument(kName, db, &path)) return MS::kFailure;
        if (!db.isFlagSet("-d") || !db.isFlagSet("-n")) return fail(kName, "-directory and -name are required");
        ScaleMapOptions options;
        options.resolution = static_cast<int>(flagDouble(db, "-r", 2048));
        options.maxHeight = static_cast<float>(flagDouble(db, "-mh", 0.0));
        options.padding = static_cast<int>(flagDouble(db, "-p", 8));
        if (options.resolution < 16 || options.resolution > 8192) return fail(kName, "-resolution must be 16 to 8192");
        if (options.maxHeight < 0.0f) return fail(kName, "-maxHeight must be 0 (measured) or more");
        std::shared_ptr<const ScaleMesh> mesh;
        MeshPtr scalp;
        status = scalesNow(kName, path, &mesh, &scalp);
        if (!status) return status;
        if (!scalp || !scalp->hasUVs()) return fail(kName, path.partialPathName() + "'s scalp has no uvs to bake onto");
        const fs::path dir = fs::u8path(flagString(db, "-d").asChar());
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::string prefix = (dir / fs::u8path(std::string(flagString(db, "-n").asChar()) + "_")).generic_u8string();
        try {
            const ScaleMaps maps = bakeScaleMaps(*scalp, *mesh, options);
            MStringArray result;
            for (const std::string& f : writeScaleMaps(maps, prefix)) result.append(f.c_str());
            result.append(MString() + maps.maxHeight);
            MGlobal::displayInfo(MString(kName) + ": " + static_cast<unsigned>(maps.tiles.size()) + " tile(s), max height " +
                                 maps.maxHeight + ", " + static_cast<int>(100.0 * maps.covered / std::max<std::size_t>(maps.texels, 1)) +
                                 "% covered, to " + prefix.c_str() + "*");
            setResult(result);
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }
        return MS::kSuccess;
    });
}
// ---------------------------------------------------------------------------
// groomLabBakeFollicleMap

const char* BakeFollicleMapCmd::kName = "groomLabBakeFollicleMap";

void* BakeFollicleMapCmd::creator() { return new BakeFollicleMapCmd(); }

MSyntax BakeFollicleMapCmd::newSyntax() {
    MSyntax s;
    s.addFlag("-d", "-directory", MSyntax::kString);
    s.addFlag("-n", "-name", MSyntax::kString);
    s.addFlag("-r", "-resolution", MSyntax::kLong);
    s.addFlag("-rad", "-radius", MSyntax::kDouble);
    s.addFlag("-ar", "-autoRadius");
    s.addFlag("-p", "-padding", MSyntax::kLong);
    s.setObjectType(MSyntax::kSelectionList, 1, 1);
    s.useSelectionAsDefault(true);
    s.enableQuery(false);
    s.enableEdit(false);
    return s;
}

MStatus BakeFollicleMapCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("BakeFollicleMapCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        MDagPath path;
        if (!groomArgument(kName, db, &path)) return MS::kFailure;
        const bool measure = db.isFlagSet("-ar");
        if (!measure && (!db.isFlagSet("-d") || !db.isFlagSet("-n"))) return fail(kName, "-directory and -name are required");
        FollicleMapOptions options;
        options.resolution = static_cast<int>(flagDouble(db, "-r", 2048));
        options.radius = static_cast<float>(flagDouble(db, "-rad", 0.0));
        options.padding = static_cast<int>(flagDouble(db, "-p", 8));
        if (options.resolution < 16 || options.resolution > 8192) return fail(kName, "-resolution must be 16 to 8192");
        if (options.radius < 0.0f) return fail(kName, "-radius must be 0 (from the roots' spacing) or more");
        // Every root at the current time (none buried: a game's strands), on the scalp it grew on.
        NodeEvaluator evaluator(path.node(), false);
        std::string error;
        MeshPtr scalp;
        const StrandsPtr strands = evaluator.evaluate(1.0f, &error, &scalp, false, true);
        if (!error.empty()) return fail(kName, error.c_str());
        if (!strands) return fail(kName, path.partialPathName() + " gave no strands");
        if (measure) {
            if (!scalp) return fail(kName, path.partialPathName() + " has no scalp");
            setResult(static_cast<double>(follicleMapAutoRadius(*scalp, *strands)));
            return MS::kSuccess;
        }
        if (!scalp || !scalp->hasUVs()) return fail(kName, path.partialPathName() + "'s scalp has no uvs to bake onto");
        const fs::path dir = fs::u8path(flagString(db, "-d").asChar());
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::string prefix = (dir / fs::u8path(std::string(flagString(db, "-n").asChar()) + "_")).generic_u8string();
        try {
            const FollicleMap map = bakeFollicleMap(*scalp, *strands, options);
            MStringArray result;
            for (const std::string& f : writeFollicleMap(map, prefix)) result.append(f.c_str());
            result.append(MString() + map.radius);
            MGlobal::displayInfo(MString(kName) + ": " + static_cast<unsigned>(map.follicles) + " follicles, " +
                                 static_cast<unsigned>(map.tiles.size()) + " tile(s), radius " + map.radius + ", to " +
                                 prefix.c_str() + "*");
            setResult(result);
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }
        return MS::kSuccess;
    });
}
}  // namespace groomlab::maya
