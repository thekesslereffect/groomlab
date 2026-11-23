// SPDX-License-Identifier: Apache-2.0
// The draw override's inputs from the scene: the material's color, the instances' prototypes, the
// overlays' occluders, the card maps' and the scalp map's textures.

#include "groom_draw_override_internal.h"
#include "gpu_lines.h"
#include "gpu_preview.h"
#include "hair_shading.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

#include <maya/MAnimControl.h>
#include <maya/MBoundingBox.h>
#include <maya/MFloatPointArray.h>
#include <maya/MFnMesh.h>
#include <maya/MIntArray.h>
#include <maya/MVector.h>
#include <maya/MGlobal.h>
#include <maya/MDagPath.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MImage.h>
#include <maya/MMessage.h>
#include <maya/MNodeMessage.h>
#include <maya/MPlug.h>
#include <maya/MFnAttribute.h>
#include <maya/MFnDagNode.h>
#include <maya/MItDag.h>
#include <maya/MPlugArray.h>
#include <maya/MStateManager.h>
#include <maya/MTextureManager.h>
#include <maya/MViewport2Renderer.h>

#include "display_theme.h"
#include "groom_shape.h"
#include "maya_inputs.h"
#include "groomlab/cards.h"
#include "groomlab/color_patterns.h"
#include "groomlab/display.h"
#include "groomlab/hair_color.h"
#include "groomlab/instances.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/texture.h"

namespace groomlab::maya {

namespace {

// A scene mesh's fingerprint in world space, for the overlays' occluders: counts, the
// inclusive matrix, the bounds and a sample of points. 0 for no mesh.
std::uint64_t occluderSignature(const MDagPath& path) {
    if (!path.isValid() || !path.hasFn(MFn::kMesh)) return 0;
    MFnMesh mesh(path);
    const int vertexCount = mesh.numVertices();
    std::uint64_t h = hashCombine(hash64(static_cast<std::uint64_t>(vertexCount)),
                                  static_cast<std::uint64_t>(mesh.numPolygons()));
    const MMatrix world = path.inclusiveMatrix();
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) h = hashFloat(h, static_cast<float>(world(r, c)));
    }
    const MBoundingBox box = mesh.boundingBox();
    for (const MPoint& corner : {box.min(), box.max()}) {
        for (int c = 0; c < 3; ++c) h = hashFloat(h, static_cast<float>(corner[c]));
    }
    constexpr int kSamples = 48;
    for (int k = 0; k < kSamples && vertexCount > 0; ++k) {
        MPoint p;
        mesh.getPoint(static_cast<int>(static_cast<std::int64_t>(k) * (vertexCount - 1) / (kSamples - 1)), p, MSpace::kObject);
        h = hashFloat(hashFloat(hashFloat(h, static_cast<float>(p.x)), static_cast<float>(p.y)), static_cast<float>(p.z));
    }
    return h ? h : 1;
}

// A cheap fingerprint of a prototype mesh's shape: its counts, its bounds and its
// points (every one of a small mesh, a spread of 64 across a big one). 0 when the
// scene has no such mesh, so a deleted or renamed prototype reads as changed.
std::uint64_t prototypeSignature(const std::string& name) {
    MDagPath path;
    if (!prototypePath(name, &path)) return 0;
    MFnMesh mesh(path);
    const int vertexCount = mesh.numVertices();
    std::uint64_t h = hashCombine(hash64(static_cast<std::uint64_t>(vertexCount)),
                                  static_cast<std::uint64_t>(mesh.numPolygons()));
    const MBoundingBox box = mesh.boundingBox();
    for (const MPoint& corner : {box.min(), box.max()}) {
        for (int c = 0; c < 3; ++c) h = hashFloat(h, static_cast<float>(corner[c]));
    }
    constexpr int kAllPointsUpTo = 4096, kSamples = 64;
    if (vertexCount <= kAllPointsUpTo) {
        MFloatPointArray points;
        mesh.getPoints(points, MSpace::kObject);
        for (unsigned i = 0; i < points.length(); ++i) {
            h = hashFloat(hashFloat(hashFloat(h, points[i].x), points[i].y), points[i].z);
        }
    } else {
        for (int k = 0; k < kSamples; ++k) {
            MPoint p;
            mesh.getPoint(static_cast<int>(static_cast<std::int64_t>(k) * (vertexCount - 1) / (kSamples - 1)), p, MSpace::kObject);
            h = hashFloat(hashFloat(hashFloat(h, static_cast<float>(p.x)), static_cast<float>(p.y)), static_cast<float>(p.z));
        }
    }
    return h ? h : 1;
}

// An image file read here (MImage: RGBA 8-bit, bottom row first). False when it isn't one.
bool readImage(const std::string& path, MImage& image, unsigned& width, unsigned& height) {
    if (path.empty() || !image.readFromFile(MString(path.c_str()), MImage::kByte)) return false;
    image.getSize(width, height);
    return width > 0 && height > 0 && image.depth() == 4 && image.pixels();
}

