// SPDX-License-Identifier: Apache-2.0
#include "lookdev_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QElapsedTimer>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QColor>
#include <QImage>
#include <QString>
#include <QVarLengthArray>
#include <QVector3D>
#include <rhi/qrhi_platform.h>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#endif

#include "groomlab/color_patterns.h"
#include "groomlab/display.h"
#include "groomlab/export_view.h"
#include "groomlab/hair_color.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"

namespace groomlab::app {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kDomSize = 1024;     // the key light's deep opacity map
constexpr int kSkyDomSize = 512;   // the sky's (ambient occlusion) and the camera's (transparency)
constexpr float kScalpHairCount = 64.0f;  // what the scalp adds to a map: opaque
constexpr int kInteractiveEnvLights = 4;  // environment lights evaluated while interacting (all 8 when refining)
constexpr int kHairTextureWidth = 4096;   // the points' and strands' textures (hair.vert fetches at this width)
// The deep opacity maps: one per light slot, the sky's (ambient occlusion), the camera's (the
// interactive frame's transparency); hair_lookdev.frag indexes them the same way.
constexpr int kDomLights = 3, kDomSky = 3, kDomCamera = 4, kDomCount = 5;
// The surfaces' shadow maps: one per light slot, fit to the surfaces' bounds (the hair's
// maps fit the hair's), the distance from the light in a float texture, filtered when read.
constexpr int kShadowSize = 2048;
// The view blocks: the camera, one per light, the sky, the camera again for the interactive
// frame's thick and thin hair draws (hair_lookdev.frag reads the pass index from pixel.w),
// then the surfaces' shadow maps' views, then the camera once more for the per-point shading
// pass (its point count in dom.y).
constexpr int kPassCamera = 0, kPassLight = 1, kPassSky = 4, kPassThick = 5, kPassThin = 6, kPassShadow = 7, kPassShade = 10, kPassCount = 11;
// The surfaces' tables: the environment BRDF (roughness x n.v), the pre-integrated skin
// profile (n.l x curvature), the prefiltered environment's roughness levels (one mip each,
// surface_lookdev.frag's kEnvLevels), the micro-detail normal.
constexpr int kBrdfLutSize = 64, kSkinLutWidth = 128, kSkinLutHeight = 64, kEnvLevels = 6, kDetailSize = 256;
// The textures' bindings in surface_lookdev.frag.
constexpr int kBindEnvPrefiltered = 18, kBindBrdf = 19, kBindSkinLut = 20, kBindAlbedo = 21, kBindNormal = 22, kBindMask = 23, kBindDetail = 24;
constexpr int kBindShadow0 = 25, kBindSssBlurred = 28, kBindSamplerNearest = 29;  // then linear, mip repeat, mip clamp
constexpr int kBindPointColors = 9;  // the hair's per-point colors (hair.vert, hair_lookdev.frag's per-point pass)
constexpr int kBindVolume0 = 34;     // the hair's voxel volumes (hair_shade.comp, surface_lookdev.frag), four
// The separable subsurface blur (sss_blur.frag): its taps, and how far the kernel reaches
// in diffuse mean free paths (Burley's profile keeps 90 percent of its mass within five of
// the longest; the rest is folded back in by the kernel's normalization).
constexpr int kSssTaps = 31, kSssKernelMax = 32;
constexpr float kSssKernelReach = 5.0f;

QShader loadLookdevShader(const QString& name) {
    QFile file(QStringLiteral(":/groomlab/shaders/") + name + QStringLiteral(".qsb"));
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning("GroomLab: shader %s is missing from the resources", qPrintable(name));
        return {};
    }
    return QShader::fromSerialized(file.readAll());
}

// Halton's low-discrepancy sequence: the sub-pixel jitter of the accumulated frames.
float halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

void put4(float* out, float a, float b, float c, float d) {
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
}

void putMatrix(float* out, const QMatrix4x4& m) { std::memcpy(out, m.constData(), 16 * sizeof(float)); }

float luminance(const RGB& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

}  // namespace

// ---- The hair's buffers -----------------------------------------------------------------------

namespace {

// What every strand's texels read from the set, looked up once per build.
struct HairChannels {
    const StrandSet& s;
    const ScalpMesh* scalp;
    const std::vector<float>& widths;
    const std::vector<float>* melanin;
    const std::vector<float>* redness;
    const std::vector<float>* colors[3];
    const std::vector<float>* pointColors[3];  // HairColor's per-point colors (all three, or none)
    const std::vector<float>* iridescence;     // per point, or null
    bool wetnessPerPoint = true;               // (before `wetness`, which sets it)
    const std::vector<float>* wetness;         // Wet's, per point or per strand (wetnessPerPoint), or null
    std::array<float, 3> brown;
    const std::vector<StrandId>& ids;
    const std::vector<StrandRoot>& roots;
    const VaneNormals vanes;  // a feather's strands' normal: its vane's

    HairChannels(const StrandSet& set, const ScalpMesh* mesh)
        : s(set),
          scalp(mesh),
          widths(set.widths()),
          melanin(set.strandChannel(kMelaninChannel)),
          redness(set.strandChannel(kMelaninRednessChannel)),
          colors{set.strandChannel(kColorChannels[0]), set.strandChannel(kColorChannels[1]), set.strandChannel(kColorChannels[2])},
          pointColors{set.pointChannel(kColorChannels[0]), set.pointChannel(kColorChannels[1]), set.pointChannel(kColorChannels[2])},
          iridescence(set.pointChannel(kIridescenceChannel)),
          wetness(wetnessChannel(set, &wetnessPerPoint)),
          brown(defaultHairColor()),
          ids(set.ids()),
          roots(set.roots()),
          vanes(set) {
        for (const std::vector<float>*& c : pointColors) {
            if (!c || c->size() != set.pointCount()) pointColors[0] = pointColors[1] = pointColors[2] = nullptr;
        }
        if (iridescence && iridescence->size() != set.pointCount()) iridescence = nullptr;
    }

    bool pointColored() const { return pointColors[0] || iridescence || wetness; }

    // Strand i's per-point colors (4 floats a point: r g b and the iridescence, or it and the
    // wetness packed: hair_color.h packWetLook), `out` at its first point: its own per point, else
    // its strand's.
    void writePointColors(std::size_t i, float* out) const {
        const std::size_t n = s.strandPointCount(i), begin = s.strandBegin(i);
        for (std::size_t k = 0; k < n; ++k, out += 4) {
            for (int c = 0; c < 3; ++c) {
                out[c] = pointColors[c] ? (*pointColors[c])[begin + k] : colors[c] ? (*colors[c])[i] : brown[c];
            }
            const float iridescent = iridescence ? (*iridescence)[begin + k] : 0.0f;
            out[3] = wetness ? packWetLook(iridescent, (*wetness)[wetnessPerPoint ? begin + k : i]) : iridescent;
        }
    }

    // Strand i's point entries (8 floats a point, `points` starting at its first point) and its
    // strand entry (12 floats).
    void write(std::size_t i, float* points, float* sd) const {
        const std::size_t n = s.strandPointCount(i);
        const std::size_t begin = s.strandBegin(i);
        const Vec3f* p = s.strandPoints(i);
        for (std::size_t k = 0; k < n; ++k) {
            // The tangent: central differences inside, one-sided at the ends.
            const Vec3f prev = p[k > 0 ? k - 1 : 0], next = p[k + 1 < n ? k + 1 : n - 1];
            Vec3f t = next - prev;
            const float len = t.length();
            t = len > 1e-12f ? t * (1.0f / len) : Vec3f{0.0f, 1.0f, 0.0f};
            float* out = points + k * 8;
            out[0] = p[k].x;
            out[1] = p[k].y;
            out[2] = p[k].z;
            out[3] = static_cast<float>(i);  // exact below 2^24 strands
            out[4] = t.x;
            out[5] = t.y;
            out[6] = t.z;
            out[7] = begin + k < widths.size() ? widths[begin + k] : 0.01f;
        }
        sd[0] = melanin ? (*melanin)[i] : kDefaultMelanin;
        sd[1] = redness ? (*redness)[i] : kDefaultMelaninRedness;
        sd[2] = unitFloat(hash64(static_cast<std::uint64_t>(i < ids.size() ? ids[i] : i) ^ 0x1D0D));
        sd[3] = static_cast<float>(begin);
        for (int c = 0; c < 3; ++c) sd[4 + c] = colors[c] ? (*colors[c])[i] : brown[c];
        sd[7] = static_cast<float>(n);
        // The root's normal: a feather's vane's, the scalp's there, else the way the strand sets out.
        Vec3f normal{0.0f, 1.0f, 0.0f};
        if (vanes.at(i, &normal)) {
        } else if (scalp && i < roots.size() && scalp->contains(roots[i])) {
            normal = scalp->normal(roots[i]);
        } else if (n >= 2) {
            normal = (p[1] - p[0]).normalized();
        }
        sd[8] = normal.x;
        sd[9] = normal.y;
        sd[10] = normal.z;
        sd[11] = 0.0f;
    }
};

void growBounds(Bounds& b, const Vec3f& p) {
    if (!b.valid) {
        b.lo = b.hi = p;
        b.valid = true;
        return;
    }
    b.lo = {std::min(b.lo.x, p.x), std::min(b.lo.y, p.y), std::min(b.lo.z, p.z)};
    b.hi = {std::max(b.hi.x, p.x), std::max(b.hi.y, p.y), std::max(b.hi.z, p.z)};
}

// The strands' bounds as the set measures them (StrandSet::extent: kept with the set, worked out
// again by the evaluation only where strands changed): a whole write and a partial one fit the
// maps to the same box.
Bounds hairBoundsOf(const StrandSet& s) {
    const StrandSet::Extent e = s.extent();
    Bounds b;
    if (!e.empty()) {
        b.lo = e.lo;
        b.hi = e.hi;
        b.valid = true;
    }
    return b;
}

// A hash of the strands' point counts: the indices and the texels' places depend on nothing else.
std::uint64_t layoutKeyOf(const StrandSet& s) {
    const std::vector<std::size_t>& offsets = s.offsets();
    return offsets.empty() ? 0 : bufferDigest(offsets.data(), offsets.size() * sizeof(std::size_t)) ^ s.strandCount();
}

}  // namespace

bool hairPointColored(const StrandSet& s) { return HairChannels(s, nullptr).pointColored(); }

void buildHairRange(const StrandSet& s, const ScalpMesh* scalp, std::size_t first, std::size_t end, std::vector<float>& points,
                    std::vector<float>& strandData, Bounds* bounds, std::vector<float>* pointColors) {
    const HairChannels channels(s, scalp);
    const std::size_t pBegin = s.strandBegin(first), pEnd = s.offsets()[end];
    points.resize((pEnd - pBegin) * 8);
    strandData.resize((end - first) * 12);
    if (pointColors) pointColors->resize((pEnd - pBegin) * 4);
    parallelFor(first, end, [&](std::size_t i) {
        channels.write(i, points.data() + (s.strandBegin(i) - pBegin) * 8, strandData.data() + (i - first) * 12);
        if (pointColors) channels.writePointColors(i, pointColors->data() + (s.strandBegin(i) - pBegin) * 4);
    }, 256);
    if (bounds) {
        for (std::size_t p = pBegin; p < pEnd; ++p) growBounds(*bounds, s.positions()[p]);
    }
}

void buildHairIndices(const StrandSet& s, HairIndices indices, HairBuffers& b) {
    const bool strip = (indices == HairIndices::Both || indices == HairIndices::Strip) && b.strip.empty();
    const bool list = (indices == HairIndices::Both || indices == HairIndices::List) && b.list.empty();
    if (!strip && !list) return;
    // The strip: two vertices a point, a restart after each strand; the list: six indices a
    // segment. Both are laid out so every strand writes its own range in parallel.
    const std::size_t strandCount = s.strandCount();
    std::vector<std::size_t> stripAt(strandCount + 1, 0), listAt(strandCount + 1, 0);
    for (std::size_t i = 0; i < strandCount; ++i) {
        const std::size_t n = s.strandPointCount(i);
        stripAt[i + 1] = stripAt[i] + (n < 2 ? 0 : 2 * n + 1);
        listAt[i + 1] = listAt[i] + (n < 2 ? 0 : 6 * (n - 1));
    }
    if (strip) b.strip.resize(stripAt[strandCount]);
    if (list) b.list.resize(listAt[strandCount]);
    parallelFor(0, strandCount, [&](std::size_t i) {
        const std::size_t n = s.strandPointCount(i);
        const std::size_t begin = s.strandBegin(i);
        if (n < 2) return;
        if (strip) {
            std::uint32_t* out = b.strip.data() + stripAt[i];
            for (std::size_t k = 0; k < n; ++k) {
                out[2 * k] = static_cast<std::uint32_t>((begin + k) * 2);
                out[2 * k + 1] = static_cast<std::uint32_t>((begin + k) * 2 + 1);
            }
            out[2 * n] = 0xFFFFFFFFu;
        }
        if (list) {
            std::uint32_t* out = b.list.data() + listAt[i];
            for (std::size_t k = 0; k + 1 < n; ++k) {
                const auto a = static_cast<std::uint32_t>((begin + k) * 2);
                const std::uint32_t quad[6] = {a, a + 1, a + 2, a + 1, a + 3, a + 2};
                std::memcpy(out + 6 * k, quad, sizeof quad);
            }
        }
    }, 256);
}

HairBuffers buildHairBuffers(const StrandSet& s, const ScalpMesh* scalp, HairIndices indices) {
    HairBuffers b;
    b.pointCount = s.pointCount();
    b.strandCount = s.strandCount();
    // Padded to whole rows of the textures (two texels a point, three a strand, four floats a
    // texel), so a texture is written from the arrays as they are.
    auto padded = [](std::size_t texels) {
        return (texels + kHairTextureWidth - 1) / kHairTextureWidth * kHairTextureWidth * 4;
    };
    b.points.resize(std::max(padded(b.pointCount * 2), std::size_t{kHairTextureWidth * 4}));
    b.strands.resize(std::max(padded(b.strandCount * 3), std::size_t{kHairTextureWidth * 4}));
    const HairChannels channels(s, scalp);
    // Colors per point (one texel each) only where the strands have their own.
    b.pointColored = channels.pointColored();
    if (b.pointColored) b.pointColors.resize(std::max(padded(b.pointCount), std::size_t{kHairTextureWidth * 4}));
    parallelFor(0, b.strandCount, [&](std::size_t i) {
        const std::size_t begin = s.strandBegin(i);
        channels.write(i, b.points.data() + begin * 8, b.strands.data() + i * 12);
        if (b.pointColored) channels.writePointColors(i, b.pointColors.data() + begin * 4);
    }, 256);
    buildHairIndices(s, indices, b);
    b.bounds = hairBoundsOf(s);
    return b;
}

// ---- GPU objects ----------------------------------------------------------------------------------

struct LookdevRenderer::FrameUniforms {
    float domViewProj[5][16];  // the deep opacity maps' views: the three lights, the sky, the camera
    float domAxis[5][4];
    float domParams[4];
    float domParams2[4];
    float eye[4];
    float mat0[4];
    float dye[4];
    float mat1[4];
    float specR[4];
    float specTRT[4];
    float specTT[4];
    float lightDir[3][4];
    float lightColor[3][4];
    float sh[9][4];
    float envDir[8][4];
    float envColor[8][4];
    float iridescence[4];  // rgb: the structural color; w: its hue's turn along a fiber
    float env[4];
    float bg[4];
    float tune[4];  // GROOMLAB_APP_TUNE: the calibration constants overridden (0: the shader's)
    float shadowParams[4];  // the shadow maps' texel (world), filter radius (texels), size, 1 when they hold surfaces
    float shadowViewProj[3][16];
    float shadowAxis[3][4];
    float debug[4];  // x: GROOMLAB_APP_DEBUG_VIEW
    // The visibility path's voxel volumes (hair_shading.glsl): the grid's corner (w: 1 when the
    // volumes hold the hair), one over its extent per axis (w: a voxel's size), x: unused, yzw:
    // toward the sky (the ambient occlusion's volume).
    float voxelLo[4];
    float voxelScale[4];
    float voxelParams[4];
    float tune2[4];  // GROOMLAB_APP_TUNE's numbers 5 to 8
    float tune3[4];  // ... 9 to 12
    float tune4[4];  // ... 13 to 16 (hair_shading.glsl's own: the environment's)
    float wet[4];    // wet hair, soaked (hair_color.h wetShadingUniform)
    float rgbLook[4];  // x: the azimuthal roughness the groom's RGB is absorbed by (hairColorSigma)
};

namespace {

// The visibility path's blocks: hair_vis.glsl's Vis, hair_voxel.glsl's Grid, hair_clear.comp's Fill.
struct VisUniforms {
    float viewProj[16];
    float eyeFrame[4];
    float target[4];
    float params[4];
    float params2[4];
    std::uint32_t counts[4];
    float voxel[4];
};
struct GridUniforms {
    float lo[4];
    float params[4];
    std::uint32_t dims[4];
    std::uint32_t counts[4];
    float toward[4];
};
struct FillUniforms {
    std::uint32_t fill[4];
    std::uint32_t values[4];
};

// The voxel grid's resolution along the hair's longest side, and the volumes: the three lights'
// and the sky's (the ambient occlusion's).
constexpr int kVoxelResolution = 128;
constexpr int kVolumeCount = 4, kVolumeSky = 3;
// The hairs a ray crosses per unit of the grid's density (fiber area over a voxel's face) toward a
// light: pi / 4, the mean projection of fibers every way (hair_bsdf.glsl's hairShadowCount takes the
// hairs crossed, as Maya's density grid counts them); toward the sky the environment's lights and
// the ambient occlusion take a little less, fitted with hair_bsdf.glsl's constants to Arnold renders
// of the same grooms in scene-linear Rec.709-sRGB (arnold_compare.py --linear-srgb: the head groom,
// five pigments, the key light alone and the environment alone).
constexpr float kVoxelCountScale = 0.785398f;
constexpr float kVoxelSkyCountScale = 0.405f;
// How far toward a light (voxels) a point reads its volume: past its own voxel and the tent around
// it, so a strand on the hair's lit side is not shadowed by the hair behind it.
// (Two voxels: the 500k head's lit side read 0.58 of Arnold's luminance at one, 1.17 at two.)
constexpr float kVoxelLookupOffset = 2.0f;
// The density's fixed point (hair_voxelize.comp sums it with integer atomics).
constexpr float kDensityFixedPoint = 65536.0f;
// Compute dispatches wider than this many groups a row go on in more rows (Direct3D 11 takes at
// most 65535 groups along an axis).
constexpr std::uint32_t kGroupsPerRow = 4096;

}  // namespace

struct LookdevRenderer::PassUniforms {
    float viewProj[16];
    float invViewProj[16];
    float view[4];
    float pixel[4];
    float axis[4];
    float dom[4];
};

struct BlitUniforms {
    float blit[4];
};

// surface_lookdev.frag's Material block.
struct LookdevRenderer::MaterialUniforms {
    float base[4];
    float params[4];
    float flags[4];
    float skin[4];
    float skin2[4];
    float eyeCenter[4];
    float eyeForward[4];
    float eyeUp[4];
    float eyeParams[4];
    float irisColor[4];
    float scleraColor[4];
    float eyeParams2[4];
    float skin3[4];  // sss mode, the mean free path rgb
    float cutout[4];  // the albedo alpha's cutoff (0: opaque), the albedo map's width in texels
};

// sss_blur.frag's Sss block: the axis, the pixel's width at unit depth, the frame (the
// jitter's seed, -1 for none), the tap count, the target's size, the kernel.
struct LookdevRenderer::SssUniforms {
    float params[4];
    float params2[4];
    float kernel[kSssKernelMax][4];
};

