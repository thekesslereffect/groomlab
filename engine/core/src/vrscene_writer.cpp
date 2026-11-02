// SPDX-License-Identifier: Apache-2.0
#include "groomlab/vrscene_writer.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "groomlab/export_view.h"
#include "groomlab/png.h"
#include "groomlab/version.h"

namespace groomlab {

namespace fs = std::filesystem;

namespace vray {

std::array<float, 3> vrayDyeColor(const std::array<float, 3>& baseColor) {
    std::array<float, 3> absorption = hairColorAbsorption(baseColor);
    for (float& a : absorption) a *= kVRayDyeAbsorption;
    return hairAbsorptionColor(absorption);
}

HairBrdf vrayHairBrdf(const HairLobes& lobes) {
    HairBrdf brdf;
    brdf.glossiness = std::clamp(kVRayGlossiness - kVRayGlossinessPerRoughness * lobes.roughness, 0.0f, 1.0f);
    brdf.softness = std::clamp(kVRaySoftnessScale * lobes.azimuthalRoughness, 0.0f, 1.0f);
    brdf.primaryTint = std::clamp(lobes.specularTint, 0.0f, 1.0f);
    brdf.highlightShift = lobes.shiftDegrees;
    brdf.ior = kVRayIor + (lobes.ior - kHairIor);  // the look's IOR is aiStandardHair's
    return brdf;
}

std::string pluginName(const std::string& name) {
    std::string out;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    if (out.empty()) out = "groom";
    if (out[0] >= '0' && out[0] <= '9') out = "_" + out;
    return out;
}

HairArrays hairArrays(const StrandSet& strands, const ScalpMesh* scalp, float scale, const HairArrays* like) {
    // Base colors (what a hair BSDF takes), per point when they vary along the strands.
    const ExportView view = describeExport(strands, scalp, kExportPointColors);
    const std::vector<float>* opacity = view.pointChannel(kOpacityChannel);
    bool wetPerPoint = false;
    const std::vector<float>* wetness = wetnessChannel(strands, &wetPerPoint);
    HairArrays out;
    out.hasTransparency = like ? like->hasTransparency : opacity != nullptr;
    out.hasUVs = like ? like->hasUVs : view.hasUVs();

    // The strands V-Ray takes: two points or more.
    std::vector<std::size_t> kept;
    std::vector<std::size_t> offsets(view.strandCount() + 1, 0);
    for (std::size_t s = 0; s < view.strandCount(); ++s) {
        offsets[s + 1] = offsets[s] + static_cast<std::size_t>(std::max(0, view.counts[s]));
        if (view.counts[s] >= 2) kept.push_back(s);
    }
    std::size_t pointTotal = 0;
    for (const std::size_t s : kept) pointTotal += static_cast<std::size_t>(view.counts[s]);

    out.counts.reserve(kept.size());
    for (const std::size_t s : kept) out.counts.push_back(view.counts[s]);
    out.xyz.reserve(3 * pointTotal);
    out.widths.reserve(pointTotal);
    out.colors.reserve(3 * pointTotal);
    if (out.hasTransparency) out.transparency.reserve(3 * pointTotal);
    const std::vector<Vec3f>& positions = strands.positions();
    const std::array<float, 3> fallback = defaultHairColor();
    for (const std::size_t s : kept) {
        for (std::size_t p = offsets[s]; p < offsets[s + 1]; ++p) {
            out.xyz.push_back(positions[p].x * scale);
            out.xyz.push_back(positions[p].y * scale);
            out.xyz.push_back(positions[p].z * scale);
            out.widths.push_back(view.radius(p) * scale);
            std::array<float, 3> c = view.hasPointColors() ? view.pointColors[p] : view.hasColors() ? view.colors[s] : fallback;
            const float wet = wetness ? (*wetness)[wetPerPoint ? p : s] : 0.0f;
            if (wet > 0.0f) {  // the fiber's absorption up (hair_color.h wetShading)
                std::array<float, 3> a = hairColorAbsorption(c);
                for (float& v : a) v *= wetShading(wet).absorption;
                c = hairAbsorptionColor(a);
            }
            c = vrayDyeColor(c);
            out.colors.insert(out.colors.end(), c.begin(), c.end());
            if (out.hasTransparency) {
                const float t = opacity ? 1.0f - std::clamp((*opacity)[p], 0.0f, 1.0f) : 0.0f;
                out.transparency.insert(out.transparency.end(), {t, t, t});
            }
        }
    }
    if (out.hasUVs) {
        out.uvw.reserve(3 * kept.size());
        for (const std::size_t s : kept) {
            const std::array<float, 2> uv = view.hasUVs() ? view.rootUVs[s] : std::array<float, 2>{0.0f, 0.0f};
            out.uvw.insert(out.uvw.end(), {uv[0], uv[1], 0.0f});
        }
    }
    return out;
}

double matrixScale(const double* m) {
    if (!m) return 1.0;
    const double det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
                       m[2] * (m[4] * m[9] - m[5] * m[8]);
    // A uniform scale: an axis's length (sqrt is exact on every platform; glibc's cbrt(27) is not
    // 3, which left 0.9999999999999999 in the Node's matrix on Linux); otherwise the mean scale.
    const double ax = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    const double ay = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    const double az = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    const bool uniform = std::abs(ax - ay) <= 1e-12 * ax && std::abs(ax - az) <= 1e-12 * ax;
    const double k = uniform ? ax : std::cbrt(std::abs(det));
    return k > 1e-12 && std::abs(k - 1.0) > 1e-9 ? k : 1.0;
}

}  // namespace vray

namespace {

void appendNumber(std::string& s, double v) {
    if (!std::isfinite(v)) v = 0.0;
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    s.append(buf, r.ptr);
}

void appendFloat(std::string& s, float v) {
    if (!std::isfinite(v)) v = 0.0f;
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    s.append(buf, r.ptr);
}

std::string hexBytes(const std::uint8_t* data, std::size_t n) {
    static const char* digits = "0123456789ABCDEF";
    std::string out(2 * n, '0');
    for (std::size_t i = 0; i < n; ++i) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 15];
    }
    return out;
}

