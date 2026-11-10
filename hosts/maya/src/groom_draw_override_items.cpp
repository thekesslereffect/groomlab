// SPDX-License-Identifier: Apache-2.0
// The draw override's render items: geometry written into their buffers (whole or in part),
// restyled, recolored.

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

#include <maya/MBoundingBox.h>
#include <maya/MHWGeometry.h>
#include <maya/MShaderManager.h>
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

// Copies n items in parallel chunks (a stream's vectors into the GPU buffer's memory).
template <class T>
void copyParallel(T* out, const T* in, std::size_t n) {
    constexpr std::size_t kChunk = std::size_t{1} << 18;
    parallelFor(0, (n + kChunk - 1) / kChunk, [&](std::size_t c) {
        const std::size_t begin = c * kChunk;
        std::memcpy(out + begin, in + begin, std::min(kChunk, n - begin) * sizeof(T));
    }, 1);
}

}  // namespace

// The overlay effect's colors stream: the lines' colors (or ones, for the item's color) with
// each vertex's pull limit in the alpha.
void GroomSubSceneOverride::uploadOverlayColors(ItemBuffers& buffers, const DisplayGeometry& geometry,
                                                const std::vector<float>& limits) {
    const auto vertexCount = static_cast<unsigned>(geometry.vertexCount());
    if (!buffers.colors) {
        const MHWRender::MVertexBufferDescriptor colorDesc("", MHWRender::MGeometry::kColor, MHWRender::MGeometry::kFloat, 4);
        buffers.colors = std::make_unique<MHWRender::MVertexBuffer>(colorDesc, softwareStaged());
    }
    auto* col = static_cast<float*>(buffers.colors->acquire(vertexCount, true));
    if (!col) throw std::runtime_error("out of video memory for the colors");
    writeOverlayColors(geometry, limits, col);
    buffers.colors->commit(col);
}

void GroomSubSceneOverride::bindGeometry(MHWRender::MRenderItem& item, const MHWRender::MVertexBufferArray& streams,
                                         const MHWRender::MIndexBuffer& indices, const MBoundingBox* bounds) {
    // The streams as bound, kept by item (the level of detail binds the strands' to its items: the
    // item's geometry() says what it drew last, not what it was just given).
    std::vector<std::pair<MString, MHWRender::MVertexBuffer*>>& kept = boundStreams_[item.name().asChar()];
    kept.clear();
    for (unsigned i = 0; i < streams.count(); ++i) kept.emplace_back(streams.getName(i), streams.getBuffer(i));
    boundSerials_[item.name().asChar()] = ++bindSerial_;
    const auto timed = currentDraw().items.find(item.name().asChar());
    if (timed != currentDraw().items.end()) {
        std::string& out = timed->second.streams;
        out.clear();
        for (const auto& [streamName, buffer] : kept) {
            if (!out.empty()) out += ' ';
            out += streamName.asChar();
            if (!buffer) continue;
            out += buffer->hasCustomResourceHandle() ? std::string("*") : ":" + std::to_string(buffer->vertexCount());
        }
    }
    setGeometryForRenderItem(item, streams, indices, bounds);
}

