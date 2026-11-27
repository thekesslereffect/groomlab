// SPDX-License-Identifier: Apache-2.0
// Guides from reference photos, part 1: images, cameras and the 2D orientation field (Gabor
// filters). See reference_guides.h.

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "groomlab/parallel.h"
#include "groomlab/portable_math.h"
#include "groomlab/reference_guides.h"
#include "groomlab/texture.h"

namespace groomlab {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

// ---- Images -------------------------------------------------------------------------------------

float RefImage::sample(float u, float v, int c) const {
    if (empty()) return 0.0f;
    const float x = std::clamp(u - 0.5f, 0.0f, static_cast<float>(width - 1));
    const float y = std::clamp(v - 0.5f, 0.0f, static_cast<float>(height - 1));
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    const float a = at(x0, y0, c) * (1 - fx) + at(x1, y0, c) * fx;
    const float b = at(x0, y1, c) * (1 - fx) + at(x1, y1, c) * fx;
    return a * (1 - fy) + b * fy;
}

RefImage RefImage::resized(int w, int h) const {
    RefImage out;
    out.width = w;
    out.height = h;
    out.channels = channels;
    out.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * static_cast<std::size_t>(channels), 0.0f);
    if (empty() || w <= 0 || h <= 0) return out;
    const float sx = static_cast<float>(width) / static_cast<float>(w), sy = static_cast<float>(height) / static_cast<float>(h);
    parallelFor(0, static_cast<std::size_t>(h), [&](std::size_t yy) {
        const int y = static_cast<int>(yy);
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < channels; ++c) {
                float value = 0.0f;
                if (sx <= 1.0f && sy <= 1.0f) {
                    value = sample((static_cast<float>(x) + 0.5f) * sx, (static_cast<float>(y) + 0.5f) * sy, c);
                } else {
                    // Area average over the source pixels the target pixel covers.
                    const int x0 = static_cast<int>(std::floor(static_cast<float>(x) * sx));
                    const int x1 = std::min(width, std::max(x0 + 1, static_cast<int>(std::ceil(static_cast<float>(x + 1) * sx))));
                    const int y0 = static_cast<int>(std::floor(static_cast<float>(y) * sy));
                    const int y1 = std::min(height, std::max(y0 + 1, static_cast<int>(std::ceil(static_cast<float>(y + 1) * sy))));
                    float sum = 0.0f;
                    int n = 0;
                    for (int j = y0; j < y1; ++j)
                        for (int i = x0; i < x1; ++i) {
                            sum += at(i, j, c);
                            ++n;
                        }
                    value = n ? sum / static_cast<float>(n) : 0.0f;
                }
                out.pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) *
                               static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = value;
            }
        }
    }, 8);
    return out;
}

RefImage RefImage::gray() const {
    RefImage out;
    out.width = width;
    out.height = height;
    out.channels = 1;
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    out.pixels.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        float sum = 0.0f;
        for (int c = 0; c < channels; ++c) sum += pixels[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)];
        out.pixels[i] = channels ? sum / static_cast<float>(channels) : 0.0f;
    }
    return out;
}

RefImage loadRefImage(const std::string& path, int channels) {
    if (channels != 1 && channels != 3 && channels != 4) throw std::invalid_argument("loadRefImage: channels is 1, 3 or 4");
    const TexturePtr texture = loadTexture(path);
    const Texture::Image* image = texture ? texture->image() : nullptr;
    if (!image || image->width <= 0) throw TextureError("cannot read the image " + path);
    RefImage out;
    out.width = image->width;
    out.height = image->height;
    out.channels = channels;
    const std::size_t n = static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height);
    out.pixels.resize(n * static_cast<std::size_t>(channels));
    for (std::size_t i = 0; i < n; ++i) {
        const float* p = &image->rgba[i * 4];
        if (channels == 1) {
            out.pixels[i] = (p[0] + p[1] + p[2]) / 3.0f;
        } else {
            for (int c = 0; c < channels; ++c)
                out.pixels[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = p[c];
        }
    }
    return out;
}

// ---- Cameras ------------------------------------------------------------------------------------

Vec3f RefCamera::eye() const {
    return {static_cast<float>(pose.position[0]), static_cast<float>(pose.position[1]), static_cast<float>(pose.position[2])};
}

Vec3f RefCamera::axis(int i) const {
    return {static_cast<float>(pose.rotation[0][static_cast<std::size_t>(i)]),
            static_cast<float>(pose.rotation[1][static_cast<std::size_t>(i)]),
            static_cast<float>(pose.rotation[2][static_cast<std::size_t>(i)])};
}