// One surface's GPU copy: its interleaved vertices and indices (made again when its version
// moves), its material block (written every frame), the bindings that name its textures.
struct LookdevRenderer::GpuSurface {
    SurfacePtr source;
    std::uint64_t version = 0;
    std::string textureKey;  // the texture files bound: the bindings are made again when they change
    std::unique_ptr<QRhiBuffer> vertices, indices, material;
    std::uint32_t indexCount = 0;
    std::unique_ptr<QRhiShaderResourceBindings> lookdevSrb, depthSrb[kDomCount], domSrb[kDomCount], shadowSrb[kDomLights];
    QRhiTexture* albedo = nullptr;
    QRhiTexture* normal = nullptr;
    QRhiTexture* mask = nullptr;
    int textureBits = 0;
};

struct LookdevRenderer::Gpu {
    QRhi* rhi = nullptr;
    // The hair.
    // The points and strands as float textures the vertex shader fetches (kHairTextureWidth
    // texels wide, two an entry), and the strip's or list's indices.
    std::unique_ptr<QRhiTexture> points, strands;
    // The points' own colors (r g b and the iridescence, a texel each) when the strands carry
    // them; else a single texel, which tells hair.vert to take each strand's color.
    std::unique_ptr<QRhiTexture> pointColors;
    std::unique_ptr<QRhiBuffer> strip, list;
    std::uint32_t indexCount = 0;
    bool useStrip = false;
    bool indexKindKnown = false;  // useStrip set by an upload (the device's primitive restart)
    // The points shaded once each for the interactive frame (hair_lookdev.frag under
    // HAIR_POINT_SHADE writes it, hair_gouraud.vert reads it): one RGBA16F texel a point.
    std::unique_ptr<QRhiTexture> shaded;
    std::unique_ptr<QRhiTextureRenderTarget> shadedTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> shadedPass;
    // Uniforms: the frame's, one Pass per view (camera, key map, sky map), the blits'.
    // The view blocks (kPass*): the camera, the three lights, the sky, and the camera again
    // twice for the interactive frame's two hair draws (the strands a pixel wide or more, the
    // thinner ones).
    std::unique_ptr<QRhiBuffer> frame, pass[kPassCount], blitAccumulate, blitTonemap;
    // The deep opacity maps (kDom*: the three lights', the sky's, the camera's): the first
    // hair's distance (a float target, depth-tested through a render buffer) and the layers.
    struct Dom {
        std::unique_ptr<QRhiTexture> front, layers;
        std::unique_ptr<QRhiRenderBuffer> depth;
        std::unique_ptr<QRhiTextureRenderTarget> depthTarget, layersTarget;
        std::unique_ptr<QRhiRenderPassDescriptor> depthPass, layersPass;
        int size = 0;
    } dom[kDomCount];
    // The surfaces' shadow maps: the distance from each light (a float target over a shared
    // depth buffer).
    struct Shadow {
        std::unique_ptr<QRhiTexture> distance;
        std::unique_ptr<QRhiTextureRenderTarget> target;
        std::unique_ptr<QRhiRenderPassDescriptor> pass;
    } shadow[kDomLights];
    std::unique_ptr<QRhiRenderBuffer> shadowDepth;
    // The skin's screen-space scattering: the prepass targets (the irradiance with the blur's
    // width in alpha, the view depth), the blur's two targets (one axis each).
    struct Sss {
        std::unique_ptr<QRhiTexture> irradiance, depth, temp, blurred;
        std::unique_ptr<QRhiRenderBuffer> depthBuffer;
        std::unique_ptr<QRhiTextureRenderTarget> prepassTarget, tempTarget, blurredTarget;
        std::unique_ptr<QRhiRenderPassDescriptor> prepassPass, blurPass;
        std::unique_ptr<QRhiBuffer> uniforms[2], blit;  // horizontal, vertical
        std::unique_ptr<QRhiShaderResourceBindings> srb[2];
    } sss;
    // The HDR scene: multisampled, resolved, accumulated (two, ping-pong).
    std::unique_ptr<QRhiTexture> hdr, resolved, accum[2];
    std::unique_ptr<QRhiRenderBuffer> hdrDepth;
    std::unique_ptr<QRhiTextureRenderTarget> hdrTarget, accumTarget[2];
    std::unique_ptr<QRhiRenderPassDescriptor> hdrPass, accumPass;
    QSize size;
    int hdrSamples = 1;
    // The environment: the map, and the map prefiltered per roughness in its mip levels.
    std::unique_ptr<QRhiTexture> environment, envPrefiltered;
    std::unique_ptr<QRhiSampler> linear, nearest, mipRepeat, mipClamp;
    // The surfaces: their tables and stand-in textures (white, a flat normal), the loaded
    // texture files by path (the mask textures by their files' names together), the surfaces.
    std::unique_ptr<QRhiTexture> brdfLut, skinLut, detail, white, flatNormal, noVolume;
    std::map<std::string, std::unique_ptr<QRhiTexture>> textures;
    std::vector<std::unique_ptr<GpuSurface>> surfaces;
    std::unique_ptr<QRhiShaderResourceBindings> surfaceTemplateSrb, surfaceDepthTemplateSrb, surfaceDomTemplateSrb;
    // Pipelines and their bindings.
    std::unique_ptr<QRhiShaderResourceBindings> hairLookdevSrb, hairThickSrb, hairThinSrb, hairDepthSrb[kDomCount], hairDomSrb[kDomCount];
    std::unique_ptr<QRhiShaderResourceBindings> pointShadeSrb, hairGouraudThickSrb, hairGouraudThinSrb;
    std::unique_ptr<QRhiShaderResourceBindings> backgroundSrb, accumulateSrb[2], tonemapSrb[3];  // tonemap: resolved, accum 0, accum 1
    std::unique_ptr<QRhiGraphicsPipeline> hairLookdev, hairThin, hairDepth, hairDom, surfaceLookdev, surfaceDepth, surfaceDom;
    std::unique_ptr<QRhiGraphicsPipeline> pointShade, hairGouraud, hairGouraudThin;
    std::unique_ptr<QRhiGraphicsPipeline> surfaceSss, sssBlur, background, accumulate, tonemap;
    QVector<quint32> targetFormat;
    int targetSamples = 0;
    bool flipY = false;
    int accumCurrent = 0;
    int tonemapSource = 0;  // what composite reads: 0 resolved, 1 + i accum[i]

    // The visibility path (HairPath::Visibility).
    struct Vis {
        // The per-pixel buffer (hair_vis.glsl: four words a pixel at the HDR targets' size), the
        // strands' level of detail exponents (one float a strand; one when not used), the voxel
        // grid's fixed point density.
        std::unique_ptr<QRhiBuffer> pixels, lod, density;
        // The Vis blocks of the raster's two passes (the shading reads the second's), the Grid
        // blocks (the voxelization's, then one per volume), the fills' blocks.
        std::unique_ptr<QRhiBuffer> uniforms[2], grid[1 + kVolumeCount], fillPixels, fillDensity;
        // The shaded hair (premultiplied, the HDR targets' size), the surfaces' view depth (the
        // raster tests against it), the grid's density and the volumes (3D, R32F).
        std::unique_ptr<QRhiTexture> hairImage, surfaceDepth, densityTexture, volumes[kVolumeCount];
        std::unique_ptr<QRhiRenderBuffer> surfaceDepthBuffer;
        std::unique_ptr<QRhiTextureRenderTarget> surfaceDepthTarget, compositeTarget;
        std::unique_ptr<QRhiRenderPassDescriptor> surfaceDepthPass, compositePass;
        // Direct3D 11: a 1 x 1 target an empty pass binds after the surfaces' depth pass, so the
        // depth texture is no longer bound as an output when the raster samples it (the device
        // would bind nothing in its place, and every strand behind the surfaces would show).
        std::unique_ptr<QRhiTexture> releaseTexture;
        std::unique_ptr<QRhiTextureRenderTarget> releaseTarget;
        std::unique_ptr<QRhiRenderPassDescriptor> releasePass;
        std::unique_ptr<QRhiSampler> clamp3d;
        std::unique_ptr<QRhiShaderResourceBindings> fillPixelsSrb, fillDensitySrb, rasterSrb[2], shadeSrb, voxelizeSrb, densitySrb,
            volumeSrb[kVolumeCount], compositeSrb;
        std::unique_ptr<QRhiComputePipeline> fill, raster, shade, voxelize, densityPs, volume;
        std::unique_ptr<QRhiGraphicsPipeline> composite;
        QSize size;                           // the per-pixel buffer's and the images'
        std::array<int, 3> dims{0, 0, 0};     // the grid's textures'
        std::size_t lodStrands = 0;           // the exponents the LOD buffer holds
        // The objects the bindings name, by QRhi's resource ids rather than their addresses (a
        // buffer freed and the next one made can come back at the same address, and bindings
        // naming the freed objects then draw from freed memory): made again when one of them was.
        static constexpr std::size_t kBound = 18;
        std::array<quint64, kBound> bound{};
        quint64 compositeOn = 0;  // the resolved texture the composite target draws into (its id)
        bool ready = false;
    } vis;
};

LookdevRenderer::LookdevRenderer() : gpu_(std::make_unique<Gpu>()) {
    static_assert(sizeof(FrameUniforms) == 1504, "the Frame block is 1504 bytes");
    static_assert(sizeof(PassUniforms) == 192, "the Pass block is 192 bytes");
    static_assert(sizeof(MaterialUniforms) == 224, "the Material block is 224 bytes");
    static_assert(sizeof(SssUniforms) == 544, "the Sss block is 544 bytes");
    static_assert(sizeof(VisUniforms) == 160, "the Vis block is 160 bytes");
    static_assert(sizeof(GridUniforms) == 80, "the Grid block is 80 bytes");
    material_ = hairPreset("groom");
    lighting_ = lightingPreset("studio");
    pointShading_ = qgetenv("GROOMLAB_APP_HAIR_SHADING") != "fragment";
    hairPath_ = qgetenv("GROOMLAB_APP_HAIR_PATH").toLower() == "legacy" ? HairPath::Legacy : HairPath::Visibility;
    lod_ = std::make_unique<StrandLod>();
}

LookdevRenderer::~LookdevRenderer() { joinBuild(); }

void LookdevRenderer::joinBuild() {
    if (buildThread_.joinable()) buildThread_.join();
}

bool LookdevRenderer::hairBuilding() const { return build_ && build_->version == hairVersion_ && uploadedVersion_ != hairVersion_; }

void LookdevRenderer::setSurfaces(std::vector<SurfacePtr> surfaces) {
    surfaces_ = std::move(surfaces);
    surfacesDirty_ = true;
    sceneChanged();
}

int LookdevRenderer::textureCount() const {
    return static_cast<int>(gpu_->textures.size()) + (gpu_->brdfLut ? 1 : 0) + (gpu_->skinLut ? 1 : 0) + (gpu_->detail ? 1 : 0);
}

bool LookdevRenderer::supported(QRhi* rhi) {
    if (!rhi) return false;
    return rhi->isFeatureSupported(QRhi::TexelFetch) && rhi->isFeatureSupported(QRhi::ThreeDimensionalTextures) &&
           rhi->isTextureFormatSupported(QRhiTexture::RGBA16F) && rhi->isTextureFormatSupported(QRhiTexture::R32F) &&
           rhi->isTextureFormatSupported(QRhiTexture::RGBA32F);
}

void LookdevRenderer::setHair(StrandsPtr strands, MeshPtr scalp, std::uint64_t version, const HairChanges& changes) {
    if (strands == strands_ && scalp == scalp_ && version == hairVersion_) return;
    strands_ = std::move(strands);
    scalp_ = std::move(scalp);
    hairVersion_ = version;
    changes_ = changes;
    pending_.reset();
    sceneChanged();
    // A whole write ahead: its buffers built on a thread of their own while the frames go on
    // drawing the hair held (one build at a time: a change while one runs builds in the frame).
    if (buildAhead_ && strands_ && gpu_->points && !partialUpload() && !(build_ && !build_->done.load(std::memory_order_acquire))) {
        joinBuild();
        auto job = std::make_shared<HairBuild>();
        job->version = hairVersion_;
        try {
            buildThread_ = std::thread([job, strands = strands_, scalp = scalp_, indices = indicesNeeded()] {
                job->buffers = std::make_shared<HairBuffers>(buildHairBuffers(*strands, scalp.get(), indices));
                job->done.store(true, std::memory_order_release);
            });
            build_ = std::move(job);
        } catch (const std::exception&) {
            build_.reset();  // no thread: built in the frame
        }
    }
}

LookdevRenderer::GpuTargets LookdevRenderer::gpuTargets() const {
    GpuTargets t;
    t.points = gpu_->points.get();
    t.width = kHairTextureWidth;
    t.pointCount = pointCount_;
    t.fresh = uploadedThisFrame_;
    return t;
}

void LookdevRenderer::hairWritten(const Bounds& bounds) {
    // The texels hold the GPU's strands now, not the CPU's of uploadedSerial_: the next change
    // writes them whole. The maps are fit to what the run can have reached.
    uploadedSerial_ = 0;
    if (bounds.valid) {
        if (!hairBounds_.valid) hairBounds_ = bounds;
        growBounds(hairBounds_, bounds.lo);
        growBounds(hairBounds_, bounds.hi);
        updateBounds();
    }
    sceneChanged();
}

void LookdevRenderer::setMaterial(const HairMaterial& material) {
    material_ = material;
    sceneChanged();
}

void LookdevRenderer::setLighting(const LightingSetup& lighting) {
    if (lighting.environment != lighting_.environment || environmentKind_ != lighting.environment) environmentDirty_ = true;
    lighting_ = lighting;
    sceneChanged();
}

void LookdevRenderer::invalidate() {
    accumulated_ = 0;
    changedSinceFrame_ = true;
}

void LookdevRenderer::sceneChanged() {
    domsStale_ = true;
    volumesStale_ = true;
    invalidate();
}

void LookdevRenderer::settle() {
    changedSinceFrame_ = false;
    changedBeforeLast_ = false;
    streamScale_ = 0.0f;
}

namespace {

// Frames further apart than this did not come back to back: the gap holds no frame's cost,
// and two changes that far apart are not a stream.
constexpr double kBackToBackMs = 200.0;
// A reduced frame that took longer than this (past one refresh at 60 Hz, with the display's
// own rounding) lowers the stream's scale.
constexpr double kOverBudgetMs = 24.0;
// The stand-in before any measurement: this card's full frame at 1.6 M points over 1920 x 1080.
constexpr double kModelMs = 46.0, kModelPoints = 1.6e6, kModelPixels = 1920.0 * 1080.0;

}  // namespace

float LookdevRenderer::chooseScale(const QSize& size) const {
    const double pixels = static_cast<double>(size.width()) * size.height();
    double full = fullFrameMs_;
    if (full <= 0.0) full = kModelMs * (static_cast<double>(pointCount_) / kModelPoints) * (pixels / kModelPixels);
    if (full <= kTargetFrameMs) return 1.0f;
    // The cost goes with the pixels: the scale that brings it to the target, in steps of a
    // twentieth so a stream's frames do not flicker between sizes.
    const float scale = static_cast<float>(std::sqrt(kTargetFrameMs / full));
    return std::clamp(std::floor(scale * 20.0f) / 20.0f, kMinScale, 1.0f);
}

void LookdevRenderer::noteFrameCost(double ms) {
    // The last frame's cost, when it says something: not the first after an upload or a maps
    // redraw (heavier than the frames that follow).
    if (lastFrameHeavy_) return;
    if (!reduced_) {
        fullFrameMs_ = fullFrameMs_ > 0.0 ? 0.7 * fullFrameMs_ + 0.3 * ms : ms;
    } else if (ms > kOverBudgetMs && streamScale_ > kMinScale) {
        streamScale_ = std::max(kMinScale, std::floor(streamScale_ * 0.8f * 20.0f) / 20.0f);
    }
}

std::vector<DirectionalLight> LookdevRenderer::activeLights() const {
    // The enabled lights, in order, as many as the shader has slots (and maps) for.
    std::vector<DirectionalLight> lights;
    for (const DirectionalLight& d : lighting_.lights) {
        if (d.enabled && static_cast<int>(lights.size()) < kDomLights) lights.push_back(d);
    }
    return lights;
}

bool LookdevRenderer::wantsAnotherFrame() const {
    // The lighting's own frame count (the Render tab's sample count), within the limit; a whole
    // write still building ahead (the frame after it lands takes it).
    if (waitingForBuild_) return true;
    return !reduced_ && lighting_.quality == Quality::Refine && accumulated_ < std::clamp(lighting_.refineFrames, 1, kMaxAccumulatedFrames);
}

void LookdevRenderer::releaseResources() {
    gpu_ = std::make_unique<Gpu>();
    uploadedVersion_ = ~std::uint64_t{0};
    uploadedSerial_ = 0;
    environmentDirty_ = true;
    surfacesDirty_ = true;
    domsStale_ = true;
    volumesStale_ = true;
    lodKey_ = 0;
    accumulated_ = 0;
    lastRenderNs_ = -1;
    reduced_ = false;
}

