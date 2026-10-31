// SPDX-License-Identifier: Apache-2.0
#include "version_cmd.h"

#include <algorithm>
#include <cstdio>

#include <maya/MArgDatabase.h>
#include <maya/MArgList.h>
#include <maya/MDagPath.h>
#include <maya/MDoubleArray.h>
#include <maya/MSyntax.h>
#include <maya/MFnDagNode.h>
#include <maya/MGlobal.h>
#include <maya/MSelectionList.h>
#include <maya/MStringArray.h>
#include <nlohmann/json.hpp>

#include "asset_files.h"
#include "groom_draw_override.h"
#include "guarded.h"
#include "groom_shape.h"
#include "groomlab/card_maps.h"
#include "groomlab/display.h"
#include "groomlab/expression.h"
#include "groomlab/flow.h"
#include "groomlab/graph_io.h"
#include "groomlab/parts.h"
#include "groomlab/version.h"
#include "hair_shading.h"
#include "maya_inputs.h"

namespace groomlab::maya {

const char* VersionCmd::kName = "groomLabVersion";

void* VersionCmd::creator() { return new VersionCmd(); }

MStatus VersionCmd::doIt(const MArgList&) {
    return groomlab::maya::guarded("VersionCmd::doIt", [&]() -> MStatus {
        setResult(MString(groomlab::kVersionString));
        return MS::kSuccess;
    });
}

const char* DefaultGraphCmd::kName = "groomLabDefaultGraph";

void* DefaultGraphCmd::creator() { return new DefaultGraphCmd(); }

MStatus DefaultGraphCmd::doIt(const MArgList&) {
    return groomlab::maya::guarded("DefaultGraphCmd::doIt", [&]() -> MStatus {
        setResult(MString(defaultGraphJson().c_str()));
        return MS::kSuccess;
    });
}

const char* OperatorsCmd::kName = "groomLabOperators";

void* OperatorsCmd::creator() { return new OperatorsCmd(); }

MStatus OperatorsCmd::doIt(const MArgList&) {
    return groomlab::maya::guarded("OperatorsCmd::doIt", [&]() -> MStatus {
        setResult(MString(operatorSpecsJson().c_str()));
        return MS::kSuccess;
    });
}

const char* ExpressionCmd::kName = "groomLabExpression";

void* ExpressionCmd::creator() { return new ExpressionCmd(); }

MSyntax ExpressionCmd::newSyntax() {
    MSyntax syntax;
    syntax.addFlag("-c", "-check", MSyntax::kString);
    syntax.addFlag("-v", "-variables", MSyntax::kString);
    return syntax;
}

MStatus ExpressionCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("ExpressionCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        MString text, variables("{}");
        db.getFlagArgument("-c", 0, text);
        if (db.isFlagSet("-v")) db.getFlagArgument("-v", 0, variables);
        std::string problem;
        try {
            const Expression e(text.asChar(), parseExpressionVariables(variables.asChar()));
            (void)e;
        } catch (const ExpressionError& err) {
            problem = err.what();
        }
        setResult(MString(problem.c_str()));
        return MS::kSuccess;
    });
}

const char* PathsCmd::kName = "groomLabPaths";

void* PathsCmd::creator() { return new PathsCmd(); }

MSyntax PathsCmd::newSyntax() {
    MSyntax syntax;
    syntax.addFlag("-r", "-resolve", MSyntax::kString);
    syntax.addFlag("-nd", "-node", MSyntax::kString);
    syntax.addFlag("-an", "-assetName", MSyntax::kString);
    syntax.addFlag("-af", "-assetFolder", MSyntax::kString);
    return syntax;
}

