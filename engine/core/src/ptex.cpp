// SPDX-License-Identifier: Apache-2.0
#include "groomlab/ptex.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "groomlab/color.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "stb_image.h"  // stbi_zlib_decode_buffer (the implementation lives in texture.cpp)
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace groomlab {

namespace {

// The file's layout (Ptex's PtexIO.h): a 64-byte header, an extension header of extheadersize
// bytes, the deflated face infos, the deflated constant data, the level infos, the levels.
constexpr std::uint32_t kMagic = 0x78657450;  // "Ptex"
constexpr std::size_t kHeaderSize = 64;
constexpr std::size_t kFaceInfoSize = 20;  // res (2 int8), adjedges, flags, adjfaces[4]
constexpr std::size_t kLevelInfoSize = 16;
enum Encoding { kConstant = 0, kZipped = 1, kDiffZipped = 2, kTiled = 3 };

struct Reader {
    const std::vector<std::uint8_t>& bytes;
    std::size_t pos = 0;

    void need(std::size_t n) const {
        if (pos + n > bytes.size() || pos + n < pos) throw PtexError("Ptex: the file is cut short");
    }
    template <class T>
    T read() {
        need(sizeof(T));
        T v;
        std::memcpy(&v, bytes.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    // A deflated block of `zipSize` bytes holding `size` bytes.
    std::vector<std::uint8_t> unzip(std::size_t zipSize, std::size_t size) {
        need(zipSize);
        std::vector<std::uint8_t> out(size);
        if (size > 0) {
            const int got = stbi_zlib_decode_buffer(
                reinterpret_cast<char*>(out.data()), static_cast<int>(size),
                reinterpret_cast<const char*>(bytes.data() + pos), static_cast<int>(zipSize));
            if (got != static_cast<int>(size)) throw PtexError("Ptex: a block does not inflate to its size");
        }
        pos += zipSize;
        return out;
    }
};

int dataSize(PtexImage::DataType t) {
    switch (t) {
        case PtexImage::DataType::UInt8:
            return 1;
        case PtexImage::DataType::UInt16:
            return 2;
        case PtexImage::DataType::Half:
            return 2;
        case PtexImage::DataType::Float:
            return 4;
    }
    return 1;
}

float halfToFloat(std::uint16_t h) {
    const int sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    float v;
    if (exponent == 0)
        v = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31)
        v = mantissa ? 0.0f : 65504.0f;  // NaN as 0, infinity as the largest half
    else
        v = std::ldexp(static_cast<float>(mantissa + 1024), exponent - 25);
    return sign ? -v : v;
}

float sampleValue(const std::uint8_t* p, PtexImage::DataType t) {
    switch (t) {
        case PtexImage::DataType::UInt8:
            return *p / 255.0f;
        case PtexImage::DataType::UInt16: {
            std::uint16_t v;
            std::memcpy(&v, p, 2);
            return v / 65535.0f;
        }
        case PtexImage::DataType::Half: {
            std::uint16_t v;
            std::memcpy(&v, p, 2);
            return halfToFloat(v);
        }
        case PtexImage::DataType::Float: {
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }
    }
    return 0.0f;
}

// Texels of `res` from a zipped block: stored a channel at a time (all of channel 0's texels, then
// channel 1's...), 8- and 16-bit ones differenced (each value the running sum of what precedes it).
std::vector<float> unpackTexels(Reader& r, std::uint32_t blockSize, bool differenced, int ures, int vres,
                                const PtexImage& image) {
    const int ds = dataSize(image.dataType), nc = image.channels;
    const std::size_t count = static_cast<std::size_t>(ures) * vres;
    std::vector<std::uint8_t> raw = r.unzip(blockSize, count * nc * ds);
    if (differenced) {
        if (image.dataType == PtexImage::DataType::UInt8) {
            std::uint8_t prev = 0;
            for (auto& b : raw) prev = b = static_cast<std::uint8_t>(b + prev);
        } else if (image.dataType == PtexImage::DataType::UInt16) {
            std::uint16_t prev = 0;
            for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
                std::uint16_t v;
                std::memcpy(&v, raw.data() + i, 2);
                prev = v = static_cast<std::uint16_t>(v + prev);
                std::memcpy(raw.data() + i, &v, 2);
            }
        }
    }
    std::vector<float> out(count * nc);
    for (int c = 0; c < nc; ++c) {
        const std::uint8_t* plane = raw.data() + c * count * ds;
        for (std::size_t i = 0; i < count; ++i) out[i * nc + c] = sampleValue(plane + i * ds, image.dataType);
    }
    return out;
}

std::vector<float> constantTexel(Reader& r, const PtexImage& image) {
    const int ds = dataSize(image.dataType);
    r.need(static_cast<std::size_t>(ds) * image.channels);
    std::vector<float> out(image.channels);
    for (int c = 0; c < image.channels; ++c)
        out[c] = sampleValue(r.bytes.data() + r.pos + c * ds, image.dataType);
    r.pos += static_cast<std::size_t>(ds) * image.channels;
    return out;
}

// One face's (or tile's) data under its data header.
void readFace(Reader& r, std::uint32_t header, PtexImage::Face& face, const PtexImage& image,
              bool allowTiles) {
    const std::uint32_t blockSize = header & 0x3fffffffu;
    const int encoding = static_cast<int>(header >> 30);
    const std::size_t end = r.pos + blockSize;
    r.need(blockSize);
    if (encoding == kConstant) {
        face.constant = true;
        face.pixels = constantTexel(r, image);
    } else if (encoding == kZipped || encoding == kDiffZipped) {
        face.constant = false;
        face.pixels = unpackTexels(r, blockSize, encoding == kDiffZipped, face.ures, face.vres, image);
    } else {
        if (!allowTiles) throw PtexError("Ptex: a tile is tiled again");
        const int tu = 1 << r.read<std::int8_t>(), tv = 1 << r.read<std::int8_t>();
        const std::uint32_t tileHeaderSize = r.read<std::uint32_t>();
        if (tu <= 0 || tv <= 0 || face.ures % tu || face.vres % tv) throw PtexError("Ptex: bad tile size");
        const int ntu = face.ures / tu, ntv = face.vres / tv;
        const std::vector<std::uint8_t> raw =
            r.unzip(tileHeaderSize, static_cast<std::size_t>(ntu) * ntv * 4);
        const int nc = image.channels;
        face.constant = false;
        face.pixels.assign(static_cast<std::size_t>(face.ures) * face.vres * nc, 0.0f);
        for (int t = 0; t < ntu * ntv; ++t) {
            std::uint32_t th;
            std::memcpy(&th, raw.data() + t * 4, 4);
            PtexImage::Face tile;
            tile.ures = tu;
            tile.vres = tv;
            readFace(r, th, tile, image, false);
            const int ox = (t % ntu) * tu, oy = (t / ntu) * tv;
            for (int y = 0; y < tv; ++y) {
                for (int x = 0; x < tu; ++x) {
                    const float* src = tile.constant
                                           ? tile.pixels.data()
                                           : tile.pixels.data() + (static_cast<std::size_t>(y) * tu + x) * nc;
                    std::copy(
                        src, src + nc,
                        face.pixels.begin() + ((static_cast<std::size_t>(oy) + y) * face.ures + ox + x) * nc);
                }
            }
        }
    }
    r.pos = end;
}

}  // namespace

void PtexImage::sample(int face, float u, float v, float* out) const {
    if (face < 0 || face >= static_cast<int>(faces.size()))
        throw PtexError("Ptex: no face " + std::to_string(face));
    const Face& f = faces[face];
    const int nc = channels;
    if (f.constant) {
        std::copy(f.pixels.begin(), f.pixels.begin() + nc, out);
        return;
    }
    const float fx = std::clamp(u, 0.0f, 1.0f) * f.ures - 0.5f,
                fy = std::clamp(v, 0.0f, 1.0f) * f.vres - 0.5f;
    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, f.ures - 1),
              y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, f.vres - 1);
    const int x1 = std::min(x0 + 1, f.ures - 1), y1 = std::min(y0 + 1, f.vres - 1);
    const float tx = std::clamp(fx - x0, 0.0f, 1.0f), ty = std::clamp(fy - y0, 0.0f, 1.0f);
    auto at = [&](int x, int y, int c) {
        return f.pixels[(static_cast<std::size_t>(y) * f.ures + x) * nc + c];
    };
    for (int c = 0; c < nc; ++c) {
        const float a = at(x0, y0, c) + (at(x1, y0, c) - at(x0, y0, c)) * tx;
        const float b = at(x0, y1, c) + (at(x1, y1, c) - at(x0, y1, c)) * tx;
        out[c] = a + (b - a) * ty;
    }
}