bool LookdevRenderer::ensureResources(QRhi* rhi, QRhiRenderTarget* target) {
    if (gpu_->rhi && gpu_->rhi != rhi) releaseResources();  // another device: nothing of the old one is usable
    // A target or buffer that cannot be made ends the path (the host draws its line path).
    auto fail = [](int line) {
        qWarning("GroomLab: the lookdev targets could not be made (lookdev_renderer.cpp:%d)", line);
        return false;
    };
    Gpu& gpu = *gpu_;
    gpu.rhi = rhi;
    gpu.flipY = rhi->isYUpInNDC() && !rhi->isYUpInFramebuffer();
    auto makeTexture = [&](QRhiTexture::Format format, const QSize& size, int samples, QRhiTexture::Flags flags) {
        std::unique_ptr<QRhiTexture> t(rhi->newTexture(format, size, samples, flags));
        if (!t->create()) t.reset();
        return t;
    };
    if (!gpu.linear) {
        // (Clamped along w too: the surfaces read the hair's voxel volumes through it.)
        gpu.linear.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                         QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        gpu.linear->create();
        // The volumes' stand-in until the visibility path makes them (one texel: no hair).
        gpu.noVolume.reset(rhi->newTexture(QRhiTexture::R32F, 1, 1, 1, 1, QRhiTexture::ThreeDimensional));
        if (!gpu.noVolume->create()) return fail(__LINE__);
        gpu.nearest.reset(rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                          QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        gpu.nearest->create();
        // The surfaces' maps (tiling) and the prefiltered environment (its levels the
        // roughness; the longitude wraps).
        gpu.mipRepeat.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::Linear,
                                            QRhiSampler::Repeat, QRhiSampler::Repeat));
        gpu.mipRepeat->create();
        gpu.mipClamp.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::Linear,
                                           QRhiSampler::Repeat, QRhiSampler::ClampToEdge));
        gpu.mipClamp->create();
    }
    if (!gpu.envPrefiltered) {
        gpu.envPrefiltered = makeTexture(QRhiTexture::RGBA32F, QSize(128, 64), 1, QRhiTexture::MipMapped);
        if (!gpu.envPrefiltered) return fail(431);
        environmentDirty_ = true;
    }
    if (!gpu.frame) {
        gpu.frame.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(FrameUniforms)));
        gpu.frame->create();
        for (auto& p : gpu.pass) {
            p.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(PassUniforms)));
            p->create();
        }
        gpu.blitAccumulate.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(BlitUniforms)));
        gpu.blitAccumulate->create();
        gpu.blitTonemap.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(BlitUniforms)));
        gpu.blitTonemap->create();
    }
    if (!gpu.environment) {
        gpu.environment = makeTexture(QRhiTexture::RGBA32F, QSize(128, 64), 1, {});
        if (!gpu.environment) return fail(448);
        environmentDirty_ = true;
    }
    // The deep opacity maps.
    for (int i = 0; i < kDomCount; ++i) {
        Gpu::Dom& d = gpu.dom[i];
        const int size = i == kDomSky || i == kDomCamera ? kSkyDomSize : kDomSize;  // the camera's weights thin strands: coarse will do
        if (d.front && d.size == size) continue;
        d = Gpu::Dom();
        d.size = size;
        domsStale_ = true;
        d.front = makeTexture(QRhiTexture::R32F, QSize(size, size), 1, QRhiTexture::RenderTarget);
        d.layers = makeTexture(QRhiTexture::RGBA16F, QSize(size, size), 1, QRhiTexture::RenderTarget);
        d.depth.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, QSize(size, size), 1));
        if (!d.front || !d.layers || !d.depth->create()) return fail(462);
        QRhiTextureRenderTargetDescription depthDesc(QRhiColorAttachment(d.front.get()));
        depthDesc.setDepthStencilBuffer(d.depth.get());
        d.depthTarget.reset(rhi->newTextureRenderTarget(depthDesc));
        d.depthPass.reset(d.depthTarget->newCompatibleRenderPassDescriptor());
        d.depthTarget->setRenderPassDescriptor(d.depthPass.get());
        if (!d.depthTarget->create()) return fail(468);
        QRhiTextureRenderTargetDescription layersDesc(QRhiColorAttachment(d.layers.get()));
        d.layersTarget.reset(rhi->newTextureRenderTarget(layersDesc));
        d.layersPass.reset(d.layersTarget->newCompatibleRenderPassDescriptor());
        d.layersTarget->setRenderPassDescriptor(d.layersPass.get());
        if (!d.layersTarget->create()) return fail(473);
    }
    // The surfaces' shadow maps, one depth buffer between them (drawn one after another).
    if (!gpu.shadowDepth) {
        gpu.shadowDepth.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, QSize(kShadowSize, kShadowSize), 1));
        if (!gpu.shadowDepth->create()) return fail(478);
        for (Gpu::Shadow& s : gpu.shadow) {
            s.distance = makeTexture(QRhiTexture::R32F, QSize(kShadowSize, kShadowSize), 1, QRhiTexture::RenderTarget);
            if (!s.distance) return fail(481);
            QRhiTextureRenderTargetDescription desc(QRhiColorAttachment(s.distance.get()));
            desc.setDepthStencilBuffer(gpu.shadowDepth.get());
            s.target.reset(rhi->newTextureRenderTarget(desc));
            s.pass.reset(s.target->newCompatibleRenderPassDescriptor());
            s.target->setRenderPassDescriptor(s.pass.get());
            if (!s.target->create()) return fail(487);
        }
        domsStale_ = true;
    }
    if (!gpu.sss.uniforms[0]) {
        for (auto& u : gpu.sss.uniforms) {
            u.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(SssUniforms)));
            u->create();
        }
        gpu.sss.blit.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(BlitUniforms)));
        gpu.sss.blit->create();
    }
    // The HDR targets, at the host's size times the lighting's resolution scale (the display
    // transform samples them bilinearly into the host's target, so a smaller picture is a
    // faster interactive one and a bigger one supersamples).
    const float scale = std::clamp(lighting_.resolutionScale, 0.25f, 2.0f);
    const QSize size(std::max(1, static_cast<int>(std::lround(target->pixelSize().width() * scale))),
                     std::max(1, static_cast<int>(std::lround(target->pixelSize().height() * scale))));
    if (size.isEmpty()) return fail(501);
    if (gpu.size != size || !gpu.hdr) {
        // The skin's screen-space targets at the same size (no multisampling: the HDR
        // pass reads them per pixel).
        Gpu::Sss& sss = gpu.sss;
        sss.irradiance = makeTexture(QRhiTexture::RGBA16F, size, 1, QRhiTexture::RenderTarget);
        sss.depth = makeTexture(QRhiTexture::R32F, size, 1, QRhiTexture::RenderTarget);
        sss.temp = makeTexture(QRhiTexture::RGBA16F, size, 1, QRhiTexture::RenderTarget);
        sss.blurred = makeTexture(QRhiTexture::RGBA16F, size, 1, QRhiTexture::RenderTarget);
        if (!sss.irradiance || !sss.depth || !sss.temp || !sss.blurred) return fail(510);
        sss.depthBuffer.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, 1));
        if (!sss.depthBuffer->create()) return fail(512);
        {
            const QRhiColorAttachment attachments[2] = {QRhiColorAttachment(sss.irradiance.get()), QRhiColorAttachment(sss.depth.get())};
            QRhiTextureRenderTargetDescription desc;
            desc.setColorAttachments(attachments, attachments + 2);
            desc.setDepthStencilBuffer(sss.depthBuffer.get());
            sss.prepassTarget.reset(rhi->newTextureRenderTarget(desc));
            if (!sss.prepassPass) sss.prepassPass.reset(sss.prepassTarget->newCompatibleRenderPassDescriptor());
            sss.prepassTarget->setRenderPassDescriptor(sss.prepassPass.get());
            if (!sss.prepassTarget->create()) return fail(521);
        }
        for (int i = 0; i < 2; ++i) {
            std::unique_ptr<QRhiTextureRenderTarget>& t = i == 0 ? sss.tempTarget : sss.blurredTarget;
            QRhiTextureRenderTargetDescription desc{QRhiColorAttachment(i == 0 ? sss.temp.get() : sss.blurred.get())};
            t.reset(rhi->newTextureRenderTarget(desc));
            if (!sss.blurPass) sss.blurPass.reset(t->newCompatibleRenderPassDescriptor());
            t->setRenderPassDescriptor(sss.blurPass.get());
            if (!t->create()) return fail(529);
        }
        // The bindings that sample these are made again.
        for (auto& srb : sss.srb) srb.reset();
        gpu.surfaceTemplateSrb.reset();
        for (auto& surface : gpu.surfaces) surface->lookdevSrb.reset();

        gpu.size = size;
        const int samples = rhi->isFeatureSupported(QRhi::MultisampleTexture) && rhi->supportedSampleCounts().contains(4) ? 4 : 1;
        gpu.hdrSamples = samples;
        gpu.hdr = makeTexture(QRhiTexture::RGBA16F, size, samples, QRhiTexture::RenderTarget);
        gpu.resolved = makeTexture(QRhiTexture::RGBA16F, size, 1, QRhiTexture::RenderTarget);
        for (auto& a : gpu.accum) a = makeTexture(QRhiTexture::RGBA16F, size, 1, QRhiTexture::RenderTarget);
        if (!gpu.hdr || !gpu.resolved || !gpu.accum[0] || !gpu.accum[1]) return fail(542);
        gpu.hdrDepth.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, samples));
        if (!gpu.hdrDepth->create()) return fail(544);
        QRhiColorAttachment color(gpu.hdr.get());
        if (samples > 1) color.setResolveTexture(gpu.resolved.get());
        QRhiTextureRenderTargetDescription hdrDesc(color);
        hdrDesc.setDepthStencilBuffer(gpu.hdrDepth.get());
        gpu.hdrTarget.reset(rhi->newTextureRenderTarget(hdrDesc));
        if (!gpu.hdrPass) gpu.hdrPass.reset(gpu.hdrTarget->newCompatibleRenderPassDescriptor());
        gpu.hdrTarget->setRenderPassDescriptor(gpu.hdrPass.get());
        if (!gpu.hdrTarget->create()) return fail(552);
        for (int i = 0; i < 2; ++i) {
            QRhiTextureRenderTargetDescription desc{QRhiColorAttachment(gpu.accum[i].get())};
            gpu.accumTarget[i].reset(rhi->newTextureRenderTarget(desc));
            if (!gpu.accumPass) gpu.accumPass.reset(gpu.accumTarget[i]->newCompatibleRenderPassDescriptor());
            gpu.accumTarget[i]->setRenderPassDescriptor(gpu.accumPass.get());
            if (!gpu.accumTarget[i]->create()) return fail(558);
        }
        // Without the multisampled texture the pass draws straight into `resolved`.
        if (samples == 1) {
            gpu.hdr.reset();
            QRhiColorAttachment direct(gpu.resolved.get());
            QRhiTextureRenderTargetDescription directDesc(direct);
            directDesc.setDepthStencilBuffer(gpu.hdrDepth.get());
            gpu.hdrTarget.reset(rhi->newTextureRenderTarget(directDesc));
            gpu.hdrTarget->setRenderPassDescriptor(gpu.hdrPass.get());
            if (!gpu.hdrTarget->create()) return fail(568);
        }
        // The bindings that sample these textures are made again.
        gpu.accumulateSrb[0].reset();
        gpu.accumulateSrb[1].reset();
        for (auto& s : gpu.tonemapSrb) s.reset();
        accumulated_ = 0;
    }
    return true;
}

bool LookdevRenderer::ensurePipelines(QRhi* rhi, QRhiRenderTarget* target) {
    Gpu& g = *gpu_;
    const QVector<quint32> format = target->renderPassDescriptor()->serializedFormat();
    const bool targetChanged = format != g.targetFormat || g.targetSamples != target->sampleCount();
    const bool hairChanged = !g.hairLookdevSrb && g.points;
    const bool hairReady = !g.points || (g.hairLookdev && !hairChanged);
    bool surfacesReady = true;
    for (const auto& s : g.surfaces) {
        if (!s->lookdevSrb) surfacesReady = false;
    }
    if (!targetChanged && hairReady && surfacesReady && g.surfaceLookdev && g.tonemap && g.accumulateSrb[0] && g.tonemapSrb[0] && g.sss.srb[0]) return true;
    using B = QRhiShaderResourceBinding;
    const B::StageFlags vs = B::VertexStage, fs = B::FragmentStage, both = B::VertexStage | B::FragmentStage;
    auto srb = [&](std::initializer_list<B> bindings) {
        std::unique_ptr<QRhiShaderResourceBindings> s(rhi->newShaderResourceBindings());
        s->setBindings(bindings);
        s->create();
        return s;
    };
    // The maps' samplers, at the bindings hair_lookdev.frag and scalp_lookdev.frag declare
    // (the camera's are the hair's alone).
    auto domTextures = [&](std::vector<B>& out, bool camera) {
        const int bindings[kDomCount][2] = {{5, 6}, {7, 8}, {14, 15}, {16, 17}, {12, 13}};
        for (int i = 0; i < kDomCount; ++i) {
            if (i == kDomCamera && !camera) continue;
            out.push_back(B::sampledTexture(bindings[i][0], fs, g.dom[i].front.get(), g.nearest.get()));
            out.push_back(B::sampledTexture(bindings[i][1], fs, g.dom[i].layers.get(), g.linear.get()));
        }
    };
    // Each map's view block: its light's, the sky's, or the camera's.
    auto domView = [&](int i) { return g.pass[i < kDomLights ? kPassLight + i : i == kDomSky ? kPassSky : kPassCamera].get(); };
    auto srbv = [&](std::vector<B> bindings) {
        std::unique_ptr<QRhiShaderResourceBindings> s(rhi->newShaderResourceBindings());
        s->setBindings(bindings.begin(), bindings.end());
        s->create();
        return s;
    };
    // The surfaces' shadow maps, which the hair shader samples too.
    auto shadowTextures = [&](std::vector<B>& out) {
        for (int i = 0; i < kDomLights; ++i) out.push_back(B::sampledTexture(kBindShadow0 + i, fs, g.shadow[i].distance.get(), g.nearest.get()));
    };
    // The hair's bindings need its buffers: made (again) once they exist.
    if (g.points) {
        const B pointsB = B::sampledTexture(2, vs, g.points.get(), g.nearest.get());
        const B strandsB = B::sampledTexture(3, vs, g.strands.get(), g.nearest.get());
        const B colorsB = B::sampledTexture(kBindPointColors, vs, g.pointColors.get(), g.nearest.get());
        std::vector<B> lookdev{B::uniformBuffer(0, fs, g.frame.get()), B::uniformBuffer(1, both, g.pass[0].get()), pointsB, strandsB, colorsB};
        domTextures(lookdev, true);
        shadowTextures(lookdev);
        g.hairLookdevSrb = srbv(lookdev);
        lookdev[1] = B::uniformBuffer(1, both, g.pass[kPassThick].get());
        g.hairThickSrb = srbv(lookdev);
        lookdev[1] = B::uniformBuffer(1, both, g.pass[kPassThin].get());
        g.hairThinSrb = srbv(lookdev);
        // The per-point shading pass reads the same, in the fragment stage, and writes the
        // shaded points; the Gouraud ribbons fetch them in the vertex stage.
        if (g.shaded) {
            std::vector<B> shade{B::uniformBuffer(0, fs, g.frame.get()), B::uniformBuffer(1, both, g.pass[kPassShade].get()),
                                 B::sampledTexture(2, fs, g.points.get(), g.nearest.get()), B::sampledTexture(3, fs, g.strands.get(), g.nearest.get()),
                                 B::sampledTexture(kBindPointColors, fs, g.pointColors.get(), g.nearest.get())};
            domTextures(shade, true);
            shadowTextures(shade);
            g.pointShadeSrb = srbv(shade);
            std::vector<B> gouraud{B::uniformBuffer(0, fs, g.frame.get()), B::uniformBuffer(1, both, g.pass[kPassThick].get()), pointsB, strandsB, colorsB,
                                   B::sampledTexture(4, vs, g.shaded.get(), g.nearest.get()),
                                   B::sampledTexture(12, fs, g.dom[kDomCamera].front.get(), g.nearest.get()),
                                   B::sampledTexture(13, fs, g.dom[kDomCamera].layers.get(), g.linear.get())};
            g.hairGouraudThickSrb = srbv(gouraud);
            gouraud[1] = B::uniformBuffer(1, both, g.pass[kPassThin].get());
            g.hairGouraudThinSrb = srbv(gouraud);
        }
        for (int i = 0; i < kDomCount; ++i) {
            g.hairDepthSrb[i] = srb({B::uniformBuffer(1, both, domView(i)), pointsB, strandsB, colorsB});
            g.hairDomSrb[i] = srb({B::uniformBuffer(1, both, domView(i)), pointsB, strandsB, colorsB,
                                   B::sampledTexture(5, fs, g.dom[i].front.get(), g.nearest.get())});
        }
    }
    // A surface's bindings: the frame, the view, its material, the maps, the tables, its
    // textures (the stand-ins where it has none). The template (every texture a stand-in,
    // the material block the first surface's or none) makes the pipelines.
    // surface_lookdev.frag reads its textures through four separate samplers (Direct3D 11
    // allows 16 sampler slots; the textures are many more).
    auto surfaceBindings = [&](const GpuSurface* s) {
        std::vector<B> out{B::uniformBuffer(0, fs, g.frame.get()), B::uniformBuffer(1, both, g.pass[kPassCamera].get()),
                           B::uniformBuffer(4, fs, s ? s->material.get() : g.blitTonemap.get())};
        const int domBindings[kDomCount - 1][2] = {{5, 6}, {7, 8}, {14, 15}, {16, 17}};
        for (int i = 0; i < kDomCount - 1; ++i) {
            out.push_back(B::texture(domBindings[i][0], fs, g.dom[i].front.get()));
            out.push_back(B::texture(domBindings[i][1], fs, g.dom[i].layers.get()));
        }
        out.push_back(B::texture(kBindEnvPrefiltered, fs, g.envPrefiltered.get()));
        out.push_back(B::texture(kBindBrdf, fs, g.brdfLut.get()));
        out.push_back(B::texture(kBindSkinLut, fs, g.skinLut.get()));
        out.push_back(B::texture(kBindAlbedo, fs, s && s->albedo ? s->albedo : g.white.get()));
        out.push_back(B::texture(kBindNormal, fs, s && s->normal ? s->normal : g.flatNormal.get()));
        out.push_back(B::texture(kBindMask, fs, s && s->mask ? s->mask : g.white.get()));
        out.push_back(B::texture(kBindDetail, fs, g.detail.get()));
        for (int i = 0; i < kDomLights; ++i) out.push_back(B::texture(kBindShadow0 + i, fs, g.shadow[i].distance.get()));
        out.push_back(B::texture(kBindSssBlurred, fs, g.sss.blurred.get()));
        for (int k = 0; k < kVolumeCount; ++k) {
            out.push_back(B::texture(kBindVolume0 + k, fs, g.vis.volumes[k] ? g.vis.volumes[k].get() : g.noVolume.get()));
        }
        out.push_back(B::sampler(kBindSamplerNearest, fs, g.nearest.get()));
        out.push_back(B::sampler(kBindSamplerNearest + 1, fs, g.linear.get()));
        out.push_back(B::sampler(kBindSamplerNearest + 2, fs, g.mipRepeat.get()));
        out.push_back(B::sampler(kBindSamplerNearest + 3, fs, g.mipClamp.get()));
        return out;
    };
    // The distance passes (a light's shadow map) read the material's cutout and albedo map.
    auto depthBindings = [&](QRhiBuffer* view, const GpuSurface* s) {
        return srb({B::uniformBuffer(1, both, view), B::uniformBuffer(4, fs, s ? s->material.get() : g.blitTonemap.get()),
                    B::sampledTexture(kBindAlbedo, fs, s && s->albedo ? s->albedo : g.white.get(), g.mipRepeat.get())});
    };
    if (!g.surfaceTemplateSrb) {
        g.surfaceTemplateSrb = srbv(surfaceBindings(nullptr));
        g.surfaceDepthTemplateSrb = depthBindings(domView(0), nullptr);
        g.surfaceDomTemplateSrb = srb({B::uniformBuffer(1, both, domView(0)), B::sampledTexture(5, fs, g.dom[0].front.get(), g.nearest.get())});
        g.backgroundSrb = srb({B::uniformBuffer(0, fs, g.frame.get()), B::uniformBuffer(1, fs, g.pass[0].get()),
                               B::uniformBuffer(3, vs, g.blitTonemap.get()),
                               B::sampledTexture(9, fs, g.environment.get(), g.linear.get())});
    }
    for (auto& s : g.surfaces) {
        if (s->lookdevSrb) continue;
        s->lookdevSrb = srbv(surfaceBindings(s.get()));
        for (int i = 0; i < kDomCount; ++i) {
            s->depthSrb[i] = depthBindings(domView(i), s.get());
            s->domSrb[i] = srb({B::uniformBuffer(1, both, domView(i)), B::sampledTexture(5, fs, g.dom[i].front.get(), g.nearest.get())});
        }
        for (int i = 0; i < kDomLights; ++i) s->shadowSrb[i] = depthBindings(g.pass[kPassShadow + i].get(), s.get());
    }
    if (!g.sss.srb[0]) {
        // The blur's two passes: the first reads the prepass' irradiance into temp, the
        // second temp into blurred, both with the depth.
        for (int i = 0; i < 2; ++i) {
            g.sss.srb[i] = srb({B::uniformBuffer(3, both, g.sss.blit.get()), B::uniformBuffer(2, fs, g.sss.uniforms[i].get()),
                                B::sampledTexture(10, fs, i == 0 ? g.sss.irradiance.get() : g.sss.temp.get(), g.nearest.get()),
                                B::sampledTexture(11, fs, g.sss.depth.get(), g.nearest.get())});
        }
    }
    if (!g.accumulateSrb[0]) {
        for (int i = 0; i < 2; ++i) {
            // Writing accum[i] from the frame and accum[1 - i].
            g.accumulateSrb[i] = srb({B::uniformBuffer(3, both, g.blitAccumulate.get()),
                                      B::sampledTexture(10, fs, g.resolved.get(), g.nearest.get()),
                                      B::sampledTexture(11, fs, g.accum[1 - i].get(), g.nearest.get())});
        }
        // The display transform samples bilinearly: the same texels at the host's size, a
        // smooth rescale when the lighting's resolution scale made the picture another size.
        g.tonemapSrb[0] = srb({B::uniformBuffer(3, both, g.blitTonemap.get()),
                               B::sampledTexture(10, fs, g.resolved.get(), g.linear.get())});
        for (int i = 0; i < 2; ++i) {
            g.tonemapSrb[1 + i] = srb({B::uniformBuffer(3, both, g.blitTonemap.get()),
                                       B::sampledTexture(10, fs, g.accum[i].get(), g.linear.get())});
        }
    }
    if (g.hairLookdev && !targetChanged && !hairChanged) return true;

    const QShader hairVert = loadLookdevShader("hair.vert"), hairLookdevFrag = loadLookdevShader("hair_lookdev.frag");
    const QShader hairDepthFrag = loadLookdevShader("hair_depth.frag"), hairDomFrag = loadLookdevShader("hair_dom.frag");
    const QShader pointsVert = loadLookdevShader("points.vert"), pointShadeFrag = loadLookdevShader("hair_point_shade.frag");
    const QShader hairGouraudVert = loadLookdevShader("hair_gouraud.vert"), hairGouraudFrag = loadLookdevShader("hair_gouraud.frag");
    const QShader surfaceVert = loadLookdevShader("surface_lookdev.vert"), surfaceFrag = loadLookdevShader("surface_lookdev.frag");
    const QShader surfaceDepthFrag = loadLookdevShader("surface_depth.frag"), surfaceDomFrag = loadLookdevShader("surface_dom.frag");
    const QShader surfaceSssFrag = loadLookdevShader("surface_sss.frag"), sssBlurFrag = loadLookdevShader("sss_blur.frag");
    const QShader fullVert = loadLookdevShader("fullscreen.vert"), backgroundFrag = loadLookdevShader("background.frag");
    const QShader accumulateFrag = loadLookdevShader("accumulate.frag"), tonemapFrag = loadLookdevShader("tonemap.frag");
    if (!hairVert.isValid() || !hairLookdevFrag.isValid() || !fullVert.isValid()) return false;

    QRhiGraphicsPipeline::TargetBlend premultiplied;
    premultiplied.enable = true;
    premultiplied.srcColor = QRhiGraphicsPipeline::One;
    premultiplied.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    premultiplied.srcAlpha = QRhiGraphicsPipeline::One;
    premultiplied.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    QRhiGraphicsPipeline::TargetBlend additive;
    additive.enable = true;
    additive.srcColor = QRhiGraphicsPipeline::One;
    additive.dstColor = QRhiGraphicsPipeline::One;
    additive.srcAlpha = QRhiGraphicsPipeline::One;
    additive.dstAlpha = QRhiGraphicsPipeline::One;
    QRhiGraphicsPipeline::TargetBlend opaque;

    const bool strips = g.useStrip;
    auto make = [&](const QShader& vert, const QShader& frag, QRhiShaderResourceBindings* layout, QRhiRenderPassDescriptor* pass,
                    int samples, QRhiGraphicsPipeline::Topology topology, bool depthTest, bool depthWrite,
                    const QRhiGraphicsPipeline::TargetBlend& blend, bool surfaceInputs, int targets = 1) {
        std::unique_ptr<QRhiGraphicsPipeline> ps(rhi->newGraphicsPipeline());
        ps->setShaderStages({{QRhiShaderStage::Vertex, vert}, {QRhiShaderStage::Fragment, frag}});
        QRhiVertexInputLayout layoutIn;
        if (surfaceInputs) {
            // surface.h's interleaved layout: position, normal, tangent, uv, (occlusion, curvature).
            layoutIn.setBindings({{SurfaceVertices::kStride * sizeof(float)}});
            layoutIn.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0},
                                    {0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float)},
                                    {0, 2, QRhiVertexInputAttribute::Float4, 6 * sizeof(float)},
                                    {0, 3, QRhiVertexInputAttribute::Float2, 10 * sizeof(float)},
                                    {0, 4, QRhiVertexInputAttribute::Float2, 12 * sizeof(float)}});
        }
        ps->setVertexInputLayout(layoutIn);
        ps->setTopology(topology);
        ps->setCullMode(QRhiGraphicsPipeline::None);
        ps->setDepthTest(depthTest);
        ps->setDepthWrite(depthWrite);
        ps->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
        ps->setSampleCount(samples);
        if (targets == 2) ps->setTargetBlends({blend, blend});
        else ps->setTargetBlends({blend});
        ps->setShaderResourceBindings(layout);
        ps->setRenderPassDescriptor(pass);
        if (!ps->create()) {
            qWarning("GroomLab: a lookdev pipeline could not be made");
            ps.reset();
        }
        return ps;
    };
    const auto hairTopology = strips ? QRhiGraphicsPipeline::TriangleStrip : QRhiGraphicsPipeline::Triangles;
    if (g.points) {
        g.hairLookdev = make(hairVert, hairLookdevFrag, g.hairLookdevSrb.get(), g.hdrPass.get(), g.hdrSamples, hairTopology,
                             true, true, premultiplied, false);
        // The thin strands of the interactive frame: blended over everything, no depth written.
        g.hairThin = make(hairVert, hairLookdevFrag, g.hairThinSrb.get(), g.hdrPass.get(), g.hdrSamples, hairTopology,
                          true, false, premultiplied, false);
        g.hairDepth = make(hairVert, hairDepthFrag, g.hairDepthSrb[0].get(), g.dom[0].depthPass.get(), 1, hairTopology, true,
                           true, opaque, false);
        g.hairDom = make(hairVert, hairDomFrag, g.hairDomSrb[0].get(), g.dom[0].layersPass.get(), 1, hairTopology, false,
                         false, additive, false);
        if (!g.hairLookdev || !g.hairThin || !g.hairDepth || !g.hairDom) return false;
        // The interactive frame's per-point shading and its Gouraud ribbons (the same two
        // draws as above, lit from the shaded points).
        if (g.shaded && pointsVert.isValid() && pointShadeFrag.isValid() && hairGouraudVert.isValid() && hairGouraudFrag.isValid()) {
            g.pointShade = make(pointsVert, pointShadeFrag, g.pointShadeSrb.get(), g.shadedPass.get(), 1, QRhiGraphicsPipeline::Triangles,
                                false, false, opaque, false);
            g.hairGouraud = make(hairGouraudVert, hairGouraudFrag, g.hairGouraudThickSrb.get(), g.hdrPass.get(), g.hdrSamples, hairTopology,
                                 true, true, premultiplied, false);
            g.hairGouraudThin = make(hairGouraudVert, hairGouraudFrag, g.hairGouraudThinSrb.get(), g.hdrPass.get(), g.hdrSamples, hairTopology,
                                     true, false, premultiplied, false);
            if (!g.pointShade || !g.hairGouraud || !g.hairGouraudThin) return false;
        }
    }
    if (!g.surfaceLookdev) {
        g.surfaceLookdev = make(surfaceVert, surfaceFrag, g.surfaceTemplateSrb.get(), g.hdrPass.get(), g.hdrSamples,
                                QRhiGraphicsPipeline::Triangles, true, true, opaque, true);
        g.surfaceDepth = make(surfaceVert, surfaceDepthFrag, g.surfaceDepthTemplateSrb.get(), g.dom[0].depthPass.get(), 1,
                              QRhiGraphicsPipeline::Triangles, true, true, opaque, true);
        g.surfaceDom = make(surfaceVert, surfaceDomFrag, g.surfaceDomTemplateSrb.get(), g.dom[0].layersPass.get(), 1,
                            QRhiGraphicsPipeline::Triangles, false, false, additive, true);
        g.background = make(fullVert, backgroundFrag, g.backgroundSrb.get(), g.hdrPass.get(), g.hdrSamples,
                            QRhiGraphicsPipeline::Triangles, false, false, opaque, false);
        g.accumulate = make(fullVert, accumulateFrag, g.accumulateSrb[0].get(), g.accumPass.get(), 1,
                            QRhiGraphicsPipeline::Triangles, false, false, opaque, false);
        // The skin's prepass (the same bindings as the HDR pass: the surfaces' own serve)
        // and the blur.
        g.surfaceSss = make(surfaceVert, surfaceSssFrag, g.surfaceTemplateSrb.get(), g.sss.prepassPass.get(), 1,
                            QRhiGraphicsPipeline::Triangles, true, true, opaque, true, 2);
        g.sssBlur = make(fullVert, sssBlurFrag, g.sss.srb[0].get(), g.sss.blurPass.get(), 1,
                         QRhiGraphicsPipeline::Triangles, false, false, opaque, false);
        if (!g.surfaceLookdev || !g.surfaceDepth || !g.surfaceDom || !g.background || !g.accumulate || !g.surfaceSss || !g.sssBlur) return false;
    }
    if (targetChanged || !g.tonemap) {
        g.tonemap = make(fullVert, tonemapFrag, g.tonemapSrb[0].get(), target->renderPassDescriptor(), target->sampleCount(),
                         QRhiGraphicsPipeline::Triangles, false, false, opaque, false);
        if (!g.tonemap) return false;
        g.targetFormat = format;
        g.targetSamples = target->sampleCount();
    }
    return true;
}