std::string base64(const std::vector<std::uint8_t>& data) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const std::uint32_t v = (std::uint32_t(data[i]) << 16) | (std::uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += table[v & 63];
    }
    if (i < data.size()) {
        const bool two = i + 1 < data.size();
        const std::uint32_t v = (std::uint32_t(data[i]) << 16) | (two ? std::uint32_t(data[i + 1]) << 8 : 0u);
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += two ? table[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

void putLE32(std::uint8_t* at, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) at[k] = static_cast<std::uint8_t>(v >> (8 * k));
}

// A list of 32-bit values (ints or floats, little-endian) as V-Ray's packed list: `kind` is
// "Int", "Float", "Vector" or "Color" (three floats an item). Compressed: "ZIPC", the raw and the
// compressed sizes (each 4 bytes little-endian, in hex), then the zlib stream in base64; else the
// bytes in hex.
std::string packedList(const char* kind, const std::vector<std::uint8_t>& bytes, bool compress) {
    std::string text = std::string("List") + kind + "Hex(\"";
    if (compress && !bytes.empty()) {
        const std::vector<std::uint8_t> z = zlibCompress(bytes);
        std::uint8_t sizes[8];
        putLE32(sizes, static_cast<std::uint32_t>(bytes.size()));
        putLE32(sizes + 4, static_cast<std::uint32_t>(z.size()));
        text += "ZIPC" + hexBytes(sizes, 8) + base64(z);
    } else {
        text += hexBytes(bytes.data(), bytes.size());
    }
    text += "\")";
    return text;
}

template <class T>
std::vector<std::uint8_t> rawBytes(const std::vector<T>& values) {
    static_assert(sizeof(T) == 4, "32-bit values");
    std::vector<std::uint8_t> bytes(values.size() * 4);
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::uint32_t v = 0;
        std::memcpy(&v, &values[i], 4);
        putLE32(bytes.data() + 4 * i, v);
    }
    return bytes;
}

std::string transformText(const double* m) {
    static const double identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (!m) m = identity;
    std::string s = "Transform(Matrix(";
    for (int row = 0; row < 3; ++row) {  // V-Ray's Matrix takes the images of the axes: Maya's rows
        s += row ? ", Vector(" : "Vector(";
        for (int k = 0; k < 3; ++k) {
            if (k) s += ", ";
            appendNumber(s, m[4 * row + k]);
        }
        s += ")";
    }
    s += "), Vector(";
    for (int k = 0; k < 3; ++k) {
        if (k) s += ", ";
        appendNumber(s, m[12 + k]);
    }
    s += "))";
    return s;
}

// One parameter of a plugin over the written frames: the first frame's value held, the frames
// whose value differs from it spooled to a file, so a value that never changes is written once
// and one that does as interpolate() keys.
struct Param {
    std::string plugin;  // which plugin's (an index into the file's plugins, by name)
    std::string name;
    std::string first;
    std::vector<std::pair<double, std::size_t>> keys;  // frame, length in the spool (0: the first's value)
    fs::path spoolPath;
    std::ofstream spool;
    bool animated = false;

    void put(double frame, std::string text) {
        if (keys.empty()) {
            first = std::move(text);
            keys.emplace_back(frame, 0);
            return;
        }
        if (text == first) {
            keys.emplace_back(frame, 0);
            return;
        }
        animated = true;
        if (!spool.is_open()) spool.open(spoolPath, std::ios::binary | std::ios::trunc);
        spool.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!spool) throw VrsceneError("cannot write '" + spoolPath.u8string() + "'");
        keys.emplace_back(frame, text.size());
    }
};

}  // namespace