std::vector<float> PtexImage::mean(int face) const {
    if (face < 0 || face >= static_cast<int>(faces.size()))
        throw PtexError("Ptex: no face " + std::to_string(face));
    const Face& f = faces[face];
    std::vector<float> out(channels, 0.0f);
    const std::size_t n = f.pixels.size() / std::max(1, channels);
    for (std::size_t i = 0; i < n; ++i) {
        for (int c = 0; c < channels; ++c) out[c] += f.pixels[i * channels + c];
    }
    for (float& v : out) v /= static_cast<float>(std::max<std::size_t>(1, n));
    return out;
}

PtexImage readPtexBytes(const std::vector<std::uint8_t>& bytes) {
    Reader r{bytes};
    if (bytes.size() < kHeaderSize) throw PtexError("Ptex: not a Ptex file (too short)");
    const std::uint32_t magic = r.read<std::uint32_t>();
    if (magic != kMagic) throw PtexError("Ptex: not a Ptex file");
    const std::uint32_t version = r.read<std::uint32_t>();
    if (version != 1) throw PtexError("Ptex: unknown version " + std::to_string(version));
    const std::uint32_t meshType = r.read<std::uint32_t>();
    const std::uint32_t dataType = r.read<std::uint32_t>();
    const std::int32_t alphaChannel = r.read<std::int32_t>();
    const std::uint16_t channels = r.read<std::uint16_t>();
    const std::uint16_t levels = r.read<std::uint16_t>();
    const std::uint32_t faceCount = r.read<std::uint32_t>();
    const std::uint32_t extHeaderSize = r.read<std::uint32_t>();
    const std::uint32_t faceInfoSize = r.read<std::uint32_t>();
    const std::uint32_t constDataSize = r.read<std::uint32_t>();
    const std::uint32_t levelInfoSize = r.read<std::uint32_t>();
    r.read<std::uint32_t>();  // minor version
    r.read<std::uint64_t>();  // level data size
    if (meshType != 1) throw PtexError("Ptex: a triangle-mesh file (only quad Ptex files are read)");
    if (dataType > 3) throw PtexError("Ptex: unknown data type");
    if (channels == 0 || levels == 0) throw PtexError("Ptex: no channels or no levels");

    PtexImage image;
    image.channels = channels;
    image.alphaChannel = alphaChannel;
    image.dataType = static_cast<PtexImage::DataType>(dataType);
    image.faces.resize(faceCount);

    r.pos = kHeaderSize + extHeaderSize;
    const std::vector<std::uint8_t> infos = r.unzip(faceInfoSize, faceCount * kFaceInfoSize);
    std::vector<bool> constant(faceCount, false);
    for (std::uint32_t f = 0; f < faceCount; ++f) {
        const auto ulog = static_cast<std::int8_t>(infos[f * kFaceInfoSize]),
                   vlog = static_cast<std::int8_t>(infos[f * kFaceInfoSize + 1]);
        if (ulog < 0 || vlog < 0 || ulog > 15 || vlog > 15) throw PtexError("Ptex: bad face resolution");
        image.faces[f].ures = 1 << ulog;
        image.faces[f].vres = 1 << vlog;
        constant[f] = (infos[f * kFaceInfoSize + 3] & 1) != 0;  // FaceInfo's flag_constant
    }
    // Each face's mean (a texel each): what a constant face holds, its level data empty.
    const std::size_t pixelSize = static_cast<std::size_t>(dataSize(image.dataType)) * channels;
    const std::vector<std::uint8_t> constData = r.unzip(constDataSize, faceCount * pixelSize);
    // Level 0 (full resolution): its info, then its face data headers (deflated), then the faces.
    r.need(levelInfoSize);
    if (levelInfoSize < kLevelInfoSize) throw PtexError("Ptex: no level info");
    r.read<std::uint64_t>();  // level 0's data size
    const std::uint32_t levelHeaderSize = r.read<std::uint32_t>();
    const std::uint32_t levelFaces = r.read<std::uint32_t>();
    if (levelFaces != faceCount) throw PtexError("Ptex: level 0 does not hold every face");
    r.pos += levelInfoSize - kLevelInfoSize;
    const std::vector<std::uint8_t> headers = r.unzip(levelHeaderSize, faceCount * 4);
    for (std::uint32_t f = 0; f < faceCount; ++f) {
        std::uint32_t header;
        std::memcpy(&header, headers.data() + f * 4, 4);
        if (constant[f]) {
            PtexImage::Face& face = image.faces[f];
            face.constant = true;
            face.pixels.resize(channels);
            for (int c = 0; c < channels; ++c) {
                face.pixels[c] = sampleValue(constData.data() + f * pixelSize + c * dataSize(image.dataType),
                                             image.dataType);
            }
            r.pos += header & 0x3fffffffu;  // (empty)
            continue;
        }
        readFace(r, header, image.faces[f], image, true);
    }
    return image;
}