// The card map texture: the color map (sRGB) darkened by the AO map (half, when there is one
// of the same size), with the coverage map in the alpha. Built here so a rewritten file is
// never served from Maya's cache (the texture's name carries the stamps), and so the card
// fragment reads one texture, the hair graph's own `map`.
MHWRender::MTexture* loadCardMapTexture(MHWRender::MTextureManager& textures, const std::string& colorPath,
                                        const std::string& coveragePath, const std::string& aoPath, std::uint64_t stamp) {
    MImage color, coverage, ao;
    unsigned width = 0, height = 0, w2 = 0, h2 = 0, w3 = 0, h3 = 0;
    if (!readImage(colorPath, color, width, height) || !readImage(coveragePath, coverage, w2, h2) || w2 != width || h2 != height) {
        return nullptr;
    }
    const bool withAo = readImage(aoPath, ao, w3, h3) && w3 == width && h3 == height;
    const std::vector<std::uint8_t> rgba = combineCardMaps(color.pixels(), coverage.pixels(), withAo ? ao.pixels() : nullptr, width, height);
    MHWRender::MTextureDescription desc;
    desc.setToDefault2DTexture();
    desc.fWidth = width;
    desc.fHeight = height;
    desc.fBytesPerRow = width * 4;
    desc.fBytesPerSlice = width * height * 4;
    desc.fMipmaps = 0;  // the whole chain, generated
    const std::string name = "groomLabCardMap:" + colorPath + "@" + std::to_string(stamp);
    return textures.acquireTexture(MString(name.c_str()), desc, rgba.data(), true);
}

// The cards' iridescence from the Color map's alpha (the bake writes it RGBA only then: 32 bits a
// pixel), in a texture's red; a single texel of none for an RGB map. Null when the file can't be read.
MHWRender::MTexture* loadCardIridescenceTexture(MHWRender::MTextureManager& textures, const std::string& colorPath,
                                                std::uint64_t stamp) {
    char header[18] = {};
    std::ifstream file(colorPath, std::ios::binary);
    if (!file.read(header, sizeof header)) header[16] = 0;
    MImage color;
    unsigned width = 1, height = 1;
    std::vector<std::uint8_t> red(4, 0);
    if (header[16] == 32 && readImage(colorPath, color, width, height)) {
        red.assign(static_cast<std::size_t>(width) * height * 4, 0);
        const unsigned char* px = color.pixels();
        for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) red[i * 4] = px[i * 4 + 3];
    } else {
        width = height = 1;
    }
    MHWRender::MTextureDescription desc;
    desc.setToDefault2DTexture();
    desc.fWidth = width;
    desc.fHeight = height;
    desc.fBytesPerRow = width * 4;
    desc.fBytesPerSlice = width * height * 4;
    desc.fMipmaps = 1;  // one level, none generated (OpenGL makes no texture of a 1x1 chain)
    const std::string name = "groomLabCardIridescence:" + colorPath + "@" + std::to_string(stamp);
    return textures.acquireTexture(MString(name.c_str()), desc, red.data(), false);
}

// How MTexture::update takes a region's pixels on this device, found the first time a region is
// uploaded (by reading the texture back): 1 the region's rows packed, 2 the whole image's rows
// from the region's first pixel; -1 neither worked (whole images are uploaded); 0 not known yet.
int& regionUploadMode() {
    static int mode = 0;
    return mode;
}

// Whether the texture holds `image`'s pixels in `rect` (rows top-down; the texture's rows are
// bottom-up, as the image's buffer). False when it cannot be read back.
bool textureHolds(MHWRender::MTexture& texture, const TexturePreview& image, const TexelRect& rect) {
    int rowPitch = 0;
    std::size_t slicePitch = 0;
    void* raw = texture.rawData(rowPitch, slicePitch);
    if (!raw) return false;
    const auto w = static_cast<std::size_t>(image.width()), h = static_cast<std::size_t>(image.height());
    bool same = rowPitch >= static_cast<int>(w * 4);
    for (std::size_t r = h - static_cast<std::size_t>(rect.y1); same && r < h - static_cast<std::size_t>(rect.y0); ++r) {
        const auto* got = static_cast<const std::uint8_t*>(raw) + r * static_cast<std::size_t>(rowPitch) + static_cast<std::size_t>(rect.x0) * 4;
        const std::uint8_t* want = image.rgba().data() + (r * w + static_cast<std::size_t>(rect.x0)) * 4;
        same = std::memcmp(got, want, static_cast<std::size_t>(rect.x1 - rect.x0) * 4) == 0;
    }
    MHWRender::MTexture::freeRawData(raw);
    return same;
}

// Uploads `rect` (rows top-down) of the brush's image into its texture: only that region, in the
// form this device takes it (regionUploadMode), else the whole image.
bool uploadPreviewRegion(MHWRender::MTexture& texture, const TexturePreview& image, const TexelRect& rect) {
    const auto w = static_cast<std::size_t>(image.width()), h = static_cast<std::size_t>(image.height());
    const std::uint8_t* rgba = image.rgba().data();
    auto whole = [&] { return texture.update(rgba, false) == MS::kSuccess; };
    if (rect.x0 == 0 && rect.y0 == 0 && rect.x1 == image.width() && rect.y1 == image.height()) return whole();
    MHWRender::MTextureUpdateRegion region;
    region.fXRangeMin = static_cast<unsigned>(rect.x0);
    region.fXRangeMax = static_cast<unsigned>(rect.x1);
    region.fYRangeMin = static_cast<unsigned>(h - static_cast<std::size_t>(rect.y1));
    region.fYRangeMax = static_cast<unsigned>(h - static_cast<std::size_t>(rect.y0));
    region.fZRangeMin = 0;
    region.fZRangeMax = 1;
    const std::size_t columns = static_cast<std::size_t>(rect.x1 - rect.x0) * 4;
    auto packed = [&] {
        static std::vector<std::uint8_t> staging;
        staging.resize(columns * (region.fYRangeMax - region.fYRangeMin));
        for (unsigned r = region.fYRangeMin; r < region.fYRangeMax; ++r) {
            std::memcpy(staging.data() + (r - region.fYRangeMin) * columns, rgba + (r * w + static_cast<std::size_t>(rect.x0)) * 4, columns);
        }
        return texture.update(staging.data(), false, static_cast<unsigned>(columns), &region) == MS::kSuccess;
    };
    auto offset = [&] {
        return texture.update(rgba + (region.fYRangeMin * w + static_cast<std::size_t>(rect.x0)) * 4, false,
                              static_cast<unsigned>(w * 4), &region) == MS::kSuccess;
    };
    int& mode = regionUploadMode();
    if (mode == 1) return packed();
    if (mode == 2) return offset();
    if (mode < 0) return whole();
    // Not known yet: each form in turn, read back.
    if (packed() && textureHolds(texture, image, rect)) {
        mode = 1;
        return true;
    }
    if (offset() && textureHolds(texture, image, rect)) {
        mode = 2;
        return true;
    }
    mode = -1;
    return whole();
}

