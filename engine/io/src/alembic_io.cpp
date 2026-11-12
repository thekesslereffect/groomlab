// SPDX-License-Identifier: Apache-2.0
#include "groomlab/alembic_io.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <unordered_map>

#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include "groomlab/export_view.h"
#include "groomlab/random.h"
#include "time_bracket.h"

namespace groomlab {

namespace Abc = Alembic::Abc;
namespace AbcG = Alembic::AbcGeom;
namespace fs = std::filesystem;

static_assert(sizeof(Imath::V3f) == sizeof(Vec3f), "Imath::V3f and Vec3f must share a layout");

namespace {

// Every writer builds its archive at "<path>.tmp" and moves it over `path`
// when it closes, so a failed export never leaves a half-written file.
struct AtomicArchive {
    fs::path target, tmp;
    Abc::OArchive archive;
    bool committed = false;

    AtomicArchive(const std::string& path) : target(fs::u8path(path)), tmp(tempPathFor(target)) {
        archive = Abc::OArchive(Alembic::AbcCoreOgawa::WriteArchive(), tmp.u8string());
    }
    void discard() {
        std::error_code ec;
        fs::remove(tmp, ec);
    }
};

// Commits `a` (the archive and everything opened on it must be gone first).
void commit(AtomicArchive& a) {
    if (a.committed) return;
    a.committed = true;
    try {
        replaceFile(a.tmp, a.target);
    } catch (const std::runtime_error& e) {
        throw AlembicError(e.what());
    }
}

std::int32_t roundedInt(float v) {
    return std::isfinite(v) ? static_cast<std::int32_t>(std::lround(v)) : 0;
}

// An Alembic object name from a group's name: letters, digits, _ and - kept, the rest _.
std::string objectNameFor(const std::string& name) {
    std::string out;
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        out += plain ? c : '_';
    }
    return out.empty() ? std::string("group") : out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

struct AlembicCurvesWriter::Impl {
    // One curves object: writeFrame's, or a group's (writeGroups).
    struct Objects {
        std::string group;                // writeGroups: the group it writes (matched by name)
        std::size_t pointsPerStrand = 0;  // 0: the writer's option
        AbcG::OXform xform;               // several groups: the group's own xform under the root
        AbcG::OCurves curves;
        AbcG::OUInt64GeomParam ids;
        AbcG::OC3fGeomParam color;                        // when the first frame had melanin channels
        std::map<std::string, AbcG::OFloatGeomParam> channels;  // the first frame's strand channels
        std::map<std::string, AbcG::OFloatGeomParam> pointChannels;  // and point channels (Options::pointChannels)
        bool hasColor = false;
        bool hasUVs = false;
    };
    // The scales' mesh (writeFrame's scales), when the first frame had some.
    struct Scales {
        AbcG::OXform xform;
        AbcG::OPolyMesh mesh;
        AbcG::OC3fGeomParam color, rootColor;
        AbcG::OFloatGeomParam id;
        AbcG::OV2fGeomParam rootUv;
        AbcG::OInt32GeomParam variant;
    };
    std::unique_ptr<AtomicArchive> archive;
    AbcG::OXform root;  // /<name>
    std::string rootName;
    std::vector<std::unique_ptr<Objects>> objects;  // created by the first frame
    std::unique_ptr<Scales> scales;
    AbcG::OVisibilityProperty strandsVisibility;  // hidden: the strands only lay scales out
    bool open = false;
    bool grouped = false;
    std::uint32_t ts = 0;
    std::size_t frames = 0;
    Options options;

    // Scratch buffers reused across frames.
    std::vector<Imath::V2f> uvs;
    std::vector<Imath::C3f> colors;
    std::vector<float> zeros, missing;