MStatus PathsCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("PathsCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        refreshMayaPaths();
        // -node: a node whose paths resolve its own way (from a referenced file: its file's folder first).
        MObject node;
        MDagPath dag;
        if (db.isFlagSet("-nd") || db.isFlagSet("-an")) {
            MString name;
            db.getFlagArgument(db.isFlagSet("-an") ? "-an" : "-nd", 0, name);
            MSelectionList sel;
            if (sel.add(name) != MS::kSuccess || sel.getDependNode(0, node) != MS::kSuccess) {
                MGlobal::displayError(MString(kName) + ": no node " + name);
                return MS::kFailure;
            }
            sel.getDagPath(0, dag);
        }
        if (db.isFlagSet("-an")) {  // a groom's asset name (its files' names)
            if (!dag.isValid()) {
                MGlobal::displayError(MString(kName) + ": -assetName takes a DAG node");
                return MS::kFailure;
            }
            setResult(MString(assetName(dag).c_str()));
            return MS::kSuccess;
        }
        if (db.isFlagSet("-af")) {  // the scene's folder for a kind of file
            MString kind;
            db.getFlagArgument("-af", 0, kind);
            setResult(MString(assetFolder(kind.asChar()).c_str()));
            return MS::kSuccess;
        }
        if (db.isFlagSet("-r")) {
            MString path;
            db.getFlagArgument("-r", 0, path);
            setResult(MString((node.isNull() ? resolveMayaPath(path.asChar()) : resolveNodePath(node, path.asChar())).c_str()));
            return MS::kSuccess;
        }
        MStringArray dirs;
        for (const std::string& d : mayaSearchPaths()) dirs.append(d.c_str());
        setResult(dirs);
        return MS::kSuccess;
    });
}

const char* GraphStatsCmd::kName = "groomLabGraphStats";

void* GraphStatsCmd::creator() { return new GraphStatsCmd(); }

MSyntax GraphStatsCmd::newSyntax() {
    MSyntax s;
    s.addFlag("-pts", "-points");
    s.addFlag("-ins", "-instances");
    s.addFlag("-evc", "-evaluations");
    s.addFlag("-dt", "-drawTimes");
    s.addFlag("-fi", "-frameInfo");
    s.addFlag("-ov", "-overlays");
    s.addFlag("-hs", "-hairShading");
    s.addFlag("-hl", "-hairLook");
    s.addFlag("-scs", "-strandColorSources");
    s.addFlag("-chr", "-channelRange", MSyntax::kString);
    s.setObjectType(MSyntax::kStringObjects, 1, 1);
    return s;
}