void GroomSubSceneOverride::updateItem(MHWRender::MSubSceneContainer& container, const MString& name,
                                       const float color[4], unsigned depthPriority,
                                       const std::shared_ptr<const DisplayGeometry>& geometry, float lineWidth,
                                       MHWRender::MTexture* mapTexture) {
    static const DisplayGeometry kNothing;
    const DisplayGeometry& batch = geometry ? *geometry : kNothing;
    MHWRender::MRenderItem* item = container.find(name);
    ItemBuffers& buffers = items_[name.asChar()];
    if (batch.empty()) {
        // Nothing to draw: take the item out and give its GPU memory back.
        if (name == kRibbonsItem) dropThinRibbons(container);  // (it draws the ribbons' buffers)
        if (item) container.remove(name);
        ownBuffersBound(name);
        releaseGeometry(buffers);
        overlayBatches_.erase(name.asChar());
        boundStreams_.erase(name.asChar());
        return;
    }

    bool perVertex = !batch.colors.empty() || (batch.lines && batch.lines->colors);
    const bool thick = lineWidth > 1.0f && !batch.triangles && !batch.lit();
    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    const MHWRender::MShaderManager* shaders = renderer->getShaderManager();
    if (!shaders) return;
    // Textured cards need the card effect; without it (it didn't compile) they draw as a plain surface.
    const bool overlay = batch.overlay && !batch.triangles && !batch.lit() && overlayShaderAvailable(*shaders);
    const int kind = batch.points ? 11  // dots: updateDots asks only where the dots effect is
                   : overlay ? 7
                   : batch.mapTexture && mapTexture && !batch.cardTextures ? 8
                   : batch.cardTextures ? (cardShaderRegistered() ? 6 : 5)
                   : batch.scaleSurface ? (scaleShaderRegistered() ? 10 : 5)
                   : batch.surface ? 5
                   : batch.lit() ? (perVertex && (hairShading_ || hairColorShaderRegistered()) ? 9 : 2)
                   : thick ? (perVertex ? 4 : 3) : perVertex ? 1 : 0;
    // Lit in one color (the strands' own colors without GroomLab's hair graph): no colors stream.
    if (kind == 2) perVertex = false;
    if (batch.lines) buffers.linePoints = batch.lines->vertexCount();  // (the hair's density grid)
    // Width as coverage: strand lines (flat, colored or lit) whose shader can draw it (not ribbons).
    // Lit strands (kinds 2 and 9): GroomLab's hair effect, or Maya's hair tube graph (hairShading_).
    // Cards too (kind 6): with GroomLab's hair, shaded as the strands are (technique HairCard), so a
    // card reads as the strands it stands for; else Maya's hair graph as before.
    const bool hair = (kind == 2 || kind == 9 || (kind == 6 && hairShaderAvailable(*shaders))) && hairShading_;
    const bool coverage = batch.lines && !batch.ribbons && widthCoverage_ &&
                          (((kind == 0 || kind == 1) && linesShaderAvailable(*shaders)) || kind == 9 ||
                           (kind == 2 && (hair || plainHairShaderRegistered())));
    if (!buffers.shader || buffers.shaderKind != kind || buffers.coverage != coverage || buffers.hair != hair) {
        MHWRender::MShaderInstance* shader = nullptr;
        if (kind == 7) {  // guides, parts, flow: over the hair
            shader = acquireOverlayShader(*shaders);
        } else if (kind == 11) {  // dots: round, a size in pixels across
            shader = acquireDotsShader(*shaders);
        } else if (kind == 8) {  // a map shown on the scalp: its image, unlit
            shader = shaders->getStockShader(MHWRender::MShaderManager::k3dSolidTextureShader);
        } else if (kind == 6 && hair) {  // GroomLab's hair with the baked card maps (or solid)
            if (!hairState_) hairState_ = std::make_shared<HairGroomState>();
            shader = acquireHairShader(*shaders, false, false, hairState_, true);
        } else if (kind == 6) {  // hair shading with the baked card maps (or solid)
            // (Its draw callbacks: alpha to coverage on a multisampled target.)
            shader = shaders->getFragmentShader("groomLabCardSurface", "outSurfaceFinal", true, cardCoveragePreDraw,
                                                cardCoveragePostDraw);
            if (shader) noteCardShader(shader);
        } else if (kind == 10) {  // scales: Maya's hair tube shading in their own colors, lit from their normals
            shader = shaders->getFragmentShader("groomLabScaleSurface", "outSurfaceFinal", true);
        } else if (kind == 2 || kind == 9) {  // lit strands: in one color (2) or their own (9, the colors stream)
            shader = acquireLitShader(*shaders, kind, coverage);
        } else if (kind == 5) {  // a plain shaded surface (specular off: Lambert-like)
            shader = shaders->getStockShader(MHWRender::MShaderManager::k3dBlinnShader);
        } else if (thick) {
            shader = shaders->getStockShader(perVertex ? MHWRender::MShaderManager::k3dCPVThickLineShader
                                                       : MHWRender::MShaderManager::k3dThickLineShader);
        } else if (coverage) {  // strand lines, width as coverage
            shader = acquireLinesShader(*shaders, perVertex);
        } else {
            shader = shaders->getStockShader(
                perVertex ? MHWRender::MShaderManager::k3dCPVSolidShader : MHWRender::MShaderManager::k3dSolidShader);
        }
        if (!shader) {  // keep what the item has rather than leave it pointing at a released shader
            if (!buffers.shader) {
                if (item) container.remove(name);
                releaseGeometry(buffers);
                return;
            }
        } else {
            if (item) item->setShader(shader);  // swap first, then let the old one go
            if (buffers.shader) shaders->releaseShader(buffers.shader);
            buffers.shader = shader;
            buffers.shaderSerial = ++shaderSerials_;
            buffers.shaderKind = kind;
            buffers.coverage = coverage;
            buffers.hair = hair;
            // The coverage is hashed (its top blended: coveragePreDraw) in the opaque pass: depth written.
            if (coverage) shader->setIsTransparent(false);
        }
        if (kind == 7 || kind == 11) buffers.shader->setIsTransparent(false);
        if (kind == 6) {
            // Cut out in the opaque pass (the fragment clips, or its alpha says the samples covered:
            // alpha to coverage), as masked cards in engines: depth-correct, no sorting.
            buffers.shader->setIsTransparent(false);
            buffers.shader->setParameter("cutoff", 0.5f);
            if (!hair) buffers.shader->setParameter("coverageCutoff", 0.5f);  // (Maya's hair graph's coverage fragment)
        }
        if (!buffers.hair) hairShaderDefaults(*buffers.shader, kind);
    }
    if (!item) {
        item = MHWRender::MRenderItem::Create(name, MHWRender::MRenderItem::MaterialSceneItem,
                                              batch.triangles ? MHWRender::MGeometry::kTriangles
                                              : batch.points  ? MHWRender::MGeometry::kPoints
                                                              : MHWRender::MGeometry::kLines);
        item->setDrawMode(MHWRender::MGeometry::kAll);
        item->depthPriority(depthPriority);
        item->setShader(buffers.shader);
        container.add(item);
    }
    buffers.vertexColors = perVertex;
    if (kind == 8) bindItemTexture(buffers, mapTexture);
    style(*item, buffers, color, lineWidth);

    DrawTimes::Item& timed = currentDraw().items[name.asChar()];
    // Ribbons as lines: made by the GPU from the lines' points where it can (writeGpuRibbons), else
    // built here on the CPU (strandRibbons' triangles) and written as any geometry.
    if (batch.lines && batch.ribbons) {
        Lap ribbonLap;
        MHWRender::MVertexBufferArray streams;
        double megabytes = 0, ms[6] = {};
        if (writeGpuRibbons(buffers, batch, streams, &megabytes, ms)) {
            timed.acquireMs += ms[0];
            timed.writeMs += ms[1] + ms[2] + ms[3];
            timed.commitMs += ms[4] + ms[5];
            for (int i = 0; i < 6; ++i) timed.gpuParts[i] += ms[i];
            timed.gpuMegabytes += megabytes;
            timed.megabytes += megabytes;
            timed.gpuFrames = true;
            timed.follow = follow_ != nullptr;
            ribbonLap();
            const MBoundingBox bounds = shape_->boundingBox();
            bindGeometry(*item, streams, *buffers.indices, &bounds);
            ownBuffersBound(name);
            item->enable(true);
            buffers.shown = true;
            if (buffers.facing) {
                updateThinRibbons(container, buffers, *item, bounds, color, lineWidth);
            } else {
                dropThinRibbons(container);
            }
            timed.bindMs += ribbonLap();
            return;
        }
        dropThinRibbons(container);
        dropGpuRibbons(buffers);
        if (follow_) followBroken_ = true;  // (the strands at rest: drawn where they are from the next update)
        const auto built = std::make_shared<const DisplayGeometry>(batch.materialized());
        timed.writeMs += ribbonLap();  // (the CPU's ribbons built)
        updateItem(container, name, color, depthPriority, built, lineWidth, mapTexture);
        return;
    }
    Lap lap;
    // A buffer the GPU can't give (out of video memory, device lost) stops the update
    // (update() clears the drawing and says so) rather than writing through null.
    auto need = [&](void* memory, const char* what) {
        if (!memory) {
            container.remove(name);
            ownBuffersBound(name);
            releaseGeometry(buffers);
            throw std::runtime_error(std::string("out of video memory for the ") + what);
        }
        return memory;
    };
    // Strand lines are written straight into the buffers (DisplayGeometry::lines); other
    // geometry is copied from its vectors. An item's buffers are kept and written again.
    const StrandLines* lines = batch.lines.get();
    const auto vertexCount = static_cast<unsigned>(batch.vertexCount());
    // Strand lines with the same point counts as the ones on the GPU: their indices and uvs are there.
    const std::uint64_t layout = lines ? lines->layoutKey() : 0;
    const bool sameLayout = layout != 0 && buffers.layout == layout && buffers.indices &&
                            buffers.positions && buffers.positions->vertexCount() == vertexCount;
    using Writer = std::function<void(float*)>;
    auto copyOf = [](const std::vector<float>& data) -> Writer { return [&data](float* out) { copyParallel(out, data.data(), data.size()); }; };
    auto acquireVertices = [&](std::unique_ptr<MHWRender::MVertexBuffer>& buffer, MHWRender::MGeometry::Semantic semantic,
                               int dimension, const char* what) {
        if (!buffer) {
            buffer = std::make_unique<MHWRender::MVertexBuffer>(
                MHWRender::MVertexBufferDescriptor("", semantic, MHWRender::MGeometry::kFloat, dimension), softwareStaged());
        }
        timed.megabytes += 4.0 * dimension * vertexCount / 1e6;
        return static_cast<float*>(need(buffer->acquire(vertexCount, true), what));
    };
    MHWRender::MVertexBufferArray vertexBuffers;
    auto stream = [&](std::unique_ptr<MHWRender::MVertexBuffer>& buffer, const char* bufferName,
                      MHWRender::MGeometry::Semantic semantic, int dimension, const Writer& write) {
        lap();
        float* out = acquireVertices(buffer, semantic, dimension, bufferName);
        timed.acquireMs += lap();
        write(out);
        timed.writeMs += lap();
        buffer->commit(out);
        timed.commitMs += lap();
        vertexBuffers.addBuffer(bufferName, buffer.get());
    };
    // Lit strand lines: their points alone, the GPU writing their frames from them (gpu_lines.h),
    // where it can; else (or a device it cannot run on) the CPU writes every stream.
    const bool litLines = (kind == 2 || kind == 9) && lines && lines->lit;
    double gpuMegabytes = 0, gpuMs[6] = {};
    lap();
    // (The level of detail's impostors: a few thousand points, written on the CPU: the GPU's line engine keeps
    // one layout and root normals, the strands', which they would replace every update.)
    const bool gpuLines = litLines && name != kLodImpostorsItem && writeGpuLines(buffers, *lines, nullptr, true, &gpuMegabytes, gpuMs);
    if (follow_ && lines && !gpuLines && name != kLodImpostorsItem) followBroken_ = true;  // (the strands at rest: drawn where they are from the next update)
    if (gpuLines) {
        lap();
        // (acquire: the points sent; write: the root normals made and sent; commit: the kernels, the uvs)
        timed.acquireMs += gpuMs[0];
        timed.writeMs += gpuMs[1] + gpuMs[2] + gpuMs[3];
        timed.commitMs += gpuMs[4] + gpuMs[5];
        for (int i = 0; i < 6; ++i) timed.gpuParts[i] += gpuMs[i];
        timed.gpuMegabytes += gpuMegabytes;
        timed.megabytes += gpuMegabytes;
        timed.gpuFrames = true;
        timed.follow = follow_ != nullptr;
        vertexBuffers.addBuffer("positions", buffers.positions.get());
    } else {
        dropGpuStreams(buffers);  // (DirectX 11: the CPU writes Maya's own buffers again)
        stream(buffers.positions, "positions", MHWRender::MGeometry::kPosition, 3,
               lines ? Writer([lines](float* out) { lines->writePositions(out); }) : copyOf(batch.positions));
    }

    if (!sameLayout) {
        lap();
        const std::size_t indexCount = batch.indexCount();
        if (!buffers.indices) buffers.indices = std::make_unique<MHWRender::MIndexBuffer>(MHWRender::MGeometry::kUnsignedInt32, softwareStaged());
        auto* idx = static_cast<std::uint32_t*>(need(buffers.indices->acquire(static_cast<unsigned>(indexCount), true), "indices"));
        timed.acquireMs += lap();
        if (lines) {
            lines->writeIndices(idx);
        } else {
            copyParallel(idx, batch.indices.data(), indexCount);
        }
        timed.writeMs += lap();
        buffers.indices->commit(idx);
        timed.commitMs += lap();
        timed.megabytes += static_cast<double>(indexCount * sizeof(std::uint32_t)) / 1e6;
        buffers.layout = layout;
        if (!gpuLines) buffers.uvsKey = 0;  // (writeGpuLines wrote the uvs for these counts already)
    }

    if (kind == 7 || kind == 11) {
        // The overlay (and dots) effect's colors stream: the colors (or ones) with each vertex's pull
        // limit in the alpha, from a ray toward the camera; the geometry is kept for the next
        // camera position.
        overlayBatches_[name.asChar()] = geometry;
        uploadOverlayColors(buffers, batch, overlayPullLimits(batch, overlayCamera_, overlayPull_, occluderMeshes_));
        overlayFresh_.insert(name.asChar());
        vertexBuffers.addBuffer("colors", buffers.colors.get());
    } else if (perVertex) {
        // Strand lines' colors (the lit strands' own, kind 9; a color mode's, one a strand) are
        // kept while they are the colors the stream holds (a tweak that moves the strands alone
        // writes none); other colors are written whole.
        const StrandColors* own = lines ? lines->colors.get() : nullptr;
        if (own && own->key != 0 && own->key == buffers.colorsKey && sameLayout && buffers.colors &&
            buffers.colors->vertexCount() == vertexCount) {
            vertexBuffers.addBuffer("colors", buffers.colors.get());
        } else {
            stream(buffers.colors, "colors", MHWRender::MGeometry::kColor, 4,
                   lines ? Writer([lines](float* out) { lines->writeColors(out); }) : copyOf(batch.colors));
        }
    }
    buffers.colorsKey = perVertex && lines && lines->colors ? lines->colors->key : 0;
    // The uvs stream as the hair graph's uvCoord reads it (a float2): the lit strands' and
    // ribbons' u (one float a vertex, DisplayGeometry::uvFloats 1) written as (u, 0.5).
    auto expandedUvs = [&batch, &copyOf]() -> Writer {
        if (batch.uvFloats != 1) return copyOf(batch.uvs);
        return [&batch](float* out) {
            const std::vector<float>& u = batch.uvs;
            parallelFor(0, u.size(), [&](std::size_t v) {
                out[v * 2] = u[v];
                out[v * 2 + 1] = 0.5f;
            });
        };
    };
    auto uvs = [&]() {
        // Strand lines' uvs (u, width) depend on their point counts and widths alone: kept while
        // those are (a tweak, a frame of playback).
        const std::uint64_t key = lines ? hashCombine(layout, lines->widthsKey()) | 1u : 0;
        if (lines && sameLayout && buffers.uvs && buffers.uvsKey == key) {
            vertexBuffers.addBuffer("uvs", buffers.uvs.get());
            return;
        }
        stream(buffers.uvs, "uvs", MHWRender::MGeometry::kTexture, 2,
               lines ? Writer([lines](float* out) { lines->writeUvs(out, 2); }) : expandedUvs());
        buffers.uvsKey = key;
        buffers.uvsIdsKey = lines ? lines->idsKey() : 0;
    };
    if (gpuLines) {  // lit strand lines whose frames (and uvs) writeGpuLines wrote
        buffers.lit = true;
        buffers.bitangents.reset();  // the strands' graphs read none (groomLabHairTubeSpecular)
        vertexBuffers.addBuffer("normals", buffers.normals.get());
        vertexBuffers.addBuffer("tangents", buffers.tangents.get());
        vertexBuffers.addBuffer("uvs", buffers.uvs.get());
    } else if ((kind == 2 || kind == 6 || kind == 9) && lines) {  // lit strand lines: the frames in one pass, the uvs
        lap();
        float* normals = acquireVertices(buffers.normals, MHWRender::MGeometry::kNormal, 3, "normals");
        float* tangents = acquireVertices(buffers.tangents, MHWRender::MGeometry::kTangent, 3, "tangents");
        timed.acquireMs += lap();
        lines->writeFrames(normals, tangents);
        timed.writeMs += lap();
        buffers.lit = true;
        buffers.normals->commit(normals);
        buffers.tangents->commit(tangents);
        timed.commitMs += lap();
        buffers.bitangents.reset();  // the strands' graphs read none (groomLabHairTubeSpecular)
        vertexBuffers.addBuffer("normals", buffers.normals.get());
        vertexBuffers.addBuffer("tangents", buffers.tangents.get());
        uvs();
    } else if (kind == 2 || kind == 6 || kind == 9 || kind == 10) {  // hair shading: normals, the strand (card, scale) frame, UVs
        stream(buffers.normals, "normals", MHWRender::MGeometry::kNormal, 3, copyOf(batch.normals));
        stream(buffers.tangents, "tangents", MHWRender::MGeometry::kTangent, 3, copyOf(batch.tangents));
        if (kind == 6 || kind == 10) {  // the cards' and scales' shading normal rides in their bitangent stream
            stream(buffers.bitangents, "bitangents", MHWRender::MGeometry::kBitangent, 3, copyOf(batch.bitangents));
        } else {
            buffers.bitangents.reset();  // ribbons: the strands' graphs
        }
        uvs();
    } else if (kind == 8) {  // the image at the UVs
        buffers.normals.reset();
        buffers.tangents.reset();
        buffers.bitangents.reset();
        uvs();
    } else if (kind == 5) {  // normals only
        stream(buffers.normals, "normals", MHWRender::MGeometry::kNormal, 3, copyOf(batch.normals));
        buffers.tangents.reset();
        buffers.bitangents.reset();
        buffers.uvs.reset();
    } else {  // flat now: the lit streams go (the uvs stay with width as coverage: its widths)
        buffers.normals.reset();
        buffers.tangents.reset();
        buffers.bitangents.reset();
        if (buffers.coverage && lines) {
            uvs();
        } else {
            buffers.uvs.reset();
            buffers.uvsKey = 0;
        }
    }
    if (!perVertex && kind != 7) buffers.colors.reset();  // overlay lines keep theirs (the pull limits ride in it)
    if (!(lines && (kind == 2 || kind == 6 || kind == 9))) buffers.lit = false;
    lap();
    const MBoundingBox bounds = shape_->boundingBox();
    bindGeometry(*item, vertexBuffers, *buffers.indices, &bounds);
    ownBuffersBound(name);  // its own buffers again (not a GPU preview's)
    item->enable(true);
    buffers.shown = true;
    timed.bindMs += lap();
}