struct VrsceneWriter::Impl {
    fs::path path;
    fs::path tmp;
    std::string name;
    Options options;
    std::vector<std::unique_ptr<Param>> params;
    bool hasUVs = false;
    bool hasTransparency = false;
    std::size_t frames = 0;
    double firstFrame = 0.0, lastFrame = 0.0;
    std::size_t strands = 0, points = 0;  // the first frame's
    bool closed = false;

    Param& param(const std::string& plugin, const std::string& paramName) {
        for (auto& p : params) {
            if (p->plugin == plugin && p->name == paramName) return *p;
        }
        auto p = std::make_unique<Param>();
        p->plugin = plugin;
        p->name = paramName;
        p->spoolPath = path;
        p->spoolPath += ".spool" + std::to_string(params.size());
        params.push_back(std::move(p));
        return *params.back();
    }

    void removeSpools() {
        for (auto& p : params) {
            if (p->spool.is_open()) p->spool.close();
            std::error_code ec;
            fs::remove(p->spoolPath, ec);
        }
    }
};

VrsceneWriter::VrsceneWriter(const std::string& path, const std::string& name, const Options& options)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = fs::u8path(path);
    impl_->tmp = tempPathFor(impl_->path);
    impl_->name = vray::pluginName(name);
    impl_->options = options;
    std::ofstream probe(impl_->tmp, std::ios::binary | std::ios::trunc);
    if (!probe) throw VrsceneError("cannot write '" + path + "'");
}

VrsceneWriter::~VrsceneWriter() {
    try {
        close();
    } catch (...) {
        impl_->removeSpools();
        std::error_code ec;
        fs::remove(impl_->tmp, ec);
    }
}

std::size_t VrsceneWriter::frameCount() const { return impl_->frames; }

void VrsceneWriter::setLobes(const HairLobes& lobes) { impl_->options.lobes = lobes; }

void VrsceneWriter::writeFrame(double frame, const StrandSet& strands, const ScalpMesh* scalp, const double* objectToWorld) {
    Impl& d = *impl_;
    if (d.closed) throw VrsceneError("writeFrame after close");
    if (d.frames > 0 && frame <= d.lastFrame) throw VrsceneError("frames must increase");

    // The transform's own scale goes into the points and radii (V-Ray does not scale hair widths
    // by a Node's transform): the Node keeps the rotation and translation (a scale that differs per
    // axis keeps the rest of it; the widths then take the mean).
    double nodeMatrix[16];
    const double k = vray::matrixScale(objectToWorld);
    if (objectToWorld) {
        std::copy(objectToWorld, objectToWorld + 16, nodeMatrix);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) nodeMatrix[4 * row + col] /= k;
        }
    }
    // Every frame keeps the first's transparency and UVs (zeros where a frame lacks them).
    vray::HairArrays like;
    like.hasTransparency = d.hasTransparency;
    like.hasUVs = d.hasUVs;
    const vray::HairArrays a =
        vray::hairArrays(strands, scalp, static_cast<float>(d.options.scale * k), d.frames > 0 ? &like : nullptr);
    if (d.frames == 0) {
        d.firstFrame = frame;
        d.hasUVs = a.hasUVs;
        d.hasTransparency = a.hasTransparency;
        d.strands = a.counts.size();
        d.points = a.widths.size();
    }

    const bool z = d.options.compress;
    const std::string hair = d.name + "@hair";
    d.param(hair, "num_hair_vertices").put(frame, packedList("Int", rawBytes(a.counts), z));
    d.param(hair, "hair_vertices").put(frame, packedList("Vector", rawBytes(a.xyz), z));
    d.param(hair, "widths").put(frame, packedList("Float", rawBytes(a.widths), z));
    d.param(hair, "colors").put(frame, packedList("Color", rawBytes(a.colors), z));
    if (a.hasTransparency) d.param(hair, "transparency").put(frame, packedList("Color", rawBytes(a.transparency), z));
    if (a.hasUVs) d.param(hair, "strand_uvw").put(frame, packedList("Vector", rawBytes(a.uvw), z));
    d.param(d.name + "@node", "transform").put(frame, transformText(objectToWorld ? nodeMatrix : nullptr));

    d.lastFrame = frame;
    ++d.frames;
}