// Whether a file's image read from the core's cache (its decode there, 8-bit) shows as MImage's
// read of the file does: checked against MImage the first time (1 yes, -1 no, 0 not yet).
int& cachedImageRoute() {
    static int route = 0;
    return route;
}

}  // namespace

void GroomSubSceneOverride::releaseScalpMapTexture() {
    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    MHWRender::MTextureManager* textures = renderer ? renderer->getTextureManager() : nullptr;
    if (scalpMapTexture_ && textures) textures->releaseTexture(scalpMapTexture_);
    scalpMapTexture_ = nullptr;
    scalpMapTextureKey_.clear();
    scalpMapPreview_ = nullptr;
}

std::string scalpMapUploadMode() {
    switch (regionUploadMode()) {
        case 1: return "region";
        case 2: return "regionInImage";
        case -1: return "whole";
        default: return "unknown";
    }
}

std::string scalpMapFileImageSource() {
    const int route = cachedImageRoute();
    return route > 0 ? "cache" : route < 0 ? "file" : "unknown";
}

MHWRender::MTexture* GroomSubSceneOverride::scalpMapTexture(const GroomShape::ScalpMapView& view) {
    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    MHWRender::MTextureManager* textures = renderer ? renderer->getTextureManager() : nullptr;
    if (!textures) return nullptr;
    const int pick = scalpMapPick(view.channel);
    auto acquire = [&](const std::string& name, unsigned width, unsigned height, const std::uint8_t* rgba) {
        releaseScalpMapTexture();
        MHWRender::MTextureDescription desc;
        desc.setToDefault2DTexture();
        desc.fWidth = width;
        desc.fHeight = height;
        desc.fBytesPerRow = width * 4;
        desc.fBytesPerSlice = width * height * 4;
        desc.fMipmaps = 1;  // painted maps read at their own texels
        scalpMapTexture_ = textures->acquireTexture(MString(("groomLabScalpMap:" + name).c_str()), desc, rgba, false);
        ++scalpMapTextureGeneration_;
    };
    if (view.preview && view.preview->width() > 0 && view.preview->height() > 0) {
        // The brush's image, kept up to date by the brush (as the viewport shows it): one texture
        // for it, the texels changed since the last upload going up as a region.
        const TexturePreview& image = *view.preview;
        const std::string key = "preview:" + std::to_string(reinterpret_cast<std::uintptr_t>(&image)) + ":" + std::to_string(image.layout());
        if (key == scalpMapTextureKey_ && scalpMapTexture_ && scalpMapPreview_ == &image) {
            const TexelRect changed = image.changedSince(scalpMapPreviewRevision_, scalpMapPreviewLayout_);
            if (!changed.empty() && !uploadPreviewRegion(*scalpMapTexture_, image, changed)) return scalpMapTexture_;
            scalpMapPreviewRevision_ = image.revision();
            return scalpMapTexture_;
        }
        acquire(key + "#" + std::to_string(scalpMapTextureGeneration_), static_cast<unsigned>(image.width()),
                static_cast<unsigned>(image.height()), image.rgba().data());
        scalpMapTextureKey_ = key;
        scalpMapPreview_ = &image;
        scalpMapPreviewRevision_ = image.revision();
        scalpMapPreviewLayout_ = image.layout();
        return scalpMapTexture_;
    }
    const std::string key = view.texture + "@" + std::to_string(fileStamp(view.texture)) + "#" +
                            (view.channels == 4 ? view.channel : std::string("gray"));
    if (key == scalpMapTextureKey_ && scalpMapTexture_) return scalpMapTexture_;
    std::vector<std::uint8_t> rgba;
    unsigned width = 0, height = 0;
    // The file as MImage reads it (RGBA 8-bit, rows bottom-up), shown as the map shows.
    auto fromFile = [&]() {
        MImage image;
        if (!readImage(view.texture, image, width, height)) return false;
        const std::size_t pixels = static_cast<std::size_t>(width) * height;
        rgba.assign(image.pixels(), image.pixels() + pixels * 4);
        scalpMapFilePixels(rgba.data(), pixels, view.channels, pick);
        return true;
    };
    // The core's decode of the file, when it holds one (the brush puts what it wrote there, the
    // graph what it read): its 8-bit texels, the file not read again.
    bool done = false;
    const TexturePtr cached = cachedImageRoute() >= 0 ? cachedTexture(view.texture) : nullptr;
    if (cached && cached->image() && cached->image()->bits == 8) {
        const Texture::Image& image = *cached->image();
        width = static_cast<unsigned>(image.width);
        height = static_cast<unsigned>(image.height);
        rgba = scalpMapCachedPixels(image, view.channels, pick);
        done = true;
        if (cachedImageRoute() == 0) {
            // The first time: the same as the file read by MImage, or the cache is not used again.
            const std::vector<std::uint8_t> mine = rgba;
            const unsigned w = width, h = height;
            if (fromFile()) {
                cachedImageRoute() = rgba == mine && width == w && height == h ? 1 : -1;
            } else {
                rgba = mine;
                width = w;
                height = h;
            }
        }
    }
    if (!done && !fromFile()) return nullptr;
    acquire(key, width, height, rgba.data());
    scalpMapTextureKey_ = key;
    return scalpMapTexture_;
}