    // A curves object `curvesName` under `parent`.
    std::unique_ptr<Objects> makeObjects(Abc::OObject parent, const std::string& curvesName) {
        auto o = std::make_unique<Objects>();
        o->curves = AbcG::OCurves(parent, curvesName, ts);
        o->ids = AbcG::OUInt64GeomParam(o->curves.getSchema().getArbGeomParams(), "strandId", false, AbcG::kUniformScope,
                                        1, ts);
        return o;
    }
    void write(Objects& o, const StrandSet& input, const ScalpMesh* scalp, bool first,
               const std::vector<Vec3f>* velocities = nullptr);
    std::vector<Imath::V3f> velocities;  // scaled, for the frame's sample
    void writeScales(const ScaleMesh& mesh);
};

void AlembicCurvesWriter::Impl::writeScales(const ScaleMesh& mesh) {
    Scales& s = *scales;
    const std::size_t faces = mesh.triangleCount();
    // Corners reversed as AbcExport writes a Maya mesh's faces (an importer turns them back).
    std::vector<std::int32_t> indices(mesh.triangles.size()), counts(faces, 3);
    std::vector<Imath::V2f> cornerUvs(mesh.triangles.size());
    std::vector<Imath::V3f> normals(mesh.triangles.size());
    for (std::size_t f = 0; f < faces; ++f) {
        for (std::size_t k = 0; k < 3; ++k) {
            const std::uint32_t v = mesh.triangles[3 * f + (k == 0 ? 0 : 3 - k)];
            indices[3 * f + k] = static_cast<std::int32_t>(v);
            cornerUvs[3 * f + k] = Imath::V2f(mesh.uvs[v][0], mesh.uvs[v][1]);
            normals[3 * f + k] = Imath::V3f(mesh.normals[v].x, mesh.normals[v].y, mesh.normals[v].z);
        }
    }
    std::vector<Imath::C3f> vertexColors(mesh.vertexCount()), rootColors(faces);
    for (std::size_t v = 0; v < vertexColors.size(); ++v) vertexColors[v] = Imath::C3f(mesh.colors[v][0], mesh.colors[v][1], mesh.colors[v][2]);
    std::vector<float> ids(faces, 0.0f);
    std::vector<Imath::V2f> rootUvs(faces);
    std::vector<std::int32_t> variants(faces, 0);
    for (const ScaleMesh::Scale& sc : mesh.scales) {
        for (std::size_t f = sc.firstTriangle; f < sc.firstTriangle + sc.triangleCount && f < faces; ++f) {
            rootColors[f] = Imath::C3f(sc.color[0], sc.color[1], sc.color[2]);
            ids[f] = sc.random;
            rootUvs[f] = Imath::V2f(sc.rootUv[0], sc.rootUv[1]);
            variants[f] = sc.variant;
        }
    }
    try {
        AbcG::XformSample identity;
        s.xform.getSchema().set(identity);
        AbcG::OPolyMeshSchema::Sample sample(Abc::P3fArraySample(reinterpret_cast<const Imath::V3f*>(mesh.points.data()), mesh.vertexCount()),
                                             Abc::Int32ArraySample(indices), Abc::Int32ArraySample(counts));
        sample.setUVs(AbcG::OV2fGeomParam::Sample(Abc::V2fArraySample(cornerUvs), AbcG::kFacevaryingScope));
        sample.setNormals(AbcG::ON3fGeomParam::Sample(Abc::N3fArraySample(normals), AbcG::kFacevaryingScope));
        s.mesh.getSchema().set(sample);
        s.color.set(AbcG::OC3fGeomParam::Sample(Abc::C3fArraySample(vertexColors), AbcG::kVertexScope));
        s.rootColor.set(AbcG::OC3fGeomParam::Sample(Abc::C3fArraySample(rootColors), AbcG::kUniformScope));
        s.id.set(AbcG::OFloatGeomParam::Sample(Abc::FloatArraySample(ids), AbcG::kUniformScope));
        s.rootUv.set(AbcG::OV2fGeomParam::Sample(Abc::V2fArraySample(rootUvs), AbcG::kUniformScope));
        s.variant.set(AbcG::OInt32GeomParam::Sample(Abc::Int32ArraySample(variants), AbcG::kUniformScope));
    } catch (const std::exception& e) {
        throw AlembicError(std::string("cannot write the scales: ") + e.what());
    }
}

AlembicCurvesWriter::AlembicCurvesWriter(const std::string& path, const std::string& name, double startSeconds,
                                         double secondsPerFrame)
    : AlembicCurvesWriter(path, name, startSeconds, secondsPerFrame, Options{}) {}

AlembicCurvesWriter::AlembicCurvesWriter(const std::string& path, const std::string& name, double startSeconds,
                                         double secondsPerFrame, const Options& options)
    : impl_(std::make_unique<Impl>()) {
    if (!(options.scale > 0.0f) || !std::isfinite(options.scale)) throw AlembicError("the scale must be positive");
    if (options.pointsPerStrand == 1) throw AlembicError("strands need at least 2 points");
    impl_->options = options;
    try {
        impl_->archive = std::make_unique<AtomicArchive>(path);
        const AbcG::TimeSampling sampling(secondsPerFrame, startSeconds);
        impl_->ts = impl_->archive->archive.addTimeSampling(sampling);
        impl_->root = AbcG::OXform(impl_->archive->archive.getTop(), name, impl_->ts);
        impl_->rootName = name;
        impl_->open = true;
    } catch (const std::exception& e) {
        if (impl_->archive) impl_->archive->discard();
        throw AlembicError("cannot create '" + path + "': " + e.what());
    }
}

AlembicCurvesWriter::~AlembicCurvesWriter() {
    try {
        close();
    } catch (...) {
        // Destructors must not throw; close() explicitly to see errors.
    }
}

std::size_t AlembicCurvesWriter::frameCount() const { return impl_ ? impl_->frames : 0; }

std::vector<std::string> AlembicCurvesWriter::objectPaths() const {
    std::vector<std::string> paths;
    if (!impl_) return paths;
    for (const auto& o : impl_->objects) paths.push_back(o->curves.getFullName());
    return paths;
}

std::vector<Vec3f> strandVelocities(const StrandSet& now, const StrandSet* before, const StrandSet* after, double seconds) {
    std::vector<Vec3f> out(now.positions().size(), Vec3f{0.0f, 0.0f, 0.0f});
    if (!(seconds > 0.0) || (!before && !after)) return out;
    auto indexOf = [](const StrandSet* set) {
        std::unordered_map<StrandId, std::size_t> index;
        if (!set) return index;
        index.reserve(set->strandCount());
        for (std::size_t s = 0; s < set->strandCount(); ++s) index.emplace(set->ids()[s], s);
        return index;
    };
    const auto beforeIndex = indexOf(before), afterIndex = indexOf(after);
    auto match = [&](const StrandSet* set, const std::unordered_map<StrandId, std::size_t>& index, std::size_t s) -> const Vec3f* {
        if (!set) return nullptr;
        const auto it = index.find(now.ids()[s]);
        if (it == index.end() || set->strandPointCount(it->second) != now.strandPointCount(s)) return nullptr;
        return set->strandPoints(it->second);
    };
    for (std::size_t s = 0; s < now.strandCount(); ++s) {
        const Vec3f* a = match(before, beforeIndex, s);
        const Vec3f* b = match(after, afterIndex, s);
        if (!a && !b) continue;
        const Vec3f* p = now.strandPoints(s);
        const float span = static_cast<float>((a && b ? 2.0 : 1.0) * seconds);
        const std::size_t begin = now.strandBegin(s);
        for (std::size_t k = 0; k < now.strandPointCount(s); ++k) {
            out[begin + k] = ((b ? b[k] : p[k]) - (a ? a[k] : p[k])) / span;
        }
    }
    return out;
}

void AlembicCurvesWriter::writeFrame(const StrandSet& input, const ScalpMesh* scalp, const ScaleMesh* scales,
                                     const std::vector<Vec3f>* velocities) {
    if (!impl_ || !impl_->open) throw AlembicError("writer is closed");
    Impl& d = *impl_;
    if (d.grouped) throw AlembicError("the strands were written in groups");
    const bool first = d.frames == 0;
    try {
        if (d.objects.empty()) d.objects.push_back(d.makeObjects(d.root, d.rootName + "Shape"));
        AbcG::XformSample identity;
        d.root.getSchema().set(identity);
        if (first && input.instances() && input.instances()->hidesStrands) {
            d.strandsVisibility = AbcG::CreateVisibilityProperty(d.objects.front()->curves, d.ts);
        }
        if (d.strandsVisibility.valid()) d.strandsVisibility.set(AbcG::kVisibilityHidden);
        if (first && scales && !scales->scales.empty()) {
            auto s = std::make_unique<Impl::Scales>();
            s->xform = AbcG::OXform(d.root, "scales", d.ts);
            s->mesh = AbcG::OPolyMesh(s->xform, "scalesShape", d.ts);
            Abc::OCompoundProperty arb = s->mesh.getSchema().getArbGeomParams();
            s->color = AbcG::OC3fGeomParam(arb, "scaleColor", false, AbcG::kVertexScope, 1, d.ts);
            s->rootColor = AbcG::OC3fGeomParam(arb, "scaleRootColor", false, AbcG::kUniformScope, 1, d.ts);
            s->id = AbcG::OFloatGeomParam(arb, "scaleId", false, AbcG::kUniformScope, 1, d.ts);
            s->rootUv = AbcG::OV2fGeomParam(arb, "scaleUV", false, AbcG::kUniformScope, 1, d.ts);
            s->variant = AbcG::OInt32GeomParam(arb, "scaleVariant", false, AbcG::kUniformScope, 1, d.ts);
            d.scales = std::move(s);
        }
    } catch (const std::exception& e) {
        throw AlembicError(std::string("cannot write frame: ") + e.what());
    }
    d.write(*d.objects.front(), input, scalp, first, velocities);
    if (d.scales) {
        static const ScaleMesh kNone;  // (the scales left the groom: an empty mesh this frame)
        d.writeScales(scales ? *scales : kNone);
    }
    ++d.frames;
}

void AlembicCurvesWriter::writeGroups(const std::vector<Group>& groups, const ScalpMesh* scalp) {
    if (!impl_ || !impl_->open) throw AlembicError("writer is closed");
    Impl& d = *impl_;
    if (d.frames > 0 && !d.grouped) throw AlembicError("the strands were written without groups");
    const StrandSet none;
    try {
        if (d.frames == 0) {
            d.grouped = true;
            std::vector<std::string> used;
            for (const Group& g : groups) {
                for (const Group& other : groups) {
                    if (&other != &g && other.name == g.name) throw AlembicError("two groups named '" + g.name + "'");
                }
                if (groups.size() == 1) {  // one group: the layout writeFrame gives
                    d.objects.push_back(d.makeObjects(d.root, d.rootName + "Shape"));
                } else {
                    std::string base = objectNameFor(g.name);
                    for (int n = 2; std::find(used.begin(), used.end(), base) != used.end(); ++n) {
                        base = objectNameFor(g.name) + "_" + std::to_string(n);
                    }
                    used.push_back(base);
                    AbcG::OXform xform(d.root, base, d.ts);
                    d.objects.push_back(d.makeObjects(xform, base + "Shape"));
                    d.objects.back()->xform = xform;
                }
                d.objects.back()->group = g.name;
                d.objects.back()->pointsPerStrand = g.pointsPerStrand;
            }
        }
        AbcG::XformSample identity;
        d.root.getSchema().set(identity);
        for (auto& o : d.objects) {
            if (o->xform.valid()) o->xform.getSchema().set(identity);
        }
    } catch (const AlembicError&) {
        throw;
    } catch (const std::exception& e) {
        throw AlembicError(std::string("cannot write frame: ") + e.what());
    }
    for (auto& o : d.objects) {
        const Group* g = nullptr;
        for (const Group& candidate : groups) {
            if (candidate.name == o->group) g = &candidate;
        }
        d.write(*o, g && g->strands ? *g->strands : none, scalp, d.frames == 0);
    }
    ++d.frames;
}

void AlembicCurvesWriter::Impl::write(Objects& o, const StrandSet& input, const ScalpMesh* scalp, bool first,
                                      const std::vector<Vec3f>* velocityValues) {
    Impl& d = *this;
    // Scaled or resampled for an importer: a copy; as they are: the strands themselves.
    const std::size_t points = o.pointsPerStrand >= 2 ? o.pointsPerStrand : d.options.pointsPerStrand;
    StrandSet adjusted;
    bool adjust = d.options.scale != 1.0f || points >= 2;
    if (adjust) adjusted = exportCopy(input, d.options.scale, points);
    if (d.options.positiveRootWidths) {
        if (!adjust) adjusted = input;  // copy-on-write: only the widths are copied, if at all
        adjust = true;
        keepRootWidthsPositive(adjusted, 1e-3f * d.options.scale);
    }
    const StrandSet& strands = adjust ? adjusted : input;
    // Cd: the hair's appearance (no per-point colors or bounds: not written here).
    const ExportView view = describeExport(strands, scalp, kExportAppearanceColors);
    const std::size_t strandCount = view.strandCount();
    Abc::OCompoundProperty arb = o.curves.getSchema().getArbGeomParams();
    if (first) {
        // The first frame decides what every frame carries, as the USD writer does.
        o.hasUVs = view.hasUVs();
        o.hasColor = view.hasColors();
        if (o.hasColor) o.color = AbcG::OC3fGeomParam(arb, "Cd", false, AbcG::kUniformScope, 1, d.ts);
        for (const std::string& name : view.strandChannels) {
            if (name == "strandId" || name == "Cd") continue;  // never shadow the fixed params
            o.channels.emplace(name, AbcG::OFloatGeomParam(arb, name, false, AbcG::kUniformScope, 1, d.ts));
        }
        if (d.options.pointChannels) {
            for (const std::string& name : strands.pointChannelNames()) {
                if (name == "strandId" || name == "Cd" || o.channels.count(name)) continue;
                o.pointChannels.emplace(name, AbcG::OFloatGeomParam(arb, name, false, AbcG::kVertexScope, 1, d.ts));
            }
        }
    }

    const auto* positions = reinterpret_cast<const Imath::V3f*>(strands.positions().data());
    // Widths as the native param: 1 per point when the set has none (the USD writer agrees).
    const std::vector<float>* widthValues = &strands.widths();
    if (widthValues->size() != view.pointCount()) {
        d.zeros.assign(view.pointCount(), 0.0f);
        for (std::size_t i = 0; i < d.zeros.size(); ++i) d.zeros[i] = view.width(i);
        widthValues = &d.zeros;
    }
    const AbcG::OFloatGeomParam::Sample widths(Abc::FloatArraySample(widthValues->data(), widthValues->size()),
                                               AbcG::kVertexScope);

    AbcG::OCurvesSchema::Sample sample(Abc::P3fArraySample(positions, view.pointCount()),
                                       Abc::Int32ArraySample(view.counts), AbcG::kLinear, AbcG::kNonPeriodic, widths);
    sample.setBasis(AbcG::kNoBasis);
    // Motion blur: each point's velocity (not resampled: the strands' own points).
    if (velocityValues && points < 2 && velocityValues->size() == view.pointCount()) {
        d.velocities.resize(velocityValues->size());
        const float scale = d.options.scale;
        for (std::size_t i = 0; i < velocityValues->size(); ++i) {
            const Vec3f& v = (*velocityValues)[i];
            d.velocities[i] = Imath::V3f(v.x * scale, v.y * scale, v.z * scale);
        }
        sample.setVelocities(Abc::V3fArraySample(d.velocities));
    }
    if (o.hasUVs) {
        if (d.options.vertexUVs) {  // the root UV at every point of its strand
            d.uvs.assign(view.pointCount(), Imath::V2f(0.0f, 0.0f));
            for (std::size_t s = 0; s < strandCount && s < view.rootUVs.size(); ++s) {
                const std::size_t begin = strands.strandBegin(s), end = begin + strands.strandPointCount(s);
                for (std::size_t k = begin; k < end; ++k) d.uvs[k] = Imath::V2f(view.rootUVs[s][0], view.rootUVs[s][1]);
            }
        } else {
            d.uvs.assign(strandCount, Imath::V2f(0.0f, 0.0f));
            for (std::size_t s = 0; s < strandCount && s < view.rootUVs.size(); ++s) {
                d.uvs[s] = Imath::V2f(view.rootUVs[s][0], view.rootUVs[s][1]);
            }
        }
        sample.setUVs(AbcG::OV2fGeomParam::Sample(Abc::V2fArraySample(d.uvs),
                                                  d.options.vertexUVs ? AbcG::kVertexScope : AbcG::kUniformScope));
    }

    try {
        o.curves.getSchema().set(sample);
        o.ids.set(AbcG::OUInt64GeomParam::Sample(Abc::UInt64ArraySample(strands.ids().data(), strands.ids().size()),
                                                 AbcG::kUniformScope));
        if (o.hasColor) {
            d.colors.assign(strandCount, Imath::C3f(0.0f, 0.0f, 0.0f));
            for (std::size_t s = 0; s < strandCount && s < view.colors.size(); ++s) {
                d.colors[s] = Imath::C3f(view.colors[s][0], view.colors[s][1], view.colors[s][2]);
            }
            o.color.set(AbcG::OC3fGeomParam::Sample(Abc::C3fArraySample(d.colors), AbcG::kUniformScope));
        }
        for (auto& [name, param] : o.channels) {
            const std::vector<float>* values = view.strandChannel(name);
            if (!values) {
                d.missing.assign(strandCount, 0.0f);
                values = &d.missing;
            }
            param.set(AbcG::OFloatGeomParam::Sample(Abc::FloatArraySample(values->data(), values->size()),
                                                    AbcG::kUniformScope));
        }
        for (auto& [name, param] : o.pointChannels) {
            const std::vector<float>* values = strands.pointChannel(name);
            if (!values || values->size() != view.pointCount()) {
                d.missing.assign(view.pointCount(), 0.0f);
                values = &d.missing;
            }
            param.set(AbcG::OFloatGeomParam::Sample(Abc::FloatArraySample(values->data(), values->size()),
                                                    AbcG::kVertexScope));
        }
    } catch (const std::exception& e) {
        throw AlembicError(std::string("cannot write frame: ") + e.what());
    }
}

void AlembicCurvesWriter::close() {
    if (!impl_) return;
    std::unique_ptr<Impl> d = std::move(impl_);
    d->strandsVisibility = AbcG::OVisibilityProperty();
    d->objects.clear();  // flushes the objects before the archive
    d->scales.reset();
    d->root = AbcG::OXform();
    if (!d->archive) return;
    std::unique_ptr<AtomicArchive> a = std::move(d->archive);
    a->archive = Abc::OArchive();  // closes the file
    commit(*a);
}

// ---------------------------------------------------------------------------
// Unreal groom writer
// ---------------------------------------------------------------------------

namespace {

// The groom's space as Unreal's importer takes points (UnrealGroomWriter: Z up, left-handed,
// centimeters): a Y-up point (x, y, z) is (x, z, y), a Z-up one (x, -y, z), times the
// centimeters per unit (in double, rounded once to float).
struct UnrealSpace {
    bool zUp = false;
    double scale = 1.0;