namespace {

// Texels [first, first + count) of a kHairTextureWidth-wide texture written from `data` (16
// bytes a texel, contiguous as the texture's rows are): a partial first row, whole rows, a
// partial last row, as the sub-region uploads QRhi takes (destination top-left and source size).
void uploadTexelRange(QRhiResourceUpdateBatch* batch, QRhiTexture* texture, std::size_t first, std::size_t count, const float* data) {
    constexpr std::size_t kTexel = 16;
    QVarLengthArray<QRhiTextureUploadEntry, 3> entries;
    std::size_t at = first;
    const std::size_t end = first + count;
    auto entry = [&](std::size_t x, std::size_t y, std::size_t w, std::size_t h) {
        QRhiTextureSubresourceUploadDescription sub(data + (at - first) * 4, static_cast<quint32>(w * h * kTexel));
        sub.setSourceSize(QSize(static_cast<int>(w), static_cast<int>(h)));
        sub.setDestinationTopLeft(QPoint(static_cast<int>(x), static_cast<int>(y)));
        entries.append(QRhiTextureUploadEntry(0, 0, sub));
        at += w * h;
    };
    const std::size_t x0 = at % kHairTextureWidth;
    if (x0 != 0) entry(x0, at / kHairTextureWidth, std::min<std::size_t>(kHairTextureWidth - x0, end - at), 1);
    if (at < end && end - at >= kHairTextureWidth) entry(0, at / kHairTextureWidth, kHairTextureWidth, (end - at) / kHairTextureWidth);
    if (at < end) entry(0, at / kHairTextureWidth, end - at, 1);
    QRhiTextureUploadDescription desc;
    desc.setEntries(entries.begin(), entries.end());
    batch->uploadTexture(texture, desc);
}

// A texture of one RGBA32F texel (made when `texture` is none or another size), written from
// `rgba` (4 floats; `bytes` holds them until the frame executes).
bool uploadSingleTexel(QRhi* rhi, QRhiResourceUpdateBatch* batch, std::unique_ptr<QRhiTexture>& texture, const std::vector<float>& rgba,
                       QByteArray& bytes) {
    if (!texture || texture->pixelSize() != QSize(1, 1)) {
        texture.reset(rhi->newTexture(QRhiTexture::RGBA32F, QSize(1, 1)));
        if (!texture->create()) {
            texture.reset();
            return false;
        }
    }
    bytes = QByteArray(reinterpret_cast<const char*>(rgba.data()), 16);
    batch->uploadTexture(texture.get(), QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, QRhiTextureSubresourceUploadDescription(bytes))));
    return true;
}

// A QRhi object's id, unique for the process's life (0: none).
quint64 resourceId(const QRhiResource* r) { return r ? r->globalResourceId() : 0; }

bool partialUploadsPreferred() {
    static const bool on = qgetenv("GROOMLAB_APP_PARTIAL_UPLOADS") != "0";
    return on;
}

// A whole texture written straight through the device where the API allows it (as
// renderer.cpp's writeBufferNative writes the buffers): UpdateSubresource on Direct3D 11 from
// `data`, rows of `rowBytes`, the driver's copy to the GPU the only one, where QRhi's batch
// would first copy the bytes into itself. Ordered as the batch would be: the device runs it
// now, before the frame's recorded draws, after the last frame's. False when it cannot.
bool writeTextureNative(QRhi* rhi, QRhiTexture* texture, const void* data, std::size_t rowBytes) {
#ifdef Q_OS_WIN
    if (rhi->backend() != QRhi::D3D11 || !texture) return false;
    const auto* handles = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
    auto* context = handles ? static_cast<ID3D11DeviceContext*>(handles->context) : nullptr;
    auto* native = reinterpret_cast<ID3D11Resource*>(static_cast<std::uintptr_t>(texture->nativeTexture().object));
    if (!context || !native) return false;
    context->UpdateSubresource(native, 0, nullptr, data, static_cast<UINT>(rowBytes), 0);
    return true;
#else
    (void)rhi;
    (void)texture;
    (void)data;
    (void)rowBytes;
    return false;
#endif
}

}  // namespace

HairIndices LookdevRenderer::indicesNeeded() const {
    // None while the GPU holds this layout (the same point counts, colored alike): what the upload
    // writes in place; else the kind drawn, both before the first upload says which.
    const Gpu& g = *gpu_;
    if (!strands_) return HairIndices::None;
    if (g.points && g.strands && g.pointColors && (g.strip || g.list) && strands_->pointCount() == pointCount_ &&
        strands_->strandCount() == strandCount_ && strands_->pointCount() > 0 && layoutKeyOf(*strands_) == uploadedLayout_ &&
        hairPointColored(*strands_) == pointColored_) {
        return HairIndices::None;
    }
    if (!g.indexKindKnown) return HairIndices::Both;
    return g.useStrip ? HairIndices::Strip : HairIndices::List;
}

bool LookdevRenderer::partialUpload() const {
    // Only some strands changed from the ones the textures hold (a brush dab's incremental
    // evaluation says which; the same point counts): their texels alone are written, in runs,
    // the indices and the rest of the texels kept.
    // Past a third of the strands (a release after a wide stroke) the whole write wins over
    // thousands of small ones.
    const Gpu& g = *gpu_;
    return partialUploads_ && partialUploadsPreferred() && g.points && g.strands && strands_ && changes_.changes && !changes_.changes->all &&
           uploadedSerial_ != 0 && changes_.changesBase == uploadedSerial_ && strands_->pointCount() == pointCount_ &&
           strands_->strandCount() == strandCount_ && layoutKeyOf(*strands_) == uploadedLayout_ &&
           changes_.changes->strands.size() * 3 < strandCount_ && hairPointColored(*strands_) == pointColored_;
}