MStatus GraphStatsCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("GraphStatsCmd::doIt", [&]() -> MStatus {
        MStatus parsed;
        MArgDatabase db(syntax(), args, &parsed);
        MStringArray names;
        MSelectionList sel;
        if (!parsed || db.getObjects(names) != MS::kSuccess || names.length() < 1 || sel.add(names[0]) != MS::kSuccess) {
            MGlobal::displayError("groomLabGraphStats: name a groomLabShape");
            return MS::kFailure;
        }
        MDagPath path;
        if (sel.getDagPath(0, path) != MS::kSuccess) return MS::kFailure;
        path.extendToShape();
        auto* shape = dynamic_cast<GroomShape*>(MFnDagNode(path).userNode());
        if (!shape) {
            MGlobal::displayError("groomLabGraphStats: " + names[0] + " is not a groomLabShape");
            return MS::kFailure;
        }
        if (db.isFlagSet("-evaluations")) {
            // How many times the groom evaluated live (after evaluating it when dirty; a frame cached
            // playback restored is not evaluated again): what shows its stats asks for them again
            // only when this moved.
            shape->frame();
            setResult(static_cast<double>(shape->evaluationCount()));
            return MS::kSuccess;
        }
        if (db.isFlagSet("-frameInfo")) {
            // The frame drawn now, evaluating nothing that is not dirty (a frame cached playback
            // restored stays the cached one): where it came from and the evaluations so far.
            // While a commit evaluates off the UI thread ("pending": groom_shape.h expectCommit) the
            // frame is the one from before it; "commitMs" the last commit's evaluation there,
            // "commits" how many so far (landed), "chained" the brush strokes pressed while one
            // evaluated (they wait for nothing: groom_shape.h chainStroke). Nothing waits for it here.
            const FramePtr frame = shape->frame();
            char buffer[448];
            std::snprintf(buffer, sizeof buffer,
                          "{\"seconds\": %.6f, \"live\": %s, \"liveEvaluation\": %llu, \"version\": %llu, \"strands\": %llu, "
                          "\"evaluations\": %llu, \"backgroundEvaluations\": %llu, \"pending\": %s, \"commitMs\": %.3f, "
                          "\"commits\": %llu, \"chained\": %llu, \"settledBy\": \"%s\"}",
                          frame ? frame->seconds : 0.0, frame && frame->liveEvaluation != 0 ? "true" : "false",
                          static_cast<unsigned long long>(frame ? frame->liveEvaluation : 0),
                          static_cast<unsigned long long>(frame ? frame->version : 0),
                          static_cast<unsigned long long>(frame && frame->view.strands ? frame->view.strands->strandCount() : 0),
                          static_cast<unsigned long long>(shape->evaluationCount()),
                          static_cast<unsigned long long>(shape->backgroundEvaluationCount()),
                          shape->commitPending() ? "true" : "false", shape->lastCommitMs(),
                          static_cast<unsigned long long>(shape->commitCount()),
                          static_cast<unsigned long long>(shape->chainedStrokes()), shape->settledBy().c_str());
            setResult(MString(buffer));
            return MS::kSuccess;
        }
        // The queries below (the strands' points, the instances, the overlays) are the committed
        // groom's: a commit evaluating off the UI thread is waited for first.
        if (db.isFlagSet("-overlays") || db.isFlagSet("-points") || db.isFlagSet("-instances") ||
            db.isFlagSet("-strandColorSources") || db.isFlagSet("-channelRange")) {
            shape->settleCommit("groomLabGraphStats");
        }
        if (db.isFlagSet("-strandColorSources")) {
            // (A groom nothing has drawn yet is evaluated first: its output pulled.)
            MPlug(shape->thisMObject(), GroomShape::aOutStrandCount).asInt();
            shape->settleCommit("groomLabGraphStats");
            // What the strand colors can read in the frame drawn (the viewed node's): {"channels":
            // the strands' channels (the attribute colors), "colorSets": the scalp's color sets,
            // painted vertex maps among them (the color set colors)}, each sorted.
            const FramePtr frame = shape->frame();
            nlohmann::json out = {{"channels", nlohmann::json::array()}, {"colorSets", nlohmann::json::array()}};
            if (frame && frame->view.strands) out["channels"] = frame->view.strands->strandChannelNames();
            if (frame && frame->view.scalp) out["colorSets"] = frame->view.scalp->colorSetNames();
            setResult(MString(out.dump().c_str()));
            return MS::kSuccess;
        }
        if (db.isFlagSet("-channelRange")) {
            // A strand channel's lowest and highest value in the frame drawn (Auto Range): [lo, hi],
            // empty without the channel.
            MString name;
            db.getFlagArgument("-channelRange", 0, name);
            MPlug(shape->thisMObject(), GroomShape::aOutStrandCount).asInt();
            shape->settleCommit("groomLabGraphStats");
            const FramePtr frame = shape->frame();
            MDoubleArray range;
            float lo = 0.0f, hi = 0.0f;
            if (frame && frame->view.strands && strandChannelRange(*frame->view.strands, name.asChar(), lo, hi)) {
                range.append(lo);
                range.append(hi);
            }
            setResult(range);
            return MS::kSuccess;
        }
        if (db.isFlagSet("-overlays")) {
            // The parts and flow of the frame drawn (world space, on the scalp as evaluated):
            // {"parts": [[[x, y, z], ...], ...], "pins": [[x, y, z], ...], "directions": [...],
            // "sinks": [...], "clumpCenters": [...]}, what the viewport draws them from.
            const FramePtr frame = shape->frame();
            std::string out = "{\"parts\": [";
            char buffer[96];
            auto point = [&](const Vec3f& p, bool first) {
                std::snprintf(buffer, sizeof buffer, "%s[%.9g, %.9g, %.9g]", first ? "" : ", ", p.x, p.y, p.z);
                out += buffer;
            };
            auto points = [&](const char* key, const std::vector<Vec3f>& list) {
                out += std::string("], \"") + key + "\": [";
                for (std::size_t i = 0; i < list.size(); ++i) point(list[i], i == 0);
            };
            if (frame && frame->view.parts) {
                const auto& lines = frame->view.parts->lines();
                for (std::size_t l = 0; l < lines.size(); ++l) {
                    out += l == 0 ? "[" : ", [";
                    for (std::size_t i = 0; i < lines[l].size(); ++i) point(lines[l][i], i == 0);
                    out += "]";
                }
            }
            const FlowPtr flow = frame ? frame->view.flow : nullptr;
            points("pins", flow ? flow->positions() : std::vector<Vec3f>{});
            points("directions", flow ? flow->directions() : std::vector<Vec3f>{});
            points("sinks", flow ? flow->sinkPositions() : std::vector<Vec3f>{});
            // A viewed Clump's centers, as their dots are drawn.
            points("clumpCenters", frame && frame->view.clumpCenters ? *frame->view.clumpCenters : std::vector<Vec3f>{});
            out += "]}";
            setResult(MString(out.c_str()));
            return MS::kSuccess;
        }
        if (db.isFlagSet("-hairShading")) {
            // GroomLab's hair shading (hair_shading.h): the lights the last hair draw took, the draws.
            std::string lights = groomlab::maya::hairLightsJson();
            lights.insert(lights.size() - 1, ", \"grid\": " + groomlab::maya::hairGridJson(shape));
            setResult(MString(lights.c_str()));
            return MS::kSuccess;
        }
        if (db.isFlagSet("-hairLook")) {
            // The hair's look, its lobes and its nodes' params (GroomSession::hairLookJson):
            // what create_hair_shader builds the Arnold shader from.
            setResult(MString(shape->hairLookJson().c_str()));
            return MS::kSuccess;
        }
        if (db.isFlagSet("-drawTimes")) {
            // Report-only: the last evaluation's time in compute and the last viewport update
            // that uploaded geometry (any groom's), timed.
            char buffer[64];
            std::snprintf(buffer, sizeof buffer, "{\"computeMs\": %.3f, \"draw\": ", shape->lastComputeMs());
            setResult(MString((buffer + lastDrawTimesJson() + "}").c_str()));
            return MS::kSuccess;
        }
        if (db.isFlagSet("-points")) {
            MDoubleArray xyz;
            if (const StrandsPtr strands = shape->evaluatedStrands()) {
                xyz.setLength(static_cast<unsigned>(strands->positions().size() * 3));
                unsigned i = 0;
                for (const Vec3f& p : strands->positions()) {
                    xyz[i++] = p.x;
                    xyz[i++] = p.y;
                    xyz[i++] = p.z;
                }
            }
            setResult(xyz);
            return MS::kSuccess;
        }
        if (db.isFlagSet("-instances")) {
            // The viewport's instances as it draws them: 16 values each, the row-major matrix
            // (rows x, y, z axes scaled, then the origin).
            MDoubleArray values;
            const StrandsPtr strands = shape->evaluatedStrands();
            if (strands && strands->instances()) {
                for (const InstanceSet::Placed& p : strands->instances()->resolve(*strands)) {
                    float m[16];
                    p.matrix(m);
                    for (const float v : m) values.append(v);
                }
            }
            setResult(values);
            return MS::kSuccess;
        }
        setResult(MString(shape->graphStatsJson().c_str()));
        return MS::kSuccess;
    });
}