bool RefCamera::project(const Vec3f& p, float& u, float& v, float* depth) const {
    const double d[3] = {p.x - pose.position[0], p.y - pose.position[1], p.z - pose.position[2]};
    double c[3];
    for (int i = 0; i < 3; ++i)
        c[i] = pose.rotation[0][static_cast<std::size_t>(i)] * d[0] + pose.rotation[1][static_cast<std::size_t>(i)] * d[1] +
               pose.rotation[2][static_cast<std::size_t>(i)] * d[2];
    const double z = -c[2];
    if (z <= 1e-9) return false;
    u = static_cast<float>(0.5 * width + offsetU + focalPx * c[0] / z);
    v = static_cast<float>(0.5 * height + offsetV - focalPx * c[1] / z);
    if (depth) *depth = static_cast<float>(z);
    return true;
}

Vec3f RefCamera::ray(float u, float v) const {
    return (axis(0) * static_cast<float>((u - 0.5 * width - offsetU) / focalPx) - axis(1) * static_cast<float>((v - 0.5 * height - offsetV) / focalPx) -
            axis(2))
        .normalized();
}

RefCamera RefCamera::scaled(double s) const {
    RefCamera out = *this;
    out.focalPx *= s;
    out.width *= s;
    out.height *= s;
    out.offsetU *= s;
    out.offsetV *= s;
    return out;
}

RefCamera RefCamera::cropped(double x0, double y0, double w, double h) const {
    RefCamera out = *this;
    // The principal point stays where it is in the photo: its place in the crop, from the crop's center.
    out.offsetU = 0.5 * width + offsetU - x0 - 0.5 * w;
    out.offsetV = 0.5 * height + offsetV - y0 - 0.5 * h;
    out.width = w;
    out.height = h;
    return out;
}

RefImage cropImage(const RefImage& image, int x0, int y0, int w, int h) {
    RefImage out;
    out.width = w;
    out.height = h;
    out.channels = image.channels;
    out.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * static_cast<std::size_t>(image.channels));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < image.channels; ++c)
                out.pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) *
                               static_cast<std::size_t>(image.channels) + static_cast<std::size_t>(c)] =
                    image.at(std::clamp(x0 + x, 0, image.width - 1), std::clamp(y0 + y, 0, image.height - 1), c);
    return out;
}

// ---- The orientation field ------------------------------------------------------------------------

namespace {

struct Kernel {
    int radius = 0;
    std::vector<float> weights;  // (2r+1)^2
};

// An even Gabor filter for strands running at `theta` (from +u toward +v): a cosine across the
// strands of period `wavelength`, a Gaussian envelope narrower across (0.45 wavelengths) than along
// (0.6), its mean taken out so a flat patch gives nothing.
Kernel gaborKernel(float theta, float wavelength) {
    Kernel k;
    const float sigmaAcross = 0.45f * wavelength, sigmaAlong = 0.6f * wavelength;
    k.radius = static_cast<int>(std::ceil(3.0f * sigmaAlong));
    const int size = 2 * k.radius + 1;
    k.weights.resize(static_cast<std::size_t>(size * size));
    const float c = pm::cos(theta), s = pm::sin(theta);
    double sum = 0.0, envelope = 0.0;
    std::vector<float> env(k.weights.size());
    for (int y = -k.radius; y <= k.radius; ++y)
        for (int x = -k.radius; x <= k.radius; ++x) {
            const float along = static_cast<float>(x) * c + static_cast<float>(y) * s;
            const float across = -static_cast<float>(x) * s + static_cast<float>(y) * c;
            const float e = pm::exp(-0.5f * (across * across / (sigmaAcross * sigmaAcross) + along * along / (sigmaAlong * sigmaAlong)));
            const std::size_t i = static_cast<std::size_t>((y + k.radius) * size + (x + k.radius));
            env[i] = e;
            k.weights[i] = e * pm::cos(2.0f * kPi * across / wavelength);
            sum += k.weights[i];
            envelope += e;
        }
    const float mean = static_cast<float>(sum / envelope);
    double norm = 0.0;
    for (std::size_t i = 0; i < k.weights.size(); ++i) {
        k.weights[i] -= mean * env[i];
        norm += std::abs(k.weights[i]);
    }
    for (float& w : k.weights) w = static_cast<float>(w / norm);
    return k;
}

// Responses |gabor_k * image| for every orientation k at every pixel whose `want` is set.
std::vector<float> gaborResponses(const RefImage& gray, const std::vector<std::uint8_t>& want, int orientations, float wavelength) {
    std::vector<Kernel> kernels;
    for (int k = 0; k < orientations; ++k) kernels.push_back(gaborKernel(kPi * static_cast<float>(k) / static_cast<float>(orientations), wavelength));
    const int w = gray.width, h = gray.height;
    std::vector<float> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * static_cast<std::size_t>(orientations), 0.0f);
    parallelFor(0, static_cast<std::size_t>(h), [&](std::size_t yy) {
        const int y = static_cast<int>(yy);
        std::vector<float> patch;
        for (int x = 0; x < w; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
            if (!want[at]) continue;
            const int r = kernels[0].radius, size = 2 * r + 1;
            patch.resize(static_cast<std::size_t>(size * size));
            for (int j = -r; j <= r; ++j) {
                const int sy = std::clamp(y + j, 0, h - 1);
                for (int i = -r; i <= r; ++i) {
                    const int sx = std::clamp(x + i, 0, w - 1);
                    patch[static_cast<std::size_t>((j + r) * size + (i + r))] = gray.pixels[static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(sx)];
                }
            }
            for (int k = 0; k < orientations; ++k) {
                const std::vector<float>& kw = kernels[static_cast<std::size_t>(k)].weights;
                float sum = 0.0f;
                for (std::size_t i = 0; i < kw.size(); ++i) sum += kw[i] * patch[i];
                out[at * static_cast<std::size_t>(orientations) + static_cast<std::size_t>(k)] = std::abs(sum);
            }
        }
    }, 2);
    return out;
}