void LookdevRenderer::uploadHair(QRhi* rhi, QRhiResourceUpdateBatch* batch) {
    Gpu& g = *gpu_;
    uploadedThisFrame_ = false;
    waitingForBuild_ = false;
    if (uploadedVersion_ == hairVersion_ && g.points) return;
    const bool partial = partialUpload();
    if (partial) {
        QElapsedTimer timer;
        timer.start();
        const StrandSet& s = *strands_;
        std::vector<float> points, strandData, pointColors;
        std::size_t strands = 0;
        for (const auto& [first, end] : strandRuns(changes_.changes->strands, 64)) {
            buildHairRange(s, scalp_.get(), first, end, points, strandData, nullptr, pointColored_ ? &pointColors : nullptr);
            const std::size_t pBegin = s.strandBegin(first), pEnd = s.offsets()[end];
            if (pEnd > pBegin) uploadTexelRange(batch, g.points.get(), pBegin * 2, (pEnd - pBegin) * 2, points.data());
            if (pEnd > pBegin && pointColored_) uploadTexelRange(batch, g.pointColors.get(), pBegin, pEnd - pBegin, pointColors.data());
            uploadTexelRange(batch, g.strands.get(), first * 3, (end - first) * 3, strandData.data());
            strands += end - first;
        }
        // The bounds a whole write gives, shrinking too (the set's extent, measured again only in
        // the blocks that changed): maps fit to a box that only ever grew shade other pixels.
        hairBounds_ = hairBoundsOf(s);
        updateBounds();
        uploadMs_ = timer.nsecsElapsed() / 1e6;
        uploadPartial_ = true;
        uploadStrands_ = strands;
        uploadedThisFrame_ = true;
        ++uploads_;
        uploadedVersion_ = hairVersion_;
        uploadedSerial_ = changes_.serial;
        pending_.reset();
        return;
    }
    if (!pending_ && build_ && build_->version == hairVersion_ && g.points) {
        // Built ahead (setBuildAhead): taken once built; until then this frame draws the hair
        // the textures hold, and asks for another.
        if (!build_->done.load(std::memory_order_acquire)) {
            waitingForBuild_ = true;
            return;
        }
        joinBuild();
        pending_ = build_->buffers;
        uploadMs_ = 0.0;  // (the build's time was the thread's)
        // The frames since drew the maps and the volumes from the hair before: again.
        sceneChanged();
    }
    build_.reset();
    if (!pending_) {
        QElapsedTimer timer;
        timer.start();
        pending_ = std::make_shared<HairBuffers>(strands_ ? buildHairBuffers(*strands_, scalp_.get(), indicesNeeded()) : HairBuffers());
        uploadMs_ = timer.nsecsElapsed() / 1e6;
    }
    HairBuffers& b = *pending_;
    uploadPartial_ = false;
    uploadStrands_ = b.strandCount;
    uploadedThisFrame_ = true;
    ++uploads_;
    uploadedSerial_ = changes_.serial;
    // The same layout as held (the same point counts: a tweak moving every point): the textures
    // and the index buffers are written again in place; else made anew (the bindings and
    // pipelines naming the textures with them).
    const std::uint64_t layout = strands_ ? layoutKeyOf(*strands_) : 0;
    // (The point colors coming or going changes their texture, and so the bindings: as a new layout.)
    const bool sameLayout = g.points && g.strands && (g.strip || g.list) && layout == uploadedLayout_ && b.pointCount == pointCount_ &&
                            b.strandCount == strandCount_ && b.pointCount > 0 && b.pointColored == pointColored_ && g.pointColors;
    uploadedLayout_ = layout;
    pointColored_ = b.pointColored;
    if (!sameLayout) {
        g.points.reset();
        g.strands.reset();
        g.pointColors.reset();
        g.strip.reset();
        g.list.reset();
        g.hairLookdevSrb.reset();
        g.hairThickSrb.reset();
        g.hairThinSrb.reset();
        g.hairLookdev.reset();
        g.hairThin.reset();
        g.pointShadeSrb.reset();
        g.hairGouraudThickSrb.reset();
        g.hairGouraudThinSrb.reset();
        g.pointShade.reset();
        g.hairGouraud.reset();
        g.hairGouraudThin.reset();
        g.indexCount = 0;
    }
    pointCount_ = b.pointCount;
    strandCount_ = b.strandCount;
    bounds_ = b.bounds;
    g.useStrip = rhi->isFeatureSupported(QRhi::PrimitiveRestart);
    g.indexKindKnown = true;
    // The indices only for a new layout, of the kind drawn (built here if the build left them
    // out: the layout changed since it was asked for).
    if (!sameLayout && strands_) buildHairIndices(*strands_, g.useStrip ? HairIndices::Strip : HairIndices::List, b);
    if (b.pointCount > 0 && (sameLayout || !(g.useStrip ? b.strip : b.list).empty())) {
        auto uploadIndices = [&](std::unique_ptr<QRhiBuffer>& buffer, const std::vector<std::uint32_t>& data) {
            buffer.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, static_cast<quint32>(data.size() * sizeof(std::uint32_t))));
            if (!buffer->create()) {
                buffer.reset();
                return false;
            }
            batch->uploadStaticBuffer(buffer.get(), data.data());
            return true;
        };
        // An RGBA32F texture kHairTextureWidth wide, the entries two texels each, the last row
        // padded: a texel fetch in the vertex shader reads them on every backend. Where the
        // device computes, the GPU look preview may write the points in place (UsedWithLoadStore).
        // The padded copy is kept between uploads (the batch holds it, shared, until the frame
        // executes: no fresh pages each time).
        const QRhiTexture::Flags flags = rhi->isFeatureSupported(QRhi::Compute) ? QRhiTexture::UsedWithLoadStore : QRhiTexture::Flags();
        auto uploadTexture = [&](std::unique_ptr<QRhiTexture>& texture, const std::vector<float>& floats, QByteArray& padded) {
            const std::size_t texels = floats.size() / 4;
            const int rows = static_cast<int>((texels + kHairTextureWidth - 1) / kHairTextureWidth);
            if (rows > rhi->resourceLimit(QRhi::TextureSizeMax)) return false;
            if (!texture || texture->pixelSize() != QSize(kHairTextureWidth, std::max(rows, 1))) {
                texture.reset(rhi->newTexture(QRhiTexture::RGBA32F, QSize(kHairTextureWidth, std::max(rows, 1)), 1, flags));
                if (!texture->create()) {
                    texture.reset();
                    return false;
                }
            }
            // A texture written again: straight through the device where it can be (the
            // buffers are padded to whole rows for that); else the padded copy for the batch.
            const std::size_t rowBytes = static_cast<std::size_t>(kHairTextureWidth) * 16;
            if (floats.size() * sizeof(float) == static_cast<std::size_t>(std::max(rows, 1)) * rowBytes &&
                writeTextureNative(rhi, texture.get(), floats.data(), rowBytes)) {
                return true;
            }
            padded.resize(static_cast<qsizetype>(std::max(rows, 1)) * kHairTextureWidth * 16);
            std::memcpy(padded.data(), floats.data(), floats.size() * sizeof(float));
            QRhiTextureSubresourceUploadDescription sub(padded);
            batch->uploadTexture(texture.get(), QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, sub)));
            return true;
        };
        // Without colors of their own the points' colors texture is one texel (hair.vert then
        // reads each strand's).
        static const std::vector<float> noPointColors(4, 0.0f);
        QByteArray single;
        const bool ok = uploadTexture(g.points, b.points, pointsScratch_) && uploadTexture(g.strands, b.strands, strandsScratch_) &&
                        (b.pointColored ? uploadTexture(g.pointColors, b.pointColors, pointColorsScratch_)
                                        : uploadSingleTexel(rhi, batch, g.pointColors, noPointColors, single)) &&
                        (sameLayout || (g.useStrip ? uploadIndices(g.strip, b.strip) : uploadIndices(g.list, b.list)));
        if (ok) {
            if (!sameLayout) g.indexCount = static_cast<std::uint32_t>(g.useStrip ? b.strip.size() : b.list.size());
            // The shaded points, one texel each (a target: the per-point shading pass draws it).
            const int rows = static_cast<int>((b.pointCount + kHairTextureWidth - 1) / kHairTextureWidth);
            if (!g.shaded || g.shaded->pixelSize() != QSize(kHairTextureWidth, std::max(rows, 1))) {
                g.shaded.reset(rhi->newTexture(QRhiTexture::RGBA16F, QSize(kHairTextureWidth, std::max(rows, 1)), 1, QRhiTexture::RenderTarget));
                if (g.shaded->create()) {
                    QRhiTextureRenderTargetDescription desc{QRhiColorAttachment(g.shaded.get())};
                    g.shadedTarget.reset(rhi->newTextureRenderTarget(desc));
                    if (!g.shadedPass) g.shadedPass.reset(g.shadedTarget->newCompatibleRenderPassDescriptor());
                    g.shadedTarget->setRenderPassDescriptor(g.shadedPass.get());
                    if (!g.shadedTarget->create()) g.shaded.reset();
                } else {
                    g.shaded.reset();
                }
                if (!g.shaded) g.shadedTarget.reset();
                g.pointShadeSrb.reset();
                g.hairGouraudThickSrb.reset();
                g.hairGouraudThinSrb.reset();
            }
        } else {
            qWarning("GroomLab: the hair's textures could not be made (%zu points)", b.pointCount);
            g.points.reset();
            g.strands.reset();
            g.pointColors.reset();
        }
    }
    hairBounds_ = b.bounds;
    updateBounds();
    uploadedVersion_ = hairVersion_;
    pending_.reset();
}

void LookdevRenderer::updateBounds() {
    // What the maps must cover: the hair's cover the hair, the surfaces' shadow maps the
    // surfaces, the camera's view both.
    surfaceBounds_ = Bounds();
    for (const SurfacePtr& s : surfaces_) {
        if (!s || !s->mesh) continue;
        for (const Vec3f& p : s->mesh->points()) {
            if (!surfaceBounds_.valid) {
                surfaceBounds_.lo = surfaceBounds_.hi = p;
                surfaceBounds_.valid = true;
                continue;
            }
            surfaceBounds_.lo = {std::min(surfaceBounds_.lo.x, p.x), std::min(surfaceBounds_.lo.y, p.y), std::min(surfaceBounds_.lo.z, p.z)};
            surfaceBounds_.hi = {std::max(surfaceBounds_.hi.x, p.x), std::max(surfaceBounds_.hi.y, p.y), std::max(surfaceBounds_.hi.z, p.z)};
        }
    }
    bounds_ = hairBounds_;
    if (surfaceBounds_.valid) {
        if (!bounds_.valid) {
            bounds_ = surfaceBounds_;
        } else {
            bounds_.lo = {std::min(bounds_.lo.x, surfaceBounds_.lo.x), std::min(bounds_.lo.y, surfaceBounds_.lo.y), std::min(bounds_.lo.z, surfaceBounds_.lo.z)};
            bounds_.hi = {std::max(bounds_.hi.x, surfaceBounds_.hi.x), std::max(bounds_.hi.y, surfaceBounds_.hi.y), std::max(bounds_.hi.z, surfaceBounds_.hi.z)};
        }
    }
}

const Surface* LookdevRenderer::screenSpaceSkin() const {
    for (const SurfacePtr& s : surfaces_) {
        if (!s || !s->mesh) continue;
        const SurfaceMaterial& m = s->material;
        if (m.kind == SurfaceKind::Skin && m.sssMode == SurfaceMaterial::SssMode::ScreenSpace && m.sssStrength > 0.0f) return s.get();
    }
    return nullptr;
}

namespace {

// An RGB float map as RGBA32F texels.
std::vector<float> rgbaOf(const std::vector<float>& rgb, std::size_t texels) {
    std::vector<float> rgba(texels * 4);
    for (std::size_t i = 0; i < texels; ++i) {
        rgba[i * 4] = rgb[i * 3];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 1.0f;
    }
    return rgba;
}

}  // namespace

void LookdevRenderer::uploadEnvironment(QRhi* /*rhi*/, QRhiResourceUpdateBatch* batch) {
    if (!environmentDirty_) return;
    environmentDirty_ = false;
    environmentKind_ = lighting_.environment;
    environment_ = makeEnvironment(lighting_.environment, 128, 64);
    sh_ = environmentSH(environment_);
    envLights_ = environmentLights(environment_, 8);
    // The brightest lights first: the interactive frames take the first few.
    std::sort(envLights_.begin(), envLights_.end(),
              [](const DirectionalLight& a, const DirectionalLight& b) { return a.intensity > b.intensity; });
    const std::size_t texels = static_cast<std::size_t>(environment_.width) * environment_.height;
    std::vector<float> rgba = rgbaOf(environment_.rgb, texels);
    QRhiTextureSubresourceUploadDescription sub(rgba.data(), static_cast<quint32>(rgba.size() * sizeof(float)));
    batch->uploadTexture(gpu_->environment.get(), QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, sub)));
    // The prefiltered levels: roughness k / (levels - 1) at half the size each (the lobe gets
    // wider as the texels do; the sharpest level is the map itself), more samples where the
    // texels are few.
    QVarLengthArray<QRhiTextureUploadEntry, kEnvLevels> entries;
    std::vector<std::vector<float>> levels(kEnvLevels);
    for (int k = 0; k < kEnvLevels; ++k) {
        const int w = std::max(environment_.width >> k, 1), h = std::max(environment_.height >> k, 1);
        const float roughness = static_cast<float>(k) / (kEnvLevels - 1);
        const EnvironmentMap level = prefilterEnvironment(environment_, roughness, w, h, std::min(64 << k, 1024));
        levels[k] = rgbaOf(level.rgb, static_cast<std::size_t>(w) * h);
        entries.append(QRhiTextureUploadEntry(0, k, QRhiTextureSubresourceUploadDescription(levels[k].data(), static_cast<quint32>(levels[k].size() * sizeof(float)))));
    }
    QRhiTextureUploadDescription desc;
    desc.setEntries(entries.begin(), entries.end());
    batch->uploadTexture(gpu_->envPrefiltered.get(), desc);
}

void LookdevRenderer::uploadTables(QRhi* rhi, QRhiResourceUpdateBatch* batch) {
    Gpu& g = *gpu_;
    if (g.brdfLut) return;
    auto makeFloatTexture = [&](std::unique_ptr<QRhiTexture>& texture, const std::vector<float>& rgba, int w, int h) {
        texture.reset(rhi->newTexture(QRhiTexture::RGBA32F, QSize(w, h), 1, {}));
        if (!texture->create()) {
            texture.reset();
            return;
        }
        batch->uploadTexture(texture.get(), QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                                                0, 0, QRhiTextureSubresourceUploadDescription(rgba.data(), static_cast<quint32>(rgba.size() * sizeof(float))))));
    };
    auto makeRgba8 = [&](std::unique_ptr<QRhiTexture>& texture, const QImage& image, bool mips) {
        texture.reset(rhi->newTexture(QRhiTexture::RGBA8, image.size(), 1,
                                      mips ? QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips : QRhiTexture::Flags()));
        if (!texture->create()) {
            texture.reset();
            return;
        }
        batch->uploadTexture(texture.get(), image);
        if (mips) batch->generateMips(texture.get());
    };
    // The tables, computed once per process: they depend on nothing of the scene.
    static const std::vector<float> brdf = [] {
        const std::vector<float> rg = environmentBrdfLUT(kBrdfLutSize);
        std::vector<float> rgba(static_cast<std::size_t>(kBrdfLutSize) * kBrdfLutSize * 4, 0.0f);
        for (std::size_t i = 0; i < static_cast<std::size_t>(kBrdfLutSize) * kBrdfLutSize; ++i) {
            rgba[i * 4] = rg[i * 2];
            rgba[i * 4 + 1] = rg[i * 2 + 1];
            rgba[i * 4 + 3] = 1.0f;
        }
        return rgba;
    }();
    static const std::vector<float> skin = rgbaOf(preintegratedSkinLUT(kSkinLutWidth, kSkinLutHeight), static_cast<std::size_t>(kSkinLutWidth) * kSkinLutHeight);
    static const QImage detail = [] {
        const std::vector<float> rgb = microNormalTexture(kDetailSize);
        QImage image(kDetailSize, kDetailSize, QImage::Format_RGBA8888);
        for (int y = 0; y < kDetailSize; ++y) {
            uchar* row = image.scanLine(y);
            for (int x = 0; x < kDetailSize; ++x) {
                const float* p = rgb.data() + (static_cast<std::size_t>(y) * kDetailSize + x) * 3;
                for (int k = 0; k < 3; ++k) row[x * 4 + k] = static_cast<uchar>(std::clamp(p[k] * 255.0f + 0.5f, 0.0f, 255.0f));
                row[x * 4 + 3] = 255;
            }
        }
        return image;
    }();
    makeFloatTexture(g.brdfLut, brdf, kBrdfLutSize, kBrdfLutSize);
    makeFloatTexture(g.skinLut, skin, kSkinLutWidth, kSkinLutHeight);
    makeRgba8(g.detail, detail, true);
    QImage white(1, 1, QImage::Format_RGBA8888);
    white.fill(QColor(255, 255, 255, 255));
    makeRgba8(g.white, white, false);
    QImage flat(1, 1, QImage::Format_RGBA8888);
    flat.fill(QColor(128, 128, 255, 255));
    makeRgba8(g.flatNormal, flat, false);
}

QRhiTexture* LookdevRenderer::textureFor(QRhi* rhi, QRhiResourceUpdateBatch* batch, const std::string& path, bool srgb) {
    if (path.empty()) return nullptr;
    Gpu& g = *gpu_;
    // A color map is an sRGB texture: the device decodes it, so its filtering and its mips
    // are linear (decoded in the shader, the mips would average sRGB values); a data map
    // (normals, the gray masks) stays as it is. The same file in both roles is two textures.
    // The file's time and size are in the key: a map written again in place (the cards baked
    // again under the same name) loads afresh for the surface made for it.
    const QFileInfo info(QString::fromStdString(path));
    const std::string key = (srgb ? path + "|srgb" : path) + "|" + std::to_string(info.lastModified().toMSecsSinceEpoch()) + "|" +
                            std::to_string(info.size());
    auto it = g.textures.find(key);
    if (it != g.textures.end()) return it->second.get();
    QImage image(QString::fromStdString(path));
    std::unique_ptr<QRhiTexture> texture;
    if (image.isNull()) {
        qWarning("GroomLab: cannot read the texture %s", path.c_str());
    } else {
        image = image.convertToFormat(QImage::Format_RGBA8888);
        const int limit = rhi->resourceLimit(QRhi::TextureSizeMax);
        if (image.width() > limit || image.height() > limit) image = image.scaled(limit, limit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QRhiTexture::Flags flags = QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips;
        if (srgb) flags |= QRhiTexture::sRGB;
        texture.reset(rhi->newTexture(QRhiTexture::RGBA8, image.size(), 1, flags));
        if (texture->create()) {
            batch->uploadTexture(texture.get(), image);
            batch->generateMips(texture.get());
        } else {
            texture.reset();
        }
    }
    QRhiTexture* raw = texture.get();
    g.textures[key] = std::move(texture);  // a failed file is remembered as null: no retry per frame
    return raw;
}

QRhiTexture* LookdevRenderer::maskTextureFor(QRhi* rhi, QRhiResourceUpdateBatch* batch, const SurfaceMaterial& m) {
    // The gray maps packed into one texture: roughness, metallic, occlusion, specular, each
    // in its channel at the largest of their sizes (white where a map is missing).
    const std::string paths[4] = {m.roughnessMap, m.metallicMap, m.occlusionMap, m.specularMap};
    if (paths[0].empty() && paths[1].empty() && paths[2].empty() && paths[3].empty()) return nullptr;
    const std::string key = "mask|" + paths[0] + "|" + paths[1] + "|" + paths[2] + "|" + paths[3];
    Gpu& g = *gpu_;
    auto it = g.textures.find(key);
    if (it != g.textures.end()) return it->second.get();
    QImage maps[4];
    QSize size(1, 1);
    for (int k = 0; k < 4; ++k) {
        if (paths[k].empty()) continue;
        maps[k] = QImage(QString::fromStdString(paths[k]));
        if (maps[k].isNull()) qWarning("GroomLab: cannot read the texture %s", paths[k].c_str());
        else size = QSize(std::max(size.width(), maps[k].width()), std::max(size.height(), maps[k].height()));
    }
    const int limit = rhi->resourceLimit(QRhi::TextureSizeMax);
    size = QSize(std::min(size.width(), limit), std::min(size.height(), limit));
    for (int k = 0; k < 4; ++k) {
        if (maps[k].isNull()) continue;
        maps[k] = maps[k].convertToFormat(QImage::Format_RGBA8888);
        if (maps[k].size() != size) maps[k] = maps[k].scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    QImage packed(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar* row = packed.scanLine(y);
        const uchar* src[4] = {nullptr, nullptr, nullptr, nullptr};
        for (int k = 0; k < 4; ++k) src[k] = maps[k].isNull() ? nullptr : maps[k].constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            for (int k = 0; k < 4; ++k) row[x * 4 + k] = src[k] ? src[k][x * 4] : 255;
        }
    }
    std::unique_ptr<QRhiTexture> texture(rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips));
    if (texture->create()) {
        batch->uploadTexture(texture.get(), packed);
        batch->generateMips(texture.get());
    } else {
        texture.reset();
    }
    QRhiTexture* raw = texture.get();
    g.textures[key] = std::move(texture);
    return raw;
}

void LookdevRenderer::uploadSurfaces(QRhi* rhi, QRhiResourceUpdateBatch* batch) {
    Gpu& g = *gpu_;
    uploadTables(rhi, batch);
    // One GPU surface per source, in order; geometry again when the version moved, textures
    // and bindings again when the files changed, the material block every frame.
    std::vector<std::unique_ptr<GpuSurface>> kept;
    for (const SurfacePtr& source : surfaces_) {
        if (!source || !source->mesh) continue;
        std::unique_ptr<GpuSurface> s;
        for (auto& old : g.surfaces) {
            if (old && old->source == source) {
                s = std::move(old);
                break;
            }
        }
        if (!s) {
            s = std::make_unique<GpuSurface>();
            s->source = source;
            s->material.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(MaterialUniforms)));
            s->material->create();
        }
        if (!s->vertices || s->version != source->version) {
            const SurfaceVertices v = buildSurfaceVertices(*source);
            s->vertices.reset();
            s->indices.reset();
            s->indexCount = 0;
            if (!v.indices.empty()) {
                s->vertices.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, static_cast<quint32>(v.data.size() * sizeof(float))));
                s->indices.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, static_cast<quint32>(v.indices.size() * sizeof(std::uint32_t))));
                if (s->vertices->create() && s->indices->create()) {
                    batch->uploadStaticBuffer(s->vertices.get(), v.data.data());
                    batch->uploadStaticBuffer(s->indices.get(), v.indices.data());
                    s->indexCount = static_cast<std::uint32_t>(v.indices.size());
                } else {
                    s->vertices.reset();
                    s->indices.reset();
                }
            }
            s->version = source->version;
        }
        const SurfaceMaterial& m = source->material;
        const std::string key = m.albedoMap + "|" + m.normalMap + "|" + m.roughnessMap + "|" + m.metallicMap + "|" + m.occlusionMap + "|" + m.specularMap;
        if (key != s->textureKey || !s->lookdevSrb) {
            s->albedo = textureFor(rhi, batch, m.albedoMap, true);
            s->normal = textureFor(rhi, batch, m.normalMap, false);
            s->mask = maskTextureFor(rhi, batch, m);
            s->textureBits = (s->albedo ? 1 : 0) + (s->normal ? 2 : 0) +
                             (s->mask ? (m.roughnessMap.empty() ? 0 : 4) + (m.metallicMap.empty() ? 0 : 8) +
                                            (m.occlusionMap.empty() ? 0 : 16) + (m.specularMap.empty() ? 0 : 32)
                                      : 0);
            s->textureKey = key;
            s->lookdevSrb.reset();
        }
        MaterialUniforms mu;
        fillMaterial(mu, *source);
        mu.flags[1] = static_cast<float>(s->textureBits + (m.detailScale > 0.0f ? 64 : 0));
        if (s->albedo) mu.cutout[1] = static_cast<float>(std::max(s->albedo->pixelSize().width(), s->albedo->pixelSize().height()));
        batch->updateDynamicBuffer(s->material.get(), 0, sizeof mu, &mu);
        kept.push_back(std::move(s));
    }
    g.surfaces = std::move(kept);
    if (surfacesDirty_) {
        surfacesDirty_ = false;
        updateBounds();
    }
    // The blur's kernel for the skin's mean free paths (the first skin surface's: every
    // skin surface in a scene shares the profile's shape, each with its own radius scale).
    if (const Surface* skin = screenSpaceSkin()) {
        RGB mfp = skin->material.sssMeanFreePath;
        for (float& v : mfp) v = std::max(v, 1e-4f);
        if (mfp != kernelMfp_ || kernel_.empty()) {
            kernelMfp_ = mfp;
            kernel_ = separableSssKernel(kSssTaps, mfp, kSssKernelReach * std::max({mfp[0], mfp[1], mfp[2]}));
        }
    }
}