const char* BakeCardsCmd::kName = "groomLabBakeCards";

void* BakeCardsCmd::creator() { return new BakeCardsCmd(); }

MSyntax BakeCardsCmd::newSyntax() {
    MSyntax s;
    s.addFlag("-d", "-directory", MSyntax::kString);
    s.addFlag("-n", "-name", MSyntax::kString);
    s.addFlag("-r", "-resolution", MSyntax::kLong);
    s.addFlag("-ao", "-occlusion", MSyntax::kDouble);
    s.addFlag("-sc", "-scalp", MSyntax::kBoolean);
    s.addFlag("-rc", "-reach", MSyntax::kDouble);
    s.addFlag("-den", "-density", MSyntax::kDouble);
    s.addFlag("-nst", "-noStore");  // an export: the groom's cardMaps keep naming its own bake
    s.addFlag("-lo", "-layout", MSyntax::kString);  // "unreal" (TGA, the default) or "unity" (PNG, an export)
    // A card map export (card_maps.h; never stored): the preset by name or as JSON, and its settings.
    s.addFlag("-pre", "-preset", MSyntax::kString);
    s.addFlag("-fmt", "-format", MSyntax::kString);
    s.addFlag("-bd", "-bitDepth", MSyntax::kLong);
    s.addFlag("-pad", "-padding", MSyntax::kLong);
    s.addFlag("-ss", "-supersample", MSyntax::kLong);
    s.addFlag("-nt", "-nameTemplate", MSyntax::kString);
    s.setObjectType(MSyntax::kSelectionList, 1, 1);
    s.useSelectionAsDefault(true);
    return s;
}