// A separable Gaussian blur of `channels` floats a pixel.
std::vector<float> blurred(const std::vector<float>& in, int w, int h, int channels, float sigma) {
    const int r = std::max(1, static_cast<int>(std::ceil(2.5f * sigma)));
    std::vector<float> g(static_cast<std::size_t>(2 * r + 1));
    for (int i = -r; i <= r; ++i) g[static_cast<std::size_t>(i + r)] = pm::exp(-0.5f * static_cast<float>(i * i) / (sigma * sigma));
    const std::size_t c = static_cast<std::size_t>(channels);
    std::vector<float> tmp(in.size(), 0.0f), out(in.size(), 0.0f);
    parallelFor(0, static_cast<std::size_t>(h), [&](std::size_t yy) {
        const int y = static_cast<int>(yy);
        for (int x = 0; x < w; ++x)
            for (int i = -r; i <= r; ++i) {
                const int px = x + i;
                if (px < 0 || px >= w) continue;
                const float k = g[static_cast<std::size_t>(i + r)];
                const std::size_t a = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * c;
                const std::size_t b = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(px)) * c;
                for (std::size_t k2 = 0; k2 < c; ++k2) tmp[a + k2] += k * in[b + k2];
            }
    }, 8);
    parallelFor(0, static_cast<std::size_t>(h), [&](std::size_t yy) {
        const int y = static_cast<int>(yy);
        for (int x = 0; x < w; ++x)
            for (int j = -r; j <= r; ++j) {
                const int py = y + j;
                if (py < 0 || py >= h) continue;
                const float k = g[static_cast<std::size_t>(j + r)];
                const std::size_t a = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * c;
                const std::size_t b = (static_cast<std::size_t>(py) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * c;
                for (std::size_t k2 = 0; k2 < c; ++k2) out[a + k2] += k * tmp[b + k2];
            }
    }, 8);
    return out;
}