void LookdevRenderer::fillMaterial(MaterialUniforms& u, const Surface& s) const {
    std::memset(&u, 0, sizeof u);
    const SurfaceMaterial& m = s.material;
    put4(u.base, m.baseColor[0], m.baseColor[1], m.baseColor[2], std::clamp(m.roughness, 0.02f, 1.0f));
    put4(u.params, m.metallic, m.specular, m.occlusion, m.normalStrength);
    const float kind = m.kind == SurfaceKind::Default ? 0.0f : m.kind == SurfaceKind::Skin ? 1.0f : 2.0f;
    put4(u.flags, kind, 0.0f, m.detailScale, m.detailStrength);
    put4(u.skin, m.sssRadius, std::clamp(m.roughness2, 0.02f, 1.0f), std::clamp(m.lobe2Weight, 0.0f, 1.0f), m.cavity);
    put4(u.skin2, m.transmission, std::clamp(m.sssStrength, 0.0f, 1.0f), m.specularMapGain, 0.0f);
    put4(u.eyeCenter, s.eyeCenter.x, s.eyeCenter.y, s.eyeCenter.z, s.eyeRadius);
    put4(u.eyeForward, s.eyeForward.x, s.eyeForward.y, s.eyeForward.z, m.irisRadius);
    put4(u.eyeUp, s.eyeUp.x, s.eyeUp.y, s.eyeUp.z, m.pupilRadius);
    put4(u.eyeParams, m.corneaIor, m.limbalWidth, m.tearLine, m.eyeOcclusion);
    put4(u.irisColor, m.irisColor[0], m.irisColor[1], m.irisColor[2], m.irisDetail);
    put4(u.scleraColor, m.scleraColor[0], m.scleraColor[1], m.scleraColor[2], m.veins);
    put4(u.eyeParams2, m.wetness, 0.0f, 0.0f, 0.0f);
    put4(u.skin3, m.sssMode == SurfaceMaterial::SssMode::ScreenSpace ? 1.0f : 0.0f, m.sssMeanFreePath[0], m.sssMeanFreePath[1],
         m.sssMeanFreePath[2]);
    put4(u.cutout, std::max(m.alphaCutoff, 0.0f), 0.0f, 0.0f, 0.0f);  // the map's width: uploadSurfaces
}

void LookdevRenderer::lightPass(PassUniforms& u, const std::array<float, 3>& toward, int which, int size, const Bounds& b, QRhi* rhi) const {
    // An orthographic view from the light over the bounds' sphere.
    QVector3D center(0.0f, 0.0f, 0.0f);
    float radius = 1.0f;
    if (b.valid) {
        center = QVector3D((b.lo.x + b.hi.x) * 0.5f, (b.lo.y + b.hi.y) * 0.5f, (b.lo.z + b.hi.z) * 0.5f);
        radius = std::max(QVector3D(b.hi.x - b.lo.x, b.hi.y - b.lo.y, b.hi.z - b.lo.z).length() * 0.5f, 1e-3f) * 1.05f;
    }
    const QVector3D dir = QVector3D(toward[0], toward[1], toward[2]).normalized();
    const QVector3D eye = center + dir * (2.0f * radius);
    const QVector3D up = std::abs(dir.y()) > 0.9f ? QVector3D(1.0f, 0.0f, 0.0f) : QVector3D(0.0f, 1.0f, 0.0f);
    QMatrix4x4 view;
    view.lookAt(eye, center, up);
    QMatrix4x4 proj = rhi->clipSpaceCorrMatrix();
    proj.ortho(-radius, radius, -radius, radius, radius, 3.0f * radius);
    const QMatrix4x4 vp = proj * view;
    putMatrix(u.viewProj, vp);
    putMatrix(u.invViewProj, vp.inverted());
    put4(u.view, dir.x(), dir.y(), dir.z(), 1.0f);
    put4(u.pixel, 2.0f * radius / static_cast<float>(size), 1.0f, material_.widthScale, static_cast<float>(which));
    const QVector3D forward = -dir;
    put4(u.axis, forward.x(), forward.y(), forward.z(), QVector3D::dotProduct(eye, forward) + radius);
    const float thickness = 0.02f * radius * std::max(lighting_.shadowSoftness, 0.05f);
    put4(u.dom, thickness, 2.0f * radius, kScalpHairCount, gpu_->flipY ? 1.0f : 0.0f);
}

void LookdevRenderer::fillFrame(FrameUniforms& u, const OrbitCamera& camera, int frame, bool refine) const {
    std::memset(&u, 0, sizeof u);
    const HairMaterial& m = material_;
    const LightingSetup& l = lighting_;
    const QVector3D eye = camera.eye();
    put4(u.eye, eye.x(), eye.y(), eye.z(), static_cast<float>(frame));
    const float pigment = m.pigment == HairMaterial::Pigment::GroomMelanin ? 0.0f : m.pigment == HairMaterial::Pigment::GroomColor ? 1.0f : 2.0f;
    put4(u.mat0, m.melanin, m.melaninRedness, m.melaninRandomize, pigment);
    put4(u.dye, m.baseColor[0], m.baseColor[1], m.baseColor[2], hairLongitudinalWidth(m.roughness));
    put4(u.mat1, hairAzimuthalSpread(m.azimuthalRoughness), m.ior, m.shift * kPi / 180.0f, m.multipleScattering);
    // Wet hair where the strands carry Wet's wetness (the points' colors pack it): core's numbers.
    const std::array<float, 4> wet = wetShadingUniform(m.roughness);
    put4(u.wet, wet[0], wet[1], wet[2], wet[3]);
    put4(u.rgbLook, m.azimuthalRoughness, 0.0f, 0.0f, 0.0f);
    const bool stochastic = (refine && frame > 0) || qEnvironmentVariableIsSet("GROOMLAB_APP_STOCHASTIC");
    const bool shadows = std::any_of(l.lights.begin(), l.lights.end(), [](const DirectionalLight& d) { return d.enabled && d.shadows; });
    const float flags = (stochastic ? 1.0f : 0.0f) + (shadows ? 2.0f : 0.0f) + (l.ambientOcclusion ? 4.0f : 0.0f);
    put4(u.specR, m.specular * m.specularTint[0], m.specular * m.specularTint[1], m.specular * m.specularTint[2], m.diffuse);
    put4(u.specTRT, m.specular2 * m.specular2Tint[0], m.specular2 * m.specular2Tint[1], m.specular2 * m.specular2Tint[2], flags);
    put4(u.specTT, m.transmission * m.transmissionTint[0], m.transmission * m.transmissionTint[1],
         m.transmission * m.transmissionTint[2], 1.0f);
    const std::vector<DirectionalLight> lights = activeLights();
    for (std::size_t slot = 0; slot < lights.size(); ++slot) {
        const DirectionalLight& d = lights[slot];
        put4(u.lightDir[slot], d.direction[0], d.direction[1], d.direction[2], d.shadows ? 1.0f : 0.0f);
        put4(u.lightColor[slot], d.color[0] * d.intensity, d.color[1] * d.intensity, d.color[2] * d.intensity, 1.0f);
    }
    for (int i = 0; i < 9; ++i) put4(u.sh[i], sh_[i][0], sh_[i][1], sh_[i][2], 0.0f);
    const int envCount = refine && frame > 0 ? 8 : kInteractiveEnvLights;
    // Few lights stand for the environment while interacting: each carries its share of the
    // rest, so the brightness holds; refining takes them all, jittered inside their regions.
    float totalLuminance = 0.0f, usedLuminance = 0.0f;
    for (std::size_t i = 0; i < envLights_.size(); ++i) {
        totalLuminance += envLights_[i].intensity;
        if (static_cast<int>(i) < envCount) usedLuminance += envLights_[i].intensity;
    }
    const float share = usedLuminance > 0.0f ? totalLuminance / usedLuminance : 1.0f;
    for (int i = 0; i < 8 && i < static_cast<int>(envLights_.size()) && i < envCount; ++i) {
        const DirectionalLight& d = envLights_[i];
        std::array<float, 3> dir = rotateY(d.direction, l.environmentRotation);
        if (stochastic) {
            // A random tilt within the light's region (about a sixth of the sphere each).
            const std::uint64_t seed = hash64(static_cast<std::uint64_t>(frame) * 8 + i);
            const float a = (unitFloat(seed) - 0.5f) * 2.0f * 0.35f, bAngle = (unitFloat(hash64(seed ^ 0x55)) - 0.5f) * 2.0f * 0.35f;
            const QVector3D v(dir[0], dir[1], dir[2]);
            const QVector3D side = QVector3D::crossProduct(v, std::abs(v.y()) > 0.9f ? QVector3D(1, 0, 0) : QVector3D(0, 1, 0)).normalized();
            const QVector3D upv = QVector3D::crossProduct(side, v).normalized();
            const QVector3D j = (v + side * std::tan(a) + upv * std::tan(bAngle)).normalized();
            dir = {j.x(), j.y(), j.z()};
        }
        put4(u.envDir[i], dir[0], dir[1], dir[2], 1.0f);
        put4(u.envColor[i], d.color[0] * d.intensity * share, d.color[1] * d.intensity * share, d.color[2] * d.intensity * share, 0.0f);
    }
    // The structural color (FeatherColor's iridescence; how much is each point's own): its color, and
    // how far its hue turns as the view runs along a fiber (color_patterns.h's iridescentTint).
    put4(u.iridescence, m.iridescenceColor[0], m.iridescenceColor[1], m.iridescenceColor[2], m.iridescenceShift);
    const float background = l.background == Background::Solid ? 0.0f : l.background == Background::Gradient ? 1.0f : 2.0f;
    put4(u.env, l.environment == Environment::None ? 0.0f : l.environmentIntensity, l.environmentRotation * kPi / 180.0f, background,
         l.backgroundIntensity);
    put4(u.bg, l.backgroundColor[0], l.backgroundColor[1], l.backgroundColor[2], 0.0f);
    // GROOMLAB_APP_TUNE="a,b,..." overrides the hair shading's calibration constants, up to 16
    // (hair_bsdf.glsl's hairCalibration 1-3, hair_shading.glsl's tune4: the Arnold comparison's tuning
    // loop, docs/DEVELOPING.md "One hair shading"); 0 keeps the shader's.
    const QList<QByteArray> tune = qgetenv("GROOMLAB_APP_TUNE").split(',');
    for (int i = 0; i < 4; ++i) u.tune[i] = tune.size() > i ? tune[i].toFloat() : 0.0f;
    for (int i = 0; i < 4; ++i) u.tune2[i] = tune.size() > 4 + i ? tune[4 + i].toFloat() : 0.0f;
    for (int i = 0; i < 4; ++i) u.tune3[i] = tune.size() > 8 + i ? tune[8 + i].toFloat() : 0.0f;
    for (int i = 0; i < 4; ++i) u.tune4[i] = tune.size() > 12 + i ? tune[12 + i].toFloat() : 0.0f;
    // GROOMLAB_APP_DEBUG_VIEW=n shows one term of the surfaces' shading (surface_lookdev.frag).
    put4(u.debug, static_cast<float>(qEnvironmentVariableIntValue("GROOMLAB_APP_DEBUG_VIEW")), 0.0f, 0.0f, 0.0f);
}

bool LookdevRenderer::prepare(QRhi* rhi, QRhiCommandBuffer* cb, QRhiRenderTarget* target) {
    hairDraws_ = 0;
    if (!supported(rhi)) return false;
    if (!ensureResources(rhi, target)) return false;
    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
    uploadHair(rhi, batch);
    uploadEnvironment(rhi, batch);
    uploadSurfaces(rhi, batch);
    cb->resourceUpdate(batch);
    // The visibility path's objects first: the surfaces' bindings name its volumes.
    if (hairPath_ == HairPath::Visibility) ensureVis(rhi);
    else gpu_->vis.ready = false;
    return ensurePipelines(rhi, target);
}

QRhiRenderPassDescriptor* LookdevRenderer::hdrPassDescriptor() const { return gpu_->hdrPass.get(); }

int LookdevRenderer::hdrSampleCount() const { return gpu_->hdrSamples; }

QMatrix4x4 LookdevRenderer::jitter(const QSize& hostSize) const {
    QMatrix4x4 m;
    const int frame = lighting_.quality == Quality::Refine ? accumulated_ : 0;
    // A pixel of the HDR targets (the host's size scaled), once they exist.
    const QSize size = gpu_ && !gpu_->size.isEmpty() ? gpu_->size : hostSize;
    if (frame > 0 && !size.isEmpty()) {
        // Halton's sequence over the pixel, the frames since the change as its index.
        const float jx = (halton(frame, 2) - 0.5f) * 2.0f / static_cast<float>(size.width());
        const float jy = (halton(frame, 3) - 0.5f) * 2.0f / static_cast<float>(size.height());
        m.translate(jx, jy, 0.0f);
    }
    return m;
}