PtexImage readPtex(const std::string& path) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in) throw PtexError("Ptex: cannot open " + path);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        return readPtexBytes(bytes);
    } catch (const PtexError& e) {
        throw PtexError(std::string(e.what()) + ": " + path);
    }
}

namespace {

// The Ptex faces of polygon `count` corners: one for a quad, one per corner for any other.
int facesOf(int count) {
    return count == 4 ? 1 : std::max(0, count);
}

void checkMesh(const PtexImage& ptex, const PtexMesh& mesh) {
    std::size_t faces = 0;
    for (int n : mesh.faceCounts) faces += facesOf(n);
    if (faces != ptex.faces.size()) {
        throw PtexError("Ptex: the file has " + std::to_string(ptex.faces.size()) + " faces and the mesh " +
                        std::to_string(faces) + " (one per quad, one per corner of other polygons)");
    }
}

// The file's channels as `channels` output values.
void convertTexel(const float* in, int inChannels, int alpha, float* out, int channels) {
    const bool gray = inChannels == 1 || (inChannels == 2 && alpha == 1);
    for (int c = 0; c < channels; ++c) {
        if (c == 3 && channels == 4) {
            out[c] = alpha >= 0 && alpha < inChannels ? in[alpha] : (inChannels >= 4 ? in[3] : 1.0f);
        } else if (gray) {
            out[c] = in[0];
        } else {
            out[c] = in[std::min(c, inChannels - 1)];
        }
    }
    if (channels == 1 && inChannels >= 3 && !gray)
        out[0] = rec709Luminance(in[0], in[1], in[2]);
}

using Uv = std::array<float, 2>;

// Paints Ptex face `face` over the UV quad q[0..3] (its corners at face parameters (0,0) (1,0)
// (1,1) (0,1)), as two triangles.
void paintQuad(const PtexImage& ptex, int face, const Uv q[4], int width, int height, int channels,
               std::vector<float>& image, std::vector<std::uint8_t>& filled, std::vector<float>& texel) {
    static const float kParams[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    static const int kTris[2][3] = {{0, 1, 2}, {0, 2, 3}};
    for (const auto& tri : kTris) {
        float x[3], y[3];
        for (int j = 0; j < 3; ++j) {
            x[j] = q[tri[j]][0] * width - 0.5f;
            y[j] = (1.0f - q[tri[j]][1]) * height - 0.5f;
        }
        const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
        if (std::fabs(area) < 1e-12f) continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({x[0], x[1], x[2]}))));
        const int x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max({x[0], x[1], x[2]}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({y[0], y[1], y[2]}))));
        const int y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max({y[0], y[1], y[2]}))));
        for (int py = y0; py <= y1; ++py) {
            for (int px = x0; px <= x1; ++px) {
                const float w0 = ((x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py)) / area;
                const float w1 = ((x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py)) / area;
                const float w2 = 1.0f - w0 - w1;
                constexpr float eps = -1e-4f;
                if (w0 < eps || w1 < eps || w2 < eps) continue;
                const float s = w0 * kParams[tri[0]][0] + w1 * kParams[tri[1]][0] + w2 * kParams[tri[2]][0];
                const float t = w0 * kParams[tri[0]][1] + w1 * kParams[tri[1]][1] + w2 * kParams[tri[2]][1];
                ptex.sample(face, s, t, texel.data());
                const std::size_t i = static_cast<std::size_t>(py) * width + px;
                convertTexel(texel.data(), ptex.channels, ptex.alphaChannel, image.data() + i * channels,
                             channels);
                filled[i] = 1;
            }
        }
    }
}

}  // namespace