float percentile(std::vector<float> values, float q) {
    if (values.empty()) return 0.0f;
    const std::size_t i = std::min(values.size() - 1, static_cast<std::size_t>(q * static_cast<float>(values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(i), values.end());
    return values[i];
}

}  // namespace

OrientationField orientationField(const RefImage& grayIn, const RefImage& mask, const OrientationOptions& options) {
    RefImage gray = grayIn.channels == 1 ? grayIn : grayIn.gray();
    const int w = gray.width, h = gray.height;
    const int n = std::max(4, options.orientations);
    const std::size_t pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    OrientationField field;
    field.width = w;
    field.height = h;
    field.angle.assign(pixels, 0.0f);
    field.confidence.assign(pixels, 0.0f);
    field.tensor.assign(pixels * 2, 0.0f);
    if (pixels == 0) return field;

    auto inMask = [&](int x, int y) {
        if (mask.empty()) return true;
        const float u = (static_cast<float>(x) + 0.5f) * static_cast<float>(mask.width) / static_cast<float>(w);
        const float v = (static_cast<float>(y) + 0.5f) * static_cast<float>(mask.height) / static_cast<float>(h);
        return mask.sample(u, v) >= 0.5f;
    };
    std::vector<std::uint8_t> inside(pixels);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) inside[static_cast<std::size_t>(y * w + x)] = inMask(x, y) ? 1 : 0;

    // Outside the mask, the hair's own local mean: the filters then see the strands' texture, not
    // the outline of hair against the backdrop (or the hairline against the face).
    if (!mask.empty()) {
        std::vector<float> num(pixels), den(pixels);
        double sum = 0.0;
        std::size_t count = 0;
        for (std::size_t i = 0; i < pixels; ++i) {
            den[i] = inside[i] ? 1.0f : 0.0f;
            num[i] = inside[i] ? gray.pixels[i] : 0.0f;
            if (inside[i]) {
                sum += gray.pixels[i];
                ++count;
            }
        }
        const float mean = count ? static_cast<float>(sum / static_cast<double>(count)) : 0.5f;
        const std::vector<float> n1 = blurred(num, w, h, 1, 2.0f * options.wavelength), d1 = blurred(den, w, h, 1, 2.0f * options.wavelength);
        const std::vector<float> n2 = blurred(num, w, h, 1, 8.0f * options.wavelength), d2 = blurred(den, w, h, 1, 8.0f * options.wavelength);
        for (std::size_t i = 0; i < pixels; ++i) {
            if (inside[i]) continue;
            gray.pixels[i] = d1[i] > 0.05f ? n1[i] / d1[i] : d2[i] > 0.02f ? n2[i] / d2[i] : mean;
        }
    }

    // Responses summed over the scales (each normalized by its strong responses), at full size.
    std::vector<float> total(pixels * static_cast<std::size_t>(n), 0.0f);
    const int scales = std::max(1, options.scales);
    for (int s = 0; s < scales; ++s) {
        const int f = 1 << s;
        const int sw = std::max(1, w / f), sh = std::max(1, h / f);
        const RefImage level = (s == 0) ? gray : gray.resized(sw, sh);
        std::vector<std::uint8_t> want(static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh), 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (inside[static_cast<std::size_t>(y * w + x)])
                    want[static_cast<std::size_t>(std::min(sh - 1, y / f) * sw + std::min(sw - 1, x / f))] = 1;
        const std::vector<float> response = gaborResponses(level, want, n, options.wavelength);
        std::vector<float> maxima;
        for (std::size_t i = 0; i < want.size(); ++i)
            if (want[i]) maxima.push_back(*std::max_element(response.begin() + static_cast<std::ptrdiff_t>(i * static_cast<std::size_t>(n)),
                                                            response.begin() + static_cast<std::ptrdiff_t>((i + 1) * static_cast<std::size_t>(n))));
        const float norm = std::max(1e-6f, percentile(maxima, 0.9f));
        parallelFor(0, static_cast<std::size_t>(h), [&](std::size_t yy) {
            const int y = static_cast<int>(yy);
            for (int x = 0; x < w; ++x) {
                const std::size_t at = static_cast<std::size_t>(y * w + x);
                if (!inside[at]) continue;
                // Bilinear from the level (pixel centers).
                const float lx = std::clamp((static_cast<float>(x) + 0.5f) / static_cast<float>(f) - 0.5f, 0.0f, static_cast<float>(sw - 1));
                const float ly = std::clamp((static_cast<float>(y) + 0.5f) / static_cast<float>(f) - 0.5f, 0.0f, static_cast<float>(sh - 1));
                const int x0 = static_cast<int>(lx), y0 = static_cast<int>(ly);
                const int x1 = std::min(x0 + 1, sw - 1), y1 = std::min(y0 + 1, sh - 1);
                const float fx = lx - static_cast<float>(x0), fy = ly - static_cast<float>(y0);
                const float wt[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                const std::size_t src[4] = {static_cast<std::size_t>(y0 * sw + x0), static_cast<std::size_t>(y0 * sw + x1),
                                            static_cast<std::size_t>(y1 * sw + x0), static_cast<std::size_t>(y1 * sw + x1)};
                for (int k = 0; k < n; ++k) {
                    float value = 0.0f, weight = 0.0f;
                    for (int c = 0; c < 4; ++c) {
                        if (!want[src[c]]) continue;
                        value += wt[c] * response[src[c] * static_cast<std::size_t>(n) + static_cast<std::size_t>(k)];
                        weight += wt[c];
                    }
                    if (weight > 0.0f) total[at * static_cast<std::size_t>(n) + static_cast<std::size_t>(k)] += value / weight / norm;
                }
            }
        }, 8);
    }

    // The peak (refined by a parabola through its neighbours).
    std::vector<float> peak(pixels, 0.0f), coherence(pixels, 0.0f), peaks, coherences;
    for (std::size_t i = 0; i < pixels; ++i) {
        if (!inside[i]) continue;
        const float* r = &total[i * static_cast<std::size_t>(n)];
        int best = 0;
        float sum = 0.0f;
        for (int k = 0; k < n; ++k) {
            sum += r[k];
            if (r[k] > r[best]) best = k;
        }
        const float mean = sum / static_cast<float>(n);
        const float a = r[(best + n - 1) % n], b = r[best], c = r[(best + 1) % n];
        const float denom = a - 2.0f * b + c;
        const float offset = std::abs(denom) > 1e-12f ? std::clamp(0.5f * (a - c) / denom, -0.5f, 0.5f) : 0.0f;
        float angle = kPi * (static_cast<float>(best) + offset) / static_cast<float>(n);
        field.angle[i] = std::fmod(angle + kPi, kPi);
        peak[i] = b;
        coherence[i] = b > 1e-9f ? std::clamp((b - mean) / b, 0.0f, 1.0f) : 0.0f;
        peaks.push_back(b);
        coherences.push_back(coherence[i]);
    }
    // The confidence: the peak's lead over the mean (coherence) against the image's usual lead,
    // times its strength against the usual strength (flat patches give little). Relative to the
    // image itself, so a fine fur and a coarse lock both count.
    const float usualLead = std::max(1e-6f, percentile(coherences, 0.75f));
    const float usualPeak = std::max(1e-6f, percentile(peaks, 0.5f));
    for (std::size_t i = 0; i < pixels; ++i) {
        if (!inside[i]) continue;
        const float conf = std::min(1.0f, coherence[i] / usualLead) * std::min(1.0f, peak[i] / (0.5f * usualPeak));
        field.confidence[i] = conf;
        field.tensor[2 * i] = conf * pm::cos(2.0f * field.angle[i]);
        field.tensor[2 * i + 1] = conf * pm::sin(2.0f * field.angle[i]);
    }

    // The mask's edge: what the filters see there is the outline (hair against the backdrop, the
    // hairline's ends), not the strands. Pixels within two wavelengths of it lose their confidence
    // and take the direction from inside (below).
    if (!mask.empty() && options.edgeWidth > 0.0f) {
        // Chamfer distance (3-4) to the nearest pixel outside the mask, in pixels.
        const float big = 1e9f;
        std::vector<float> dist(pixels);
        for (std::size_t i = 0; i < pixels; ++i) dist[i] = inside[i] ? big : 0.0f;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float& d = dist[static_cast<std::size_t>(y * w + x)];
                if (x > 0) d = std::min(d, dist[static_cast<std::size_t>(y * w + x - 1)] + 1.0f);
                if (y > 0) {
                    d = std::min(d, dist[static_cast<std::size_t>((y - 1) * w + x)] + 1.0f);
                    if (x > 0) d = std::min(d, dist[static_cast<std::size_t>((y - 1) * w + x - 1)] + 1.4142f);
                    if (x + 1 < w) d = std::min(d, dist[static_cast<std::size_t>((y - 1) * w + x + 1)] + 1.4142f);
                }
            }
        for (int y = h - 1; y >= 0; --y)
            for (int x = w - 1; x >= 0; --x) {
                float& d = dist[static_cast<std::size_t>(y * w + x)];
                if (x + 1 < w) d = std::min(d, dist[static_cast<std::size_t>(y * w + x + 1)] + 1.0f);
                if (y + 1 < h) {
                    d = std::min(d, dist[static_cast<std::size_t>((y + 1) * w + x)] + 1.0f);
                    if (x + 1 < w) d = std::min(d, dist[static_cast<std::size_t>((y + 1) * w + x + 1)] + 1.4142f);
                    if (x > 0) d = std::min(d, dist[static_cast<std::size_t>((y + 1) * w + x - 1)] + 1.4142f);
                }
            }
        // Pixels on the image's own border count as near an edge too (the filters run off it).
        const float band = options.edgeWidth * options.wavelength;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t at = static_cast<std::size_t>(y * w + x);
                if (!inside[at]) continue;
                const float d = std::min(dist[at], static_cast<float>(std::min({x + 1, y + 1, w - x, h - y})));
                const float keep = std::clamp((d - 1.0f) / band, 0.0f, 1.0f);
                if (keep >= 1.0f) continue;
                field.confidence[at] *= keep;
                field.tensor[2 * at] *= keep;
                field.tensor[2 * at + 1] *= keep;
            }
    }

    // Weak pixels inside the mask take the mean of the strong ones' vectors around them (a
    // normalized convolution, two reaches): the direction carries into flat and noisy patches.
    if (options.fillWeak) {
        std::vector<float> strong(pixels * 2, 0.0f), in(pixels, 0.0f), have(pixels, 0.0f);
        for (std::size_t i = 0; i < pixels; ++i) {
            if (!inside[i]) continue;
            in[i] = 1.0f;
            if (field.confidence[i] < options.confidenceFloor) continue;
            strong[2 * i] = field.tensor[2 * i];
            strong[2 * i + 1] = field.tensor[2 * i + 1];
            have[i] = 1.0f;
        }
        const float near = 2.0f * options.wavelength, far = 6.0f * options.wavelength;
        const std::vector<float> s1 = blurred(strong, w, h, 2, near), i1 = blurred(in, w, h, 1, near), h1 = blurred(have, w, h, 1, near);
        const std::vector<float> s2 = blurred(strong, w, h, 2, far), i2 = blurred(in, w, h, 1, far);
        for (std::size_t i = 0; i < pixels; ++i) {
            if (!inside[i] || field.confidence[i] >= options.confidenceFloor) continue;
            if (h1[i] > 0.25f * i1[i] && i1[i] > 1e-6f) {
                field.tensor[2 * i] = s1[2 * i] / i1[i];
                field.tensor[2 * i + 1] = s1[2 * i + 1] / i1[i];
            } else if (i2[i] > 1e-6f) {
                field.tensor[2 * i] = s2[2 * i] / i2[i];
                field.tensor[2 * i + 1] = s2[2 * i + 1] / i2[i];
            }
        }
    }

    // A Gaussian blur inside the mask (normalized by the mask's weight).
    const float sigma = options.smoothSigma;
    if (sigma > 0.0f) {
        const int r = static_cast<int>(std::ceil(2.5f * sigma));
        std::vector<float> g(static_cast<std::size_t>(2 * r + 1));
        for (int i = -r; i <= r; ++i) g[static_cast<std::size_t>(i + r)] = pm::exp(-0.5f * static_cast<float>(i * i) / (sigma * sigma));
        std::vector<float> tmp(pixels * 3, 0.0f), out(pixels * 3, 0.0f);  // x, y, weight
        parallelFor(0, static_cast<std::size_t>(h), [&](std::size_t yy) {
            const int y = static_cast<int>(yy);
            for (int x = 0; x < w; ++x) {
                float a = 0, b = 0, c = 0;
                for (int i = -r; i <= r; ++i) {
                    const int px = x + i;
                    if (px < 0 || px >= w) continue;
                    const std::size_t o = static_cast<std::size_t>(y * w + px);
                    if (!inside[o]) continue;
                    const float k = g[static_cast<std::size_t>(i + r)];
                    a += k * field.tensor[2 * o];
                    b += k * field.tensor[2 * o + 1];
                    c += k;
                }
                const std::size_t at = static_cast<std::size_t>(y * w + x);
                tmp[3 * at] = a;
                tmp[3 * at + 1] = b;
                tmp[3 * at + 2] = c;
            }
        }, 8);
        parallelFor(0, static_cast<std::size_t>(h), [&](std::size_t yy) {
            const int y = static_cast<int>(yy);
            for (int x = 0; x < w; ++x) {
                float a = 0, b = 0, c = 0;
                for (int j = -r; j <= r; ++j) {
                    const int py = y + j;
                    if (py < 0 || py >= h) continue;
                    const std::size_t o = static_cast<std::size_t>(py * w + x);
                    const float k = g[static_cast<std::size_t>(j + r)];
                    a += k * tmp[3 * o];
                    b += k * tmp[3 * o + 1];
                    c += k * tmp[3 * o + 2];
                }
                const std::size_t at = static_cast<std::size_t>(y * w + x);
                out[3 * at] = a;
                out[3 * at + 1] = b;
                out[3 * at + 2] = c;
            }
        }, 8);
        for (std::size_t i = 0; i < pixels; ++i) {
            if (!inside[i] || out[3 * i + 2] <= 1e-6f) {
                field.tensor[2 * i] = field.tensor[2 * i + 1] = 0.0f;
                continue;
            }
            field.tensor[2 * i] = out[3 * i] / out[3 * i + 2];
            field.tensor[2 * i + 1] = out[3 * i + 1] / out[3 * i + 2];
        }
    }
    return field;
}

