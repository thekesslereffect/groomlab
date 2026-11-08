// SPDX-License-Identifier: Apache-2.0
#include "groomlab/usd_writer.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

#include "groomlab/export_view.h"
#include "groomlab/parallel.h"
#include "groomlab/version.h"

namespace groomlab {

namespace fs = std::filesystem;

std::string usdIdentifier(const std::string& name) {
    std::string out;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    if (out.empty() || (out[0] >= '0' && out[0] <= '9')) out = "_" + out;
    return out;
}

namespace {

// Appends numbers to a text buffer in USD's syntax, shortest round-trip form.
class TextBuffer {
public:
    void number(float v) {
        if (!std::isfinite(v)) v = 0.0f;
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof(buf), v);
        text_.append(buf, r.ptr);
    }
    void number(double v) {
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof(buf), v);
        text_.append(buf, r.ptr);
    }
    template <class Int>
    void integer(Int v) {
        char buf[24];
        const auto r = std::to_chars(buf, buf + sizeof(buf), v);
        text_.append(buf, r.ptr);
    }
    void raw(const char* s) { text_ += s; }
    void raw(const std::string& s) { text_ += s; }
    void reserve(std::size_t bytes) { text_.reserve(bytes); }
    void vec3(const Vec3f& p) {
        text_ += '(';
        number(p.x);
        text_ += ", ";
        number(p.y);
        text_ += ", ";
        number(p.z);
        text_ += ')';
    }
    void vec2(const std::array<float, 2>& p) {
        text_ += '(';
        number(p[0]);
        text_ += ", ";
        number(p[1]);
        text_ += ')';
    }
    void color(const std::array<float, 3>& c) { vec3({c[0], c[1], c[2]}); }
    void quaternion(const UsdOrientation& r) {
        text_ += '(';
        number(r.w);
        text_ += ", ";
        number(r.x);
        text_ += ", ";
        number(r.y);
        text_ += ", ";
        number(r.z);
        text_ += ')';
    }
    std::string& str() { return text_; }

private:
    std::string text_;
};

// Text for inside a quoted USD string (a doc): quotes become apostrophes, and
// backslashes and line breaks, which would escape or end the string, spaces.
std::string usdDocText(std::string text) {
    for (char& c : text) {
        if (c == '"') c = '\'';
        if (c == '\\' || c == '\n' || c == '\r') c = ' ';
    }
    return text;
}

// One time-sampled attribute: its declaration and a spool file holding
// "frame: value," entries until the layer is assembled.
struct SampledAttribute {
    std::string declaration;  // e.g. "point3f[] points"
    std::string metadata;     // e.g. "interpolation = \"vertex\""; empty: none
    fs::path spoolPath;
    std::ofstream spool;
    // The first frame's entries, held (an export of one frame writes its file once, never spooled),
    // spooled when a second frame comes.
    std::string held;
    void put(const std::string& text, bool first) {
        if (first) {
            held += text;
        } else {
            spool << text;
        }
    }
};

// A prototype's geometry as a Mesh prim's body (static), `indent` before each line.
std::string prototypeMeshBody(const InstancePrototype& mesh, const std::string& indent) {
    TextBuffer b;
    b.raw(indent + "uniform token subdivisionScheme = \"none\"\n");
    b.raw(indent + "int[] faceVertexCounts = [");
    for (std::size_t t = 0; t < mesh.triangles.size() / 3; ++t) b.raw(t ? ", 3" : "3");
    b.raw("]\n" + indent + "int[] faceVertexIndices = [");
    for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
        if (i) b.raw(", ");
        b.integer(mesh.triangles[i]);
    }
    Vec3f lo = mesh.points.empty() ? Vec3f{} : mesh.points[0], hi = lo;
    b.raw("]\n" + indent + "point3f[] points = [");
    for (std::size_t i = 0; i < mesh.points.size(); ++i) {
        if (i) b.raw(", ");
        b.vec3(mesh.points[i]);
        lo = {std::min(lo.x, mesh.points[i].x), std::min(lo.y, mesh.points[i].y), std::min(lo.z, mesh.points[i].z)};
        hi = {std::max(hi.x, mesh.points[i].x), std::max(hi.y, mesh.points[i].y), std::max(hi.z, mesh.points[i].z)};
    }
    b.raw("]\n" + indent + "float3[] extent = [");
    b.vec3(lo);
    b.raw(", ");
    b.vec3(hi);
    b.raw("]\n");
    if (mesh.normals.size() == mesh.points.size()) {
        b.raw(indent + "normal3f[] normals = [");
        for (std::size_t i = 0; i < mesh.normals.size(); ++i) {
            if (i) b.raw(", ");
            b.vec3(mesh.normals[i]);
        }
        b.raw("] (\n" + indent + "    interpolation = \"vertex\"\n" + indent + ")\n");
    }
    if (mesh.uvs.size() == mesh.points.size()) {
        b.raw(indent + "texCoord2f[] primvars:st = [");
        for (std::size_t i = 0; i < mesh.uvs.size(); ++i) {
            if (i) b.raw(", ");
            b.vec2(mesh.uvs[i]);
        }
        b.raw("] (\n" + indent + "    interpolation = \"vertex\"\n" + indent + ")\n");
    }
    return std::move(b.str());
}

}  // namespace