bool GroomSubSceneOverride::updateItemPartly(MHWRender::MSubSceneContainer& container, const MString& name,
                                             const float color[4], const DisplayItem& item) {
    const StrandLines* lines = item.geometry ? item.geometry->lines.get() : nullptr;
    MHWRender::MRenderItem* renderItem = container.find(name);
    auto it = items_.find(name.asChar());
    if (!item.updated || !lines || !lines->strands || !renderItem || it == items_.end()) return false;
    ItemBuffers& buffers = it->second;
    const auto vertexCount = static_cast<unsigned>(lines->vertexCount());
    const bool lit = lines->lit;
    // Lit lines in the strands' own colors (kind 9) have their colors stream to keep or write in
    // part too; the color modes' lines (and lit ones drawn in one color) none.
    const StrandColors* colors = lit && buffers.shaderKind == 9 ? lines->colors.get() : nullptr;
    // The uvs (u, width) are written with the points where the shader reads their widths (width as
    // coverage; turning it on or off writes the lines whole).
    const bool withUvs = buffers.coverage && buffers.uvs;
    if ((lines->colors && !lit) || !buffers.shader || !buffers.positions || !buffers.indices ||
        buffers.layout != lines->layoutKey() || buffers.positions->vertexCount() != vertexCount || buffers.lit != lit ||
        (lit && (!buffers.normals || !buffers.tangents)) || buffers.vertexColors != (colors != nullptr) ||
        (colors && (!buffers.colors || buffers.colors->vertexCount() != vertexCount)) ||
        (buffers.coverage && (!buffers.uvs || buffers.uvs->vertexCount() != vertexCount))) {
        return false;
    }
    // Colors other than the stream holds: the changed strands' (only they can differ: the colors
    // come with the strands, HairColor's incremental runs color only the strands it was handed).
    const bool recolor = colors && (colors->key == 0 || colors->key != buffers.colorsKey);
    constexpr std::size_t kColorFloats = 4;  // RGBA
    DrawTimes::Item& timed = currentDraw().items[name.asChar()];
    Lap lap;
    const StrandSet& strands = *lines->strands;
    const std::vector<std::pair<std::size_t, std::size_t>> runs = strandRuns(item.updated->strands, 64);
    timed.partialStrands = item.updated->strands.size();
    // Lit lines whose frames the GPU writes (gpu_lines.h): the changed strands' points (and uvs),
    // the frames by the GPU; their own colors (kind 9) in Maya's buffer as below.
    double gpuMegabytes = 0, gpuMs[6] = {};
    const bool gpuLines = lit && writeGpuLines(buffers, *lines, &runs, withUvs, &gpuMegabytes, gpuMs);
    if (!gpuLines && buffers.gpuStreams) {  // streams the GPU cannot write now: the item written whole (on the CPU)
        timed.partialStrands = 0;
        return false;
    }
    if (gpuLines) {
        timed.writeMs += lap();
        timed.megabytes += gpuMegabytes;
        timed.gpuFrames = true;
        for (int i = 0; i < 6; ++i) timed.gpuParts[i] += gpuMs[i];
        timed.gpuMegabytes += gpuMegabytes;
        if (recolor && softwareStaged()) {
            // (DirectX 11: the staged copy taken as it is, the changed strands written into it, sent once.)
            auto* rgba = static_cast<float*>(buffers.colors->acquire(vertexCount, false));
            if (!rgba) {
                timed.partialStrands = 0;
                return false;  // written whole
            }
            parallelFor(0, runs.size(), [&](std::size_t r) {
                lines->writeColors(rgba + strands.strandBegin(runs[r].first) * kColorFloats, runs[r].first, runs[r].second);
            }, 1);
            buffers.colors->commit(rgba);
            timed.megabytes += 4.0 * static_cast<double>(kColorFloats * vertexCount) / 1e6;
        } else if (recolor) {
            thread_local std::vector<float> scratch;
            for (const auto& [first, end] : runs) {
                const std::size_t begin = strands.strandBegin(first), n = strands.offsets()[end] - begin;
                scratch.resize(n * kColorFloats);
                lines->writeColors(scratch.data(), first, end);
                buffers.colors->update(scratch.data(), static_cast<unsigned>(begin), static_cast<unsigned>(n), false);
                timed.megabytes += 4.0 * static_cast<double>(kColorFloats * n) / 1e6;
            }
        }
        timed.commitMs += lap();
    } else if (softwareStaged()) {
        // DirectX 11: the buffers keep a system-memory copy, and every update() sends the whole
        // buffer to the GPU again (~5 ms for a 33 MB stream: a dab of 50 runs over four streams
        // took 200-600 ms at 500k strands, a release 2.5 s). So the copy is taken as it is
        // (acquire, not write-only: it holds what was written last), the changed strands written
        // into it where they are, and each stream sent once.
        float* positions = static_cast<float*>(buffers.positions->acquire(vertexCount, false));
        float* frames[2] = {nullptr, nullptr};
        float* rgba = nullptr;
        float* uv = nullptr;
        if (lit && positions) {
            frames[0] = static_cast<float*>(buffers.normals->acquire(vertexCount, false));
            frames[1] = static_cast<float*>(buffers.tangents->acquire(vertexCount, false));
        }
        if (recolor && positions) rgba = static_cast<float*>(buffers.colors->acquire(vertexCount, false));
        if (withUvs && positions) uv = static_cast<float*>(buffers.uvs->acquire(vertexCount, false));
        timed.acquireMs += lap();
        if (!positions || (lit && (!frames[0] || !frames[1])) || (recolor && !rgba) || (withUvs && !uv)) {
            // Not given (the device's memory): what was acquired goes back untouched, and the
            // item is written whole.
            if (positions) buffers.positions->commit(positions);
            if (frames[0]) buffers.normals->commit(frames[0]);
            if (frames[1]) buffers.tangents->commit(frames[1]);
            if (rgba) buffers.colors->commit(rgba);
            if (uv) buffers.uvs->commit(uv);
            timed.partialStrands = 0;
            return false;
        }
        parallelFor(0, runs.size(), [&](std::size_t r) {
            const std::size_t begin = strands.strandBegin(runs[r].first), at = begin * 3;
            lines->writePositions(positions + at, runs[r].first, runs[r].second);
            if (lit) lines->writeFrames(frames[0] + at, frames[1] + at, runs[r].first, runs[r].second);
            if (rgba) lines->writeColors(rgba + begin * kColorFloats, runs[r].first, runs[r].second);
            if (uv) lines->writeUvs(uv + begin * 2, 2, runs[r].first, runs[r].second);
        }, 1);
        timed.writeMs += lap();
        buffers.positions->commit(positions);
        if (lit) {
            buffers.normals->commit(frames[0]);
            buffers.tangents->commit(frames[1]);
        }
        if (rgba) buffers.colors->commit(rgba);
        if (uv) buffers.uvs->commit(uv);
        timed.commitMs += lap();
        timed.megabytes += 4.0 * static_cast<double>((3 * (lit ? 3 : 1) + (rgba ? kColorFloats : 0) + (uv ? 2 : 0)) * vertexCount) / 1e6;  // each stream sent whole
    } else {
        // OpenGL (GPU-only buffers): the changed strands' vertices, run after run side by side in
        // one scratch array, then each run into the GPU buffers where its vertices are.
        std::vector<std::size_t> at(runs.size() + 1, 0);
        for (std::size_t r = 0; r < runs.size(); ++r) {
            at[r + 1] = at[r] + (strands.offsets()[runs[r].second] - strands.strandBegin(runs[r].first));
        }
        thread_local std::vector<float> scratch;
        const std::size_t streams = lit ? 3 : 1, colorFloats = recolor ? kColorFloats : 0, uvFloats = withUvs ? 2 : 0;
        scratch.resize((streams * 3 + colorFloats + uvFloats) * at.back());
        float* positions = scratch.data();
        float* frames[2] = {positions + at.back() * 3, positions + 2 * at.back() * 3};
        float* rgba = positions + streams * 3 * at.back();
        float* uv = rgba + colorFloats * at.back();
        parallelFor(0, runs.size(), [&](std::size_t r) {
            lines->writePositions(positions + at[r] * 3, runs[r].first, runs[r].second);
            if (lit) lines->writeFrames(frames[0] + at[r] * 3, frames[1] + at[r] * 3, runs[r].first, runs[r].second);
            if (recolor) lines->writeColors(rgba + at[r] * colorFloats, runs[r].first, runs[r].second);
            if (withUvs) lines->writeUvs(uv + at[r] * 2, 2, runs[r].first, runs[r].second);
        }, 1);
        timed.writeMs += lap();
        for (std::size_t r = 0; r < runs.size(); ++r) {
            const auto first = static_cast<unsigned>(strands.strandBegin(runs[r].first));
            const auto count = static_cast<unsigned>(at[r + 1] - at[r]);
            buffers.positions->update(positions + at[r] * 3, first, count, false);
            if (lit) {
                buffers.normals->update(frames[0] + at[r] * 3, first, count, false);
                buffers.tangents->update(frames[1] + at[r] * 3, first, count, false);
            }
            if (recolor) buffers.colors->update(rgba + at[r] * colorFloats, first, count, false);
            if (withUvs) buffers.uvs->update(uv + at[r] * 2, first, count, false);
            timed.megabytes += 4.0 * static_cast<double>((streams * 3 + colorFloats + uvFloats) * count) / 1e6;
        }
        timed.commitMs += lap();
    }
    if (colors) buffers.colorsKey = colors->key;
    // The uvs hold these strands' widths now (the others' did not change).
    if (withUvs && !gpuLines) buffers.uvsKey = hashCombine(lines->layoutKey(), lines->widthsKey()) | 1u;
    style(*renderItem, buffers, color, item.lineWidth);
    // The same buffers with the groom's bounds as they are now (the strands moved).
    MHWRender::MVertexBufferArray vertexBuffers;
    addStreams(vertexBuffers, buffers);
    const MBoundingBox bounds = shape_->boundingBox();
    bindGeometry(*renderItem, vertexBuffers, *buffers.indices, &bounds);
    ownBuffersBound(name);
    renderItem->enable(true);
    buffers.shown = true;
    timed.bindMs += lap();
    return true;
}