void GroomSubSceneOverride::materialDirty(MObject&, void* clientData) {
    auto* self = static_cast<GroomSubSceneOverride*>(clientData);
    self->colorDirty_ = true;
    MHWRender::MRenderer::setGeometryDrawDirty(self->object_);
}

// Watches the shading group and material litColor() read, so a change to them
// (a new surfaceShader, another color) dirties the color without walking them
// on every refresh. Re-registered when they change.
void GroomSubSceneOverride::watchMaterial(const MObject& engine, const MObject& material) {
    if (engine == watchedEngine_ && material == watchedMaterial_) return;
    MMessage::removeCallbacks(callbacks_);
    callbacks_.clear();
    watchedEngine_ = engine;
    watchedMaterial_ = material;
    for (MObject* node : {&watchedEngine_, &watchedMaterial_}) {
        if (node->isNull()) continue;
        MStatus status;
        const MCallbackId id = MNodeMessage::addNodeDirtyCallback(*node, materialDirty, this, &status);
        if (status) callbacks_.append(id);
    }
}

// Keeps the prototype meshes `names` (the instances' prototypes), fetching the
// new ones and refetching those whose scene mesh no longer matches its
// fingerprint; prototypes no longer instanced are dropped.
void GroomSubSceneOverride::refreshPrototypes(const std::vector<std::string>& names) {
    for (auto it = prototypes_.begin(); it != prototypes_.end();) {
        it = std::find(names.begin(), names.end(), it->first) == names.end() ? prototypes_.erase(it) : std::next(it);
    }
    for (const std::string& name : names) {
        PrototypeMesh& mesh = prototypes_[name];
        const std::uint64_t signature = prototypeSignature(name);
        if (mesh.signature == signature && (mesh.found || signature == 0)) continue;
        readInstancePrototype(name, mesh);
        mesh.signature = signature;
    }
    prototypesChecked_ = std::chrono::steady_clock::now();
    watchPrototypes();
}

bool GroomSubSceneOverride::prototypesChanged() const {
    if (prototypes_.empty()) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now - prototypesChecked_ < std::chrono::milliseconds(500)) return false;
    prototypesChecked_ = now;
    for (const auto& [name, mesh] : prototypes_) {
        if (prototypeSignature(name) != mesh.signature) return true;
    }
    return false;
}

void GroomSubSceneOverride::prototypeDirty(MObject&, void* clientData) {
    auto* self = static_cast<GroomSubSceneOverride*>(clientData);
    self->prototypesChecked_ = {};  // look at the meshes on the next redraw
    MHWRender::MRenderer::setGeometryDrawDirty(self->object_);
}

// Dirty callbacks on the prototype meshes found, re-registered when they are other nodes.
void GroomSubSceneOverride::watchPrototypes() {
    std::vector<MObjectHandle> nodes;
    for (const auto& [name, mesh] : prototypes_) {
        MDagPath path;
        if (mesh.found && prototypePath(name, &path)) nodes.emplace_back(path.node());
    }
    bool same = nodes.size() == watchedPrototypes_.size();
    for (std::size_t i = 0; same && i < nodes.size(); ++i) {
        same = watchedPrototypes_[i].isValid() && nodes[i].objectRef() == watchedPrototypes_[i].objectRef();
    }
    if (same) return;
    MMessage::removeCallbacks(prototypeCallbacks_);
    prototypeCallbacks_.clear();
    watchedPrototypes_ = nodes;
    for (MObjectHandle& handle : nodes) {
        MObject node = handle.object();
        MStatus status;
        const MCallbackId id = MNodeMessage::addNodeDirtyCallback(node, prototypeDirty, this, &status);
        if (status) prototypeCallbacks_.append(id);
    }
}