double usdMetersPerUnit(const std::string& unit) {
    if (unit == "m") return 1.0;
    if (unit == "mm") return 0.001;
    if (unit == "in") return 0.0254;
    if (unit == "ft") return 0.3048;
    return 0.01;
}

UsdOrientation usdOrientation(const Vec3f& x, const Vec3f& y, const Vec3f& z) {
    // Rows are the images of the axes (row vectors, as USD): the rotation is their transpose,
    // each axis by its own length (a strand-fitted instance is longer along +Y), which
    // becomes the per-axis scale (USD scales, then orients: the axes stay at right angles).
    const Vec3f s{x.length() > 0.0f ? x.length() : 1.0f, y.length() > 0.0f ? y.length() : 1.0f,
                  z.length() > 0.0f ? z.length() : 1.0f};
    const Vec3f a = x / s.x, b = y / s.y, c = z / s.z;
    const float m00 = a.x, m01 = b.x, m02 = c.x, m10 = a.y, m11 = b.y, m12 = c.y, m20 = a.z, m21 = b.z, m22 = c.z;
    const float trace = m00 + m11 + m22;
    UsdOrientation r;
    r.scale = s;
    if (trace > 0.0f) {
        const float k = 0.5f / std::sqrt(trace + 1.0f);
        r.w = 0.25f / k, r.x = (m21 - m12) * k, r.y = (m02 - m20) * k, r.z = (m10 - m01) * k;
    } else if (m00 > m11 && m00 > m22) {
        const float k = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
        r.w = (m21 - m12) / k, r.x = 0.25f * k, r.y = (m01 + m10) / k, r.z = (m02 + m20) / k;
    } else if (m11 > m22) {
        const float k = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
        r.w = (m02 - m20) / k, r.x = (m01 + m10) / k, r.y = 0.25f * k, r.z = (m12 + m21) / k;
    } else {
        const float k = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
        r.w = (m10 - m01) / k, r.x = (m02 + m20) / k, r.y = (m12 + m21) / k, r.z = 0.25f * k;
    }
    return r;
}

struct UsdCurvesWriter::Impl {
    fs::path path;
    fs::path tmp;  // written here, then moved over `path` on close
    std::string name;
    Options options;
    std::vector<std::unique_ptr<SampledAttribute>> attributes;  // fixed order once the first frame is written
    std::vector<std::string> strandChannels;  // channel names, from the first frame
    std::vector<std::string> pointChannels;
    bool hasUVs = false;
    bool hasColors = false;  // primvars:displayColor (strandColors: HairColor's RGB, else from melanin)
    bool unrealClumps = false, unrealRoughness = false;  // primvars:groom_clump_id, groom_roughness
    // Instances (Instance node): a PointInstancer beside the curves, its
    // prototypes placeholders to reference geometry into.
    std::vector<std::string> prototypes;
    std::size_t instanceStart = 0;  // index of the first instancer attribute in `attributes`
    // Scales (the first frame's): their mesh's attributes from scaleStart, their instancer's
    // from scaleInstanceStart, the instancer's prototypes (the scales' InstanceSet's, all of them).
    bool scaleMesh = false, scaleInstancer = false, hideStrands = false;
    std::vector<std::string> scalePrototypes;
    std::size_t scaleStart = 0, scaleInstanceStart = 0;
    std::size_t frames = 0;
    double firstFrame = 0.0;
    double lastFrame = 0.0;
    bool closed = false;