std::vector<float> ptexToImage(const PtexImage& ptex, const PtexMesh& mesh, int width, int height,
                               int channels, int padding, double* covered) {
    checkMesh(ptex, mesh);
    if (mesh.uvIds.empty() || mesh.us.empty()) throw PtexError("Ptex: the mesh has no UVs");
    if (width <= 0 || height <= 0 || channels < 1 || channels > 4) throw PtexError("Ptex: bad image size");
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    std::vector<float> image(pixels * channels, 0.0f);
    std::vector<std::uint8_t> filled(pixels, 0);
    std::vector<float> texel(ptex.channels);

    std::size_t corner = 0;
    int face = 0;
    for (const int n : mesh.faceCounts) {
        if (n < 3 || corner + n > mesh.uvIds.size()) {
            corner += std::max(0, n);
            face += facesOf(n);
            continue;
        }
        std::vector<Uv> uv(n);
        bool valid = true;
        for (int k = 0; k < n; ++k) {
            const int id = mesh.uvIds[corner + k];
            valid = valid && id >= 0 && id < static_cast<int>(mesh.us.size()) &&
                    id < static_cast<int>(mesh.vs.size());
            uv[k] = valid ? Uv{mesh.us[id], mesh.vs[id]} : Uv{0.0f, 0.0f};
        }
        if (valid && n == 4) {
            const Uv q[4] = {uv[0], uv[1], uv[2], uv[3]};
            paintQuad(ptex, face, q, width, height, channels, image, filled, texel);
        } else if (valid) {
            // Ptex's sub-faces: corner k's quad runs from the corner (u toward the next edge's middle,
            // v toward the previous edge's) to the polygon's middle.
            Uv centre{0.0f, 0.0f};
            for (const Uv& p : uv) centre = {centre[0] + p[0] / n, centre[1] + p[1] / n};
            auto mid = [](const Uv& a, const Uv& b) {
                return Uv{(a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f};
            };
            for (int k = 0; k < n; ++k) {
                const Uv q[4] = {uv[k], mid(uv[k], uv[(k + 1) % n]), centre, mid(uv[(k + n - 1) % n], uv[k])};
                paintQuad(ptex, face + k, q, width, height, channels, image, filled, texel);
            }
        }
        corner += n;
        face += facesOf(n);
    }

    const std::size_t coveredCount = static_cast<std::size_t>(std::count(filled.begin(), filled.end(), 1));
    if (covered) *covered = pixels ? static_cast<double>(coveredCount) / pixels : 0.0;
    // Padding: each pass gives an empty pixel the mean of its filled neighbours.
    for (int pass = 0; pass < padding; ++pass) {
        std::vector<std::uint8_t> next = filled;
        bool any = false;
        for (int py = 0; py < height; ++py) {
            for (int px = 0; px < width; ++px) {
                const std::size_t i = static_cast<std::size_t>(py) * width + px;
                if (filled[i]) continue;
                float sum[4] = {0, 0, 0, 0};
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int qx = px + dx, qy = py + dy;
                        if (qx < 0 || qy < 0 || qx >= width || qy >= height) continue;
                        const std::size_t j = static_cast<std::size_t>(qy) * width + qx;
                        if (!filled[j]) continue;
                        for (int c = 0; c < channels; ++c) sum[c] += image[j * channels + c];
                        ++count;
                    }
                }
                if (count) {
                    for (int c = 0; c < channels; ++c) image[i * channels + c] = sum[c] / count;
                    next[i] = 1;
                    any = true;
                }
            }
        }
        filled.swap(next);
        if (!any) break;
    }
    // The rest: the mean of what is filled.
    double mean[4] = {0, 0, 0, 0};
    std::size_t n = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        if (!filled[i]) continue;
        for (int c = 0; c < channels; ++c) mean[c] += image[i * channels + c];
        ++n;
    }
    for (std::size_t i = 0; i < pixels; ++i) {
        if (filled[i]) continue;
        for (int c = 0; c < channels; ++c)
            image[i * channels + c] = n ? static_cast<float>(mean[c] / n) : 0.0f;
    }
    return image;
}