MStatus BakeCardsCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("BakeCardsCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        MSelectionList sel;
        db.getObjects(sel);
        MDagPath path;
        if (sel.getDagPath(0, path) != MS::kSuccess) return MS::kFailure;
        path.extendToShape();
        auto* shape = dynamic_cast<GroomShape*>(MFnDagNode(path).userNode());
        if (!shape) {
            MGlobal::displayError(MString(kName) + ": select or name a groomLabShape");
            return MS::kFailure;
        }
        // Named by default after the groom's asset name (its full path, file safe): two grooms
        // of the same short name, or a namespaced one, never meet in a file name.
        MString directory, name = assetName(path).c_str();
        if (!db.isFlagSet("-d") || db.getFlagArgument("-d", 0, directory) != MS::kSuccess) {
            MGlobal::displayError(MString(kName) + ": -directory is required");
            return MS::kFailure;
        }
        if (db.isFlagSet("-n")) db.getFlagArgument("-n", 0, name);
        int resolution = 0;  // the Cards nodes' atlasSize
        if (db.isFlagSet("-r")) db.getFlagArgument("-r", 0, resolution);
        double occlusion = 1.0;
        if (db.isFlagSet("-ao")) db.getFlagArgument("-ao", 0, occlusion);
        bool scalp = false;
        if (db.isFlagSet("-sc")) db.getFlagArgument("-sc", 0, scalp);
        double reach = 0.0, density = 1.0;
        if (db.isFlagSet("-rc")) db.getFlagArgument("-rc", 0, reach);
        if (db.isFlagSet("-den")) db.getFlagArgument("-den", 0, density);
        MString layout = "unreal";
        if (db.isFlagSet("-lo")) db.getFlagArgument("-lo", 0, layout);
        // Unity's maps are an export only: the groom's cards view reads the Unreal layout.
        const bool store = !db.isFlagSet("-nst") && layout == "unreal";
        try {
            MStringArray result;
            if (db.isFlagSet("-pre")) {  // a card map export, as its preset packs it
                MString presetText;
                db.getFlagArgument("-pre", 0, presetText);
                const std::string text = presetText.asChar();
                CardMapPreset preset;
                if (!text.empty() && text.front() == '{') {
                    preset = cardMapPresetFromJson(text);
                } else if (const auto found = findCardMapPreset(text)) {
                    preset = *found;
                } else {
                    throw std::runtime_error("no card map preset named '" + text + "'");
                }
                CardMapExport settings;
                settings.resolution = resolution > 0 ? std::clamp(resolution, 16, 8192) : 0;
                MString value;
                if (db.isFlagSet("-fmt") && db.getFlagArgument("-fmt", 0, value) == MS::kSuccess) settings.format = value.asChar();
                if (db.isFlagSet("-bd")) db.getFlagArgument("-bd", 0, settings.bitDepth);
                if (db.isFlagSet("-pad")) db.getFlagArgument("-pad", 0, settings.padding);
                if (db.isFlagSet("-ss")) db.getFlagArgument("-ss", 0, settings.supersample);
                if (db.isFlagSet("-nt") && db.getFlagArgument("-nt", 0, value) == MS::kSuccess) settings.nameTemplate = value.asChar();
                for (const std::string& p : shape->exportCardMaps(directory.asChar(), name.asChar(), preset, settings, static_cast<float>(occlusion))) {
                    result.append(p.c_str());
                }
                setResult(result);
                return MS::kSuccess;
            }
            if (scalp) {  // the groom baked onto its scalp, in the scalp's UVs
                for (const std::string& p : shape->bakeScalp(directory.asChar(), name.asChar(),
                                                             resolution > 0 ? std::clamp(resolution, 16, 16384) : 2048,
                                                             static_cast<float>(reach), static_cast<float>(density),
                                                             layout.asChar())) {
                    result.append(p.c_str());
                }
                setResult(result);
                return MS::kSuccess;
            }
            for (const std::string& p : shape->bakeCards(directory.asChar(), name.asChar(), resolution > 0 ? std::clamp(resolution, 16, 16384) : 0,
                                                             static_cast<float>(occlusion), store, layout.asChar())) {
                result.append(p.c_str());
            }
            setResult(result);
        } catch (const std::exception& e) {
            MGlobal::displayError(MString(kName) + ": " + e.what());
            return MS::kFailure;
        }
        return MS::kSuccess;
    });
}

}  // namespace groomlab::maya