OrientationField coarseField(const OrientationField& field, const RefImage& mask, float sigma) {
    OrientationField out = field;
    const int w = field.width, h = field.height;
    const std::size_t pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<float> in(pixels, 1.0f);
    if (!mask.empty())
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                in[static_cast<std::size_t>(y * w + x)] =
                    mask.sample((static_cast<float>(x) + 0.5f) * static_cast<float>(mask.width) / static_cast<float>(w),
                                (static_cast<float>(y) + 0.5f) * static_cast<float>(mask.height) / static_cast<float>(h)) >= 0.5f ? 1.0f : 0.0f;
    std::vector<float> t(pixels * 2);
    for (std::size_t i = 0; i < pixels; ++i) {
        t[2 * i] = field.tensor[2 * i] * in[i];
        t[2 * i + 1] = field.tensor[2 * i + 1] * in[i];
    }
    const std::vector<float> bt = blurred(t, w, h, 2, sigma), bw = blurred(in, w, h, 1, sigma);
    for (std::size_t i = 0; i < pixels; ++i) {
        const float k = in[i] > 0.0f && bw[i] > 1e-6f ? 1.0f / bw[i] : 0.0f;
        out.tensor[2 * i] = bt[2 * i] * k;
        out.tensor[2 * i + 1] = bt[2 * i + 1] * k;
    }
    return out;
}