std::vector<float> ptexToVertexValues(const PtexImage& ptex, const PtexMesh& mesh, int channels) {
    checkMesh(ptex, mesh);
    if (channels < 1 || channels > 4) throw PtexError("Ptex: bad channel count");
    std::vector<float> out(static_cast<std::size_t>(std::max(0, mesh.vertexCount)) * channels, 0.0f);
    std::vector<int> counts(std::max(0, mesh.vertexCount), 0);
    std::vector<float> texel(ptex.channels), value(channels);
    const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    std::size_t corner = 0;
    int face = 0;
    for (const int n : mesh.faceCounts) {
        for (int k = 0; k < n && corner + k < mesh.vertexIds.size(); ++k) {
            const int vtx = mesh.vertexIds[corner + k];
            if (vtx < 0 || vtx >= mesh.vertexCount) continue;
            // A quad's corner k; another polygon's corner is its sub-face k's first corner.
            if (n == 4)
                ptex.sample(face, corners[k][0], corners[k][1], texel.data());
            else
                ptex.sample(face + k, 0.0f, 0.0f, texel.data());
            convertTexel(texel.data(), ptex.channels, ptex.alphaChannel, value.data(), channels);
            for (int c = 0; c < channels; ++c) out[static_cast<std::size_t>(vtx) * channels + c] += value[c];
            ++counts[vtx];
        }
        corner += std::max(0, n);
        face += facesOf(n);
    }
    for (int v = 0; v < mesh.vertexCount; ++v) {
        if (!counts[v]) continue;
        for (int c = 0; c < channels; ++c) out[static_cast<std::size_t>(v) * channels + c] /= counts[v];
    }
    return out;
}

}  // namespace groomlab
