// SPDX-License-Identifier: Apache-2.0
// Viewport geometry (display.h): parts, flow, the overlays' pull limits, cards and their map,
// the map shown on the scalp.

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

#include "groomlab/curve.h"
#include "groomlab/display.h"
#include "groomlab/frames.h"
#include "groomlab/parallel.h"
#include "groomlab/png.h"
#include "groomlab/portable_math.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/texture.h"

namespace groomlab {

void appendPartLines(DisplayGeometry& geometry, const PartLines& parts) {
    for (const auto& line : parts.lines()) appendPolyline(geometry, line.data(), line.size());
}

void buildFlowOverlay(const FlowField& flowField, const ScalpMesh& scalpMesh, DisplayGeometry& arrows, DisplayGeometry& field) {
    const FlowField* flow = &flowField;
    const ScalpMesh* scalp = &scalpMesh;
    if (scalp->pointCount() == 0) return;
    Vec3f lo = scalp->points().front(), hi = lo;
    for (const Vec3f& p : scalp->points()) {
        lo = Vec3f(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
        hi = Vec3f(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
    }
    const float size = (hi - lo).length();
    const float lift = size * 0.002f;  // off the skin, so the lines don't z-fight it
    auto arrow = [&](DisplayGeometry& batch, const Vec3f& at, const Vec3f& dir, const Vec3f& normal, float length, bool head) {
        const Vec3f base = at + normal * lift, tip = base + dir * length;
        appendPolyline(batch, std::array<Vec3f, 2>{base, tip}.data(), 2);
        if (!head) return;
        const Vec3f side = normal.cross(dir).normalized() * (0.3f * length);
        appendPolyline(batch, std::array<Vec3f, 3>{tip - dir * (0.35f * length) + side, tip, tip - dir * (0.35f * length) - side}.data(), 3);
    };
    // Each stroke as one smooth curve along its pins with an arrowhead at its end (a run breaks
    // where pins were erased); pins on their own as arrows.
    const auto& positions = flow->positions();
    std::map<int, std::vector<std::size_t>> strokes;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (flow->lines()[i] >= 0) {
            strokes[flow->lines()[i]].push_back(i);
            continue;
        }
        const Vec3f n = scalp->closestPoint(positions[i]).normal.normalized();
        arrow(arrows, positions[i], flow->directions()[i], n, size * 0.03f, true);
    }
    const float head = size * 0.03f;
    for (auto& [line, members] : strokes) {
        std::stable_sort(members.begin(), members.end(), [&](std::size_t a, std::size_t b) { return flow->seqs()[a] < flow->seqs()[b]; });
        std::size_t start = 0;
        while (start < members.size()) {
            std::size_t end = start + 1;
            while (end < members.size() && flow->seqs()[members[end]] == flow->seqs()[members[end - 1]] + 1) ++end;
            std::vector<Vec3f> run;
            Vec3f lastNormal;
            for (std::size_t k = start; k < end; ++k) {
                lastNormal = scalp->closestPoint(positions[members[k]]).normal.normalized();
                run.push_back(positions[members[k]] + lastNormal * lift);
            }
            const std::size_t last = members[end - 1];
            if (run.size() == 1) {
                arrow(arrows, positions[last], flow->directions()[last], lastNormal, head, true);
            } else {
                // The curve through the pins, carried on a little past the last one to the arrowhead.
                const Vec3f tip = run.back() + flow->directions()[last] * (0.6f * head);
                run.push_back(tip);
                const std::vector<Vec3f> smooth = smoothPolyline(run.data(), run.size(), 6);
                appendPolyline(arrows, smooth.data(), smooth.size());
                const Vec3f dir = flow->directions()[last];
                const Vec3f side = lastNormal.cross(dir).normalized() * (0.3f * head);
                appendPolyline(arrows, std::array<Vec3f, 3>{tip - dir * (0.35f * head) + side, tip, tip - dir * (0.35f * head) - side}.data(), 3);
            }
            start = end;
        }
    }
    // Sinks: their radius as a ring on the scalp and six streamlines turning around the center.
    auto onScalp = [&](const Vec3f& p, Vec3f* normal) {
        const SurfacePoint sp = scalp->closestPoint(p);
        if (sp.root.face < 0) return p;
        if (normal) *normal = sp.normal.normalized();
        return sp.position + sp.normal.normalized() * lift;
    };
    for (std::size_t s = 0; s < flow->sinkPositions().size(); ++s) {
        const Vec3f c = flow->sinkPositions()[s], n = flow->sinkNormals()[s];
        const float r = flow->sinkRadii()[s];
        const Vec3f x = anyPerpendicular(n).normalized(), y = n.cross(x);
        std::vector<Vec3f> ring;
        for (int k = 0; k <= 48; ++k) {
            const float a = 6.2831853f * static_cast<float>(k) / 48.0f;
            ring.push_back(onScalp(c + (x * pm::cos(a) + y * pm::sin(a)) * r, nullptr));
        }
        appendPolyline(arrows, ring.data(), ring.size());
        for (int k = 0; k < 6; ++k) {
            const float a = 6.2831853f * static_cast<float>(k) / 6.0f;
            Vec3f p = c + (x * pm::cos(a) + y * pm::sin(a)) * (0.2f * r), normal = n;
            std::vector<Vec3f> line{onScalp(p, &normal)};
            Vec3f heading;
            for (int step = 0; step < 24; ++step) {
                float w = 0.0f;
                const Vec3f d = flow->sinkDirection(s, p, normal, &w);
                if (w <= 0.05f || d.length() < 0.5f) break;
                heading = d;
                p = p + d * (0.06f * r);
                const Vec3f q = onScalp(p, &normal);
                p = q - normal * lift;
                line.push_back(q);
                if ((p - c).length() > 0.85f * r && flow->sinkAngles()[s] < 1.6f) break;  // parting out: stop near the ring
            }
            if (line.size() < 2) continue;
            appendPolyline(arrows, line.data(), line.size());
            const Vec3f tip = line.back(), side = normal.cross(heading).normalized() * (0.06f * r);
            appendPolyline(arrows, std::array<Vec3f, 3>{tip - heading * (0.1f * r) + side, tip, tip - heading * (0.1f * r) - side}.data(), 3);
        }
    }
    const std::size_t stride = std::max<std::size_t>(1, scalp->pointCount() / 1500);
    for (std::size_t v = 0; v < scalp->pointCount(); v += stride) {
        const SurfacePoint sp = scalp->closestPoint(scalp->points()[v]);
        if (sp.root.face < 0) continue;
        const Vec3f n = sp.normal.normalized();
        const Vec3f d = flow->at(sp.position, n);
        if (d.length() > 0.5f) arrow(field, sp.position, d, n, size * 0.012f, true);
    }
}

std::vector<float> overlayPullLimits(const std::vector<Vec3f>& points, const Vec3f& camera, float pull,
                                     const std::vector<std::shared_ptr<const ScalpMesh>>& occluders) {
    std::vector<float> limits(points.size(), 1.0f);
    if (occluders.empty() || pull <= 0.0f || points.empty()) return limits;
    // A ray per point (every `stride`-th on big batches, its neighbours taking its answer:
    // consecutive points are along one line), from just off the point toward the camera.
    const std::size_t stride = std::max<std::size_t>(1, points.size() / 40000);
    const float eps = 1e-3f * pull;
    const std::size_t samples = (points.size() + stride - 1) / stride;
    parallelFor(0, samples, [&](std::size_t k) {
        const std::size_t i = k * stride;
        const Vec3f& p = points[i];
        Vec3f dir = camera - p;
        const float away = dir.length();
        float limit = 1.0f;
        if (away > 1e-6f) {
            dir = dir * (1.0f / away);
            float nearest = pull;
            const Vec3f origin = p + dir * eps;
            for (const auto& mesh : occluders) {
                SurfacePoint hit;
                float t = 0.0f;
                if (mesh && mesh->intersectRay(origin, dir, &hit, &t) && t < nearest) nearest = std::max(t, 0.0f);
            }
            // Something in the way: short of it, so a dot drawn about the point stays behind it.
            if (nearest < pull) nearest *= 0.9f;
            limit = std::clamp(nearest / pull, 0.0f, 1.0f);
        }
        for (std::size_t j = i; j < std::min(i + stride, points.size()); ++j) limits[j] = limit;
    });
    return limits;
}

std::vector<float> overlayPullLimits(const DisplayGeometry& geometry, const Vec3f& camera, float pull,
                                     const std::vector<std::shared_ptr<const ScalpMesh>>& occluders) {
    if (occluders.empty() || pull <= 0.0f) return std::vector<float>(geometry.vertexCount(), 1.0f);
    std::vector<Vec3f> points(geometry.vertexCount());
    const float* p = geometry.positions.data();
    for (std::size_t v = 0; v < points.size(); ++v) points[v] = {p[v * 3], p[v * 3 + 1], p[v * 3 + 2]};
    return overlayPullLimits(points, camera, pull, occluders);
}

void writeOverlayColors(const DisplayGeometry& geometry, const std::vector<float>& limits, float* col) {
    const std::size_t vertexCount = geometry.vertexCount();
    const bool perVertex = geometry.colors.size() == vertexCount * 4;
    for (std::size_t v = 0; v < vertexCount; ++v) {
        const float* rgb = perVertex ? geometry.colors.data() + v * 4 : nullptr;
        col[v * 4] = rgb ? rgb[0] : 1.0f;
        col[v * 4 + 1] = rgb ? rgb[1] : 1.0f;
        col[v * 4 + 2] = rgb ? rgb[2] : 1.0f;
        col[v * 4 + 3] = v < limits.size() ? limits[v] : 1.0f;
    }
}

DisplayGeometry cardGeometry(const CardMesh& cardMesh, bool textured) {
    DisplayGeometry cardBatch;
    cardBatch.triangles = true;
    cardBatch.surface = true;
    cardBatch.cardTextures = true;
    const std::size_t count = cardMesh.vertexCount();
    cardBatch.positions.assign(count * 3, 0.0f);
    cardBatch.normals.assign(count * 3, 0.0f);
    cardBatch.tangents.assign(count * 3, 0.0f);
    cardBatch.bitangents.assign(count * 3, 0.0f);
    cardBatch.uvs.assign(count * 2, 0.0f);
    cardBatch.colors.assign(count * 4, 0.5f);  // the card effect's tint (its fcolor input)
    auto put = [](std::vector<float>& into, std::size_t v, const Vec3f& value) {
        into[v * 3] = value.x;
        into[v * 3 + 1] = value.y;
        into[v * 3 + 2] = value.z;
    };
    // Cards in parallel: each writes its own vertices.
    parallelFor(0, cardMesh.cards.size(), [&](std::size_t c) {
        const CardMesh::Card& card = cardMesh.cards[c];
        const auto columns = static_cast<std::size_t>(std::max(card.across, 1) + 1);
        for (int k = 0; k < card.stations; ++k) {
            const int k0 = std::max(k - 1, 0), k1 = std::min(k + 1, card.stations - 1);
            for (std::size_t e = 0; e < columns; ++e) {
                const std::size_t v = card.firstVertex + static_cast<std::size_t>(k) * columns + e;
                if (v >= count) continue;
                const Vec3f& n = cardMesh.normals[v];
                // Up the card, root to tip: what the hair shading takes as the strand direction.
                const Vec3f t = (cardMesh.positions[card.firstVertex + static_cast<std::size_t>(k1) * columns + e] -
                                 cardMesh.positions[card.firstVertex + static_cast<std::size_t>(k0) * columns + e]).normalized();
                put(cardBatch.positions, v, cardMesh.positions[v]);
                put(cardBatch.normals, v, n);
                put(cardBatch.tangents, v, t);
                // The shading normal again: the card effect reads it from here, a stream two-sided
                // lighting never flips (a strand's drawing has no back to flip, and a card standing
                // for strands shades as they do from either side).
                put(cardBatch.bitangents, v, n);
                if (textured) cardBatch.uvs[v * 2] = cardMesh.u[v], cardBatch.uvs[v * 2 + 1] = cardMesh.v[v];  // the atlas
                // The tint as the exports' vertex color has it: half of it (0.5 none).
                const std::array<float, 3> tint = v < cardMesh.tints.size() ? cardMesh.tints[v] : std::array<float, 3>{1.0f, 1.0f, 1.0f};
                for (std::size_t k = 0; k < 3; ++k) cardBatch.colors[v * 4 + k] = 0.5f * tint[k];
                // 1 plus the card's iridescence, as the lit strands' colors carry theirs.
                cardBatch.colors[v * 4 + 3] = 1.0f + (v < cardMesh.iridescence.size() ? cardMesh.iridescence[v] : 0.0f);
            }
        }
    }, 64);
    const std::size_t quads = cardMesh.quads.size() / 4;
    cardBatch.indices.resize(quads * 6);
    for (std::size_t q = 0; q < quads; ++q) {
        const std::uint32_t* in = cardMesh.quads.data() + q * 4;
        std::uint32_t* out = cardBatch.indices.data() + q * 6;
        out[0] = in[0], out[1] = in[1], out[2] = in[2], out[3] = in[0], out[4] = in[2], out[5] = in[3];
    }
    return cardBatch;
}

DisplayGeometry cardWireGeometry(const DisplayGeometry& cards) {
    DisplayGeometry wire;
    if (!cards.triangles || cards.indices.size() % 6 != 0) return wire;
    // Each quad is two triangles (a, b, c) (a, c, d): its edges ab, bc, cd, da; a neighbor's
    // shared edge is kept once.
    std::vector<std::uint64_t> edges;
    edges.reserve(cards.indices.size() / 6 * 4);
    for (std::size_t q = 0; q < cards.indices.size(); q += 6) {
        const std::uint32_t v[4] = {cards.indices[q], cards.indices[q + 1], cards.indices[q + 2], cards.indices[q + 5]};
        for (int e = 0; e < 4; ++e) {
            const std::uint32_t a = v[e], b = v[(e + 1) % 4];
            edges.push_back((static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b));
        }
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    wire.positions = cards.positions;
    wire.indices.reserve(edges.size() * 2);
    for (const std::uint64_t e : edges) {
        wire.indices.push_back(static_cast<std::uint32_t>(e >> 32));
        wire.indices.push_back(static_cast<std::uint32_t>(e));
    }
    return wire;
}

std::vector<std::uint8_t> combineCardMaps(const std::uint8_t* c, const std::uint8_t* cov, const std::uint8_t* occ,
                                          std::size_t width, std::size_t height) {
    // sRGB <-> linear by table, the AO darkening in linear light.
    float toLinear[256];
    for (int i = 0; i < 256; ++i) {
        const float v = static_cast<float>(i) / 255.0f;
        toLinear[i] = v <= 0.04045f ? v / 12.92f : pm::pow((v + 0.055f) / 1.055f, 2.4f);
    }
    auto toSrgb = [](float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        return static_cast<std::uint8_t>((v <= 0.0031308f ? 12.92f * v : 1.055f * pm::pow(v, 1.0f / 2.4f) - 0.055f) * 255.0f + 0.5f);
    };
    const std::size_t pixels = width * height;
    std::vector<std::uint8_t> rgba(pixels * 4);
    parallelFor(0, pixels, [&](std::size_t i) {
        const std::size_t at = i * 4;
        if (occ) {
            const float dark = 1.0f - 0.5f * (1.0f - static_cast<float>(occ[at]) / 255.0f);  // half the AO
            for (std::size_t k = 0; k < 3; ++k) rgba[at + k] = toSrgb(toLinear[c[at + k]] * dark);
        } else {
            for (std::size_t k = 0; k < 3; ++k) rgba[at + k] = c[at + k];
        }
        rgba[at + 3] = cov[at];
    });
    return rgba;
}

void writeCardBakePreview(const std::string& prefix) {
    // Read as a viewport loads them (8-bit RGBA, a gray map in r g b); the bytes back from the
    // loader's floats exactly.
    int width = 0, height = 0;
    auto bytes = [&](const char* map) {
        const TexturePtr texture = loadTexture(prefix + map + ".tga");
        const Texture::Image* image = texture ? texture->image() : nullptr;
        if (!image) throw std::runtime_error("cannot read " + prefix + map + ".tga");
        if (width == 0) {
            width = image->width;
            height = image->height;
        } else if (image->width != width || image->height != height) {
            throw std::runtime_error("the card maps of " + prefix + " differ in size");
        }
        std::vector<std::uint8_t> out(image->rgba.size());
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<std::uint8_t>(std::lround(std::clamp(image->rgba[i], 0.0f, 1.0f) * 255.0f));
        }
        return out;
    };
    const std::vector<std::uint8_t> color = bytes("Color"), coverage = bytes("Coverage"), occlusion = bytes("AO");
    const std::vector<std::uint8_t> preview = combineCardMaps(color.data(), coverage.data(), occlusion.data(),
                                                              static_cast<std::size_t>(width), static_cast<std::size_t>(height));
    writeCompressedPng(prefix + "Preview.png", width, height, 4, preview);
}

DisplayGeometry scalpMapGeometry(const ScalpMesh* scalp, const ScalpMapView& view, bool textured) {
    DisplayGeometry batch;
    textured = textured && scalp && scalp->hasUVs();
    const std::size_t per = view.channels == 4 ? 4 : 1;
    const bool perVertex = scalp && view.values && view.values->size() == scalp->pointCount() * per;
    if (!textured && !perVertex) return batch;
    batch.triangles = true;
    const std::vector<Vec3f>& points = scalp->points();
    // Area-weighted vertex normals, to lift the surface off the mesh (and light it).
    std::vector<Vec3f> normals(points.size());
    Vec3f lo = points.empty() ? Vec3f{} : points[0], hi = lo;
    for (const Vec3f& p : points) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    for (const auto& tri : scalp->triangles()) {
        const Vec3f& a = points[static_cast<std::size_t>(tri[0])];
        const Vec3f n = (points[static_cast<std::size_t>(tri[1])] - a).cross(points[static_cast<std::size_t>(tri[2])] - a);
        for (const std::int32_t v : tri) normals[static_cast<std::size_t>(v)] += n;
    }
    const float lift = 0.0015f * std::max((hi - lo).length(), 1e-6f);
    for (Vec3f& n : normals) n = n.normalized();
    auto put = [](std::vector<float>& into, const Vec3f& value) {
        into.push_back(value.x);
        into.push_back(value.y);
        into.push_back(value.z);
    };
    if (textured) {
        batch.mapTexture = true;  // the image at the corners' uvs
        const auto& uvs = scalp->cornerUVs();
        std::uint32_t next = 0;
        for (std::size_t tri = 0; tri < scalp->triangleCount(); ++tri) {
            for (std::size_t k = 0; k < 3; ++k) {
                const auto v = static_cast<std::size_t>(scalp->triangles()[tri][k]);
                put(batch.positions, points[v] + normals[v] * lift);
                batch.uvs.push_back(uvs[tri * 3 + k][0]);
                batch.uvs.push_back(uvs[tri * 3 + k][1]);
                batch.indices.push_back(next++);
            }
        }
        return batch;
    }
    for (std::size_t v = 0; v < points.size(); ++v) {
        put(batch.positions, points[v] + normals[v] * lift);
        // Gray per vertex, or a color map's color (or its picked channel as gray).
        const float* x = view.values->data() + v * per;
        const std::array<float, 3> c =
            view.shown(per == 1 ? std::array<float, 4>{x[0], x[0], x[0], 1.0f} : std::array<float, 4>{x[0], x[1], x[2], x[3]});
        batch.colors.insert(batch.colors.end(),
                            {std::clamp(c[0], 0.0f, 1.0f), std::clamp(c[1], 0.0f, 1.0f), std::clamp(c[2], 0.0f, 1.0f), 1.0f});
    }
    for (const auto& tri : scalp->triangles()) {
        for (const std::int32_t v : tri) batch.indices.push_back(static_cast<std::uint32_t>(v));
    }
    return batch;
}

int scalpMapPick(const std::string& channel) {
    return channel == "r" ? 0 : channel == "g" ? 1 : channel == "b" ? 2 : channel == "a" ? 3 : -1;
}

void scalpMapFilePixels(std::uint8_t* rgba, std::size_t pixels, int channels, int pick) {
    if (channels != 4) {
        for (std::size_t i = 0; i < pixels; ++i) rgba[i * 4 + 3] = 255;  // the map is its gray; nothing cut out
        return;
    }
    // A color map: its color (alpha not cutting anything out), or one channel picked as gray.
    for (std::size_t i = 0; i < pixels; ++i) {
        if (pick >= 0) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = rgba[i * 4 + static_cast<std::size_t>(pick)];
        rgba[i * 4 + 3] = 255;
    }
}

std::vector<std::uint8_t> scalpMapCachedPixels(const Texture::Image& image, int channels, int pick) {
    const auto width = static_cast<std::size_t>(image.width), height = static_cast<std::size_t>(image.height);
    const std::size_t pixels = width * height;
    std::vector<std::uint8_t> bytes(pixels * 4);
    parallelFor(0, pixels, [&](std::size_t i) {
        for (std::size_t c = 0; c < 4; ++c) {
            bytes[i * 4 + c] = static_cast<std::uint8_t>(std::lround(std::clamp(image.rgba[i * 4 + c], 0.0f, 1.0f) * 255.0f));
        }
    }, 1 << 14);
    std::vector<std::uint8_t> rgba(pixels * 4);
    displayPixels(bytes.data(), image.width, image.height, 4, MapDisplay{channels == 4 ? 4 : 1, pick, false},
                  TexelRect::all(image.width, image.height), rgba.data());
    return rgba;
}

}  // namespace groomlab