void VrsceneWriter::close() {
    Impl& d = *impl_;
    if (d.closed) return;
    d.closed = true;

    std::ofstream out(d.tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
        d.removeSpools();
        throw VrsceneError("cannot write '" + d.path.u8string() + "'");
    }
    for (auto& p : d.params) {
        if (p->spool.is_open()) p->spool.close();
    }

    const std::string& n = d.name;
    std::string head = "// GroomLab " + std::string(kVersionString) + " V-Ray scene";
    if (!d.options.comment.empty()) {
        std::string comment = d.options.comment;
        std::replace(comment.begin(), comment.end(), '\n', ' ');
        std::replace(comment.begin(), comment.end(), '\r', ' ');
        head += ": " + comment;
    }
    head += "\n// " + std::to_string(d.strands) + " strands, " + std::to_string(d.points) + " points";
    if (d.frames > 0) {
        head += ", frames ";
        appendNumber(head, d.firstFrame);
        if (d.frames > 1) {
            head += " to ";
            appendNumber(head, d.lastFrame);
        }
    }
    head += "\n\nSettingsUnitsInfo " + n + "@units {\n  meters_scale=";
    appendNumber(head, d.options.metersPerUnit);
    head += ";\n  scene_upDir=" + std::string(d.options.upAxis == "Z" ? "Vector(0, 0, 1)" : "Vector(0, 1, 0)") + ";\n  frames_scale=";
    appendNumber(head, d.options.framesPerSecond);
    head += ";\n}\n\n";
    out << head;

    // A parameter's value: as it is, or its keys.
    std::vector<char> block;
    auto writeParam = [&](Param& p) {
        out << "  " << p.name << "=";
        if (!p.animated) {
            out << p.first << ";\n";
            return;
        }
        out << "interpolate(";
        std::ifstream in(p.spoolPath, std::ios::binary);
        for (std::size_t k = 0; k < p.keys.size(); ++k) {
            std::string frame;
            appendNumber(frame, p.keys[k].first);
            out << (k ? ",\n    (" : "\n    (") << frame << ", ";
            if (p.keys[k].second == 0) {
                out << p.first;
            } else {
                block.resize(p.keys[k].second);
                in.read(block.data(), static_cast<std::streamsize>(block.size()));
                out.write(block.data(), in.gcount());
            }
            out << ")";
        }
        out << ");\n";
    };
    auto writePlugin = [&](const std::string& type, const std::string& plugin, const std::string& fixed) {
        out << type << " " << plugin << " {\n";
        for (auto& p : d.params) {
            if (p->plugin == plugin) writeParam(*p);
        }
        out << fixed << "}\n\n";
    };

    std::string hairFixed = "  geom_splines=1;\n  min_pixel_width=";
    appendFloat(hairFixed, std::max(0.0f, d.options.minPixelWidth));
    hairFixed += ";\n";
    writePlugin("GeomMayaHair", n + "@hair", hairFixed);

    out << "TexHairSampler " << n << "@hairSampler {\n}\n\n";
    const vray::HairBrdf b = vray::vrayHairBrdf(d.options.lobes);
    std::string brdf = "BRDFHair4 " + n + "@hairBrdf {\n  melanin=0;\n  pheomelanin=0;\n  dye_color=" + n +
                       "@hairSampler::hair_color;\n  glossiness=";
    appendFloat(brdf, b.glossiness);
    brdf += ";\n  softness=";
    appendFloat(brdf, b.softness);
    brdf += ";\n  primary_glossiness_boost=0;\n  primary_tint=AColor(";
    for (int k = 0; k < 3; ++k) {
        appendFloat(brdf, b.primaryTint);
        brdf += ", ";
    }
    brdf += "1);\n  highlight_shift=";
    appendFloat(brdf, b.highlightShift);
    brdf += ";\n  ior=";
    appendFloat(brdf, b.ior);
    brdf += ";\n  trace_depth=" + std::to_string(b.traceDepth) + ";\n  glint_strength=0;\n  glitter_strength=0;\n";
    if (d.hasTransparency) brdf += "  transparency=" + n + "@hairSampler::hair_transparency;\n";
    brdf += "}\n\n";
    out << brdf;
    out << "MtlSingleBRDF " << n << "@hairMtl {\n  brdf=" << n << "@hairBrdf;\n}\n\n";
    // (No nsamples: V-Ray's default takes SettingsMotionBlur's samples; 1 would keep the hair still,
    // its keys and all.)
    writePlugin("Node", n + "@node", "  geometry=" + n + "@hair;\n  material=" + n + "@hairMtl;\n  visible=1;\n");

    out.close();
    const bool ok = static_cast<bool>(out);
    d.removeSpools();
    if (!ok) {
        std::error_code ec;
        fs::remove(d.tmp, ec);
        throw VrsceneError("cannot write '" + d.path.u8string() + "'");
    }
    try {
        replaceFile(d.tmp, d.path);
    } catch (const std::runtime_error& e) {
        throw VrsceneError(e.what());
    }
}

}  // namespace groomlab