    SampledAttribute& add(const std::string& declaration, const std::string& metadata = {}) {
        auto a = std::make_unique<SampledAttribute>();
        a->declaration = declaration;
        a->metadata = metadata;
        a->spoolPath = path;
        a->spoolPath += ".spool" + std::to_string(attributes.size());
        a->spool.open(a->spoolPath, std::ios::binary | std::ios::trunc);
        if (!a->spool) throw UsdError("cannot write '" + a->spoolPath.u8string() + "'");
        attributes.push_back(std::move(a));
        return *attributes.back();
    }

    void removeSpools() {
        for (auto& a : attributes) {
            a->spool.close();
            std::error_code ec;
            fs::remove(a->spoolPath, ec);
        }
    }

    void removeTemp() {
        std::error_code ec;
        fs::remove(tmp, ec);
    }
};

UsdCurvesWriter::UsdCurvesWriter(const std::string& path, const std::string& name, const Options& options)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = fs::u8path(path);
    impl_->tmp = tempPathFor(impl_->path);
    impl_->name = usdIdentifier(name.empty() ? "groom" : name);
    impl_->options = options;
    // Fail now, not after every frame; the target itself is untouched until close().
    std::ofstream probe(impl_->tmp, std::ios::binary | std::ios::trunc);
    if (!probe) throw UsdError("cannot write '" + path + "'");
}

UsdCurvesWriter::~UsdCurvesWriter() {
    try {
        close();
    } catch (...) {
        impl_->removeSpools();
        impl_->removeTemp();
    }
}

std::size_t UsdCurvesWriter::frameCount() const { return impl_->frames; }