    Imath::V3f point(const Vec3f& p) const {
        const double x = p.x * scale, y = p.y * scale, z = p.z * scale;
        return zUp ? Imath::V3f(static_cast<float>(x), static_cast<float>(-y), static_cast<float>(z))
                   : Imath::V3f(static_cast<float>(x), static_cast<float>(z), static_cast<float>(y));
    }
};

// One curves object with Unreal's groom attributes.
struct UnrealCurves {
    AbcG::OCurves curves;
    AbcG::OInt32GeomParam versionMajor, versionMinor, groupId, groomId, guide, clumpId;
    AbcG::OUInt32GeomParam idLow, idHigh;
    AbcG::OStringGeomParam groupName;
    AbcG::OFloatGeomParam width, roughness, iridescence, ao;
    AbcG::OV2fGeomParam rootUv;
    AbcG::OV3fGeomParam color;
    bool hasUv = false;
    bool hasColor = false;
    bool hasIridescence = false;
    bool iridescenceAo = false;
    bool hasRoughness = false;
    bool hasClumps = false;
    bool isGuide = false;
    // Scratch buffers reused across frames.
    std::vector<std::int32_t> ids, groups, clumps;
    std::vector<std::uint32_t> low, high;
    std::vector<float> widths, values;
    std::vector<Imath::V2f> uvs;
    std::vector<Imath::V3f> colors, points;