void GroomSubSceneOverride::hideItem(MHWRender::MSubSceneContainer& container, const MString& name) {
    if (MHWRender::MRenderItem* renderItem = container.find(name)) renderItem->enable(false);
    auto it = items_.find(name.asChar());
    if (it != items_.end()) it->second.shown = false;
}

void GroomSubSceneOverride::addStreams(MHWRender::MVertexBufferArray& streams, const ItemBuffers& b, MHWRender::MVertexBuffer* positions,
                                       MHWRender::MVertexBuffer* normals, MHWRender::MVertexBuffer* tangents,
                                       MHWRender::MVertexBuffer* uvs) {
    streams.addBuffer("positions", positions ? positions : b.positions.get());
    if (b.lit && b.normals && b.tangents) {
        streams.addBuffer("normals", normals ? normals : b.normals.get());
        streams.addBuffer("tangents", tangents ? tangents : b.tangents.get());
    }
    if ((b.lit || b.coverage) && b.uvs) streams.addBuffer("uvs", uvs ? uvs : b.uvs.get());
    if (b.vertexColors && b.colors) streams.addBuffer("colors", b.colors.get());
}

void GroomSubSceneOverride::releaseGeometry(ItemBuffers& buffers) {
    buffers.layout = buffers.uvsKey = buffers.uvsIdsKey = 0;
    buffers.followRest = buffers.followTo = 0;
    buffers.colorsKey = 0;
    buffers.ribbonLayout = buffers.ribbonUvsKey = buffers.ribbonColorsKey = buffers.ribbonIdsKey = 0;
    buffers.lit = false;
    buffers.ribbonPoints.reset();
    buffers.ribbonUvs.reset();
    buffers.positions.reset();
    buffers.colors.reset();
    buffers.normals.reset();
    buffers.tangents.reset();
    buffers.bitangents.reset();
    buffers.uvs.reset();
    buffers.indices.reset();
    buffers.lineIndices.reset();
    buffers.gpuStreams.reset();  // after the MVertexBuffers naming them
    buffers.ribbonStreams.reset();
    buffers.shown = false;
}