void UsdCurvesWriter::writeFrame(double frame, const StrandSet& strands, const ScalpMesh* scalp, const ScaleMesh* scales) {
    Impl& d = *impl_;
    if (d.closed) throw UsdError("writeFrame after close");
    if (d.frames > 0 && frame <= d.lastFrame) throw UsdError("frames must increase");

    // displayColor: the hair's appearance (no per-point colors: not written).
    const ExportView view = describeExport(strands, scalp, kExportBounds | kExportAppearanceColors);
    if (d.frames == 0) {
        d.firstFrame = frame;
        d.hasUVs = view.hasUVs();
        d.hasColors = view.hasColors();
        d.strandChannels = view.strandChannels;
        d.pointChannels = view.pointChannels;
        d.add("int[] curveVertexCounts");
        d.add("point3f[] points");
        d.add("float3[] extent");
        d.add("float[] widths", "interpolation = \"vertex\"");
        d.add("int64[] primvars:strandId", "interpolation = \"uniform\"");
        if (d.hasUVs) d.add("texCoord2f[] primvars:st", "interpolation = \"uniform\"");
        if (d.hasColors) d.add("color3f[] primvars:displayColor", "interpolation = \"uniform\"");
        // One namespace for both kinds of channel: no two primvars share a name.
        std::vector<std::string> channels = d.strandChannels;
        channels.insert(channels.end(), d.pointChannels.begin(), d.pointChannels.end());
        const std::vector<std::string> names = usdChannelNames(channels);
        for (std::size_t i = 0; i < d.strandChannels.size(); ++i) {
            d.add("float[] primvars:" + names[i], "interpolation = \"uniform\"");
        }
        for (std::size_t i = 0; i < d.pointChannels.size(); ++i) {
            d.add("float[] primvars:" + names[d.strandChannels.size() + i], "interpolation = \"vertex\"");
        }
        // Unreal's groom attributes: its USD import reads the primvars named groom_* of a curves
        // prim with its GroomAPI (USDGroomConversion; its scalar types: int, float, float2, float3).
        d.add("int[] primvars:groom_id", "interpolation = \"uniform\"");
        if (d.hasUVs) d.add("float2[] primvars:groom_root_uv", "interpolation = \"uniform\"");
        if (d.hasColors) d.add("float3[] primvars:groom_color", "interpolation = \"uniform\"");
        d.unrealClumps = view.strandChannel(kClumpIdChannel) != nullptr;
        d.unrealRoughness = view.strandChannel(kRoughnessChannel) != nullptr;
        if (d.unrealClumps) d.add("int[] primvars:groom_clump_id", "interpolation = \"uniform\"");
        if (d.unrealRoughness) d.add("float[] primvars:groom_roughness", "interpolation = \"uniform\"");
        d.instanceStart = d.attributes.size();
        const InstanceSet* set = strands.instances().get();
        if (set && std::any_of(set->instances.begin(), set->instances.end(), [](const InstanceSet::Instance& i) { return !i.meshed(); })) {
            d.prototypes = set->prototypes;
            d.add("int[] protoIndices");
            d.add("point3f[] positions");
            d.add("quath[] orientations");
            d.add("float3[] scales");
            d.add("int64[] ids");
        }
        d.hideStrands = set && set->hidesStrands;
        d.scaleStart = d.attributes.size();
        const UsdCurvesWriter::Options::Scales mode = d.options.scales;
        using Mode = UsdCurvesWriter::Options::Scales;
        if (scales && !scales->scales.empty() && set) {
            d.scaleMesh = mode == Mode::Mesh || mode == Mode::Both;
            d.scaleInstancer = mode == Mode::Instancer || mode == Mode::Both;
        }
        if (d.scaleMesh) {
            d.add("int[] faceVertexCounts");
            d.add("int[] faceVertexIndices");
            d.add("point3f[] points");
            d.add("float3[] extent");
            d.add("normal3f[] normals", "interpolation = \"vertex\"");
            d.add("texCoord2f[] primvars:st", "interpolation = \"vertex\"");
            d.add("color3f[] primvars:displayColor", "interpolation = \"vertex\"");
            d.add("color3f[] primvars:scaleColor", "interpolation = \"vertex\"");
            d.add("color3f[] primvars:scaleRootColor", "interpolation = \"uniform\"");
            d.add("float[] primvars:scaleId", "interpolation = \"uniform\"");
            d.add("float2[] primvars:scaleUV", "interpolation = \"uniform\"");
            d.add("int[] primvars:scaleVariant", "interpolation = \"uniform\"");
        }
        d.scaleInstanceStart = d.attributes.size();
        if (d.scaleInstancer) {
            d.scalePrototypes = set->prototypes;
            d.add("int[] protoIndices");
            d.add("point3f[] positions");
            d.add("quath[] orientations");
            d.add("float3[] scales");
            d.add("int64[] ids");
            // Per instance (a PointInstancer's vertex primvars): its root's color, as the mesh's
            // per-face scaleRootColor (one color: a plate is not shaded along its strand).
            d.add("color3f[] primvars:scaleRootColor", "interpolation = \"vertex\"");
            d.add("float[] primvars:scaleId", "interpolation = \"vertex\"");
            d.add("float2[] primvars:scaleUV", "interpolation = \"vertex\"");
            d.add("int[] primvars:scaleVariant", "interpolation = \"vertex\"");
        }
    }
    d.lastFrame = frame;
    ++d.frames;
    const bool holding = d.frames == 1;  // the first frame: held (SampledAttribute::put)
    if (d.frames == 2) {  // the first frame's entries spooled ahead of this one's
        for (auto& a : d.attributes) {
            a->spool << a->held;
            std::string().swap(a->held);
            if (!a->spool) throw UsdError("cannot write '" + a->spoolPath.u8string() + "'");
        }
    }

    const std::size_t strandCount = strands.strandCount();
    const std::size_t pointCount = strands.pointCount();
    std::size_t next = 0;
    auto emit = [&](const auto& writeValue) {
        TextBuffer b;
        b.raw("        ");
        b.number(frame);
        b.raw(": [");
        writeValue(b);
        b.raw("],\n");
        SampledAttribute& a = *d.attributes[next++];
        a.put(b.str(), holding);
        if (!a.spool) throw UsdError("cannot write '" + a.spoolPath.u8string() + "'");
    };
    auto list = [](TextBuffer& b, std::size_t count, const auto& item) {
        for (std::size_t i = 0; i < count; ++i) {
            if (i) b.raw(", ");
            item(i);
        }
    };
    // A list as `emit` writes it, its items formatted side by side in runs of `kRun` (each into
    // its own text) and written in order, a batch of runs at a time: the same text, and never
    // more than a batch of it in memory.
    constexpr std::size_t kRun = 16384, kBatch = 64;
    auto emitList = [&](std::size_t count, const auto& item) {  // item(TextBuffer&, i)
        SampledAttribute& a = *d.attributes[next++];
        TextBuffer head;
        head.raw("        ");
        head.number(frame);
        head.raw(": [");
        a.put(head.str(), holding);
        std::vector<std::string> runs;
        for (std::size_t first = 0; first < count; first += kRun * kBatch) {
            const std::size_t last = std::min(count, first + kRun * kBatch);
            runs.assign((last - first + kRun - 1) / kRun, std::string());
            parallelFor(0, runs.size(), [&](std::size_t r) {
                TextBuffer b;
                const std::size_t begin = first + r * kRun, end = std::min(last, begin + kRun);
                b.reserve((end - begin) * 24);
                for (std::size_t i = begin; i < end; ++i) {
                    if (i) b.raw(", ");
                    item(b, i);
                }
                runs[r] = std::move(b.str());
            }, 1);
            for (const std::string& run : runs) a.put(run, holding);
        }
        a.put("],\n", holding);
        if (!a.spool) throw UsdError("cannot write '" + a.spoolPath.u8string() + "'");
    };

    emitList(strandCount, [&](TextBuffer& b, std::size_t s) { b.integer(view.counts[s]); });
    const std::vector<Vec3f>& positions = strands.positions();
    emitList(pointCount, [&](TextBuffer& b, std::size_t i) { b.vec3(positions[i]); });
    emit([&](TextBuffer& b) {
        if (pointCount == 0) return;
        // The radii pad the bounds so renderers don't cull strand edges.
        b.vec3(view.boundsLo);
        b.raw(", ");
        b.vec3(view.boundsHi);
    });
    emitList(pointCount, [&](TextBuffer& b, std::size_t i) { b.number(view.width(i)); });
    const std::vector<StrandId>& ids = strands.ids();
    emitList(strandCount, [&](TextBuffer& b, std::size_t s) { b.integer(static_cast<long long>(ids[s])); });
    if (d.hasUVs) {
        emitList(strandCount, [&](TextBuffer& b, std::size_t s) {
            const std::array<float, 2> uv = s < view.rootUVs.size() ? view.rootUVs[s] : std::array<float, 2>{0.0f, 0.0f};
            b.raw("(");
            b.number(uv[0]);
            b.raw(", ");
            b.number(uv[1]);
            b.raw(")");
        });
    }
    if (d.hasColors) {
        emitList(strandCount, [&](TextBuffer& b, std::size_t s) {
            const std::array<float, 3> c = s < view.colors.size() ? view.colors[s] : std::array<float, 3>{0.0f, 0.0f, 0.0f};
            b.vec3({c[0], c[1], c[2]});
        });
    }
    // The channels of the first frame, in its order; a frame missing one writes zeros.
    for (const std::string& c : d.strandChannels) {
        const std::vector<float>* values = view.strandChannel(c);
        emitList(strandCount, [&](TextBuffer& b, std::size_t s) { b.number(values ? (*values)[s] : 0.0f); });
    }
    for (const std::string& c : d.pointChannels) {
        const std::vector<float>* values = view.pointChannel(c);
        emitList(pointCount, [&](TextBuffer& b, std::size_t i) { b.number(values ? (*values)[i] : 0.0f); });
    }
    // Unreal's: groom_id a dense index (a non-negative int, as the Unreal Alembic export's).
    emitList(strandCount, [&](TextBuffer& b, std::size_t s) { b.integer(static_cast<long long>(s)); });
    if (d.hasUVs) {
        emitList(strandCount, [&](TextBuffer& b, std::size_t s) {
            const std::array<float, 2> uv = s < view.rootUVs.size() ? view.rootUVs[s] : std::array<float, 2>{0.0f, 0.0f};
            b.raw("(");
            b.number(uv[0]);
            b.raw(", ");
            b.number(uv[1]);
            b.raw(")");
        });
    }
    if (d.hasColors) {
        emitList(strandCount, [&](TextBuffer& b, std::size_t s) {
            const std::array<float, 3> c = s < view.colors.size() ? view.colors[s] : std::array<float, 3>{0.0f, 0.0f, 0.0f};
            b.vec3({c[0], c[1], c[2]});
        });
    }
    if (d.unrealClumps) {
        const std::vector<float>* clumps = view.strandChannel(kClumpIdChannel);
        emitList(strandCount, [&](TextBuffer& b, std::size_t s) {
            b.integer(clumps ? static_cast<long long>(std::lround((*clumps)[s])) : -1LL);
        });
    }
    if (d.unrealRoughness) {
        const std::vector<float>* rough = view.strandChannel(kRoughnessChannel);
        emitList(strandCount, [&](TextBuffer& b, std::size_t s) { b.number(rough ? std::clamp((*rough)[s], 0.0f, 1.0f) : 0.0f); });
    }
    if (!d.prototypes.empty()) {
        // Only prototypes known from the first frame; others are left out (and the scales:
        // their own prims).
        std::vector<InstanceSet::Placed> placed;
        if (strands.instances()) {
            for (const auto& p : strands.instances()->resolve(strands)) {
                if (!p.meshed && p.prototype < d.prototypes.size() &&
                    strands.instances()->prototypes[p.prototype] == d.prototypes[p.prototype]) {
                    placed.push_back(p);
                }
            }
        }
        std::vector<UsdOrientation> frames(placed.size());
        for (std::size_t i = 0; i < placed.size(); ++i) frames[i] = usdOrientation(placed[i].x, placed[i].y, placed[i].z);
        emit([&](TextBuffer& b) { list(b, placed.size(), [&](std::size_t i) { b.integer(static_cast<long long>(placed[i].prototype)); }); });
        emit([&](TextBuffer& b) { list(b, placed.size(), [&](std::size_t i) { b.vec3(placed[i].origin); }); });
        emit([&](TextBuffer& b) { list(b, placed.size(), [&](std::size_t i) { b.quaternion(frames[i]); }); });
        emit([&](TextBuffer& b) { list(b, placed.size(), [&](std::size_t i) { b.vec3(frames[i].scale); }); });
        emit([&](TextBuffer& b) { list(b, placed.size(), [&](std::size_t i) { b.integer(static_cast<long long>(placed[i].id)); }); });
    }
    // A frame without scales writes empty lists (the scales left the groom).
    static const ScaleMesh kNoScales;
    const ScaleMesh& sm = scales ? *scales : kNoScales;
    if (d.scaleMesh) {
        const std::size_t faces = sm.triangleCount(), vertices = sm.vertexCount();
        // Per face: whose scale it is.
        std::vector<std::uint32_t> owner(faces, 0);
        for (std::size_t k = 0; k < sm.scales.size(); ++k) {
            const ScaleMesh::Scale& s = sm.scales[k];
            for (std::size_t f = s.firstTriangle; f < s.firstTriangle + s.triangleCount && f < faces; ++f) owner[f] = static_cast<std::uint32_t>(k);
        }
        emitList(faces, [&](TextBuffer& b, std::size_t) { b.raw("3"); });
        emitList(sm.triangles.size(), [&](TextBuffer& b, std::size_t i) { b.integer(sm.triangles[i]); });
        emitList(vertices, [&](TextBuffer& b, std::size_t i) { b.vec3(sm.points[i]); });
        emit([&](TextBuffer& b) {
            if (vertices == 0) return;
            Vec3f lo = sm.points[0], hi = lo;
            for (const Vec3f& p : sm.points) {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
            b.vec3(lo);
            b.raw(", ");
            b.vec3(hi);
        });
        emitList(vertices, [&](TextBuffer& b, std::size_t i) { b.vec3(sm.normals[i]); });
        emitList(vertices, [&](TextBuffer& b, std::size_t i) { b.vec2(sm.uvs[i]); });
        emitList(vertices, [&](TextBuffer& b, std::size_t i) { b.color(sm.colors[i]); });
        emitList(vertices, [&](TextBuffer& b, std::size_t i) { b.color(sm.colors[i]); });
        emitList(faces, [&](TextBuffer& b, std::size_t f) { b.color(sm.scales[owner[f]].color); });
        emitList(faces, [&](TextBuffer& b, std::size_t f) { b.number(sm.scales[owner[f]].random); });
        emitList(faces, [&](TextBuffer& b, std::size_t f) { b.vec2(sm.scales[owner[f]].rootUv); });
        emitList(faces, [&](TextBuffer& b, std::size_t f) { b.integer(static_cast<int>(sm.scales[owner[f]].variant)); });
    }
    if (d.scaleInstancer) {
        // Each scale's frame (tilted and raised, before the bend) placing its prototype as it is.
        std::vector<const ScaleMesh::Scale*> placed;
        for (const ScaleMesh::Scale& s : sm.scales) {
            const InstanceSet* set = strands.instances().get();
            if (set && s.prototype < d.scalePrototypes.size() && s.prototype < set->prototypes.size() &&
                set->prototypes[s.prototype] == d.scalePrototypes[s.prototype]) {
                placed.push_back(&s);
            }
        }
        std::vector<UsdOrientation> frames(placed.size());
        for (std::size_t i = 0; i < placed.size(); ++i) frames[i] = usdOrientation(placed[i]->x, placed[i]->y, placed[i]->z);
        emitList(placed.size(), [&](TextBuffer& b, std::size_t i) { b.integer(static_cast<long long>(placed[i]->prototype)); });
        emitList(placed.size(), [&](TextBuffer& b, std::size_t i) { b.vec3(placed[i]->origin); });
        emitList(placed.size(), [&](TextBuffer& b, std::size_t i) { b.quaternion(frames[i]); });
        emitList(placed.size(), [&](TextBuffer& b, std::size_t i) { b.vec3(frames[i].scale); });
        emitList(placed.size(), [&](TextBuffer& b, std::size_t i) { b.integer(static_cast<long long>(placed[i]->id)); });
        emitList(placed.size(), [&](TextBuffer& b, std::size_t i) { b.color(placed[i]->color); });
        emitList(placed.size(), [&](TextBuffer& b, std::size_t i) { b.number(placed[i]->random); });
        emitList(placed.size(), [&](TextBuffer& b, std::size_t i) { b.vec2(placed[i]->rootUv); });
        emitList(placed.size(), [&](TextBuffer& b, std::size_t i) { b.integer(static_cast<int>(placed[i]->variant)); });
    }
}

void UsdCurvesWriter::close() {
    Impl& d = *impl_;
    if (d.closed) return;
    d.closed = true;

    // Assembled beside the target and moved over it: the target is never half-written.
    std::ofstream out(d.tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
        d.removeSpools();
        throw UsdError("cannot write '" + d.path.u8string() + "'");
    }

    TextBuffer header;
    header.raw("#usda 1.0\n(\n    defaultPrim = \"" + d.name + "\"\n");
    header.raw("    doc = \"GroomLab " + std::string(kVersionString));
    if (!d.options.comment.empty()) header.raw(": " + usdDocText(d.options.comment));
    header.raw("\"\n");
    // The layout's version (web/content/docs/usd-layout.mdx), on the layer and on the root prim
    // (customLayerData is out of reach once the layer is referenced into a stage; customData stays).
    const std::string marker = "        dictionary groomlab = {\n            int layoutVersion = " +
                               std::to_string(kUsdLayoutVersion) + "\n            string version = \"" +
                               std::string(kVersionString) + "\"\n        }\n";
    header.raw("    customLayerData = {\n" + marker + "    }\n");
    if (d.frames > 0) {
        header.raw("    startTimeCode = ");
        header.number(d.firstFrame);
        header.raw("\n    endTimeCode = ");
        header.number(d.lastFrame);
        header.raw("\n");
    }
    header.raw("    framesPerSecond = ");
    header.number(d.options.framesPerSecond);
    header.raw("\n    timeCodesPerSecond = ");
    header.number(d.options.framesPerSecond);
    header.raw("\n    metersPerUnit = ");
    header.number(d.options.metersPerUnit);
    header.raw("\n    upAxis = \"" + d.options.upAxis + "\"\n)\n\n");
    // Unreal's GroomAPI (its USD schema) on the groom's Xform: its importer makes a groom of the
    // curves under it (on the curves prim itself it does not). A reader without the schema ignores
    // an applied schema it does not know.
    header.raw("def Xform \"" + d.name + "\" (\n    customData = {\n" + marker + "    }\n    kind = \"component\"\n" +
               "    prepend apiSchemas = [\"GroomAPI\"]\n)\n{\n");
    header.raw("    def BasisCurves \"strands\"\n    {\n");
    header.raw("        uniform token type = \"linear\"\n        uniform token wrap = \"nonperiodic\"\n");
    if (d.hideStrands) header.raw("        token visibility = \"invisible\"\n");
    out << header.str();

    std::vector<char> block(std::size_t(1) << 20);
    auto writeAttribute = [&](SampledAttribute& a) {
        a.spool.close();
        if (!a.metadata.empty()) out << "        " << a.declaration << " (\n            " << a.metadata << "\n        )\n";
        out << "        " << a.declaration << ".timeSamples = {\n";
        out.write(a.held.data(), static_cast<std::streamsize>(a.held.size()));  // one frame: all of it
        std::string().swap(a.held);
        // Copied in large blocks (a stream buffer's << goes a character at a time).
        std::ifstream in(a.spoolPath, std::ios::binary);
        while (in.read(block.data(), static_cast<std::streamsize>(block.size())) || in.gcount() > 0) {
            out.write(block.data(), in.gcount());
        }
        out << "        }\n";
    };
    for (std::size_t i = 0; i < d.instanceStart; ++i) writeAttribute(*d.attributes[i]);
    out << "    }\n";
    // A PointInstancer over `names` with the attributes [first, last); its prototypes a Mesh each
    // where the geometry is known (a built-in plate, a host's mesh), else an Xform to reference
    // geometry into.
    auto writeInstancer = [&](const std::string& prim, const std::vector<std::string>& names, std::size_t first, std::size_t last) {
        // Prim names unique however the prototype names differ ("a-b" and "a_b": a_b and a_b_2).
        const std::vector<std::string> prims = usdIdentifiers(names);
        out << "\n    def PointInstancer \"" << prim << "\"\n    {\n        rel prototypes = [";
        for (std::size_t p = 0; p < prims.size(); ++p) {
            out << (p ? ", " : "") << "</" << d.name << "/" << prim << "/prototypes/" << prims[p] << ">";
        }
        out << "]\n";
        for (std::size_t i = first; i < last; ++i) writeAttribute(*d.attributes[i]);
        out << "\n        def Scope \"prototypes\"\n        {\n";
        for (std::size_t p = 0; p < prims.size(); ++p) {
            const InstancePrototype* mesh = builtinPrototype(names[p]);
            if (!mesh && d.options.prototypes) mesh = d.options.prototypes(names[p]);
            if (mesh && mesh->found && !mesh->triangles.empty()) {
                out << "            def Mesh \"" << prims[p] << "\" (\n                doc = \"GroomLab prototype '"
                    << usdDocText(names[p]) << "'\"\n            )\n            {\n"
                    << prototypeMeshBody(*mesh, "                ") << "            }\n";
            } else {
                out << "            def Xform \"" << prims[p] << "\" (\n                doc = \"GroomLab prototype '"
                    << usdDocText(names[p]) << "': reference its geometry here\"\n            )\n            {\n            }\n";
            }
        }
        out << "        }\n    }\n";
    };
    if (!d.prototypes.empty()) writeInstancer("instances", d.prototypes, d.instanceStart, d.scaleStart);
    if (d.scaleMesh) {
        out << "\n    def Mesh \"scales\"\n    {\n        uniform token subdivisionScheme = \"none\"\n";
        for (std::size_t i = d.scaleStart; i < d.scaleInstanceStart; ++i) writeAttribute(*d.attributes[i]);
        out << "    }\n";
    }
    if (d.scaleInstancer) writeInstancer("scaleInstances", d.scalePrototypes, d.scaleInstanceStart, d.attributes.size());
    out << "}\n";
    out.close();
    const bool ok = static_cast<bool>(out);
    d.removeSpools();
    if (!ok) {
        d.removeTemp();
        throw UsdError("cannot write '" + d.path.u8string() + "'");
    }
    try {
        replaceFile(d.tmp, d.path);
    } catch (const std::runtime_error& e) {
        throw UsdError(e.what());
    }
}

}  // namespace groomlab