    UnrealCurves(AbcG::OXform& parent, const std::string& name, std::uint32_t ts, bool guides, const ExportView& first,
                 bool iridescenceAsAo = false)
        : curves(parent, name, ts), hasUv(first.hasUVs()), hasColor(first.hasColors() || first.hasPointColors()),
          hasIridescence(!guides && first.pointChannel(kIridescenceChannel) != nullptr),
          iridescenceAo(iridescenceAsAo && hasIridescence),
          hasRoughness(first.strandChannel(kRoughnessChannel) != nullptr),
          hasClumps(first.strandChannel(kClumpIdChannel) != nullptr), isGuide(guides) {
        Abc::OCompoundProperty arb = curves.getSchema().getArbGeomParams();
        versionMajor = AbcG::OInt32GeomParam(arb, "groom_version_major", false, AbcG::kConstantScope, 1, ts);
        versionMinor = AbcG::OInt32GeomParam(arb, "groom_version_minor", false, AbcG::kConstantScope, 1, ts);
        groupName = AbcG::OStringGeomParam(arb, "groom_group_name", false, AbcG::kConstantScope, 1, ts);
        groupId = AbcG::OInt32GeomParam(arb, "groom_group_id", false, AbcG::kUniformScope, 1, ts);
        groomId = AbcG::OInt32GeomParam(arb, "groom_id", false, AbcG::kUniformScope, 1, ts);
        // The stable 64-bit strand ID, for tools matching strands back to the groom.
        idLow = AbcG::OUInt32GeomParam(arb, "groomlab_strand_id_low", false, AbcG::kUniformScope, 1, ts);
        idHigh = AbcG::OUInt32GeomParam(arb, "groomlab_strand_id_high", false, AbcG::kUniformScope, 1, ts);
        width = AbcG::OFloatGeomParam(arb, "groom_width", false, AbcG::kVertexScope, 1, ts);
        if (guides) guide = AbcG::OInt32GeomParam(arb, "groom_guide", false, AbcG::kConstantScope, 1, ts);
        if (hasUv) rootUv = AbcG::OV2fGeomParam(arb, "groom_root_uv", false, AbcG::kUniformScope, 1, ts);
        // Color per vertex: Epic's groom schema lists groom_color at vertex scope
        // only. The color nodes' point channels (a tip color, bands, a feather pattern) go
        // through as they are; a per-strand color repeats along the strand.
        if (hasColor) color = AbcG::OV3fGeomParam(arb, "groom_color", false, AbcG::kVertexScope, 1, ts);
        if (hasRoughness) roughness = AbcG::OFloatGeomParam(arb, "groom_roughness", false, AbcG::kUniformScope, 1, ts);
        if (hasClumps) clumpId = AbcG::OInt32GeomParam(arb, "groom_clump_id", false, AbcG::kUniformScope, 1, ts);
        // FeatherColor's iridescence per vertex: GroomLab's own attribute, and Unreal's AO when asked.
        if (hasIridescence) iridescence = AbcG::OFloatGeomParam(arb, "groom_iridescence", false, AbcG::kVertexScope, 1, ts);
        if (iridescenceAo) ao = AbcG::OFloatGeomParam(arb, "groom_ao", false, AbcG::kVertexScope, 1, ts);
    }

