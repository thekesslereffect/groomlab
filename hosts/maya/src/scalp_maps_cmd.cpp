// SPDX-License-Identifier: Apache-2.0
#include "scalp_maps_cmd.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <maya/MArgDatabase.h>
#include <maya/MArgList.h>
#include <maya/MDagPath.h>
#include <maya/MDoubleArray.h>
#include <maya/MFnDagNode.h>
#include <maya/MGlobal.h>
#include <maya/MPlug.h>
#include <maya/MSelectionList.h>
#include <maya/MStringArray.h>

#include "asset_files.h"
#include "groom_shape.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/asset_files.h"
#include "groomlab/texture.h"
#include "command_args.h"
#include "guarded.h"
#include "maya_inputs.h"
#include "groomlab/scalp_maps.h"

namespace groomlab::maya {

namespace {

const char* kList[] = {"-ls", "-list"};
const char* kKindOf[] = {"-ko", "-kindOf"};
const char* kAdd[] = {"-mk", "-make"};  // "-add" and "-new" are refused by MSyntax
const char* kKind[] = {"-k", "-kind"};
const char* kValue[] = {"-v", "-value"};
const char* kResolution[] = {"-res", "-resolution"};
const char* kTexture[] = {"-tx", "-texture"};
const char* kRemove[] = {"-rm", "-remove"};
const char* kRename[] = {"-rn", "-rename"};
const char* kFill[] = {"-f", "-fill"};
const char* kConvert[] = {"-cv", "-convert"};
const char* kValues[] = {"-val", "-values"};
const char* kColor[] = {"-clr", "-color"};
const char* kFillColor[] = {"-fc", "-fillColor"};
const char* kFillChannels[] = {"-fch", "-fillChannels"};
const char* kPaintMode[] = {"-pmd", "-paintMode"};
const char* kChannelsOf[] = {"-cho", "-channelsOf"};
const char* kSetChannels[] = {"-sch", "-setChannels"};
const char* kRelink[] = {"-rl", "-relink"};
const char* kCopyFrom[] = {"-cf", "-copyFrom"};
const char* kDeleteFile[] = {"-df", "-deleteFile"};
// The scalp's faces (polygon indices, space separated): -make makes the map 0 with -value (or
// -fillColor) on those faces only; -fill fills those faces only. A guide layer: the guides
// rooted on them.
const char* kFaces[] = {"-fcs", "-faces"};


bool hasLayer(const GuideHierarchy& h, const std::string& name) {
    const std::vector<std::string> names = h.attributeNames();
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::string kindOf(const ScalpMaps& maps, const GuideHierarchy& h, const std::string& name) {
    const auto it = maps.maps.find(name);
    if (it != maps.maps.end()) return it->second.kind;
    return hasLayer(h, name) ? "guides" : "";
}

// 1 (gray) or 4 (color); guide layers are gray; 0 for no map.
int channelsOf(const ScalpMaps& maps, const GuideHierarchy& h, const std::string& name) {
    const auto it = maps.maps.find(name);
    if (it != maps.maps.end()) return it->second.channels == kColorMap ? kColorMap : kGrayMap;
    return hasLayer(h, name) ? kGrayMap : 0;
}

// `count` vertices of a map with `channels`, every one `color` (a gray map: its r).
std::vector<float> filledVertexMap(std::size_t count, int channels, const PaintColor& color) {
    std::vector<float> out;
    out.reserve(count * static_cast<std::size_t>(channels));
    for (std::size_t i = 0; i < count; ++i) {
        if (channels == kColorMap) {
            out.insert(out.end(), color.begin(), color.end());
        } else {
            out.push_back(color[0]);
        }
    }
    return out;
}

}  // namespace

const char* ScalpMapsCmd::kName = "groomLabScalpMaps";

void* ScalpMapsCmd::creator() { return new ScalpMapsCmd(); }

MSyntax ScalpMapsCmd::newSyntax() {
    MSyntax s;
    s.addFlag(kList[0], kList[1]);
    s.addFlag(kKindOf[0], kKindOf[1], MSyntax::kString);
    s.addFlag(kAdd[0], kAdd[1], MSyntax::kString);
    s.addFlag(kKind[0], kKind[1], MSyntax::kString);
    s.addFlag(kValue[0], kValue[1], MSyntax::kDouble);
    s.addFlag(kResolution[0], kResolution[1], MSyntax::kLong);
    s.addFlag(kTexture[0], kTexture[1], MSyntax::kString);
    s.addFlag(kRemove[0], kRemove[1], MSyntax::kString);
    s.addFlag(kRename[0], kRename[1], MSyntax::kString, MSyntax::kString);
    s.addFlag(kFill[0], kFill[1], MSyntax::kString);
    s.addFlag(kConvert[0], kConvert[1], MSyntax::kString);
    s.addFlag(kValues[0], kValues[1], MSyntax::kString);
    s.addFlag(kColor[0], kColor[1]);
    s.addFlag(kFillColor[0], kFillColor[1], MSyntax::kDouble, MSyntax::kDouble, MSyntax::kDouble, MSyntax::kDouble);
    s.addFlag(kFillChannels[0], kFillChannels[1], MSyntax::kString);
    s.addFlag(kPaintMode[0], kPaintMode[1], MSyntax::kString);
    s.addFlag(kChannelsOf[0], kChannelsOf[1], MSyntax::kString);
    s.addFlag(kSetChannels[0], kSetChannels[1], MSyntax::kString, MSyntax::kLong);
    s.addFlag(kRelink[0], kRelink[1], MSyntax::kString);
    s.addFlag(kCopyFrom[0], kCopyFrom[1], MSyntax::kString);
    s.addFlag(kDeleteFile[0], kDeleteFile[1]);
    s.addFlag(kFaces[0], kFaces[1], MSyntax::kString);
    s.setObjectType(MSyntax::kSelectionList, 0, 1);
    s.useSelectionAsDefault(true);
    s.enableQuery(false);
    s.enableEdit(false);
    return s;
}

MStatus ScalpMapsCmd::doIt(const MArgList& args) {
    return groomlab::maya::guarded("ScalpMapsCmd::doIt", [&]() -> MStatus {
        MStatus status;
        MArgDatabase db(syntax(), args, &status);
        if (!status) return status;

        MDagPath groomPath;
        if (!groomArgument(kName, db, &groomPath)) return MS::kFailure;
        MFnDagNode groomFn(groomPath);
        auto* shape = dynamic_cast<GroomShape*>(groomFn.userNode());
        if (!shape) return fail(kName, "the groom node is not loaded");

        const int actions = int(db.isFlagSet(kList[0])) + int(db.isFlagSet(kKindOf[0])) + int(db.isFlagSet(kAdd[0])) +
                            int(db.isFlagSet(kRemove[0])) + int(db.isFlagSet(kRename[0])) + int(db.isFlagSet(kFill[0])) +
                            int(db.isFlagSet(kConvert[0])) + int(db.isFlagSet(kValues[0])) + int(db.isFlagSet(kChannelsOf[0])) +
                            int(db.isFlagSet(kSetChannels[0])) + int(db.isFlagSet(kRelink[0]));
        if (actions != 1) return fail(kName, "give exactly one action flag");

        // Questions about the maps: the shape's parses of scalpMaps and guideData (kept while they
        // are unchanged); nothing evaluates.
        if (db.isFlagSet(kList[0]) || db.isFlagSet(kKindOf[0]) || db.isFlagSet(kChannelsOf[0])) {
            std::string error;
            const ScalpMaps& stored = shape->scalpMaps(&error);
            if (!error.empty()) return fail(kName, error.c_str());
            const std::shared_ptr<const GuideHierarchy> layers = shape->storedHierarchy(&error);
            if (!layers) return fail(kName, error.c_str());
            if (db.isFlagSet(kKindOf[0])) {
                setResult(MString(kindOf(stored, *layers, flagString(db, kKindOf[0]).asChar()).c_str()));
            } else if (db.isFlagSet(kChannelsOf[0])) {
                setResult(channelsOf(stored, *layers, flagString(db, kChannelsOf[0]).asChar()));
            } else {
                MStringArray out;
                for (const std::string& layer : layers->attributeNames()) {
                    if (stored.maps.count(layer)) continue;  // a stored map of the same name wins
                    out.append((layer + "|guides||1").c_str());
                }
                for (const auto& [name, m] : stored.maps) {
                    out.append((name + "|" + m.kind + "|" + m.texture + "|" + std::to_string(m.channels == kColorMap ? 4 : 1)).c_str());
                }
                setResult(out);
            }
            return MS::kSuccess;
        }

        // The stored maps, the guides (their layers), and the scalp they are painted on.
        MPlug mapsPlug = groomFn.findPlug("scalpMaps", true);
        MPlug guidesPlug = groomFn.findPlug("guideData", true);
        ScalpMaps maps;
        try {
            maps = ScalpMaps::fromJson(mapsPlug.asString().asChar());
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }
        const MeshPtr scalp = shape->evaluatedCageScalp();  // vertex maps are stored per vertex of the cage
        refreshMayaPaths();  // texture files resolve against the scene's folder and project as they are now
        std::string error;
        std::shared_ptr<GuideHierarchy> h = toHierarchy(guidesPlug.asString().asChar(), scalp.get(), &error, groomScaleOf(shape->thisMObject()));
        if (!h) return fail(kName, error.c_str());

        if (db.isFlagSet(kValues[0])) {
            const std::string name = flagString(db, kValues[0]).asChar();
            const auto it = maps.maps.find(name);
            if (it == maps.maps.end() || it->second.kind != "vertices") return fail(kName, ("no vertex map named " + name).c_str());
            MDoubleArray out;
            for (const float v : it->second.values) out.append(v);
            setResult(out);
            return MS::kSuccess;
        }

        // Edits: what changes is written back at the end (maps, guides), each undoable.
        noteFileWritten();  // texture files may change: what kept their bytes reads them again
        bool mapsChanged = false, guidesChanged = false, relinked = false;
        const float value = static_cast<float>(std::clamp(db.isFlagSet(kValue[0]) ? [&] { double d = 0; db.getFlagArgument(kValue[0], 0, d); return d; }() : 0.0, 0.0, 1.0));
        int resolution = 1024;
        if (db.isFlagSet(kResolution[0])) db.getFlagArgument(kResolution[0], 0, resolution);
        resolution = std::clamp(resolution, 4, 16384);
        const std::string texturePath = flagString(db, kTexture[0]).asChar();
        // A color: -fillColor, else the value as gray (alpha 1).
        PaintColor color{value, value, value, 1.0f};
        if (db.isFlagSet(kFillColor[0])) {
            for (unsigned k = 0; k < 4; ++k) {
                double d = 0.0;
                db.getFlagArgument(kFillColor[0], k, d);
                color[k] = static_cast<float>(std::clamp(d, 0.0, 1.0));
            }
        }
        unsigned fillMask = kAllChannels;
        if (db.isFlagSet(kFillChannels[0])) {
            try {
                fillMask = channelMask(flagString(db, kFillChannels[0]).asChar());
            } catch (const std::invalid_argument& e) {
                return fail(kName, (std::string("-fillChannels: ") + e.what()).c_str());
            }
        }
        PaintMode paintMode = PaintMode::Replace;
        if (db.isFlagSet(kPaintMode[0])) {
            try {
                paintMode = paintModeFromString(flagString(db, kPaintMode[0]).asChar());
            } catch (const std::invalid_argument& e) {
                return fail(kName, (std::string("-paintMode: ") + e.what()).c_str());
            }
        }
        const MObject groomNode = groomPath.node();
        auto resolve = [&](const std::string& stored) { return resolveNodePath(groomNode, stored); };
        // A new file never replaces one that is there (another groom's, or a map removed
        // earlier): it gets a free name beside it, stored the way the given path was.
        auto freshTexture = [&](const std::string& stored) {
            const std::string full = resolve(stored);
            const std::string free = freeFilePath(full);
            return free == full ? stored : portableMayaPath(free);
        };
        auto texturePaint = [&](const std::string& stored, int channels) -> TexturePaint {
            if (!scalp) throw std::runtime_error("the groom has no scalp mesh");
            return *openTexturePaint(*scalp, resolve(stored), channels, resolution);  // as every host opens it (scalp_paint.h)
        };
        // Writes the file; one that was there keeps its bytes for undo. Only ever a PNG (what
        // painting writes): a map linked to another format is relinked to a PNG copy first.
        auto writeTexture = [&](const TexturePaint& paint, const std::string& stored) {
            const std::string full = resolve(stored);
            if (!isPngPath(full)) {
                throw std::runtime_error(full + " is not a PNG, and a texture map is written as PNG: link the map to "
                                                "its image again (groomlab.set_map_texture paints a PNG copy)");
            }
            std::error_code ec;
            const std::filesystem::path folder = std::filesystem::u8path(full).parent_path();
            if (!folder.empty()) std::filesystem::create_directories(folder, ec);
            std::vector<std::uint8_t> before = fileStamp(full) != 0 ? fileBytes(full) : std::vector<std::uint8_t>{};
            paint.save(full);
            if (!before.empty()) rewritten_.push_back({full, std::move(before), fileBytes(full)});
        };
        // Before rewriting a texture map's file another groom reads too: this groom's own copy.
        auto ownFile = [&](const std::string& name) {
            const std::string full = resolve(maps.maps[name].texture);
            if (!fileUsedByOtherGroom(groomNode, full)) return false;
            maps.maps[name].texture = portableMayaPath(forkTextureFile(groomPath, name, full));
            return true;
        };
        const bool makeColor = db.isFlagSet(kColor[0]);
        // -faces: the polygons a make or fill is limited to (none given: everywhere).
        const bool onFaces = db.isFlagSet(kFaces[0]);
        std::vector<std::int32_t> faces;
        if (onFaces) {
            std::istringstream words(flagString(db, kFaces[0]).asChar());
            long long f = 0;
            while (words >> f) {
                if (f >= 0 && f <= std::numeric_limits<std::int32_t>::max()) faces.push_back(static_cast<std::int32_t>(f));
            }
            if (faces.empty()) return fail(kName, "-faces names no face");
            if (!scalp) return fail(kName, "the groom has no scalp mesh");
        }
        // A guide layer on faces: the guides rooted on them (their root's triangle in one).
        const std::vector<std::uint8_t> faceTriangles = onFaces ? polygonTriangles(*scalp, faces) : std::vector<std::uint8_t>{};
        auto rootedOnFaces = [&](const HierarchyGuide& g) {
            return g.root.face >= 0 && static_cast<std::size_t>(g.root.face) < faceTriangles.size() &&
                   faceTriangles[static_cast<std::size_t>(g.root.face)] != 0;
        };

        try {
            if (db.isFlagSet(kAdd[0])) {
                const std::string name = flagString(db, kAdd[0]).asChar();
                const std::string kind = flagString(db, kKind[0], "guides").asChar();
                if (!validMapName(name)) return fail(kName, mapNameError(name).c_str());  // the hosts' one rule (scalp_maps.h)
                if (!kindOf(maps, *h, name).empty()) return fail(kName, ("the groom already has a map named " + name).c_str());
                if (makeColor && kind == "guides") {
                    return fail(kName, "a guide layer is gray: make a color map on the scalp's vertices or as a texture");
                }
                // On faces: 0 (a color map: black, alpha 1) with the value or color on those faces.
                const PaintColor base{0.0f, 0.0f, 0.0f, 1.0f};
                if (kind == "guides") {
                    for (const HierarchyGuide& g : h->guides()) {
                        if (h->groupDepth(g.group) != 0) continue;  // children inherit
                        h->setAttribute(g.id, name, !onFaces || rootedOnFaces(g) ? value : 0.0f);
                    }
                    guidesChanged = true;
                } else if (kind == "vertices") {
                    if (!scalp) return fail(kName, "the groom has no scalp mesh");
                    const int channels = makeColor ? kColorMap : kGrayMap;
                    ScalpMapEntry entry{"vertices", filledVertexMap(scalp->pointCount(), channels, onFaces ? base : color), ""};
                    if (onFaces) fillVertexMapFaces(entry.values, channels, kAllChannels, color, PaintMode::Replace, *scalp, faces);
                    entry.channels = channels;
                    maps.maps[name] = std::move(entry);
                    mapsChanged = true;
                } else if (kind == "texture") {
                    if (texturePath.empty()) return fail(kName, "a texture map needs -texture <file>");
                    if (!scalp) return fail(kName, "the groom has no scalp mesh");
                    TexturePaint paint = makeColor ? TexturePaint(*scalp, resolution, resolution, onFaces ? base : color)
                                                   : TexturePaint(*scalp, resolution, resolution, onFaces ? 0.0f : value);
                    if (onFaces) paint.fillTriangles(faceTriangles, color, kAllChannels);
                    const std::string stored = freshTexture(texturePath);
                    writeTexture(paint, stored);
                    ScalpMapEntry entry{"texture", {}, stored};
                    entry.channels = makeColor ? kColorMap : kGrayMap;
                    maps.maps[name] = std::move(entry);
                    mapsChanged = true;
                } else {
                    return fail(kName, "-kind takes guides, vertices or texture");
                }
            } else if (db.isFlagSet(kRemove[0])) {
                const std::string name = flagString(db, kRemove[0]).asChar();
                const std::string kind = kindOf(maps, *h, name);
                if (kind.empty()) return fail(kName, ("no map named " + name).c_str());
                if (kind == "guides") {
                    h->removeAttribute(name);
                    guidesChanged = true;
                } else {
                    // A texture map's file stays on disk, unless -deleteFile and nothing else
                    // reads it (another map of this groom, another groom): its bytes kept for undo.
                    const std::string file = kind == "texture" ? resolve(maps.maps[name].texture) : "";
                    maps.maps.erase(name);
                    mapsChanged = true;
                    const bool shared = !file.empty() && (fileUsedByOtherGroom(groomNode, file) ||
                        std::any_of(maps.maps.begin(), maps.maps.end(), [&](const auto& m) {
                            return m.second.kind == "texture" && sameFile(resolve(m.second.texture), file);
                        }));
                    if (db.isFlagSet(kDeleteFile[0]) && !file.empty() && !shared && fileStamp(file) != 0) {
                        deleted_ = {file, fileBytes(file), {}};
                    }
                }
            } else if (db.isFlagSet(kRename[0])) {
                MString from, to;
                db.getFlagArgument(kRename[0], 0, from);
                db.getFlagArgument(kRename[0], 1, to);
                const std::string kind = kindOf(maps, *h, from.asChar());
                if (kind.empty()) return fail(kName, "no map named " + from);
                if (!validMapName(to.asChar())) return fail(kName, mapNameError(to.asChar()).c_str());
                if (!kindOf(maps, *h, to.asChar()).empty()) return fail(kName, "the groom already has a map named " + to);
                if (kind == "guides") {
                    h->renameAttribute(from.asChar(), to.asChar());
                    guidesChanged = true;
                } else {
                    auto node = maps.maps.extract(from.asChar());
                    node.key() = to.asChar();
                    // A texture map's file follows its name when -texture gives the new file:
                    // moved on disk (and back on undo), so the name on disk and in the Paint tab agree.
                    // A file another groom reads too is copied instead (it keeps its file).
                    if (kind == "texture" && !texturePath.empty() && texturePath != node.mapped().texture) {
                        const std::string source = resolve(node.mapped().texture);
                        const std::string target = resolve(texturePath);
                        if (fileStamp(source) != 0) {
                            if (fileStamp(target) != 0) return fail(kName, ("a file is in the way: " + target).c_str());
                            copied_ = fileUsedByOtherGroom(groomNode, source);
                            std::error_code ec;
                            std::filesystem::create_directories(std::filesystem::u8path(target).parent_path(), ec);
                            if (copied_) {
                                std::filesystem::copy_file(std::filesystem::u8path(source), std::filesystem::u8path(target), ec);
                            } else {
                                std::filesystem::rename(std::filesystem::u8path(source), std::filesystem::u8path(target), ec);
                            }
                            if (ec) return fail(kName, ("could not rename " + source + ": " + ec.message()).c_str());
                            moved_ = {source, target};
                        }
                        node.mapped().texture = texturePath;
                    }
                    maps.maps.insert(std::move(node));
                    mapsChanged = true;
                }
            } else if (db.isFlagSet(kFill[0])) {
                const std::string name = flagString(db, kFill[0]).asChar();
                const std::string kind = kindOf(maps, *h, name);
                if (kind.empty()) return fail(kName, ("no map named " + name).c_str());
                const bool byColor = db.isFlagSet(kFillColor[0]);
                if (!db.isFlagSet(kValue[0]) && !byColor) return fail(kName, "-fill needs -value or -fillColor");
                const int channels = channelsOf(maps, *h, name);
                if (byColor && channels != kColorMap) {
                    return fail(kName, ("-fillColor fills a color map; " + name + " is gray (-value fills it)").c_str());
                }
                // A color map: the color in the channels asked for, or a value in r g b (the alpha kept).
                const unsigned mask = byColor ? fillMask : (fillMask & 0x7u);
                if (kind == "guides") {
                    for (const HierarchyGuide& g : h->guides()) {
                        if (onFaces && !rootedOnFaces(g)) continue;
                        float current = 0.0f;
                        h->effectiveAttribute(g, name, &current);
                        h->setAttribute(g.id, name, compositePaintValue(current, value, paintMode));
                    }
                    guidesChanged = true;
                } else if (kind == "vertices") {
                    ScalpMapEntry& entry = maps.maps[name];
                    const std::size_t per = static_cast<std::size_t>(channels);
                    if (scalp && entry.values.size() != scalp->pointCount() * per) {
                        // Refitted to this scalp.
                        entry.values = filledVertexMap(scalp->pointCount(), channels, PaintColor{0.0f, 0.0f, 0.0f, 1.0f});
                    }
                    const PaintColor target = per == 1 ? PaintColor{value, value, value, value} : color;
                    if (onFaces) {
                        fillVertexMapFaces(entry.values, channels, mask, target, paintMode, *scalp, faces);
                    } else {
                        fillMapValues(entry.values, channels, mask, target, paintMode);
                    }
                    mapsChanged = true;
                } else {
                    // A file another groom reads too: this groom gets its own copy first.
                    mapsChanged = ownFile(name) || mapsChanged;
                    TexturePaint paint = texturePaint(maps.maps[name].texture, channels);
                    if (onFaces) paint.fillTriangles(faceTriangles, channels == kColorMap ? color : PaintColor{value, value, value, value}, mask, paintMode);
                    else if (channels == kColorMap) paint.fill(color, mask, paintMode);
                    else paint.fill(value, paintMode);
                    writeTexture(paint, maps.maps[name].texture);
                }
            } else if (db.isFlagSet(kConvert[0])) {
                const std::string name = flagString(db, kConvert[0]).asChar();
                const std::string from = kindOf(maps, *h, name);
                const std::string to = flagString(db, kKind[0]).asChar();
                if (from.empty()) return fail(kName, ("no map named " + name).c_str());
                if (to != "guides" && to != "vertices" && to != "texture") return fail(kName, "-kind takes guides, vertices or texture");
                if (!scalp) return fail(kName, "the groom has no scalp mesh");
                if (from == to) {
                    setResult(MString(to.c_str()));
                    return MS::kSuccess;
                }
                // The source as a function on the scalp: per vertex, or an image; gray or color.
                std::vector<float> vertices;
                TexturePtr image;
                const GuideSet flat = from == "guides" ? h->flatten() : GuideSet{};
                const int channels = channelsOf(maps, *h, name);
                if (from == "vertices") {
                    vertices = maps.maps[name].values;
                    if (vertices.size() != scalp->pointCount() * static_cast<std::size_t>(channels)) {
                        return fail(kName, ("map " + name + " does not fit this scalp").c_str());
                    }
                } else if (from == "texture") {
                    const std::string full = resolve(maps.maps[name].texture);
                    if (fileStamp(full) == 0) return fail(kName, ("map " + name + "'s image is missing: " + full).c_str());
                    image = loadTexture(full);
                }
                // Into the target.
                if (to == "vertices") {
                    std::vector<float> values = from == "guides" ? vertexMapFromGuides(flat, name, *scalp)
                                                                 : vertexMapFromTexture(*image, *scalp, channels);
                    ScalpMapEntry entry{"vertices", std::move(values), ""};
                    entry.channels = channels;
                    maps.maps[name] = std::move(entry);
                } else if (to == "texture") {
                    const std::string wanted = !texturePath.empty() ? texturePath : maps.maps.count(name) ? maps.maps[name].texture : std::string();
                    if (wanted.empty()) return fail(kName, "a texture map needs -texture <file>");
                    const std::string stored = freshTexture(wanted);
                    TexturePaint paint = from == "guides" ? textureFromGuides(flat, name, *scalp, resolution, resolution)
                                                          : textureFromVertexMap(vertices, channels, *scalp, resolution, resolution);
                    writeTexture(paint, stored);
                    ScalpMapEntry entry{"texture", {}, stored};
                    entry.channels = channels;
                    maps.maps[name] = std::move(entry);
                } else {  // guides: gray, a color map's luminance
                    if (channels == kColorMap) {
                        MGlobal::displayWarning(MString(kName) + ": " + name.c_str() +
                                                " is a color map; on the guides it keeps its luminance (a guide layer is gray)");
                        if (from == "vertices") vertices = convertMapChannels(vertices, kColorMap, kGrayMap);
                    }
                    std::vector<StrandRoot> roots;
                    for (const HierarchyGuide& g : h->guides()) roots.push_back(g.root);
                    const std::vector<float> values = from == "vertices" ? rootValuesFromVertexMap(vertices, *scalp, roots)
                                                                          : rootValuesFromTexture(*image, *scalp, roots);
                    std::size_t i = 0;
                    for (const HierarchyGuide& g : h->guides()) h->setAttribute(g.id, name, values[i++]);
                    guidesChanged = true;
                    maps.maps.erase(name);
                }
                if (from == "guides") {
                    h->removeAttribute(name);
                    guidesChanged = true;
                }
                mapsChanged = true;
                setResult(MString(to.c_str()));
            } else if (db.isFlagSet(kRelink[0])) {
                // The map reads another image: a PNG in place, or a PNG copy of any image.
                const std::string name = flagString(db, kRelink[0]).asChar();
                const std::string kind = kindOf(maps, *h, name);
                if (kind.empty()) return fail(kName, ("no map named " + name).c_str());
                if (texturePath.empty()) return fail(kName, "-relink needs -texture <file>");
                if (!isPngPath(texturePath)) {
                    return fail(kName, "a texture map's file is a PNG (painting writes PNG): give another image as "
                                       "-copyFrom and a .png path to copy it to");
                }
                const int channels = channelsOf(maps, *h, name);  // a guide layer's: gray
                std::string stored = texturePath;
                if (db.isFlagSet(kCopyFrom[0])) {
                    const std::string source = resolve(flagString(db, kCopyFrom[0]).asChar());
                    if (fileStamp(source) == 0) return fail(kName, ("no image at " + source).c_str());
                    const TexturePtr image = loadTexture(source);  // throws when it is not an image
                    if (image->isUdim()) return fail(kName, "a map's image is one file over UVs 0-1, not a UDIM tile set");
                    stored = freshTexture(texturePath);
                    const std::string full = resolve(stored);
                    std::error_code ec;
                    const std::filesystem::path folder = std::filesystem::u8path(full).parent_path();
                    if (!folder.empty()) std::filesystem::create_directories(folder, ec);
                    if (isPngPath(source)) {
                        std::filesystem::copy_file(std::filesystem::u8path(source), std::filesystem::u8path(full), ec);
                        if (ec) return fail(kName, ("could not copy " + source + ": " + ec.message()).c_str());
                    } else {
                        writeImagePng(*image, full);
                    }
                    created_ = {full, {}, fileBytes(full)};
                } else {
                    const std::string full = resolve(stored);
                    if (fileStamp(full) == 0) return fail(kName, ("no image at " + full).c_str());
                    if (loadTexture(full)->isUdim()) {
                        return fail(kName, "a map's image is one file over UVs 0-1, not a UDIM tile set");
                    }
                }
                if (kind == "guides") {
                    h->removeAttribute(name);
                    guidesChanged = true;
                }
                ScalpMapEntry entry{"texture", {}, stored};
                entry.channels = channels;
                maps.maps[name] = std::move(entry);
                mapsChanged = relinked = true;
                setResult(MString(stored.c_str()));
            } else if (db.isFlagSet(kSetChannels[0])) {
                MString nameArg;
                int wanted = 0;
                db.getFlagArgument(kSetChannels[0], 0, nameArg);
                db.getFlagArgument(kSetChannels[0], 1, wanted);
                const std::string name = nameArg.asChar();
                const std::string kind = kindOf(maps, *h, name);
                if (kind.empty()) return fail(kName, ("no map named " + name).c_str());
                if (wanted != kGrayMap && wanted != kColorMap) return fail(kName, "-setChannels takes 1 (gray) or 4 (color)");
                if (kind == "guides") {
                    if (wanted == kColorMap) {
                        return fail(kName, ("a guide layer is gray: convert " + name + " to vertices or a texture first").c_str());
                    }
                    setResult(kGrayMap);
                    return MS::kSuccess;
                }
                const int channels = channelsOf(maps, *h, name);
                if (channels != wanted) {
                    ScalpMapEntry& entry = maps.maps[name];
                    if (kind == "vertices") {
                        entry.values = convertMapChannels(entry.values, channels, wanted);
                    } else {
                        ownFile(name);
                        TexturePaint paint = texturePaint(entry.texture, channels);
                        paint.setChannels(wanted);
                        writeTexture(paint, entry.texture);
                    }
                    entry.channels = wanted;
                    mapsChanged = true;
                }
                setResult(wanted);
            }
        } catch (const std::exception& e) {
            return fail(kName, e.what());
        }

        if (mapsChanged) modifier_.newPlugValueString(mapsPlug, maps.toJson().c_str());
        if (guidesChanged) modifier_.newPlugValueString(guidesPlug, h->toJson().c_str());
        if ((!mapsChanged && !guidesChanged) || relinked) {
            // A texture rewritten in place, or another image read: the groom reads it again (the
            // Map node's cache key has the file's stamp).
            MPlug tick = groomFn.findPlug("brushTick", true);
            modifier_.newPlugValueInt(tick, tick.asInt() + 1);
        }
        undoable_ = true;
        MStatus done = modifier_.doIt();
        removeDeleted();
        return done;
    });
}

namespace {

// A renamed texture map's file, moved from -> to (a no-op when either is empty or the move fails:
// the map then names a file that is not there, and the Map node reports it).
void moveFile(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty()) return;
    std::error_code ec;
    std::filesystem::rename(std::filesystem::u8path(from), std::filesystem::u8path(to), ec);
    if (ec) MGlobal::displayWarning(MString(ScalpMapsCmd::kName) + ": could not move " + from.c_str() + " to " + to.c_str());
}

// A renamed map's shared file, copied from -> to (redo), or the copy removed (undo: to only).
void copyFile(const std::string& from, const std::string& to) {
    if (to.empty()) return;
    std::error_code ec;
    if (from.empty()) {
        std::filesystem::remove(std::filesystem::u8path(to), ec);
        return;
    }
    std::filesystem::copy_file(std::filesystem::u8path(from), std::filesystem::u8path(to),
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) MGlobal::displayWarning(MString(ScalpMapsCmd::kName) + ": could not copy " + from.c_str() + " to " + to.c_str());
}

}  // namespace

void ScalpMapsCmd::removeDeleted() {
    if (deleted_.path.empty()) return;
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(deleted_.path), ec);
    if (ec) MGlobal::displayWarning(MString(kName) + ": could not delete " + deleted_.path.c_str());
}

