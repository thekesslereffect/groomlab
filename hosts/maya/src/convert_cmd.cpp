// SPDX-License-Identifier: Apache-2.0
#include "convert_cmd.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <variant>

#include <maya/MArgDatabase.h>
#include <maya/MArgList.h>
#include <maya/MDGModifier.h>
#include <maya/MDagModifier.h>
#include <maya/MFnSet.h>
#include <maya/MSelectionList.h>
#include <maya/MDagPath.h>
#include <maya/MDoubleArray.h>
#include <maya/MFloatArray.h>
#include <maya/MFloatPointArray.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MFnDoubleArrayData.h>
#include <maya/MFnMesh.h>
#include <maya/MFnNumericAttribute.h>
#include <maya/MFnNurbsCurve.h>
#include <maya/MFnStringData.h>
#include <maya/MFnTransform.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MGlobal.h>
#include <maya/MIntArray.h>
#include <maya/MPlug.h>
#include <maya/MPointArray.h>
#include <maya/MStringArray.h>

#include "command_args.h"
#include "groom_shape.h"
#include "groomlab/guide_hierarchy.h"
#include "guarded.h"
#include "maya_inputs.h"

namespace groomlab::maya {

namespace {

const char* kGuideCurves[] = {"-gcv", "-guideCurves"};
const char* kStrandCurves[] = {"-scv", "-strandCurves"};
const char* kStrandMesh[] = {"-smh", "-strandMesh"};
const char* kGuide[] = {"-gid", "-guide"};
const char* kGroup[] = {"-g", "-group"};
const char* kFlat[] = {"-fl", "-flat"};
const char* kNode[] = {"-nd", "-node"};
const char* kDensity[] = {"-dn", "-density"};
const char* kMaxCount[] = {"-mc", "-maxCount"};
const char* kAttributes[] = {"-at", "-attributes"};
const char* kDegree[] = {"-dg", "-degree"};
const char* kNameFlag[] = {"-n", "-name"};
const char* kStyle[] = {"-st", "-style"};
const char* kSides[] = {"-sds", "-sides"};
const char* kWidthScale[] = {"-ws", "-widthScale"};
const char* kStrandUVs[] = {"-suv", "-strandUVs"};
const char* kMake[] = {"-mk", "-make"};

// "colorR" -> "glColorR"; anything but letters, digits and _ becomes _.
MString attributeName(const std::string& channel) {
    std::string out = "gl";
    for (std::size_t i = 0; i < channel.size(); ++i) {
        const char c = channel[i];
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        out += !ok ? '_' : (i == 0 ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
    }
    return out.c_str();
}

// A node's name, namespace and path gone ("|charA|ns:hair" -> "hair").
std::string leafName(const MString& path) {
    std::string s = path.asChar();
    const auto bar = s.find_last_of('|');
    if (bar != std::string::npos) s = s.substr(bar + 1);
    const auto colon = s.find_last_of(':');
    if (colon != std::string::npos) s = s.substr(colon + 1);
    return s;
}

// Every channel name valid as a Maya attribute: letters, digits, _; not starting with a digit.
std::string safeName(const std::string& s) {
    std::string out;
    for (const char c : s) out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
    return out;
}

void addString(MFnDependencyNode& fn, const MString& name, const MString& value) {
    MFnTypedAttribute t;
    MObject attr = t.create(name, name, MFnData::kString);
    fn.addAttribute(attr);
    MPlug(fn.object(), attr).setString(value);
}

void addNumber(MFnDependencyNode& fn, const MString& name, double value) {
    MFnNumericAttribute n;
    MObject attr = n.create(name, name, MFnNumericData::kDouble, 0.0);
    n.setKeyable(false);
    fn.addAttribute(attr);
    MPlug(fn.object(), attr).setDouble(value);
}

void addArray(MFnDependencyNode& fn, const MString& name, const std::vector<double>& values) {
    MFnTypedAttribute t;
    MObject attr = t.create(name, name, MFnData::kDoubleArray);
    fn.addAttribute(attr);
    MFnDoubleArrayData data;
    MObject object = data.create(MDoubleArray(values.data(), static_cast<unsigned>(values.size())));
    MPlug(fn.object(), attr).setMObject(object);
}

}  // namespace

const char* ConvertCmd::kName = "groomLabConvert";

void* ConvertCmd::creator() { return new ConvertCmd(); }

MSyntax ConvertCmd::newSyntax() {
    MSyntax s;
    s.addFlag(kGuideCurves[0], kGuideCurves[1]);
    s.addFlag(kStrandCurves[0], kStrandCurves[1]);
    s.addFlag(kStrandMesh[0], kStrandMesh[1]);
    s.addFlag(kGuide[0], kGuide[1], MSyntax::kString);
    s.makeFlagMultiUse(kGuide[0]);
    s.addFlag(kGroup[0], kGroup[1], MSyntax::kString);
    s.addFlag(kFlat[0], kFlat[1]);
    s.addFlag(kNode[0], kNode[1], MSyntax::kString);
    s.addFlag(kDensity[0], kDensity[1], MSyntax::kDouble);
    s.addFlag(kMaxCount[0], kMaxCount[1], MSyntax::kLong);
    s.addFlag(kAttributes[0], kAttributes[1]);
    s.addFlag(kDegree[0], kDegree[1], MSyntax::kLong);
    s.addFlag(kNameFlag[0], kNameFlag[1], MSyntax::kString);
    s.addFlag(kStyle[0], kStyle[1], MSyntax::kString);
    s.addFlag(kSides[0], kSides[1], MSyntax::kLong);
    s.addFlag(kWidthScale[0], kWidthScale[1], MSyntax::kDouble);
    s.addFlag(kStrandUVs[0], kStrandUVs[1]);
    s.addFlag(kMake[0], kMake[1], MSyntax::kString);
    s.setObjectType(MSyntax::kSelectionList, 1, 1);
    s.useSelectionAsDefault(true);
    s.enableQuery(false);
    s.enableEdit(false);
    return s;
}

MStatus ConvertCmd::doIt(const MArgList& args) {
    return guarded("ConvertCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;
        MDagPath groomPath;
        if (!groomArgument(kName, db, &groomPath)) return MS::kFailure;
        MFnDagNode groomFn(groomPath);
        auto* shape = dynamic_cast<GroomShape*>(groomFn.userNode());
        if (!shape) return fail(kName, "the groom node is not loaded");
        const int actions = int(db.isFlagSet(kGuideCurves[0])) + int(db.isFlagSet(kStrandCurves[0])) +
                            int(db.isFlagSet(kStrandMesh[0]));
        if (actions != 1) return fail(kName, "give one of -guideCurves, -strandCurves or -strandMesh");

        MDagPath transformPath = groomPath;
        transformPath.pop();
        const std::string prefix = leafName(transformPath.partialPathName());
        int degree = 1;
        if (db.isFlagSet(kDegree[0])) db.getFlagArgument(kDegree[0], 0, degree);
        if (degree != 1 && degree != 3) return fail(kName, "-degree is 1 or 3");
        degree_ = degree;

        if (db.isFlagSet(kGuideCurves[0])) {
            const HierarchyPtr h = shape->authoredHierarchy();
            if (!h || h->guides().empty()) return fail(kName, "the groom has no guides");
            std::vector<const HierarchyGuide*> chosen;
            if (db.isFlagSet(kGuide[0])) {
                for (unsigned i = 0; i < db.numberOfFlagUses(kGuide[0]); ++i) {
                    MArgList a;
                    db.getFlagArgumentList(kGuide[0], i, a);
                    const MString text = a.asString(0);
                    const HierarchyGuide* g = h->guide(std::strtoull(text.asChar(), nullptr, 10));
                    if (!g) return fail(kName, "no guide " + text);
                    chosen.push_back(g);
                }
            } else if (db.isFlagSet(kGroup[0])) {
                const MString name = flagString(db, kGroup[0]);
                const GroupId gid = h->findGroup(name.asChar());
                if (gid == kNoGroup) return fail(kName, "no guide group named " + name);
                for (const GuideId id : h->guidesInGroup(gid)) chosen.push_back(h->guide(id));
            } else {
                for (const HierarchyGuide& g : h->guides()) chosen.push_back(&g);
            }
            // The groups' transforms: the chosen guides' groups and their ancestors, nested.
            const bool flat = db.isFlagSet(kFlat[0]);
            std::map<GroupId, int> groupIndex;
            std::function<int(GroupId)> groupOf = [&](GroupId gid) -> int {
                if (flat) return -1;
                const auto found = groupIndex.find(gid);
                if (found != groupIndex.end()) return found->second;
                const GuideGroup* g = h->group(gid);
                if (!g) return -1;
                Group made;
                made.parent = g->parent != kNoGroup ? groupOf(g->parent) : -1;
                made.name = (prefix + "_" + safeName(g->name)).c_str();
                groups_.push_back(made);
                return groupIndex[gid] = static_cast<int>(groups_.size()) - 1;
            };
            for (const HierarchyGuide* g : chosen) {
                if (g->points.size() < 2) continue;
                Curve c;
                c.points = g->points;
                c.group = groupOf(g->group);
                c.name = (prefix + "_guide" + std::to_string(g->id)).c_str();
                const GuideGroup* group = h->group(g->group);
                c.strings.emplace_back("glGuideId", std::to_string(g->id).c_str());
                c.strings.emplace_back("glGuideGroup", group ? group->name.c_str() : "");
                curves_.push_back(std::move(c));
            }
            total_ = made_ = curves_.size();
            rootName_ = flagString(db, kNameFlag[0], (prefix + "_guideCurves").c_str());
        } else {
            // A node's strands (or guides), as the groom evaluates them now.
            const std::string node = flagString(db, kNode[0]).asChar();
            const double density = flagDouble(db, kDensity[0], 1.0);
            if (density <= 0.0 || density > 1.0) return fail(kName, "-density is above 0, at most 1");
            int maxCount = 20000;
            if (db.isFlagSet(kMaxCount[0])) db.getFlagArgument(kMaxCount[0], 0, maxCount);
            if (maxCount < 1) return fail(kName, "-maxCount is at least 1");
            Value value;
            try {
                value = shape->evaluateNode(node, static_cast<float>(density));
            } catch (const std::exception& e) {
                return fail(kName, e.what());
            }
            StrandsPtr strands;
            if (const StrandsPtr* s = std::get_if<StrandsPtr>(&value); s && *s) {
                strands = *s;
            } else if (const GuidesPtr* g = std::get_if<GuidesPtr>(&value); g && *g) {
                auto set = std::make_shared<StrandSet>();
                for (const Guide& guide : (*g)->guides()) set->addStrand(guide.id, guide.points, {}, guide.root);
                strands = set;
            } else {
                return fail(kName, MString("node '") + node.c_str() + "' does not output strands or guides");
            }
            if (!strands || strands->empty()) return fail(kName, "there are no strands to convert");
            total_ = strands->strandCount();
            const std::vector<std::size_t> keep = thinStrandsById(*strands, static_cast<std::size_t>(maxCount));
            made_ = keep.size();
            const StrandSet kept = keep.size() == total_ ? *strands : strands->subset(keep);

            if (db.isFlagSet(kStrandMesh[0])) {
                StrandMeshOptions o;
                const std::string make = flagString(db, kMake[0], "auto").asChar();
                if (make == "strands") o.make = StrandMeshOptions::Make::Strands;
                else if (make == "scales") o.make = StrandMeshOptions::Make::Scales;
                else if (make == "feathers") o.make = StrandMeshOptions::Make::Feathers;
                else if (make != "auto") return fail(kName, "-make is auto, strands, scales or feathers");
                const std::string style = flagString(db, kStyle[0], "tube").asChar();
                if (style != "tube" && style != "ribbon") return fail(kName, "-style is tube or ribbon");
                o.style = style == "ribbon" ? StrandMeshOptions::Style::Ribbon : StrandMeshOptions::Style::Tube;
                int sides = 4;
                if (db.isFlagSet(kSides[0])) db.getFlagArgument(kSides[0], 0, sides);
                if (sides < 3 || sides > 64) return fail(kName, "-sides is 3 to 64");
                o.sides = sides;
                o.widthScale = static_cast<float>(flagDouble(db, kWidthScale[0], 1.0));
                if (o.widthScale <= 0.0f) return fail(kName, "-widthScale is above 0");
                o.strandUVs = db.isFlagSet(kStrandUVs[0]);
                const MeshPtr scalp = joinedScalpFromNode(groomPath.node());  // roots on every scalp of the groom
                // Scales and feathers whole (their plates and rachis are the strand set's); tubes thinned.
                const bool whole = resolveMeshMake(*strands, o) != StrandMeshOptions::Make::Strands;
                if (whole) made_ = total_;
                strandMesh_ = buildStrandMesh(whole ? *strands : kept, scalp.get(), o);
                if (strandMesh_.mesh.faceCounts.empty()) return fail(kName, "nothing to make a mesh of (no faces)");
                mesh_ = true;
                rootName_ = flagString(db, kNameFlag[0], (prefix + "_strandMesh").c_str());
            } else {
                const bool attributes = db.isFlagSet(kAttributes[0]);
                tagsOnShape_ = true;
                const std::vector<std::string> strandChannels = attributes ? kept.strandChannelNames()
                                                                           : std::vector<std::string>{};
                const std::vector<std::string> pointChannels = attributes ? kept.pointChannelNames()
                                                                          : std::vector<std::string>{};
                curves_.reserve(kept.strandCount());
                for (std::size_t k = 0; k < kept.strandCount(); ++k) {
                    const std::size_t count = kept.strandPointCount(k), begin = kept.strandBegin(k);
                    if (count < 2) continue;
                    Curve c;
                    const Vec3f* p = kept.strandPoints(k);
                    c.points.assign(p, p + count);
                    c.name = (prefix + "_strand" + std::to_string(curves_.size())).c_str();
                    if (attributes) {
                        c.numbers.reserve(strandChannels.size());
                        c.strings.emplace_back("glStrandId", std::to_string(kept.ids()[k]).c_str());
                        const auto& w = kept.widths();
                        c.arrays.emplace_back("glWidth", std::vector<double>(w.begin() + static_cast<std::ptrdiff_t>(begin),
                                                                             w.begin() + static_cast<std::ptrdiff_t>(begin + count)));
                        for (const std::string& name : strandChannels) {
                            c.numbers.emplace_back(attributeName(name), (*kept.strandChannel(name))[k]);
                        }
                        for (const std::string& name : pointChannels) {
                            const std::vector<float>& v = *kept.pointChannel(name);
                            c.arrays.emplace_back(attributeName(name),
                                                  std::vector<double>(v.begin() + static_cast<std::ptrdiff_t>(begin),
                                                                      v.begin() + static_cast<std::ptrdiff_t>(begin + count)));
                        }
                    }
                    curves_.push_back(std::move(c));
                }
                made_ = curves_.size();
                rootName_ = flagString(db, kNameFlag[0], (prefix + "_strandCurves").c_str());
            }
        }
        if (!mesh_ && curves_.empty()) return fail(kName, "nothing to convert");
        undoable_ = true;
        return redoIt();
    });
}

MStatus ConvertCmd::build() {
    MStatus status;
    MFnTransform rootFn;
    MObject root = rootFn.create(MObject::kNullObj, &status);
    if (!status) return status;
    root_ = MObjectHandle(root);
    rootName_ = rootFn.setName(rootName_);

    if (mesh_) {
        const RachisMesh& m = strandMesh_.mesh;
        MFloatPointArray points(static_cast<unsigned>(m.points.size()));
        for (std::size_t i = 0; i < m.points.size(); ++i) points.set(static_cast<unsigned>(i), m.points[i].x, m.points[i].y, m.points[i].z);
        const MIntArray counts(m.faceCounts.data(), static_cast<unsigned>(m.faceCounts.size()));
        const MIntArray connects(m.faceVertices.data(), static_cast<unsigned>(m.faceVertices.size()));
        const MFloatArray u(m.u.data(), static_cast<unsigned>(m.u.size())), v(m.v.data(), static_cast<unsigned>(m.v.size()));
        MFnMesh meshFn;
        meshFn.create(static_cast<int>(points.length()), static_cast<int>(counts.length()), points, counts, connects,
                      u, v, root, &status);
        if (!status) return status;
        const MIntArray uvIds(m.faceUVs.data(), static_cast<unsigned>(m.faceUVs.size()));
        meshFn.assignUVs(counts, uvIds);
        if (!strandMesh_.u2.empty()) {
            MString set("strandUVs");
            meshFn.createUVSet(set);
            const MFloatArray u2(strandMesh_.u2.data(), static_cast<unsigned>(strandMesh_.u2.size()));
            const MFloatArray v2(strandMesh_.v2.data(), static_cast<unsigned>(strandMesh_.v2.size()));
            meshFn.setUVs(u2, v2, &set);
            meshFn.assignUVs(counts, uvIds, &set);
        }
        meshFn.setName(rootName_ + "Shape");
        // The default shader, assigned here with the node it belongs to: a sets command run
        // after this one in the same undo step would keep the node undo deletes and redo
        // makes anew, and Maya reads the freed node when it flushes the undo queue.
        MSelectionList shaders;
        MObject shadingGroup;
        if (shaders.add("initialShadingGroup") == MS::kSuccess && shaders.getDependNode(0, shadingGroup) == MS::kSuccess) {
            MDagPath shapePath;
            if (MDagPath::getAPathTo(meshFn.object(), shapePath) == MS::kSuccess) MFnSet(shadingGroup).addMember(shapePath);
        }
        return MS::kSuccess;
    }

    std::vector<MObject> groups(groups_.size());
    for (std::size_t i = 0; i < groups_.size(); ++i) {  // parents come first (made before their children)
        MFnTransform fn;
        groups[i] = fn.create(groups_[i].parent >= 0 ? groups[static_cast<std::size_t>(groups_[i].parent)] : root, &status);
        if (!status) return status;
        fn.setName(groups_[i].name);
    }
    MPointArray cvs;
    MDoubleArray knots;
    for (const Curve& c : curves_) {
        MFnTransform tfn;
        MObject transform = tfn.create(c.group >= 0 ? groups[static_cast<std::size_t>(c.group)] : root, &status);
        if (!status) return status;
        tfn.setName(c.name);
        const int n = static_cast<int>(c.points.size());
        const int degree = std::min(degree_, n - 1);
        cvs.setLength(static_cast<unsigned>(n));
        for (int i = 0; i < n; ++i) cvs.set(static_cast<unsigned>(i), c.points[i].x, c.points[i].y, c.points[i].z);
        MFnNurbsCurve cfn;
        MObject curve;
        if (degree == 1) {
            knots.setLength(static_cast<unsigned>(n));
            for (int i = 0; i < n; ++i) knots[static_cast<unsigned>(i)] = i;
            curve = cfn.create(cvs, knots, 1, MFnNurbsCurve::kOpen, false, false, transform, &status);
        } else {
            curve = cfn.createWithEditPoints(cvs, degree, MFnNurbsCurve::kOpen, false, false, false, transform, &status);
        }
        if (!status) return status;
        cfn.setName(c.name + "Shape");
        for (const auto& [name, value] : c.strings) {
            if (tagsOnShape_) {
                addString(cfn, name, value);
            } else {
                addString(tfn, name, value);
            }
        }
        for (const auto& [name, value] : c.numbers) addNumber(cfn, name, value);
        for (const auto& [name, values] : c.arrays) addArray(cfn, name, values);
    }
    return MS::kSuccess;
}

MStatus ConvertCmd::redoIt() {
    // Built from the data kept (doIt and every redo): undo deletes the nodes outright.
    return guarded("ConvertCmd::redoIt", [&]() -> MStatus {
        const MStatus built = build();
        if (!built) {
            undoIt();
            return fail(kName, "could not make the nodes");
        }
        MDagPath path;
        MDagPath::getAPathTo(root_.object(), path);
        MStringArray result;
        result.append(path.fullPathName());
        result.append(MString() + static_cast<unsigned>(made_));
        result.append(MString() + static_cast<unsigned>(total_));
        clearResult();
        setResult(result);
        return MS::kSuccess;
    });
}

MStatus ConvertCmd::undoIt() {
    // Deleted through a modifier of this call alone, discarded at once (redo rebuilds from the
    // data kept): never by running a command from inside an undo, which leaves Maya's undo
    // state pointing at freed nodes, and never by a modifier kept holding them, which brings
    // Maya down when the undo queue is flushed with it.
    return guarded("ConvertCmd::undoIt", [&]() -> MStatus {
        if (!root_.isValid() || !root_.isAlive()) return MS::kSuccess;
        MObject root = root_.object();
        root_ = MObjectHandle();
        MDagModifier remove;
        MStatus status = remove.deleteNode(root);
        if (!status) return status;
        return remove.doIt();
    });
}

}  // namespace groomlab::maya