void GroomSubSceneOverride::dropGpuStreams(ItemBuffers& buffers) {
    if (!buffers.gpuStreams) return;
    buffers.positions.reset();
    buffers.normals.reset();
    buffers.tangents.reset();
    buffers.uvs.reset();
    buffers.gpuStreams.reset();  // after the MVertexBuffers naming them
    buffers.uvsKey = buffers.uvsIdsKey = 0;
    buffers.followRest = buffers.followTo = 0;
}

void GroomSubSceneOverride::restyleItem(MHWRender::MSubSceneContainer& container, const MString& name,
                                        const float color[4], float lineWidth) {
    MHWRender::MRenderItem* item = container.find(name);
    auto it = items_.find(name.asChar());
    if (!item || it == items_.end() || !it->second.shown || !it->second.shader) return;
    style(*item, it->second, color, lineWidth);
}

bool GroomSubSceneOverride::recolorable(MHWRender::MSubSceneContainer& container, std::size_t vertexCount) {
    const auto it = items_.find(kColoredStrandsItem.asChar());
    if (!container.find(kColoredStrandsItem) || it == items_.end()) return false;
    const ItemBuffers& buffers = it->second;
    return buffers.shown && buffers.shader && buffers.shaderKind == 1 && buffers.positions && buffers.indices && buffers.colors &&
           vertexCount != 0 && buffers.positions->vertexCount() == vertexCount;
}