MStatus ScalpMapsCmd::redoIt() {
    return groomlab::maya::guarded("ScalpMapsCmd::redoIt", [&]() -> MStatus {
        noteFileWritten();  // texture files may change: what kept their bytes reads them again
        for (const FileEdit& edit : rewritten_) putFileBytes(edit.path, edit.after, kName);
        if (!created_.path.empty()) putFileBytes(created_.path, created_.after, kName);
        if (copied_) {
            copyFile(moved_.first, moved_.second);
        } else {
            moveFile(moved_.first, moved_.second);
        }
        MStatus done = modifier_.doIt();
        removeDeleted();
        return done;
    });
}

MStatus ScalpMapsCmd::undoIt() {
    return groomlab::maya::guarded("ScalpMapsCmd::undoIt", [&]() -> MStatus {
        noteFileWritten();  // texture files may change: what kept their bytes reads them again
        if (!deleted_.path.empty()) putFileBytes(deleted_.path, deleted_.before, kName);  // before the map names it again
        for (auto edit = rewritten_.rbegin(); edit != rewritten_.rend(); ++edit) putFileBytes(edit->path, edit->before, kName);
        MStatus status = modifier_.undoIt();
        if (!created_.path.empty()) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::u8path(created_.path), ec);
        }
        if (copied_) {
            copyFile("", moved_.second);
        } else {
            moveFile(moved_.second, moved_.first);
        }
        return status;
    });
}

}  // namespace groomlab::maya