std::array<float, 3> GroomSubSceneOverride::litColor(MObject* engineOut, MObject* materialOut, bool* groomColors) {
    if (groomColors) *groomColors = false;
    // A hair color (hair_color.h): the groom's own, a material's base color, or an aiStandardHair's
    // melanin as melaninColor.
    const StrandsPtr strands = shape_->handedStrands();  // (their channels: at rest or carried alike)
    auto channelMean = [&](const std::string& channel, bool* found) {
        return means_.mean(strands.get(), shape_->evaluationVersion(), channel, found);
    };
    MPlug display(object_, GroomShape::aDisplayColor);
    std::array<float, 3> color{display.child(0).asFloat(), display.child(1).asFloat(), display.child(2).asFloat()};
    if (!MPlug(object_, GroomShape::aUseMaterialColor).asBool()) return color;
    // instObjGroups[0] -> shadingEngine.dagSetMembers; its surfaceShader holds the material.
    MFnDependencyNode fn(object_);
    MPlug groups = fn.findPlug("instObjGroups", false);
    MPlugArray sets;
    if (!groups.isNull() && groups.numElements() > 0) groups.elementByPhysicalIndex(0).connectedTo(sets, false, true);
    for (unsigned i = 0; i < sets.length(); ++i) {
        MFnDependencyNode engine(sets[i].node());
        if (engine.typeName() != "shadingEngine") continue;
        if (engineOut) *engineOut = sets[i].node();
        // RenderMan for Maya's Hypershade puts its material on rman__surface and a lambert stand-in
        // on surfaceShader: its material is the one the groom renders with.
        MPlugArray shaders;
        const MPlug rfmSurface = engine.findPlug("rman__surface", false);
        if (!rfmSurface.isNull()) rfmSurface.connectedTo(shaders, true, false);
        if (shaders.length() == 0) engine.findPlug("surfaceShader", false).connectedTo(shaders, true, false);
        if (shaders.length() == 0) continue;
        MFnDependencyNode material(shaders[0].node());
        if (materialOut) *materialOut = shaders[0].node();
        auto rgb = [&](const char* name, std::array<float, 3>& out) {
            MPlug plug = material.findPlug(name, false);
            if (plug.isNull() || plug.numChildren() != 3) return false;
            out = {plug.child(0).asFloat(), plug.child(1).asFloat(), plug.child(2).asFloat()};
            return true;
        };
        std::array<float, 3> base{1.0f, 1.0f, 1.0f};
        if (material.typeName() == "aiStandardHair") {
            // Made from the groom (create_hair_shader): its color reads the strands' own channels
            // (HairColor's RGB as hair_color, or melanin and redness), so the strands show theirs,
            // as the render does.
            // The plug create_hair_shader's wet network (multiplyDivide nodes named <shader>_wet...,
            // shading.py _wet_network) scales in front of `plug`: the dry look the viewport draws (the
            // wetness comes from the strands themselves).
            auto dry = [](MPlug plug) {
                for (int i = 0; i < 4 && !plug.isNull() && plug.isDestination(); ++i) {
                    const MPlug source = plug.source();
                    MFnDependencyNode node(source.node());
                    if (node.typeName() != "multiplyDivide" || node.name().indexW("_wet") < 0) break;
                    const MString name = source.partialName(false, false, false, false, false, true);
                    plug = node.findPlug(name == "outputX" || name == "ox" ? "input1X" : "input1", false);
                }
                return plug;
            };
            auto userData = [&](const char* name, const char* type) {
                const MPlug plug = dry(material.findPlug(name, false));
                if (plug.isNull() || !plug.isDestination()) return false;
                MObject source = plug.source().node();
                // HairColor's color shift in front of it (create_hair_shader's aiColorCorrect,
                // render-only): what it shifts.
                if (MFnDependencyNode(source).typeName() == "aiColorCorrect") {
                    const MPlug input = MFnDependencyNode(source).findPlug("input", false);
                    if (input.isNull() || !input.isDestination()) return false;
                    source = input.source().node();
                }
                return MFnDependencyNode(source).typeName() == type;
            };
            const bool groomLook = strands && (strands->strandChannel("colorR") || strands->strandChannel("melanin"));
            if (groomLook && (userData("baseColor", "aiUserDataColor") || userData("melanin", "aiUserDataFloat"))) {
                if (groomColors) *groomColors = true;
                return strandsLookColor(strands.get(), shape_->evaluationVersion(), color, means_);
            }
            rgb("baseColor", base);
            // Driven by user data (create_hair_shader): the groom's average of that
            // strand channel, else the user data node's default.
            auto value = [&](const char* name) {
                MPlug plug = dry(material.findPlug(name, false));
                if (!plug.isDestination()) return plug.asFloat();
                MFnDependencyNode data(plug.source().node());
                MPlug channel = data.findPlug("attribute", false), fallback = data.findPlug("default", false);
                bool found = false;
                const float mean = channel.isNull() ? 0.0f : channelMean(channel.asString().asChar(), &found);
                if (found) return mean;
                return fallback.isNull() ? plug.asFloat() : fallback.asFloat();
            };
            return melaninColor(value("melanin"), value("melaninRedness"), base);
        }
        // Other renderers' hair materials (issue 100): their color as they set it, or, where a
        // network drives it (user data, primvars, a hair color node), the groom's own colors, as
        // GroomLab's own networks read them.
        auto driven = [&](const char* name) {
            const MPlug plug = material.findPlug(name, false);
            return !plug.isNull() && plug.isDestination();
        };
        auto groomLook = [&]() {
            if (groomColors) *groomColors = true;
            return strandsLookColor(strands.get(), shape_->evaluationVersion(), color, means_);
        };
        auto number = [&](const char* name, float fallback) {
            const MPlug plug = material.findPlug(name, false);
            return plug.isNull() ? fallback : plug.asFloat();
        };
        const MString type = material.typeName();
        if (type == "RedshiftPrincipledHair" || type == "VRayHairNextMtl") {
            // Albedo (Redshift) or diffuse (V-Ray) by its mix, else melanin and pheomelanin under a tint.
            const bool redshift = type == "RedshiftPrincipledHair";
            const char* albedo = redshift ? "albedo" : "diffuse_color";
            if (number(redshift ? "albedoMix" : "diffuse_amount", 0.0f) >= 0.5f) {
                if (driven(albedo)) return groomLook();
                if (rgb(albedo, base)) return base;
            }
            if (driven("melanin") || driven("pheomelanin")) return groomLook();
            rgb(redshift ? "tint" : "dye_color", base);
            return melaninColor(number("melanin", 0.3f), number("pheomelanin", 0.0f), base);
        }
        // A RenderMan color input: its value, or what the PxrHairColor feeding it makes (its artistic
        // color, else its melanin and redness under its dye). False when another network drives it
        // (GroomLab's own reads the strands' primvars) or the input is not there.
        auto rfmColor = [&](const char* name, std::array<float, 3>& out) {
            const MPlug plug = material.findPlug(name, false);
            if (plug.isNull() || plug.numChildren() != 3) return false;
            if (!plug.isDestination()) {
                out = {plug.child(0).asFloat(), plug.child(1).asFloat(), plug.child(2).asFloat()};
                return true;
            }
            const MFnDependencyNode hair(plug.source().node());
            if (hair.typeName() != "PxrHairColor") return false;
            auto input = [&](const char* attr, std::array<float, 3>* rgbOut, float* valueOut) {
                const MPlug p = hair.findPlug(attr, false);
                if (p.isNull() || p.isDestination()) return false;
                if (rgbOut) *rgbOut = {p.child(0).asFloat(), p.child(1).asFloat(), p.child(2).asFloat()};
                if (valueOut) *valueOut = p.asFloat();
                return true;
            };
            float mode = 0.0f, melanin = 0.0f, redness = 0.0f;
            std::array<float, 3> dye{1.0f, 1.0f, 1.0f};
            if (!input("mode", nullptr, &mode) || !input("dye", &dye, nullptr)) return false;
            if (mode >= 0.5f) return input("Color", &out, nullptr);  // Artistic
            if (!input("melanin", nullptr, &melanin) || !input("redness", nullptr, &redness)) return false;
            out = melaninColor(melanin, redness, dye);
            return true;
        };
        if (type == "LamaHairChiang") {
            // Its color is the transmission's (TT) under its tint.
            std::array<float, 3> tt{}, tint{1.0f, 1.0f, 1.0f};
            if (!rfmColor("colorTT", tt) || !rfmColor("colorTintTT", tint)) return groomLook();
            return {tt[0] * tint[0], tt[1] * tint[1], tt[2] * tint[2]};
        }
        if (type == "PxrMarschnerHair") {
            // Its color is in the transmission and second reflection (TT, TRT), the diffuse lobe off
            // unless its gain is set.
            if (number("diffuseGain", 0.0f) > 0.0f) {
                return rfmColor("diffuseColor", base) ? base : groomLook();
            }
            std::array<float, 3> tt{}, trt{};
            if (!rfmColor("specularColorTT", tt) || !rfmColor("specularColorTRT", trt)) return groomLook();
            return {0.5f * (tt[0] + trt[0]), 0.5f * (tt[1] + trt[1]), 0.5f * (tt[2] + trt[2])};
        }
        if (rgb("baseColor", base) || rgb("color", base) || rgb("diffuseColor", base)) return base;
        if (rgb("outColor", base)) return base;
    }
    // No material: a HairColor node's RGB, else melanin channels, as the render would show it (the
    // strands' mean: each strand takes its own where they vary, DisplaySettings::strandLookColors).
    if (groomColors) *groomColors = true;
    return strandsLookColor(strands.get(), shape_->evaluationVersion(), color, means_);
}