void GroomSubSceneOverride::recolorStrands(MHWRender::MSubSceneContainer& container, const StrandColors& colors,
                                           const float color[4]) {
    MHWRender::MRenderItem* item = container.find(kColoredStrandsItem);
    ItemBuffers& buffers = items_[kColoredStrandsItem.asChar()];
    const auto vertexCount = static_cast<unsigned>(colors.vertexCount());
    if (!item || !recolorable(container, vertexCount)) throw std::runtime_error("the strand lines to recolor are gone");
    // Written again in place (never a new buffer, never one freed while the item uses it).
    auto* rgba = static_cast<float*>(buffers.colors->acquire(vertexCount, true));
    if (!rgba) throw std::runtime_error("out of video memory for the colors");
    colors.write(rgba);
    buffers.colors->commit(rgba);
    buffers.colorsKey = colors.key;  // (0: no key; the colors it was built with are gone)
    MHWRender::MVertexBufferArray vertexBuffers;
    addStreams(vertexBuffers, buffers);
    const MBoundingBox bounds = shape_->boundingBox();
    bindGeometry(*item, vertexBuffers, *buffers.indices, &bounds);
    ownBuffersBound(kColoredStrandsItem);
    style(*item, buffers, color, 1.0f);
}
}  // namespace groomlab::maya