bool LookdevRenderer::render(QRhi* rhi, QRhiCommandBuffer* cb, QRhiRenderTarget* target, const OrbitCamera& camera,
                             const QMatrix4x4& viewProj, const std::function<void(QRhiCommandBuffer*)>& depthItems) {
    Gpu& g = *gpu_;
    surfaceDraws_ = 0;
    if (!g.tonemap || !g.surfaceLookdev || g.size.isEmpty()) return false;
    (void)target;
    const bool refine = lighting_.quality == Quality::Refine;
    const int frame = refine ? accumulated_ : 0;
    const QVector3D eye = camera.eye();
    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
    const bool stochasticFrame = (refine && frame > 0) || qEnvironmentVariableIsSet("GROOMLAB_APP_STOCHASTIC");

    // Adaptive interactive quality: the last frame measured by this one's start when they
    // came back to back; a stream (this frame and the last both after a change, close
    // together, or the host's drag) draws at the stream's scale into the targets' corner.
    if (!clock_.isValid()) clock_.start();
    const qint64 now = clock_.nsecsElapsed();
    const double sinceLast = lastRenderNs_ >= 0 ? (now - lastRenderNs_) / 1e6 : kBackToBackMs;
    if (sinceLast < kBackToBackMs) noteFrameCost(sinceLast);
    lastRenderNs_ = now;
    const bool adaptive = lighting_.interactiveQuality == InteractiveQuality::Adaptive;
    const bool stream = adaptive && !stochasticFrame && (interacting_ || (changedSinceFrame_ && changedBeforeLast_ && sinceLast < kBackToBackMs));
    changedBeforeLast_ = changedSinceFrame_;
    changedSinceFrame_ = false;
    if (!stream) streamScale_ = 0.0f;
    else if (streamScale_ <= 0.0f) streamScale_ = chooseScale(g.size);
    frameScale_ = stream ? streamScale_ : 1.0f;
    reduced_ = frameScale_ < 0.999f;
    lastFrameHeavy_ = uploadedThisFrame_ || domsStale_;
    const QSize size = reduced_ ? QSize(std::max(1, static_cast<int>(std::floor(g.size.width() * frameScale_ + 0.5f))),
                                        std::max(1, static_cast<int>(std::floor(g.size.height() * frameScale_ + 0.5f))))
                                : g.size;

    // The lights in their slots (each with a map when it casts) and the sky.
    const std::vector<DirectionalLight> lights = activeLights();
    std::array<float, 3> skyDirection{0.0f, 1.0f, 0.0f};
    {
        // The environment's brightest direction, leaning up: the occlusion from above.
        const float lx = luminance(sh_[3]), ly = luminance(sh_[1]), lz = luminance(sh_[2]);
        QVector3D d(lx, ly, lz);
        d = (QVector3D(0.0f, 1.0f, 0.0f) * (d.length() + 1e-6f) + d * 0.5f).normalized();
        skyDirection = {d.x(), d.y(), d.z()};
    }
    FrameUniforms fu;
    fillFrame(fu, camera, frame, refine);
    PassUniforms pu[kPassCount];
    std::memset(pu, 0, sizeof pu);
    putMatrix(pu[0].viewProj, viewProj);
    putMatrix(pu[0].invViewProj, viewProj.inverted());
    put4(pu[0].view, eye.x(), eye.y(), eye.z(), 0.0f);
    put4(pu[0].pixel, 2.0f * std::tan(camera.fovDegrees() * kPi / 360.0f) / static_cast<float>(size.height()), 1.0f,
         material_.widthScale, static_cast<float>(kPassCamera));
    // The camera's own deep opacity map (the interactive frame's transparency): distances along
    // the view axis, layers a hundredth of the groom's size thick.
    const QVector3D forward = (camera.target() - eye).normalized();
    const Bounds& hb = hairBounds_.valid ? hairBounds_ : bounds_;
    const float radius = hb.valid ? std::max(QVector3D(hb.hi.x - hb.lo.x, hb.hi.y - hb.lo.y, hb.hi.z - hb.lo.z).length() * 0.5f, 1e-3f) : 1.0f;
    put4(pu[0].axis, forward.x(), forward.y(), forward.z(), QVector3D::dotProduct(eye, forward));
    put4(pu[0].dom, 0.01f * radius, 0.0f, kScalpHairCount, g.flipY ? 1.0f : 0.0f);
    // The hair's maps over the hair, the surfaces' shadow maps over the surfaces.
    bool anyCaster = false;
    for (const auto& s : g.surfaces) anyCaster = anyCaster || (s->indexCount > 0 && s->source->castsShadows);
    for (int i = 0; i < kDomLights; ++i) {
        const std::array<float, 3> toward = static_cast<std::size_t>(i) < lights.size() ? lights[i].direction : std::array<float, 3>{0.0f, 1.0f, 0.0f};
        lightPass(pu[kPassLight + i], toward, kPassLight + i, kDomSize, hairBounds_, rhi);
        lightPass(pu[kPassShadow + i], toward, kPassShadow + i, kShadowSize, surfaceBounds_, rhi);
    }
    lightPass(pu[kPassSky], skyDirection, kPassSky, kSkyDomSize, hairBounds_, rhi);
    for (int i = 0; i < kDomCount; ++i) {
        const PassUniforms& view = pu[i < kDomLights ? kPassLight + i : i == kDomSky ? kPassSky : kPassCamera];
        std::memcpy(fu.domViewProj[i], view.viewProj, sizeof fu.domViewProj[i]);
        std::memcpy(fu.domAxis[i], view.axis, sizeof fu.domAxis[i]);
    }
    for (int i = 0; i < kDomLights; ++i) {
        std::memcpy(fu.shadowViewProj[i], pu[kPassShadow + i].viewProj, sizeof fu.shadowViewProj[i]);
        std::memcpy(fu.shadowAxis[i], pu[kPassShadow + i].axis, sizeof fu.shadowAxis[i]);
    }
    put4(fu.domParams, pu[kPassLight].dom[0], material_.shadowDensity, pu[kPassLight].dom[0] * 0.5f, pu[kPassCamera].dom[0]);
    // x: a hair map's texel in world units (the surfaces lean their look-ups off themselves
    // by a texel or two, so a map's own quantization does not shadow them: the normal
    // offset of Holbert 2011, "Shadow Mapping: GPU-based Tips and Techniques").
    put4(fu.domParams2, pu[kPassLight].pixel[0], g.flipY ? 1.0f : 0.0f, lighting_.ambientOcclusion ? 1.0f : 0.0f, 1.0f / kDomSize);
    // The surfaces' shadow maps: their texel, the filter's radius in texels (the softness
    // widens it), their size, whether they hold anything.
    put4(fu.shadowParams, pu[kPassShadow].pixel[0], 1.5f * std::max(lighting_.shadowSoftness, 0.0f), static_cast<float>(kShadowSize),
         anyCaster ? 1.0f : 0.0f);

    // The visibility path: its blocks (the raster's two passes, the voxel grid's and each
    // volume's), the voxel fields of the Frame block, the level of detail for this view.
    const bool hairReady = g.points && g.indexCount > 0 && g.hairLookdev;
    Gpu::Vis& v = g.vis;
    const bool vis = hairReady && hairPath_ == HairPath::Visibility && v.ready && gridDims_[0] > 0;
    lastPath_ = vis ? HairPath::Visibility : HairPath::Legacy;
    // The volumes a frame needs: a light's when it casts, the sky's for the ambient occlusion.
    bool volumeWanted[kVolumeCount] = {false, false, false, lighting_.ambientOcclusion};
    std::array<float, 3> volumeToward[kVolumeCount] = {{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, skyDirection};
    for (std::size_t i = 0; i < lights.size() && i < 3; ++i) {
        volumeWanted[i] = lights[i].shadows;
        volumeToward[i] = lights[i].direction;
    }
    std::uint32_t pointGroupsX = 1, pointGroupsY = 1;
    if (vis) {
        const float vs = voxelSize_;
        put4(fu.voxelLo, gridLo_.x, gridLo_.y, gridLo_.z, 1.0f);
        static const QList<QByteArray> tuning = qgetenv("GROOMLAB_APP_VOXEL_SCALE").split(',');
        const float offset = tuning.size() > 2 && tuning[2].toFloat() > 0.0f ? tuning[2].toFloat() : kVoxelLookupOffset;
        put4(fu.voxelScale, 1.0f / (static_cast<float>(gridDims_[0]) * vs), 1.0f / (static_cast<float>(gridDims_[1]) * vs),
             1.0f / (static_cast<float>(gridDims_[2]) * vs), offset * vs);
        put4(fu.voxelParams, kVoxelCountScale, skyDirection[0], skyDirection[1], skyDirection[2]);
        selectLod(rhi, batch, camera, size);
        // A thread a point (its segment), in rows of kGroupsPerRow groups.
        const std::uint32_t groups = static_cast<std::uint32_t>((pointCount_ + 255) / 256);
        pointGroupsX = std::max<std::uint32_t>(std::min(groups, kGroupsPerRow), 1);
        pointGroupsY = std::max<std::uint32_t>((groups + pointGroupsX - 1) / pointGroupsX, 1);
        VisUniforms vu;
        std::memset(&vu, 0, sizeof vu);
        putMatrix(vu.viewProj, viewProj);
        put4(vu.eyeFrame, eye.x(), eye.y(), eye.z(), stochasticFrame ? static_cast<float>(frame) : 0.0f);
        put4(vu.target, static_cast<float>(size.width()), static_cast<float>(size.height()), static_cast<float>(g.size.width()),
             static_cast<float>(g.size.height()));
        put4(vu.params, 1.0f / std::max(pu[kPassCamera].pixel[0], 1e-12f), material_.widthScale, fu.specTT[3],
             rhi->isYUpInFramebuffer() ? 0.0f : 1.0f);
        vu.counts[0] = static_cast<std::uint32_t>(pointCount_);
        vu.counts[1] = pointGroupsX * 256u;
        vu.counts[2] = static_cast<std::uint32_t>(strandCount_);
        for (int pass = 0; pass < 2; ++pass) {
            put4(vu.params2, lodActive_ ? 1.0f : 0.0f, g.surfaces.empty() ? 0.0f : 1.0f, rhi->isYUpInNDC() ? 1.0f : 0.0f, static_cast<float>(pass));
            batch->updateDynamicBuffer(v.uniforms[pass].get(), 0, sizeof vu, &vu);
        }
        FillUniforms fill{};
        const std::uint32_t pixels = static_cast<std::uint32_t>(g.size.width() * g.size.height());
        fill.fill[0] = pixels;
        fill.fill[1] = std::min((pixels + 255) / 256, kGroupsPerRow) * 256u;
        fill.values[0] = 0xFFFFFFFFu;
        fill.values[1] = 0u;
        batch->updateDynamicBuffer(v.fillPixels.get(), 0, sizeof fill, &fill);
        if (volumesStale_) {
            const std::uint32_t quads = static_cast<std::uint32_t>(
                (static_cast<std::size_t>(gridDims_[0]) * gridDims_[1] * gridDims_[2] + 3) / 4);
            FillUniforms zero{};
            zero.fill[0] = quads;
            zero.fill[1] = std::min((quads + 255) / 256, kGroupsPerRow) * 256u;
            batch->updateDynamicBuffer(v.fillDensity.get(), 0, sizeof zero, &zero);
            GridUniforms gu;
            std::memset(&gu, 0, sizeof gu);
            put4(gu.lo, gridLo_.x, gridLo_.y, gridLo_.z, voxelSize_);
            // GROOMLAB_APP_VOXEL_SCALE="lights,sky" overrides the count scales (the Arnold
            // comparison's tuning loop).
            static const QList<QByteArray> scales = qgetenv("GROOMLAB_APP_VOXEL_SCALE").split(',');
            const float lightScale = scales.size() > 0 && scales[0].toFloat() > 0.0f ? scales[0].toFloat() : kVoxelCountScale;
            const float skyScale = scales.size() > 1 && scales[1].toFloat() > 0.0f ? scales[1].toFloat() : kVoxelSkyCountScale;
            put4(gu.params, material_.widthScale, fu.specTT[3], kDensityFixedPoint, lightScale);
            for (int k = 0; k < 3; ++k) gu.dims[k] = static_cast<std::uint32_t>(gridDims_[k]);
            gu.counts[0] = static_cast<std::uint32_t>(pointCount_);
            gu.counts[1] = pointGroupsX * 256u;
            batch->updateDynamicBuffer(v.grid[0].get(), 0, sizeof gu, &gu);
            for (int k = 0; k < kVolumeCount; ++k) {
                gu.params[3] = k == kVolumeSky ? skyScale : lightScale;
                put4(gu.toward, volumeToward[k][0], volumeToward[k][1], volumeToward[k][2], 0.0f);
                batch->updateDynamicBuffer(v.grid[1 + k].get(), 0, sizeof gu, &gu);
            }
        }
    }
    batch->updateDynamicBuffer(g.frame.get(), 0, sizeof fu, &fu);
    // The interactive frame's two hair draws see the camera's block with their pass index; the
    // per-point shading pass the same with the point count.
    pu[kPassThick] = pu[kPassCamera];
    pu[kPassThick].pixel[3] = static_cast<float>(kPassThick);
    pu[kPassThin] = pu[kPassCamera];
    pu[kPassThin].pixel[3] = static_cast<float>(kPassThin);
    pu[kPassShade] = pu[kPassCamera];
    pu[kPassShade].pixel[3] = static_cast<float>(kPassShade);
    pu[kPassShade].dom[1] = static_cast<float>(pointCount_);
    for (int i = 0; i < kPassCount; ++i) batch->updateDynamicBuffer(g.pass[i].get(), 0, sizeof pu[i], &pu[i]);
    BlitUniforms accumulateU{{1.0f / static_cast<float>(frame + 1), 0.0f, 0.0f, g.flipY ? 1.0f : 0.0f}};
    batch->updateDynamicBuffer(g.blitAccumulate.get(), 0, sizeof accumulateU, &accumulateU);
    const float toneMap = lighting_.toneMap == ToneMap::None ? 0.0f : lighting_.toneMap == ToneMap::ACES ? 1.0f : 2.0f;
    // z: a reduced frame's scale (tonemap.frag works the corner's size out as `size` is, from
    // the targets'; 0: the whole target).
    BlitUniforms tonemapU{{std::pow(2.0f, lighting_.exposure), toneMap, reduced_ ? frameScale_ : 0.0f, g.flipY ? 1.0f : 0.0f}};
    batch->updateDynamicBuffer(g.blitTonemap.get(), 0, sizeof tonemapU, &tonemapU);

    const bool hair = g.points && g.indexCount > 0 && g.hairLookdev;
    // Each pass's viewport, set again after every pipeline bound in it (see passViewport()).
    auto viewport = [&](const QSize& s) {
        passViewport_ = QRhiViewport(0, 0, static_cast<float>(s.width()), static_cast<float>(s.height()));
        cb->setViewport(passViewport_);
    };
    auto bind = [&](QRhiGraphicsPipeline* ps) {
        cb->setGraphicsPipeline(ps);
        cb->setViewport(passViewport_);
    };
    auto drawHair = [&](QRhiGraphicsPipeline* ps, QRhiShaderResourceBindings* srb) {
        bind(ps);
        cb->setShaderResources(srb);
        cb->setVertexInput(0, 0, nullptr, g.useStrip ? g.strip.get() : g.list.get(), 0, QRhiCommandBuffer::IndexUInt32);
        cb->drawIndexed(g.indexCount);
        ++hairDraws_;
    };
    // Every surface with one pipeline: `which` picks the lookdev bindings (-1, the prepass
    // shares them), a hair map's, or a shadow map's (kPassShadow + i).
    auto drawSurfaces = [&](QRhiGraphicsPipeline* ps, int which, bool castersOnly) {
        for (const auto& s : g.surfaces) {
            if (s->indexCount == 0 || !s->vertices || !s->lookdevSrb) continue;
            if (castersOnly && !s->source->castsShadows) continue;
            bind(ps);
            cb->setShaderResources(which < 0 ? s->lookdevSrb.get() : which >= kPassShadow ? s->shadowSrb[which - kPassShadow].get()
                                   : ps == g.surfaceDepth.get() ? s->depthSrb[which].get() : s->domSrb[which].get());
            const QRhiCommandBuffer::VertexInput input(s->vertices.get(), 0);
            cb->setVertexInput(0, 1, &input, s->indices.get(), 0, QRhiCommandBuffer::IndexUInt32);
            cb->drawIndexed(s->indexCount);
            ++surfaceDraws_;
        }
    };
    auto fullscreen = [&](QRhiGraphicsPipeline* ps, QRhiShaderResourceBindings* srb) {
        bind(ps);
        cb->setShaderResources(srb);
        cb->draw(3);
    };

    // The deep opacity maps: the key light's when it casts, the sky's for ambient occlusion.
    // They depend on the hair, the lights and the material alone, not on the camera: drawn
    // again only when one of those changed (a camera move, or the accumulated frames, keep them).
    // The camera's map is the interactive frame's (a stochastic frame needs none) and follows
    // the camera: drawn whenever the frame is.
    QRhiResourceUpdateBatch* pending = batch;
    for (int i = 0; i < kDomCount; ++i) {
        if (i != kDomCamera && !domsStale_) continue;
        if (i == kDomCamera && (stochasticFrame || vis)) continue;
        // (The visibility path's shadows are the voxel volumes': its maps stay clear.)
        const bool wanted = !vis && (i < kDomLights ? (static_cast<std::size_t>(i) < lights.size() && lights[i].shadows)
                                     : i == kDomSky ? lighting_.ambientOcclusion : true);
        // The hair's maps hold the hair alone: the surfaces shadow through their own maps
        // (below), which the hair reads too.
        if (!wanted || !hair) {
            // Left clear: nothing in the way. The distance map is cleared too, so a surface
            // reading its thickness finds nothing in front of it.
            cb->beginPass(g.dom[i].depthTarget.get(), Qt::black, {1.0f, 0}, pending);
            pending = nullptr;
            cb->endPass();
            cb->beginPass(g.dom[i].layersTarget.get(), Qt::transparent, {1.0f, 0});
            cb->endPass();
            continue;
        }
        const QSize domSize(g.dom[i].size, g.dom[i].size);
        cb->beginPass(g.dom[i].depthTarget.get(), Qt::black, {1.0f, 0}, pending);
        pending = nullptr;
        viewport(domSize);
        drawHair(g.hairDepth.get(), g.hairDepthSrb[i].get());
        cb->endPass();
        cb->beginPass(g.dom[i].layersTarget.get(), Qt::transparent, {1.0f, 0});
        viewport(domSize);
        drawHair(g.hairDom.get(), g.hairDomSrb[i].get());
        cb->endPass();
    }
    // The surfaces' shadow maps: the casters' distance from each light that casts (cleared
    // to nothing otherwise), drawn again with the hair's maps.
    if (domsStale_) {
        for (int i = 0; i < kDomLights; ++i) {
            const bool wanted = static_cast<std::size_t>(i) < lights.size() && lights[i].shadows && anyCaster;
            cb->beginPass(g.shadow[i].target.get(), Qt::black, {1.0f, 0}, pending);
            pending = nullptr;
            if (wanted) {
                viewport(QSize(kShadowSize, kShadowSize));
                drawSurfaces(g.surfaceDepth.get(), kPassShadow + i, true);
            }
            cb->endPass();
        }
    }
    domsStale_ = false;

    // The skin's screen-space scattering: the skin's irradiance and depth, then the blur
    // along x and along y (Jimenez 2015), for the HDR pass to read back.
    if (screenSpaceSkin() && !kernel_.empty()) {
        SssUniforms su[2];
        std::memset(su, 0, sizeof su);
        const int taps = std::min(static_cast<int>(kernel_.size()), kSssKernelMax);
        for (int axis = 0; axis < 2; ++axis) {
            put4(su[axis].params, axis == 0 ? 1.0f : 0.0f, axis == 0 ? 0.0f : 1.0f, pu[kPassCamera].pixel[0],
                 stochasticFrame ? static_cast<float>(frame) : -1.0f);
            put4(su[axis].params2, static_cast<float>(taps), 0.0f, static_cast<float>(size.width()), static_cast<float>(size.height()));
            for (int k = 0; k < taps; ++k) {
                const SssKernelSample& sample = kernel_[static_cast<std::size_t>(k)];
                put4(su[axis].kernel[k], sample.offset, sample.weight[0], sample.weight[1], sample.weight[2]);
            }
        }
        QRhiResourceUpdateBatch* sssBatch = pending ? pending : rhi->nextResourceUpdateBatch();
        pending = nullptr;
        for (int axis = 0; axis < 2; ++axis) sssBatch->updateDynamicBuffer(g.sss.uniforms[axis].get(), 0, sizeof su[axis], &su[axis]);
        const BlitUniforms sssBlit{{0.0f, 0.0f, 0.0f, g.flipY ? 1.0f : 0.0f}};
        sssBatch->updateDynamicBuffer(g.sss.blit.get(), 0, sizeof sssBlit, &sssBlit);
        cb->beginPass(g.sss.prepassTarget.get(), Qt::transparent, {1.0f, 0}, sssBatch);
        viewport(size);
        drawSurfaces(g.surfaceSss.get(), -1, false);
        cb->endPass();
        for (int axis = 0; axis < 2; ++axis) {
            cb->beginPass(axis == 0 ? g.sss.tempTarget.get() : g.sss.blurredTarget.get(), Qt::transparent, {1.0f, 0});
            viewport(size);
            fullscreen(g.sssBlur.get(), g.sss.srb[axis].get());
            cb->endPass();
        }
    }

    // The visibility path's compute passes (each its own pass: the next reads what it wrote).
    auto computePass = [&](QRhiComputePipeline* ps, std::initializer_list<QRhiShaderResourceBindings*> srbs, std::uint32_t gx,
                           std::uint32_t gy, std::uint32_t gz) {
        cb->beginComputePass(pending);
        pending = nullptr;
        cb->setComputePipeline(ps);
        for (QRhiShaderResourceBindings* srb : srbs) {
            cb->setShaderResources(srb);
            cb->dispatch(std::max<std::uint32_t>(gx, 1), std::max<std::uint32_t>(gy, 1), std::max<std::uint32_t>(gz, 1));
        }
        cb->endComputePass();
    };
    auto rows = [](std::uint32_t threads, std::uint32_t perGroup, std::uint32_t& gx, std::uint32_t& gy) {
        const std::uint32_t groups = std::max<std::uint32_t>((threads + perGroup - 1) / perGroup, 1);
        gx = std::min(groups, kGroupsPerRow);
        gy = (groups + gx - 1) / gx;
    };
    if (vis) {
        // The voxel volumes, when the hair or the lights changed: the grid cleared, the strands'
        // density dropped in, smoothed into its texture, marched toward each light that casts.
        if (volumesStale_) {
            std::uint32_t gx = 1, gy = 1;
            rows(static_cast<std::uint32_t>((static_cast<std::size_t>(gridDims_[0]) * gridDims_[1] * gridDims_[2] + 3) / 4), 256, gx, gy);
            computePass(v.fill.get(), {v.fillDensitySrb.get()}, gx, gy, 1);
            computePass(v.voxelize.get(), {v.voxelizeSrb.get()}, pointGroupsX, pointGroupsY, 1);
            const auto d = [&](int k) { return static_cast<std::uint32_t>((gridDims_[k] + 3) / 4); };
            computePass(v.densityPs.get(), {v.densitySrb.get()}, d(0), d(1), d(2));
            cb->beginComputePass();
            cb->setComputePipeline(v.volume.get());
            for (int k = 0; k < kVolumeCount; ++k) {
                if (!volumeWanted[k]) continue;
                cb->setShaderResources(v.volumeSrb[k].get());
                cb->dispatch(d(0), d(1), d(2));
            }
            cb->endComputePass();
            hairDraws_ += 2;
            volumesStale_ = false;
        }
        // The surfaces' view depth (the hair behind them is not drawn), the per-pixel buffer
        // cleared, the strands rasterized twice (the coverage and the nearest kept fragment, then
        // its segment), the visible fragments shaded.
        cb->beginPass(v.surfaceDepthTarget.get(), Qt::transparent, {1.0f, 0}, pending);
        pending = nullptr;
        if (!g.surfaces.empty()) {
            viewport(size);
            drawSurfaces(g.surfaceDepth.get(), kDomCamera, false);
        }
        cb->endPass();
        if (v.releaseTarget) {
            // (Direct3D 11: the depth texture unbound as an output before the raster reads it.)
            cb->beginPass(v.releaseTarget.get(), Qt::transparent, {1.0f, 0});
            cb->endPass();
        }
        std::uint32_t gx = 1, gy = 1;
        rows(static_cast<std::uint32_t>(g.size.width() * g.size.height()), 256, gx, gy);
        computePass(v.fill.get(), {v.fillPixelsSrb.get()}, gx, gy, 1);
        computePass(v.raster.get(), {v.rasterSrb[0].get()}, pointGroupsX, pointGroupsY, 1);
        computePass(v.raster.get(), {v.rasterSrb[1].get()}, pointGroupsX, pointGroupsY, 1);
        computePass(v.shade.get(), {v.shadeSrb.get()}, static_cast<std::uint32_t>((size.width() + 7) / 8),
                    static_cast<std::uint32_t>((size.height() + 7) / 8), 1);
        hairDraws_ += 3;
    }

    // The interactive frame's shading, once per point (the maps are drawn by now: the
    // shading reads them), into the shaded points the ribbons fetch.
    const bool pointShaded = hair && !vis && !stochasticFrame && pointShading_ && g.pointShade && g.shadedTarget;
    if (pointShaded) {
        const QSize shadedSize = g.shaded->pixelSize();
        cb->beginPass(g.shadedTarget.get(), Qt::black, {1.0f, 0}, pending);
        pending = nullptr;
        viewport(shadedSize);
        fullscreen(g.pointShade.get(), g.pointShadeSrb.get());
        cb->endPass();
    }

    // The scene in linear HDR.
    cb->beginPass(g.hdrTarget.get(), Qt::black, {1.0f, 0}, pending);
    pending = nullptr;
    viewport(size);
    fullscreen(g.background.get(), g.backgroundSrb.get());
    drawSurfaces(g.surfaceLookdev.get(), -1, false);
    if (depthItems) depthItems(cb);
    if (vis) {
        // (The visibility path's hair goes over the resolved scene below.)
    } else if (hair && stochasticFrame) {
        // A stochastic frame: every strand, kept by its coverage, opaque.
        drawHair(g.hairLookdev.get(), g.hairLookdevSrb.get());
    } else if (pointShaded) {
        // The interactive frame: the opaque strands with their depth, then the thin ones over
        // them, each lit from its points.
        drawHair(g.hairGouraud.get(), g.hairGouraudThickSrb.get());
        drawHair(g.hairGouraudThin.get(), g.hairGouraudThinSrb.get());
    } else if (hair) {
        // The same, shaded per fragment (GROOMLAB_APP_HAIR_SHADING=fragment).
        drawHair(g.hairLookdev.get(), g.hairThickSrb.get());
        drawHair(g.hairThin.get(), g.hairThinSrb.get());
    }
    cb->endPass();
    if (vis) {
        // The shaded hair over the resolved scene (premultiplied by its alpha).
        cb->beginPass(v.compositeTarget.get(), Qt::black, {1.0f, 0});
        viewport(size);
        fullscreen(v.composite.get(), v.compositeSrb.get());
        cb->endPass();
    }

    // The running mean (a reduced frame is not kept: the full one starts the mean), then the
    // display transform into the host's target.
    int source = 0;  // the tonemap reads: 0 resolved, 1 + i accum[i]
    if (refine && !reduced_) {
        const int next = 1 - g.accumCurrent;
        cb->beginPass(g.accumTarget[next].get(), Qt::black, {1.0f, 0});
        viewport(size);
        fullscreen(g.accumulate.get(), g.accumulateSrb[next].get());
        cb->endPass();
        g.accumCurrent = next;
        source = 1 + next;
        accumulated_ = std::min(accumulated_ + 1, kMaxAccumulatedFrames);
    }
    g.tonemapSource = source;
    return true;
}

// ---- The visibility path ---------------------------------------------------------------------

bool LookdevRenderer::visSupported(QRhi* rhi) const {
    return rhi && rhi->isFeatureSupported(QRhi::Compute) && rhi->isFeatureSupported(QRhi::ThreeDimensionalTextures) &&
           rhi->isTextureFormatSupported(QRhiTexture::RGBA16F, QRhiTexture::UsedWithLoadStore) &&
           rhi->isTextureFormatSupported(QRhiTexture::R32F, QRhiTexture::ThreeDimensional | QRhiTexture::UsedWithLoadStore);
}

void LookdevRenderer::fitVoxelGrid() {
    // Cubic voxels, kVoxelResolution along the hair's longest side (less the two-voxel margin each
    // way, so a strand at the bounds and the tent around it stay inside).
    const Bounds& b = hairBounds_;
    if (!b.valid) {
        gridDims_ = {0, 0, 0};
        return;
    }
    const Vec3f extent = b.hi - b.lo;
    const float longest = std::max({extent.x, extent.y, extent.z, 1e-4f});
    voxelSize_ = longest / static_cast<float>(kVoxelResolution - 4);
    gridLo_ = b.lo - Vec3f{2.0f, 2.0f, 2.0f} * voxelSize_;
    const float e[3] = {extent.x, extent.y, extent.z};
    for (int k = 0; k < 3; ++k) gridDims_[k] = std::clamp(static_cast<int>(std::ceil(e[k] / voxelSize_)) + 4, 4, kVoxelResolution);
}

bool LookdevRenderer::ensureVis(QRhi* rhi) {
    Gpu& g = *gpu_;
    Gpu::Vis& v = g.vis;
    v.ready = false;
    if (!visSupported(rhi) || !g.points || !g.strands || !g.pointColors || !g.resolved || g.size.isEmpty()) return false;
    auto fail = [](int line) {
        qWarning("GroomLab: the visibility path could not be made (lookdev_renderer.cpp:%d)", line);
        return false;
    };
    using B = QRhiShaderResourceBinding;
    const B::StageFlags cs = B::ComputeStage;
    auto buffer = [&](std::unique_ptr<QRhiBuffer>& b, QRhiBuffer::Type type, QRhiBuffer::UsageFlags usage, quint32 bytes) {
        if (b && b->size() >= bytes) return true;
        b.reset(rhi->newBuffer(type, usage, bytes));
        return b->create();
    };
    if (!v.clamp3d) {
        v.clamp3d.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        v.clamp3d->create();
        for (auto& u : v.uniforms) {
            if (!buffer(u, QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(VisUniforms))) return fail(__LINE__);
        }
        for (auto& u : v.grid) {
            if (!buffer(u, QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(GridUniforms))) return fail(__LINE__);
        }
        if (!buffer(v.fillPixels, QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(FillUniforms)) ||
            !buffer(v.fillDensity, QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(FillUniforms))) {
            return fail(__LINE__);
        }
    }
    // The per-pixel buffer, the hair image and the surfaces' depth at the HDR targets' size.
    if (v.size != g.size || !v.pixels) {
        v.pixels.reset();
        const auto bytes = static_cast<quint32>(static_cast<std::size_t>(g.size.width()) * g.size.height() * 16);
        if (!buffer(v.pixels, QRhiBuffer::Static, QRhiBuffer::StorageBuffer, bytes)) return fail(__LINE__);
        v.hairImage.reset(rhi->newTexture(QRhiTexture::RGBA16F, g.size, 1, QRhiTexture::UsedWithLoadStore));
        if (!v.hairImage->create()) return fail(__LINE__);
        v.surfaceDepth.reset(rhi->newTexture(QRhiTexture::R32F, g.size, 1, QRhiTexture::RenderTarget));
        v.surfaceDepthBuffer.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, g.size, 1));
        if (!v.surfaceDepth->create() || !v.surfaceDepthBuffer->create()) return fail(__LINE__);
        QRhiTextureRenderTargetDescription depthDesc{QRhiColorAttachment(v.surfaceDepth.get())};
        depthDesc.setDepthStencilBuffer(v.surfaceDepthBuffer.get());
        v.surfaceDepthTarget.reset(rhi->newTextureRenderTarget(depthDesc));
        if (!v.surfaceDepthPass) v.surfaceDepthPass.reset(v.surfaceDepthTarget->newCompatibleRenderPassDescriptor());
        v.surfaceDepthTarget->setRenderPassDescriptor(v.surfaceDepthPass.get());
        if (!v.surfaceDepthTarget->create()) return fail(__LINE__);
        v.size = g.size;
    }
    // Direct3D 11 keeps a pass's targets bound as outputs after it ends, until the next pass
    // binds its own, and a texture bound as an output reads as nothing in a compute shader (the
    // device forces its view to null): the raster would find no surface in front of any strand.
    // The other backends make the texture readable themselves (their barriers).
    if (rhi->backend() == QRhi::D3D11 && !v.releaseTarget) {
        v.releaseTexture.reset(rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1), 1, QRhiTexture::RenderTarget));
        if (!v.releaseTexture->create()) return fail(__LINE__);
        v.releaseTarget.reset(rhi->newTextureRenderTarget({QRhiColorAttachment(v.releaseTexture.get())}));
        v.releasePass.reset(v.releaseTarget->newCompatibleRenderPassDescriptor());
        v.releaseTarget->setRenderPassDescriptor(v.releasePass.get());
        if (!v.releaseTarget->create()) return fail(__LINE__);
    }
    // The composite pass draws over the resolved HDR scene, keeping it.
    if (!v.compositeTarget || v.compositeOn != resourceId(g.resolved.get())) {
        v.compositeTarget.reset(rhi->newTextureRenderTarget({QRhiColorAttachment(g.resolved.get())},
                                                            QRhiTextureRenderTarget::PreserveColorContents));
        if (!v.compositePass) v.compositePass.reset(v.compositeTarget->newCompatibleRenderPassDescriptor());
        v.compositeTarget->setRenderPassDescriptor(v.compositePass.get());
        if (!v.compositeTarget->create()) return fail(__LINE__);
        v.compositeOn = resourceId(g.resolved.get());
    }
    // The level of detail's exponents, one a strand (a single one stands in until selected).
    if (!v.lod || v.lodStrands < std::max<std::size_t>(strandCount_, 1)) {
        v.lod.reset();
        v.lodStrands = std::max<std::size_t>(strandCount_, 1);
        if (!buffer(v.lod, QRhiBuffer::Static, QRhiBuffer::StorageBuffer, static_cast<quint32>(v.lodStrands * sizeof(float)))) return fail(__LINE__);
        lodKey_ = 0;
    }
    // The voxel grid's density and its textures, at the grid's size.
    fitVoxelGrid();
    if (gridDims_[0] <= 0) return false;
    if (v.dims != gridDims_ || !v.densityTexture) {
        const std::size_t voxels = static_cast<std::size_t>(gridDims_[0]) * gridDims_[1] * gridDims_[2];
        v.density.reset();
        if (!buffer(v.density, QRhiBuffer::Static, QRhiBuffer::StorageBuffer, static_cast<quint32>((voxels + 3) / 4 * 16))) return fail(__LINE__);
        auto volume = [&](std::unique_ptr<QRhiTexture>& t) {
            t.reset(rhi->newTexture(QRhiTexture::R32F, gridDims_[0], gridDims_[1], gridDims_[2], 1,
                                    QRhiTexture::ThreeDimensional | QRhiTexture::UsedWithLoadStore));
            return t->create();
        };
        if (!volume(v.densityTexture)) return fail(__LINE__);
        for (auto& t : v.volumes) {
            if (!volume(t)) return fail(__LINE__);
        }
        v.dims = gridDims_;
        volumesStale_ = true;
        // The surfaces read the volumes too (their hair shadows): their bindings are made again.
        for (auto& s : g.surfaces) s->lookdevSrb.reset();
    }
    // The bindings, made again when an object they name was.
    const std::array<quint64, Gpu::Vis::kBound> bound{resourceId(g.points.get()),
                                                      resourceId(g.strands.get()),
                                                      resourceId(g.pointColors.get()),
                                                      resourceId(g.frame.get()),
                                                      resourceId(g.blitTonemap.get()),
                                                      resourceId(v.pixels.get()),
                                                      resourceId(v.lod.get()),
                                                      resourceId(v.density.get()),
                                                      resourceId(v.hairImage.get()),
                                                      resourceId(v.surfaceDepth.get()),
                                                      resourceId(v.densityTexture.get()),
                                                      resourceId(v.volumes[0].get()),
                                                      resourceId(v.volumes[1].get()),
                                                      resourceId(v.volumes[2].get()),
                                                      resourceId(v.volumes[3].get()),
                                                      resourceId(g.shadow[0].distance.get()),
                                                      resourceId(g.shadow[1].distance.get()),
                                                      resourceId(g.shadow[2].distance.get())};
    if (bound != v.bound || !v.rasterSrb[0] || v.volumeSrb[0] == nullptr || v.shadeSrb == nullptr) {
        auto srb = [&](std::initializer_list<B> bindings) {
            std::unique_ptr<QRhiShaderResourceBindings> s(rhi->newShaderResourceBindings());
            s->setBindings(bindings);
            s->create();
            return s;
        };
        const B pointsB = B::sampledTexture(2, cs, g.points.get(), g.nearest.get());
        const B strandsB = B::sampledTexture(3, cs, g.strands.get(), g.nearest.get());
        v.fillPixelsSrb = srb({B::uniformBuffer(1, cs, v.fillPixels.get()), B::bufferLoadStore(4, cs, v.pixels.get())});
        v.fillDensitySrb = srb({B::uniformBuffer(1, cs, v.fillDensity.get()), B::bufferLoadStore(4, cs, v.density.get())});
        for (int k = 0; k < 2; ++k) {
            v.rasterSrb[k] = srb({B::uniformBuffer(1, cs, v.uniforms[k].get()), pointsB, strandsB, B::bufferLoadStore(4, cs, v.pixels.get()),
                                  B::bufferLoadStore(5, cs, v.lod.get()), B::sampledTexture(6, cs, v.surfaceDepth.get(), g.nearest.get())});
        }
        v.shadeSrb = srb({B::uniformBuffer(0, cs, g.frame.get()), B::uniformBuffer(1, cs, v.uniforms[1].get()), pointsB, strandsB,
                          B::bufferLoadStore(4, cs, v.pixels.get()), B::imageStore(7, cs, v.hairImage.get(), 0),
                          B::sampledTexture(9, cs, g.pointColors.get(), g.nearest.get()),
                          B::sampledTexture(25, cs, g.shadow[0].distance.get(), g.nearest.get()),
                          B::sampledTexture(26, cs, g.shadow[1].distance.get(), g.nearest.get()),
                          B::sampledTexture(27, cs, g.shadow[2].distance.get(), g.nearest.get()),
                          B::sampledTexture(34, cs, v.volumes[0].get(), v.clamp3d.get()), B::sampledTexture(35, cs, v.volumes[1].get(), v.clamp3d.get()),
                          B::sampledTexture(36, cs, v.volumes[2].get(), v.clamp3d.get()), B::sampledTexture(37, cs, v.volumes[3].get(), v.clamp3d.get())});
        v.voxelizeSrb = srb({B::uniformBuffer(1, cs, v.grid[0].get()), pointsB, strandsB, B::bufferLoadStore(4, cs, v.density.get())});
        v.densitySrb = srb({B::uniformBuffer(1, cs, v.grid[0].get()), B::bufferLoadStore(4, cs, v.density.get()),
                            B::imageStore(5, cs, v.densityTexture.get(), 0)});
        for (int k = 0; k < kVolumeCount; ++k) {
            v.volumeSrb[k] = srb({B::uniformBuffer(1, cs, v.grid[1 + k].get()), B::sampledTexture(6, cs, v.densityTexture.get(), v.clamp3d.get()),
                                  B::imageStore(5, cs, v.volumes[k].get(), 0)});
        }
        v.compositeSrb = srb({B::uniformBuffer(3, B::VertexStage | B::FragmentStage, g.blitTonemap.get()),
                              B::sampledTexture(10, B::FragmentStage, v.hairImage.get(), g.nearest.get())});
        v.bound = bound;
        volumesStale_ = true;
    }
    if (!v.raster) {
        auto compute = [&](std::unique_ptr<QRhiComputePipeline>& ps, const char* name, QRhiShaderResourceBindings* layout) {
            const QShader shader = loadLookdevShader(QString::fromLatin1(name));
            if (!shader.isValid()) return false;
            ps.reset(rhi->newComputePipeline());
            ps->setShaderStage({QRhiShaderStage::Compute, shader});
            ps->setShaderResourceBindings(layout);
            if (!ps->create()) {
                ps.reset();
                return false;
            }
            return true;
        };
        if (!compute(v.fill, "hair_clear.comp", v.fillPixelsSrb.get()) || !compute(v.raster, "hair_raster.comp", v.rasterSrb[0].get()) ||
            !compute(v.shade, "hair_shade.comp", v.shadeSrb.get()) || !compute(v.voxelize, "hair_voxelize.comp", v.voxelizeSrb.get()) ||
            !compute(v.densityPs, "hair_density.comp", v.densitySrb.get()) || !compute(v.volume, "hair_volume.comp", v.volumeSrb[0].get())) {
            v.raster.reset();
            return fail(__LINE__);
        }
        const QShader fullVert = loadLookdevShader(QStringLiteral("fullscreen.vert"));
        const QShader compositeFrag = loadLookdevShader(QStringLiteral("hair_composite.frag"));
        v.composite.reset(rhi->newGraphicsPipeline());
        v.composite->setShaderStages({{QRhiShaderStage::Vertex, fullVert}, {QRhiShaderStage::Fragment, compositeFrag}});
        QRhiGraphicsPipeline::TargetBlend premultiplied;
        premultiplied.enable = true;
        premultiplied.srcColor = QRhiGraphicsPipeline::One;
        premultiplied.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        premultiplied.srcAlpha = QRhiGraphicsPipeline::One;
        premultiplied.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        v.composite->setTargetBlends({premultiplied});
        v.composite->setShaderResourceBindings(v.compositeSrb.get());
        v.composite->setRenderPassDescriptor(v.compositePass.get());
        if (!v.composite->create()) {
            v.raster.reset();
            return fail(__LINE__);
        }
    }
    v.ready = true;
    return true;
}