void GroomSubSceneOverride::releaseCardTextures() {
    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    MHWRender::MTextureManager* textures = renderer ? renderer->getTextureManager() : nullptr;
    // The card shader is not unbound first: assigning a null texture to an effect parameter
    // crashes inside Maya (OGS makes a texture instance of it). The shader keeps its own hold
    // on a texture it was given, and update() rebinds the new ones (bindCardTextures) or swaps
    // the shader before anything draws again.
    for (CardMap& map : cardMaps_) {
        if (map.texture && textures) textures->releaseTexture(map.texture);
        map = CardMap{};
    }
    if (cardIridescence_ && textures) textures->releaseTexture(cardIridescence_);
    cardIridescence_ = nullptr;
    cardMapsLoaded_.clear();
}

bool GroomSubSceneOverride::cardTexturesLoaded() const {
    return cardMaps_[kCardColorMap].texture != nullptr;  // the one texture of the maps
}

bool GroomSubSceneOverride::cardFilesChanged() const {
    if (cardMapsLoaded_.empty()) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now - cardFilesChecked_ < std::chrono::milliseconds(500)) return false;
    cardFilesChecked_ = now;
    for (const CardMap& map : cardMaps_) {
        if (!map.path.empty() && fileStamp(map.path) != map.stamp) return true;
    }
    return false;
}

bool GroomSubSceneOverride::loadCardTextures(const std::string& maps) {
    if (maps == cardMapsLoaded_) {
        cardFilesChecked_ = {};  // look now, not in half a second
        if (!cardFilesChanged()) return cardTexturesLoaded();
    }
    releaseCardTextures();
    cardMapsLoaded_ = maps;
    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    MHWRender::MTextureManager* textures = renderer ? renderer->getTextureManager() : nullptr;
    if (!textures || maps.empty()) return false;
    const std::map<std::string, std::string> paths = parseCardMaps(maps);
    const char* names[kCardMapCount] = {"Color", "Coverage", "AO"};
    for (int k = 0; k < kCardMapCount; ++k) {
        auto it = paths.find(names[k]);
        if (it == paths.end()) continue;
        CardMap& map = cardMaps_[static_cast<std::size_t>(k)];
        map.path = resolveNodePath(object_, it->second);  // stored portable (relative to the scene's folder)
        map.stamp = fileStamp(map.path);  // 0 (no file) is remembered too: the file appearing reloads
    }
    // One texture of the three files, on the color map's entry; the others only keep their
    // paths and stamps (for the reload when they change).
    const CardMap& color = cardMaps_[kCardColorMap];
    const CardMap& coverage = cardMaps_[kCardCoverageMap];
    const CardMap& ao = cardMaps_[kCardAoMap];
    if (color.stamp != 0 && coverage.stamp != 0) {
        const std::uint64_t stamp = hashCombine(hashCombine(color.stamp, coverage.stamp), ao.stamp);
        cardMaps_[kCardColorMap].texture = loadCardMapTexture(*textures, color.path, coverage.path, ao.stamp != 0 ? ao.path : std::string(), stamp);
        if (cardMaps_[kCardColorMap].texture) cardIridescence_ = loadCardIridescenceTexture(*textures, color.path, stamp);
    }
    cardFilesChecked_ = std::chrono::steady_clock::now();
    return cardTexturesLoaded();
}