    // `fixedGroup` < 0: each curve's group from the groupId channel; `firstId`: where groom_id starts.
    void write(const ExportView& view, const UnrealSpace& space, const std::string& group, int fixedGroup = -1,
               std::int32_t firstId = 0) {
        const StrandSet& s = *view.strands;
        const std::size_t strandCount = view.strandCount();
        // groom_id is a dense index, stable in export order: Unreal reads a
        // non-negative int, which a 64-bit hash would not survive truncation to.
        ids.resize(strandCount);
        groups.assign(strandCount, std::max(0, fixedGroup));
        low.resize(strandCount);
        high.resize(strandCount);
        const std::vector<float>* groupChannel = fixedGroup < 0 ? view.strandChannel(kGroupIdChannel) : nullptr;
        for (std::size_t i = 0; i < strandCount; ++i) {
            ids[i] = firstId + static_cast<std::int32_t>(i);
            low[i] = static_cast<std::uint32_t>(s.ids()[i] & 0xFFFFFFFFu);
            high[i] = static_cast<std::uint32_t>(s.ids()[i] >> 32);
            if (groupChannel) groups[i] = std::max(0, roundedInt((*groupChannel)[i]));
        }
        // The set's widths (1 per point when it has none), in centimeters as the points.
        const std::vector<float>* pointWidths = &s.widths();
        if (pointWidths->size() != view.pointCount() || space.scale != 1.0) {
            widths.resize(view.pointCount());
            for (std::size_t i = 0; i < widths.size(); ++i) widths[i] = static_cast<float>(view.width(i) * space.scale);
            pointWidths = &widths;
        }
        const Abc::FloatArraySample widthSample(pointWidths->data(), pointWidths->size());
        // The points in Unreal's space (UnrealSpace).
        points.resize(view.pointCount());
        for (std::size_t i = 0; i < points.size(); ++i) points[i] = space.point(s.positions()[i]);
        const AbcG::OFloatGeomParam::Sample curveWidths(widthSample, AbcG::kVertexScope);
        AbcG::OCurvesSchema::Sample sample(Abc::P3fArraySample(points),
                                           Abc::Int32ArraySample(view.counts), AbcG::kLinear, AbcG::kNonPeriodic,
                                           curveWidths);
        sample.setBasis(AbcG::kNoBasis);
        curves.getSchema().set(sample);
        const std::int32_t major = 1, minor = 5;
        versionMajor.set(AbcG::OInt32GeomParam::Sample(Abc::Int32ArraySample(&major, 1), AbcG::kConstantScope));
        versionMinor.set(AbcG::OInt32GeomParam::Sample(Abc::Int32ArraySample(&minor, 1), AbcG::kConstantScope));
        groupName.set(AbcG::OStringGeomParam::Sample(Abc::StringArraySample(&group, 1), AbcG::kConstantScope));
        groupId.set(AbcG::OInt32GeomParam::Sample(Abc::Int32ArraySample(groups), AbcG::kUniformScope));
        groomId.set(AbcG::OInt32GeomParam::Sample(Abc::Int32ArraySample(ids), AbcG::kUniformScope));
        idLow.set(AbcG::OUInt32GeomParam::Sample(Abc::UInt32ArraySample(low), AbcG::kUniformScope));
        idHigh.set(AbcG::OUInt32GeomParam::Sample(Abc::UInt32ArraySample(high), AbcG::kUniformScope));
        width.set(AbcG::OFloatGeomParam::Sample(widthSample, AbcG::kVertexScope));
        if (isGuide) {
            const std::int32_t one = 1;
            guide.set(AbcG::OInt32GeomParam::Sample(Abc::Int32ArraySample(&one, 1), AbcG::kConstantScope));
        }
        if (hasUv) {
            uvs.assign(strandCount, Imath::V2f(0.0f, 0.0f));
            for (std::size_t i = 0; i < strandCount && i < view.rootUVs.size(); ++i) {
                uvs[i] = Imath::V2f(view.rootUVs[i][0], view.rootUVs[i][1]);
            }
            rootUv.set(AbcG::OV2fGeomParam::Sample(Abc::V2fArraySample(uvs), AbcG::kUniformScope));
        }
        if (hasColor) {
            colors.assign(view.pointCount(), Imath::V3f(0.0f, 0.0f, 0.0f));
            const bool perPoint = view.pointColors.size() == colors.size();  // a tip blend, bands, a pattern
            for (std::size_t i = 0; i < strandCount; ++i) {
                const std::size_t begin = s.strandBegin(i), end = begin + s.strandPointCount(i);
                const Imath::V3f own = i < view.colors.size()
                                           ? Imath::V3f(view.colors[i][0], view.colors[i][1], view.colors[i][2])
                                           : Imath::V3f(0.0f, 0.0f, 0.0f);
                for (std::size_t k = begin; k < end; ++k) {
                    const std::array<float, 3>* c = perPoint ? &view.pointColors[k] : nullptr;
                    colors[k] = c ? Imath::V3f((*c)[0], (*c)[1], (*c)[2]) : own;
                }
            }
            color.set(AbcG::OV3fGeomParam::Sample(Abc::V3fArraySample(colors), AbcG::kVertexScope));
        }
        if (hasRoughness) {
            values.assign(strandCount, 0.0f);  // a frame without the channel writes zeros
            if (const std::vector<float>* channel = view.strandChannel(kRoughnessChannel)) {
                for (std::size_t i = 0; i < strandCount; ++i) values[i] = std::clamp((*channel)[i], 0.0f, 1.0f);
            }
            roughness.set(AbcG::OFloatGeomParam::Sample(Abc::FloatArraySample(values), AbcG::kUniformScope));
        }
        if (hasClumps) {
            clumps.assign(strandCount, -1);
            if (const std::vector<float>* channel = view.strandChannel(kClumpIdChannel)) {
                for (std::size_t i = 0; i < strandCount; ++i) clumps[i] = roundedInt((*channel)[i]);
            }
            clumpId.set(AbcG::OInt32GeomParam::Sample(Abc::Int32ArraySample(clumps), AbcG::kUniformScope));
        }
        if (hasIridescence) {
            values.assign(view.pointCount(), 0.0f);  // a frame without the channel writes zeros
            if (const std::vector<float>* channel = view.pointChannel(kIridescenceChannel)) {
                for (std::size_t i = 0; i < values.size(); ++i) values[i] = std::clamp((*channel)[i], 0.0f, 1.0f);
            }
            const Abc::FloatArraySample sample(values);
            iridescence.set(AbcG::OFloatGeomParam::Sample(sample, AbcG::kVertexScope));
            if (iridescenceAo) ao.set(AbcG::OFloatGeomParam::Sample(sample, AbcG::kVertexScope));
        }
    }
};

}  // namespace

struct UnrealGroomWriter::Impl {
    struct GroupObjects {
        int id = 0;
        std::string name;
        std::unique_ptr<UnrealCurves> strands, guides;
    };
    struct Objects {
        AbcG::OXform xform;
        std::unique_ptr<UnrealCurves> strands, guides;
        std::vector<GroupObjects> groups;  // writeGroups
    };
    std::unique_ptr<AtomicArchive> archive;
    std::unique_ptr<Objects> objects;
    std::uint32_t ts = 0;
    std::string group;
    std::size_t frames = 0;
    bool iridescenceAo = false;
    UnrealSpace space;
};

void UnrealGroomWriter::setIridescenceAsAo(bool on) {
    if (impl_) impl_->iridescenceAo = on;
}

void UnrealGroomWriter::setSourceSpace(const std::string& upAxis, double centimetersPerUnit) {
    if (upAxis != "Y" && upAxis != "Z") throw std::invalid_argument("the up axis is \"Y\" or \"Z\", not \"" + upAxis + "\"");
    if (!(centimetersPerUnit > 0.0) || !std::isfinite(centimetersPerUnit)) {
        throw std::invalid_argument("the centimeters per unit must be positive");
    }
    if (!impl_) throw AlembicError("writer is closed");
    if (impl_->frames > 0) throw AlembicError("the space is set before the first frame");
    impl_->space.zUp = upAxis == "Z";
    impl_->space.scale = centimetersPerUnit;
}

UnrealGroomWriter::UnrealGroomWriter(const std::string& path, const std::string& name, const std::string& groupName,
                                     double startSeconds, double secondsPerFrame)
    : impl_(std::make_unique<Impl>()) {
    try {
        impl_->archive = std::make_unique<AtomicArchive>(path);
        impl_->ts = impl_->archive->archive.addTimeSampling(AbcG::TimeSampling(secondsPerFrame, startSeconds));
        impl_->objects = std::make_unique<Impl::Objects>();
        impl_->objects->xform = AbcG::OXform(impl_->archive->archive.getTop(), name, impl_->ts);
        impl_->group = groupName;
    } catch (const std::exception& e) {
        if (impl_->archive) impl_->archive->discard();
        throw AlembicError("cannot create '" + path + "': " + e.what());
    }
}

UnrealGroomWriter::~UnrealGroomWriter() {
    try {
        close();
    } catch (...) {
    }
}

std::size_t UnrealGroomWriter::frameCount() const { return impl_ ? impl_->frames : 0; }

void UnrealGroomWriter::writeFrame(const StrandSet& strands, const StrandSet* guides, const ScalpMesh* scalp) {
    if (!impl_ || !impl_->objects) throw AlembicError("writer is closed");
    Impl& d = *impl_;
    Impl::Objects& o = *d.objects;
    if (d.frames > 0 && !o.strands) throw AlembicError("the groom was written in groups");
    try {
        const ExportView view = describeExport(strands, scalp, kExportPointColors | kExportAppearanceColors);
        const StrandSet none;
        const ExportView guideView = describeExport(guides ? *guides : none, nullptr, kExportPointColors | kExportAppearanceColors);
        if (d.frames == 0) {
            o.strands = std::make_unique<UnrealCurves>(o.xform, "strands", d.ts, false, view, d.iridescenceAo);
            if (guides && !guides->empty()) o.guides = std::make_unique<UnrealCurves>(o.xform, "guides", d.ts, true, guideView);
        }
        AbcG::XformSample identity;
        o.xform.getSchema().set(identity);
        o.strands->write(view, d.space, d.group);
        if (o.guides) o.guides->write(guideView, d.space, d.group);
    } catch (const AlembicError&) {
        throw;
    } catch (const std::exception& e) {
        throw AlembicError(std::string("cannot write frame: ") + e.what());
    }
    ++d.frames;
}

void UnrealGroomWriter::writeGroups(const std::vector<Group>& groups, const ScalpMesh* scalp) {
    if (!impl_ || !impl_->objects) throw AlembicError("writer is closed");
    Impl& d = *impl_;
    Impl::Objects& o = *d.objects;
    if (d.frames > 0 && o.groups.empty() && o.strands) throw AlembicError("the groom was written without groups");
    try {
        const StrandSet none;
        auto find = [&groups](int id) -> const Group* {
            for (const Group& g : groups) {
                if (g.id == id) return &g;
            }
            return nullptr;
        };
        // Each group's strands and guides described once (the first frame's make the objects too).
        std::vector<ExportView> strandViews, guideViews;
        auto describeGroups = [&](auto&& groupOf) {
            for (std::size_t i = 0; i < groupOf.size(); ++i) {
                const Group* g = groupOf[i];
                strandViews.push_back(describeExport(g && g->strands ? *g->strands : none, scalp, kExportPointColors | kExportAppearanceColors));
                guideViews.push_back(describeExport(g && g->guides ? *g->guides : none, nullptr, kExportPointColors | kExportAppearanceColors));
            }
        };
        if (d.frames == 0) {
            const bool single = groups.size() == 1;
            std::vector<std::string> used;
            std::vector<const Group*> given;
            for (const Group& g : groups) given.push_back(&g);
            describeGroups(given);
            std::size_t index = 0;
            for (const Group& g : groups) {
                if (find(g.id) != &g) throw AlembicError("two groups with the id " + std::to_string(g.id));
                std::string base = objectNameFor(g.name);
                if (std::find(used.begin(), used.end(), base) != used.end()) base += "_" + std::to_string(g.id);
                used.push_back(base);
                Impl::GroupObjects objects;
                objects.id = g.id;
                objects.name = g.name;
                objects.strands = std::make_unique<UnrealCurves>(o.xform, single ? std::string("strands") : "strands_" + base,
                                                                 d.ts, false, strandViews[index], d.iridescenceAo);
                if (g.guides && !g.guides->empty()) {
                    objects.guides = std::make_unique<UnrealCurves>(o.xform, single ? std::string("guides") : "guides_" + base,
                                                                    d.ts, true, guideViews[index]);
                }
                o.groups.push_back(std::move(objects));
                ++index;
            }
        } else {
            std::vector<const Group*> found;
            for (const Impl::GroupObjects& objects : o.groups) found.push_back(find(objects.id));
            describeGroups(found);
        }
        AbcG::XformSample identity;
        o.xform.getSchema().set(identity);
        // On the first frame the groups are the objects, in order (two with one id were refused).
        std::int32_t strandIds = 0, guideIds = 0;
        for (std::size_t i = 0; i < o.groups.size(); ++i) {
            Impl::GroupObjects& objects = o.groups[i];
            const ExportView& strands = strandViews[i];
            objects.strands->write(strands, d.space, objects.name, objects.id, strandIds);
            strandIds += static_cast<std::int32_t>(strands.strandCount());
            if (objects.guides) {
                const ExportView& guides = guideViews[i];
                objects.guides->write(guides, d.space, objects.name, objects.id, guideIds);
                guideIds += static_cast<std::int32_t>(guides.strandCount());
            }
        }
    } catch (const AlembicError&) {
        throw;
    } catch (const std::exception& e) {
        throw AlembicError(std::string("cannot write frame: ") + e.what());
    }
    ++d.frames;
}

void UnrealGroomWriter::close() {
    if (!impl_) return;
    std::unique_ptr<Impl> d = std::move(impl_);
    d->objects.reset();
    if (!d->archive) return;
    std::unique_ptr<AtomicArchive> a = std::move(d->archive);
    a->archive = Abc::OArchive();
    commit(*a);
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

namespace {

bool findCurves(const Abc::IObject& parent, const std::string& shapeName, AbcG::ICurves& out) {
    for (std::size_t i = 0; i < parent.getNumChildren(); ++i) {
        const Abc::IObject child = parent.getChild(i);
        if (AbcG::ICurves::matches(child.getHeader()) && (shapeName.empty() || child.getName() == shapeName)) {
            out = AbcG::ICurves(child, Abc::kWrapExisting);
            return true;
        }
        if (findCurves(child, shapeName, out)) return true;
    }
    return false;
}

// A curves sample's layout, checked before anything indexes it: positions and
// counts present, no negative count, and the counts fit the positions. A
// malformed (or truncated) file is an error, not an out-of-bounds read.
struct CurveLayout {
    std::size_t curves = 0;
    std::size_t points = 0;  // sum of the counts
};

CurveLayout checkedLayout(const Abc::P3fArraySamplePtr& positions, const Abc::Int32ArraySamplePtr& counts,
                          const std::string& what) {
    if (!positions || !counts) throw AlembicError(what + ": a curves sample has no positions or vertex counts");
    CurveLayout layout;
    layout.curves = counts->size();
    for (std::size_t c = 0; c < counts->size(); ++c) {
        const std::int32_t n = (*counts)[c];
        if (n < 0) throw AlembicError(what + ": curve " + std::to_string(c) + " has a negative vertex count");
        layout.points += static_cast<std::size_t>(n);
    }
    if (layout.points > positions->size()) {
        throw AlembicError(what + ": the vertex counts add up to " + std::to_string(layout.points) +
                           " points but the sample holds " + std::to_string(positions->size()));
    }
    return layout;
}

struct FlattenState {
    Abc::ISampleSelector sel;
    double seconds = 0.0;  // positions blend between the samples around it
    std::string objectName;
    StrandSet out;
};

std::uint64_t hashPath(const std::string& s) {
    std::uint64_t h = hash64(s.size());
    for (unsigned char c : s) h = hashCombine(h, c);
    return h;
}

void appendCurves(const AbcG::ICurves& curves, const Imath::M44d& world, FlattenState& st) {
    AbcG::ICurvesSchema schema = curves.getSchema();
    AbcG::ICurvesSchema::Sample sample;
    schema.get(sample, st.sel);

    Abc::P3fArraySamplePtr positions = sample.getPositions();
    const Abc::Int32ArraySamplePtr counts = sample.getCurvesNumVertices();
    const CurveLayout layout = checkedLayout(positions, counts, curves.getFullName());

    // Between two samples with the same curves, blend the positions (e.g. a
    // simulation cache read at a motion-blur subframe).
    std::vector<Imath::V3f> blended;
    const TimeBracket br = timeBracket(schema.getTimeSampling(), schema.getNumSamples(), st.seconds);
    if (br.blends()) {
        AbcG::ICurvesSchema::Sample a, b;
        schema.get(a, Abc::ISampleSelector(br.a));
        schema.get(b, Abc::ISampleSelector(br.b));
        const Abc::P3fArraySamplePtr pa = a.getPositions(), pb = b.getPositions();
        const Abc::Int32ArraySamplePtr ca = a.getCurvesNumVertices(), cb = b.getCurvesNumVertices();
        const bool sameCurves = pa && pb && ca && cb && pa->size() == pb->size() && ca->size() == cb->size() &&
                                std::equal(ca->get(), ca->get() + ca->size(), cb->get());
        if (sameCurves) {
            checkedLayout(pa, ca, curves.getFullName());
            const auto t = static_cast<float>(br.t);
            blended.resize(pa->size());
            for (std::size_t i = 0; i < blended.size(); ++i) blended[i] = (*pa)[i] * (1.0f - t) + (*pb)[i] * t;
            positions = pa;  // same layout; values come from `blended`
        }
    }
    auto position = [&](std::size_t i) -> const Imath::V3f& { return blended.empty() ? (*positions)[i] : blended[i]; };

    Abc::FloatArraySamplePtr widths;
    if (const AbcG::IFloatGeomParam wp = schema.getWidthsParam()) widths = wp.getExpandedValue(st.sel).getVals();
    const bool perPointWidths = widths && widths->size() == positions->size();

    Abc::UInt64ArraySamplePtr ids;
    const Abc::ICompoundProperty arb = schema.getArbGeomParams();
    if (arb && arb.getPropertyHeader("strandId")) {
        ids = AbcG::IUInt64GeomParam(arb, "strandId").getExpandedValue(st.sel).getVals();
    }
    const std::uint64_t pathHash = hashPath(curves.getFullName());
    // Float params per vertex: point channels (a simulation bake's wetness), at the nearest sample.
    std::vector<std::pair<std::string, Abc::FloatArraySamplePtr>> perPoint;
    for (std::size_t k = 0; arb && k < arb.getNumProperties(); ++k) {
        const Abc::PropertyHeader& header = arb.getPropertyHeader(k);
        if (!AbcG::IFloatGeomParam::matches(header) || AbcG::GetGeometryScope(header.getMetaData()) != AbcG::kVertexScope) continue;
        const Abc::FloatArraySamplePtr values = AbcG::IFloatGeomParam(arb, header.getName()).getExpandedValue(st.sel).getVals();
        if (values && values->size() == positions->size()) perPoint.emplace_back(header.getName(), values);
    }
    const std::size_t firstPoint = st.out.pointCount();

    std::size_t offset = 0;
    for (std::size_t c = 0; c < layout.curves; ++c) {
        const auto n = static_cast<std::size_t>((*counts)[c]);
        std::vector<Vec3f> pts(n);
        for (std::size_t k = 0; k < n; ++k) {
            const Imath::V3f& p = position(offset + k);
            Imath::V3d q;
            world.multVecMatrix(Imath::V3d(p.x, p.y, p.z), q);
            pts[k] = {static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z)};
        }
        std::vector<float> w;
        if (perPointWidths) w.assign(widths->get() + offset, widths->get() + offset + n);
        const StrandId id = ids && c < ids->size() ? (*ids)[c] : hashCombine(pathHash, c);
        st.out.addStrand(id, pts, w);
        offset += n;
    }
    for (const auto& [name, values] : perPoint) {
        std::vector<float>* channel = st.out.mutablePointChannel(name);
        if (!channel) channel = &st.out.addPointChannel(name, 0.0f);
        std::copy(values->get(), values->get() + values->size(), channel->begin() + static_cast<std::ptrdiff_t>(firstPoint));
    }
}

void flatten(const Abc::IObject& obj, const Imath::M44d& parentWorld, bool included, FlattenState& st) {
    Imath::M44d world = parentWorld;
    if (AbcG::IXform::matches(obj.getHeader())) {
        AbcG::IXform xform(obj, Abc::kWrapExisting);
        AbcG::XformSample xs;
        xform.getSchema().get(xs, st.sel);
        world = xform.getSchema().getInheritsXforms() ? xs.getMatrix() * parentWorld : xs.getMatrix();
    }
    included = included || obj.getName() == st.objectName;
    if (included && AbcG::ICurves::matches(obj.getHeader())) {
        appendCurves(AbcG::ICurves(obj, Abc::kWrapExisting), world, st);
    }
    for (std::size_t i = 0; i < obj.getNumChildren(); ++i) flatten(obj.getChild(i), world, included, st);
}

}  // namespace

struct AlembicCurvesReader::Impl {
    std::string path;
    std::string objectName;
    Abc::IArchive archive;
};

AlembicCurvesReader::AlembicCurvesReader(const std::string& path, const std::string& objectName)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = path;
    impl_->objectName = objectName;
    try {
        impl_->archive = Abc::IArchive(Alembic::AbcCoreOgawa::ReadArchive(), path);
    } catch (const std::exception& e) {
        throw AlembicError("cannot read '" + path + "': " + e.what());
    }
}

AlembicCurvesReader::~AlembicCurvesReader() = default;

namespace {

// The first and last sample times of every curves object `included` (under `objectName`, or
// all of them), for a timeline: objects of one sample do not count (nothing moves there).
void curvesTimeRange(const Abc::IObject& obj, const std::string& objectName, bool included, double* lo, double* hi,
                     bool* any) {
    included = included || obj.getName() == objectName;
    if (included && AbcG::ICurves::matches(obj.getHeader())) {
        const AbcG::ICurvesSchema schema = AbcG::ICurves(obj, Abc::kWrapExisting).getSchema();
        const std::size_t n = schema.getNumSamples();
        if (n > 1) {
            const double first = schema.getTimeSampling()->getSampleTime(0);
            const double last = schema.getTimeSampling()->getSampleTime(static_cast<Abc::index_t>(n - 1));
            *lo = *any ? std::min(*lo, first) : first;
            *hi = *any ? std::max(*hi, last) : last;
            *any = true;
        }
    }
    for (std::size_t i = 0; i < obj.getNumChildren(); ++i) curvesTimeRange(obj.getChild(i), objectName, included, lo, hi, any);
}

}  // namespace

bool AlembicCurvesReader::timeRange(double* startSeconds, double* endSeconds) const {
    double lo = 0.0, hi = 0.0;
    bool any = false;
    try {
        curvesTimeRange(impl_->archive.getTop(), impl_->objectName, impl_->objectName.empty(), &lo, &hi, &any);
    } catch (const std::exception& e) {
        throw AlembicError("cannot read '" + impl_->path + "': " + e.what());
    }
    if (!any) return false;
    if (startSeconds) *startSeconds = lo;
    if (endSeconds) *endSeconds = hi;
    return true;
}

StrandSet AlembicCurvesReader::at(double seconds) const {
    FlattenState st{Abc::ISampleSelector(seconds, Abc::ISampleSelector::kNearIndex), seconds, impl_->objectName, {}};
    try {
        flatten(impl_->archive.getTop(), Imath::M44d(), impl_->objectName.empty(), st);
    } catch (const AlembicError& e) {
        throw AlembicError("cannot read '" + impl_->path + "': " + e.what());
    } catch (const std::exception& e) {
        throw AlembicError("cannot read '" + impl_->path + "': " + e.what());
    }
    return std::move(st.out);
}

StrandSet readAlembicCurvesAt(const std::string& path, double seconds, const std::string& objectName) {
    return AlembicCurvesReader(path, objectName).at(seconds);
}

std::vector<AlembicCurvesFrame> readAlembicCurves(const std::string& path, const std::string& name) {
    std::vector<AlembicCurvesFrame> frames;
    try {
        Abc::IArchive archive(Alembic::AbcCoreOgawa::ReadArchive(), path);
        AbcG::ICurves curves;
        const std::string shapeName = name.empty() ? std::string() : name + "Shape";
        if (!findCurves(archive.getTop(), shapeName, curves)) {
            throw AlembicError("no curves" + (name.empty() ? std::string() : " named '" + shapeName + "'"));
        }

        AbcG::ICurvesSchema& schema = curves.getSchema();
        const AbcG::IFloatGeomParam widthsParam = schema.getWidthsParam();
        const AbcG::IV2fGeomParam uvsParam = schema.getUVsParam();
        AbcG::IUInt64GeomParam idsParam;
        const Abc::ICompoundProperty arb = schema.getArbGeomParams();
        if (arb && arb.getPropertyHeader("strandId")) idsParam = AbcG::IUInt64GeomParam(arb, "strandId");

        const std::size_t sampleCount = schema.getNumSamples();
        frames.resize(sampleCount);
        for (std::size_t i = 0; i < sampleCount; ++i) {
            const Abc::ISampleSelector sel(static_cast<Abc::index_t>(i));
            AbcG::ICurvesSchema::Sample sample;
            schema.get(sample, sel);

            const Abc::P3fArraySamplePtr positions = sample.getPositions();
            const Abc::Int32ArraySamplePtr counts = sample.getCurvesNumVertices();
            const CurveLayout layout = checkedLayout(positions, counts, curves.getFullName() + " sample " + std::to_string(i));
            Abc::FloatArraySamplePtr widths;
            if (widthsParam) widths = widthsParam.getExpandedValue(sel).getVals();
            Abc::UInt64ArraySamplePtr ids;
            if (idsParam) ids = idsParam.getExpandedValue(sel).getVals();

            AlembicCurvesFrame& frame = frames[i];
            frame.seconds = schema.getTimeSampling()->getSampleTime(static_cast<Abc::index_t>(i));
            if (const Abc::V3fArraySamplePtr v = sample.getVelocities()) {
                frame.velocities.reserve(v->size());
                for (std::size_t k = 0; k < v->size(); ++k) frame.velocities.push_back({(*v)[k].x, (*v)[k].y, (*v)[k].z});
            }
            if (uvsParam) {
                const Abc::V2fArraySamplePtr uvs = uvsParam.getExpandedValue(sel).getVals();
                if (uvs) {
                    frame.uvs.reserve(uvs->size());
                    for (std::size_t k = 0; k < uvs->size(); ++k) frame.uvs.push_back({(*uvs)[k].x, (*uvs)[k].y});
                }
            }

            std::size_t offset = 0;
            const bool perPointWidths = widths && widths->size() == positions->size();
            for (std::size_t c = 0; c < layout.curves; ++c) {
                const auto n = static_cast<std::size_t>((*counts)[c]);
                std::vector<Vec3f> pts(n);
                std::vector<float> w;
                for (std::size_t k = 0; k < n; ++k) {
                    const Imath::V3f& p = (*positions)[offset + k];
                    pts[k] = {p.x, p.y, p.z};
                }
                if (perPointWidths) w.assign(widths->get() + offset, widths->get() + offset + n);
                const StrandId id = ids && c < ids->size() ? (*ids)[c] : static_cast<StrandId>(c);
                frame.strands.addStrand(id, pts, w);
                offset += n;
            }
        }
    } catch (const AlembicError& e) {
        throw AlembicError("cannot read '" + path + "': " + e.what());
    } catch (const std::exception& e) {
        throw AlembicError("cannot read '" + path + "': " + e.what());
    }
    return frames;
}

}  // namespace groomlab