void LookdevRenderer::selectLod(QRhi* /*rhi*/, QRhiResourceUpdateBatch* batch, const OrbitCamera& camera, const QSize& size) {
    Gpu::Vis& v = gpu_->vis;
    lodActive_ = false;
    drawnStrands_ = strandCount_;
    if (!lodOn_ || !strands_ || strands_->strandCount() != strandCount_ || size.height() <= 0) {
        lodPrevious_ = StrandLodView{};
        return;
    }
    lod_->setStrands(strands_, "clumpId");
    if (!lod_->hasWidths()) return;
    const float pixelScale = linePixelScale(1.0f / std::tan(0.5f * camera.fovDegrees() * kPi / 180.0f), static_cast<float>(size.height()));
    const QVector3D eye = camera.eye(), look = camera.target() - eye;
    // (The camera orbits its target: the view keeps what an orbit about it at this distance shows.)
    const QVector3D target = camera.target();
    const StrandLodView view = lodView(Vec3f(eye.x(), eye.y(), eye.z()), Vec3f(look.x(), look.y(), look.z()), pixelScale, false,
                                       lod_->center(), Vec3f(target.x(), target.y(), target.z()), lodPrevious_);
    lodPrevious_ = view;
    StrandLodSettings settings;
    settings.error = lodError_;
    settings.widthScale = material_.widthScale;
    settings.density = 1.0f;
    settings.impostors = false;     // (a thinned clump stands for its members: hashed alone, as impostors would be)
    settings.splitBlended = false;  // (nothing blends: the visibility buffer keeps hashed fragments, the alpha is analytic)
    const StrandLod::Selection& sel = lod_->select(view, settings);
    if (!sel.active) return;
    lodActive_ = true;
    drawnStrands_ = sel.drawnStrands;
    if (sel.key == lodKey_) return;
    std::vector<float> exponents(v.lodStrands, 0.0f);
    for (int k = 0; k < kLodClassCount; ++k) {
        const float e = lodClassExponent(k);
        for (std::uint32_t s : sel.strands[static_cast<std::size_t>(k)]) {
            if (s < exponents.size()) exponents[s] = e;
        }
    }
    batch->uploadStaticBuffer(v.lod.get(), 0, static_cast<quint32>(exponents.size() * sizeof(float)), exponents.data());
    lodKey_ = sel.key;
}

void LookdevRenderer::composite(QRhiCommandBuffer* cb, const QRhiViewport& viewport) {
    Gpu& g = *gpu_;
    if (!g.tonemap || !g.tonemapSrb[g.tonemapSource]) return;
    cb->setGraphicsPipeline(g.tonemap.get());
    cb->setViewport(viewport);
    cb->setShaderResources(g.tonemapSrb[g.tonemapSource].get());
    cb->draw(3);
}

}  // namespace groomlab::app