void GroomSubSceneOverride::refreshOccluders() {
    // The groom's own scalp moves with the frame drawn (another frame, one cached playback
    // restored): it is that frame's scalp at every draw, whatever the walk below last found.
    if (const MeshPtr shown = shape_->evaluatedScalp()) {
        for (std::size_t i = 0; i < occluders_.size(); ++i) {
            if (occluders_[i].scalp && occluders_[i].mesh != shown && shown->triangleCount() > 0) {
                occluders_[i].mesh = shown;
                occluderMeshes_[i] = shown;
            }
        }
    }
    // Playing or scrubbing: the other occluders found before stay (looking costs a walk of the scene).
    if (MAnimControl::isPlaying() || MAnimControl::isScrubbing()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - occludersChecked_ < std::chrono::milliseconds(500)) return;
    occludersChecked_ = now;
    if (!displayFlag(GroomShape::aDisplayOverlaysOnTop)) {
        occluders_.clear();
        occluderMeshes_.clear();
        return;
    }
    // An overlay point (guides, parts, flow: within the groom's bounds or on its scalp) moves at
    // most overlayPull_ toward the camera: only meshes within that of them can stop it.
    const MeshPtr scalp = shape_->evaluatedScalp();
    if (scalp && scalp->contentHash() != scalpBoundsOf_) {
        scalpBoundsOf_ = scalp->contentHash();
        scalpBounds_ = MBoundingBox();
        for (const Vec3f& p : scalp->points()) scalpBounds_.expand(MPoint(p.x, p.y, p.z));
    }
    MBoundingBox reach = shape_->boundingBox();
    if (scalp && scalp->pointCount() > 0) reach.expand(scalpBounds_);
    {
        const double grow = static_cast<double>(overlayPull_) * 1.01 + 1e-3 * (reach.max() - reach.min()).length();
        reach.expand(reach.min() - MVector(grow, grow, grow));
        reach.expand(reach.max() + MVector(grow, grow, grow));
    }
    // The scalp (the mesh feeding inMesh's world space) is the groom's own scalp mesh, already
    // converted with its tree built: used as it is.
    std::string scalpPath;
    {
        const MPlug source = MPlug(object_, GroomShape::aInMesh).source();
        MDagPath path;
        if (!source.isNull() && MFnAttribute(source.attribute()).name() == "worldMesh" &&
            MDagPath::getAPathTo(source.node(), path) == MS::kSuccess) {
            scalpPath = path.fullPathName().asChar();
        }
    }
    // The card mesh on outCards is never one: the overlays go over the cards.
    std::set<std::string> skipped;
    {
        MPlugArray destinations;
        MPlug(object_, GroomShape::aOutCards).connectedTo(destinations, false, true);
        for (unsigned i = 0; i < destinations.length(); ++i) {
            MDagPath path;
            if (MDagPath::getAPathTo(destinations[i].node(), path) == MS::kSuccess) skipped.insert(path.fullPathName().asChar());
        }
    }
    constexpr std::size_t kTriangleBudget = 3000000;  // past it the rest of the scene doesn't hide overlays
    std::vector<Occluder> found;
    std::size_t triangles = 0;
    for (MItDag it(MItDag::kDepthFirst, MFn::kMesh); !it.isDone(); it.next()) {
        MDagPath path;
        if (it.getPath(path) != MS::kSuccess) continue;
        MFnDagNode dag(path);
        if (dag.isIntermediateObject() || !path.isVisible()) continue;
        const std::string full = path.fullPathName().asChar();
        if (skipped.count(full)) continue;
        MFnMesh mesh(path);
        if (mesh.numPolygons() <= 0) continue;
        MBoundingBox bounds = mesh.boundingBox();
        bounds.transformUsing(path.inclusiveMatrix());
        if (!bounds.intersects(reach)) continue;  // too far to stop any overlay's pull
        if (triangles + static_cast<std::size_t>(mesh.numPolygons()) > kTriangleBudget) break;
        if (!scalpPath.empty() && full == scalpPath && scalp && scalp->triangleCount() > 0) {
            Occluder occluder;
            occluder.path = full;
            occluder.mesh = scalp;
            occluder.scalp = true;
            triangles += static_cast<std::size_t>(mesh.numPolygons());
            found.push_back(std::move(occluder));
            continue;
        }
        const std::uint64_t signature = occluderSignature(path);
        Occluder occluder;
        occluder.path = full;
        occluder.signature = signature;
        auto known = std::find_if(occluders_.begin(), occluders_.end(), [&](const Occluder& o) { return o.path == full; });
        if (known != occluders_.end() && known->signature == signature && known->signature != 0 && known->mesh) {
            occluder.mesh = known->mesh;  // unchanged: the tree it has
        } else {
            MFloatPointArray points;
            mesh.getPoints(points, MSpace::kWorld);
            MIntArray counts, vertices;
            mesh.getTriangles(counts, vertices);
            std::vector<Vec3f> positions(points.length());
            for (unsigned i = 0; i < points.length(); ++i) positions[i] = {points[i].x, points[i].y, points[i].z};
            std::vector<ScalpMesh::Triangle> tris(vertices.length() / 3);
            for (std::size_t t = 0; t < tris.size(); ++t) {
                tris[t] = {vertices[static_cast<unsigned>(t * 3)], vertices[static_cast<unsigned>(t * 3 + 1)],
                           vertices[static_cast<unsigned>(t * 3 + 2)]};
            }
            if (tris.empty()) continue;
            occluder.mesh = std::make_shared<const ScalpMesh>(std::move(positions), std::move(tris));
        }
        triangles += static_cast<std::size_t>(mesh.numPolygons());
        found.push_back(std::move(occluder));
    }
    occluders_ = std::move(found);
    occluderMeshes_.clear();
    for (const Occluder& o : occluders_) occluderMeshes_.push_back(o.mesh);
}