CurlEstimate measureCurl(const OrientationField& fine, const OrientationField& coarse, const RefImage& mask) {
    CurlEstimate est;
    const int w = fine.width, h = fine.height;
    auto inMask = [&](float u, float v) {
        if (u < 0 || v < 0 || u >= static_cast<float>(w) || v >= static_cast<float>(h)) return false;
        return mask.empty() || mask.sample(u * static_cast<float>(mask.width) / static_cast<float>(w),
                                           v * static_cast<float>(mask.height) / static_cast<float>(h)) >= 0.5f;
    };
    const int spacing = std::max(8, std::max(w, h) / 40);
    const int half = 64;  // samples each way along a streamline (1 pixel apart)
    std::vector<float> wavelengths, amplitudes;
    for (int sy = spacing / 2; sy < h; sy += spacing)
        for (int sx = spacing / 2; sx < w; sx += spacing) {
            if (!inMask(static_cast<float>(sx) + 0.5f, static_cast<float>(sy) + 0.5f)) continue;
            // Trace both ways along the coarse flow; at each step the fine direction's angle to it.
            std::vector<float> forward, backward;
            for (int dir = -1; dir <= 1; dir += 2) {
                float u = static_cast<float>(sx) + 0.5f, v = static_cast<float>(sy) + 0.5f, pu = 0, pv = 0;
                bool first = true;
                for (int k = 0; k < half; ++k) {
                    float cu = 0, cv = 0, cc = 0, fu = 0, fv = 0, fc = 0;
                    if (!inMask(u, v) || !coarse.sample(u, v, cu, cv, cc) || !fine.sample(u, v, fu, fv, fc) || cc < 0.05f) break;
                    if (first) {
                        pu = cu * static_cast<float>(dir);
                        pv = cv * static_cast<float>(dir);
                        first = false;
                    } else if (cu * pu + cv * pv < 0.0f) {
                        cu = -cu;
                        cv = -cv;
                    }
                    if (cu * pu + cv * pv < 0.0f) {
                        cu = -cu;
                        cv = -cv;
                    }
                    pu = cu;
                    pv = cv;
                    // The fine direction's signed angle to the coarse one, as lines (-90..90 degrees).
                    float a = pm::atan2(cu * fv - cv * fu, cu * fu + cv * fv);
                    if (a > 0.5f * kPi) a -= kPi;
                    if (a < -0.5f * kPi) a += kPi;
                    (dir < 0 ? backward : forward).push_back(a);
                    u += cu;
                    v += cv;
                }
            }
            std::vector<float> line(backward.rbegin(), backward.rend());
            line.insert(line.end(), forward.begin(), forward.end());
            if (line.size() < 48) continue;
            ++est.streamlines;
            double mean = 0.0;
            for (float a : line) mean += a;
            mean /= static_cast<double>(line.size());
            double var = 0.0;
            for (float a : line) var += (a - mean) * (a - mean);
            var /= static_cast<double>(line.size());
            if (var < 1e-6) continue;
            // The autocorrelation's first peak past its first dip.
            const int maxLag = static_cast<int>(line.size()) / 2;
            std::vector<double> ac(static_cast<std::size_t>(maxLag) + 1, 0.0);
            for (int lag = 0; lag <= maxLag; ++lag) {
                double sum = 0.0;
                for (std::size_t i = 0; i + static_cast<std::size_t>(lag) < line.size(); ++i)
                    sum += (line[i] - mean) * (line[i + static_cast<std::size_t>(lag)] - mean);
                ac[static_cast<std::size_t>(lag)] = sum / (static_cast<double>(line.size() - static_cast<std::size_t>(lag)) * var);
            }
            int lag = 1;
            while (lag < maxLag && ac[static_cast<std::size_t>(lag)] > 0.0) ++lag;  // past the first zero
            int best = -1;
            for (int k = lag + 1; k < maxLag; ++k)
                if (ac[static_cast<std::size_t>(k)] > ac[static_cast<std::size_t>(k - 1)] && ac[static_cast<std::size_t>(k)] >= ac[static_cast<std::size_t>(k + 1)]) {
                    best = k;
                    break;
                }
            if (best < 4 || ac[static_cast<std::size_t>(best)] < 0.2) continue;
            wavelengths.push_back(static_cast<float>(best));
            amplitudes.push_back(static_cast<float>(std::sqrt(2.0 * var)));
        }
    if (est.streamlines) est.periodicShare = static_cast<float>(wavelengths.size()) / static_cast<float>(est.streamlines);
    auto med = [](std::vector<float> v) {
        if (v.empty()) return 0.0f;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    est.wavelengthPx = med(wavelengths);
    est.amplitude = med(amplitudes);
    return est;
}

bool OrientationField::sample(float u, float v, float& du, float& dv, float& conf) const {
    if (width <= 0 || u < 0.0f || v < 0.0f || u >= static_cast<float>(width) || v >= static_cast<float>(height)) return false;
    const float x = std::clamp(u - 0.5f, 0.0f, static_cast<float>(width - 1));
    const float y = std::clamp(v - 0.5f, 0.0f, static_cast<float>(height - 1));
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    auto t = [&](int px, int py, int c) { return tensor[2 * static_cast<std::size_t>(py * width + px) + static_cast<std::size_t>(c)]; };
    float a[2];
    for (int c = 0; c < 2; ++c)
        a[c] = (t(x0, y0, c) * (1 - fx) + t(x1, y0, c) * fx) * (1 - fy) + (t(x0, y1, c) * (1 - fx) + t(x1, y1, c) * fx) * fy;
    conf = std::sqrt(a[0] * a[0] + a[1] * a[1]);
    if (conf <= 1e-6f) {
        du = 1.0f;
        dv = 0.0f;
        return true;
    }
    const float half = 0.5f * pm::atan2(a[1], a[0]);
    du = pm::cos(half);
    dv = pm::sin(half);
    return true;
}

std::vector<std::uint8_t> OrientationField::colors(bool smoothed) const {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * static_cast<std::size_t>(height); ++i) {
        float a = angle[i], conf = confidence[i];
        if (smoothed) {
            conf = std::sqrt(tensor[2 * i] * tensor[2 * i] + tensor[2 * i + 1] * tensor[2 * i + 1]);
            a = std::fmod(0.5f * pm::atan2(tensor[2 * i + 1], tensor[2 * i]) + kPi, kPi);
        }
        // Hue: the direction over 180 degrees.
        const float hue = std::min(a / kPi, 0.99999f) * 6.0f;
        const int sector = static_cast<int>(hue) % 6;
        const float f = hue - std::floor(hue);
        float rgb[3];
        switch (sector) {
            case 0: rgb[0] = 1; rgb[1] = f; rgb[2] = 0; break;
            case 1: rgb[0] = 1 - f; rgb[1] = 1; rgb[2] = 0; break;
            case 2: rgb[0] = 0; rgb[1] = 1; rgb[2] = f; break;
            case 3: rgb[0] = 0; rgb[1] = 1 - f; rgb[2] = 1; break;
            case 4: rgb[0] = f; rgb[1] = 0; rgb[2] = 1; break;
            default: rgb[0] = 1; rgb[1] = 0; rgb[2] = 1 - f; break;
        }
        const float value = std::clamp(conf, 0.0f, 1.0f);
        for (int c = 0; c < 3; ++c) out[3 * i + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(std::lround(255.0f * rgb[c] * value));
    }
    return out;
}

}  // namespace groomlab