void GroomSubSceneOverride::bindItemTexture(ItemBuffers& buffers, MHWRender::MTexture* texture) {
    if (!buffers.shader || !texture) return;  // never a null assignment: Maya crashes making a texture instance of it
    MHWRender::MTextureAssignment assignment;
    assignment.texture = texture;
    if (buffers.shader->setParameter("map", assignment) != MS::kSuccess) {
        MStringArray names;
        buffers.shader->parameterList(names);
        MString all;
        for (unsigned i = 0; i < names.length(); ++i) all += names[i] + " ";
        MGlobal::displayWarning(MString("GroomLab: the texture shader has no texture parameter 'map'; it has: ") + all);
    }
    MHWRender::MSamplerStateDesc samplerDesc;
    samplerDesc.filter = MHWRender::MSamplerState::kMinMagMipLinear;
    samplerDesc.addressU = samplerDesc.addressV = MHWRender::MSamplerState::kTexClamp;
    if (const MHWRender::MSamplerState* sampler = MHWRender::MStateManager::acquireSamplerState(samplerDesc)) {
        buffers.shader->setParameter("textureSampler", *sampler);
    }
}

void GroomSubSceneOverride::bindCardTextures(ItemBuffers& buffers) {
    if (!buffers.shader || buffers.shaderKind != 6) return;
    auto bind = [&](const char* parameter, MHWRender::MTexture* texture) {
        if (!texture) return;  // never a null assignment: Maya crashes making a texture instance of it
        MHWRender::MTextureAssignment assignment;
        assignment.texture = texture;
        if (buffers.shader->setParameter(parameter, assignment) != MS::kSuccess) {
            MStringArray names;
            buffers.shader->parameterList(names);
            MString all;
            for (unsigned i = 0; i < names.length(); ++i) all += names[i] + " ";
            MGlobal::displayWarning(MString("GroomLab: the card shader has no texture parameter '") + parameter +
                                    "'; it has: " + all);
        }
    };
    bind("map", cardMaps_[kCardColorMap].texture);  // color x AO, the coverage in its alpha
    if (!buffers.hair) {
        bindCardIridescence(buffers, bind);  // (GroomLab's hair: the cards' tint carries it)
        // Maya's hair graph: its alpha to coverage after its last fragment reads the map's alpha too.
        bind("coverageMap", cardMaps_[kCardColorMap].texture);
    }
    if (MHWRender::MTexture* map = cardMaps_[kCardColorMap].texture) {
        // Its size: how many texels a pixel spans, for the coverage kept as it shrinks.
        MHWRender::MTextureDescription desc;
        map->textureDescription(desc);
        const float size = static_cast<float>(std::max(desc.fWidth, desc.fHeight));
        buffers.shader->setParameter("mapSize", size);
        if (!buffers.hair) buffers.shader->setParameter("coverageMapSize", size);
    }
}

template <class Bind>
void GroomSubSceneOverride::bindCardIridescence(ItemBuffers& buffers, const Bind& bind) {
    // The iridescence (the Color map's alpha); never unbound (OpenGL reads a null texture): the
    // root-to-tip ramp stands in without one (nothing baked, the solid color drawing), where the
    // cards' tint (no iridescence) zeroes it.
    bind("irMap", cardIridescence_ ? cardIridescence_ : rootToTipTexture());
    // Its sampler is named for it (texture X, sampler XSampler): OpenGL's GLSL joins each sampler to
    // its texture by that name, and with a second texture in the effect a sampler named otherwise
    // drew from a null texture (Maya crashed on its first draw of the cards).
    MHWRender::MSamplerStateDesc samplerDesc;
    samplerDesc.filter = MHWRender::MSamplerState::kMinMagLinear_MipPoint;
    samplerDesc.addressU = samplerDesc.addressV = MHWRender::MSamplerState::kTexClamp;
    if (const MHWRender::MSamplerState* sampler = MHWRender::MStateManager::acquireSamplerState(samplerDesc)) {
        buffers.shader->setParameter("irMapSampler", *sampler);
    }
}

// A root-to-tip brightness ramp for the hair tube shader's color map: roots
// a little darker (self-shadowed), as rendered hair reads.
MHWRender::MTexture* GroomSubSceneOverride::rootToTipTexture() {
    if (rootToTip_) return rootToTip_;
    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    MHWRender::MTextureManager* textures = renderer ? renderer->getTextureManager() : nullptr;
    if (!textures) return nullptr;
    constexpr unsigned kWidth = 32;
    const std::vector<std::uint8_t> pixels = rootToTipRamp(kWidth);
    MHWRender::MTextureDescription desc;
    desc.setToDefault2DTexture();
    desc.fWidth = kWidth;
    desc.fHeight = 1;
    desc.fBytesPerRow = kWidth * 4;
    desc.fBytesPerSlice = kWidth * 4;
    rootToTip_ = textures->acquireTexture("groomLabRootToTip", desc, pixels.data(), false);
    return rootToTip_;
}
}  // namespace groomlab::maya
