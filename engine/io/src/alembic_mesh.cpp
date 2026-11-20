// SPDX-License-Identifier: Apache-2.0
// Animated mesh positions from Alembic (scalp caches for groom files).

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>

#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include "groomlab/alembic_io.h"
#include "groomlab/export_view.h"
#include "time_bracket.h"

namespace groomlab {

namespace Abc = Alembic::Abc;
namespace AbcG = Alembic::AbcGeom;

namespace {

Imath::M44d xformAt(const AbcG::IXform& xform, double seconds) {
    AbcG::IXformSchema schema = xform.getSchema();
    const TimeBracket br = timeBracket(schema.getTimeSampling(), schema.getNumSamples(), seconds);
    AbcG::XformSample a;
    schema.get(a, Abc::ISampleSelector(br.a));
    if (br.a == br.b || br.t == 0.0) return a.getMatrix();
    AbcG::XformSample b;
    schema.get(b, Abc::ISampleSelector(br.b));
    // Linear blend of the matrices: fine across one frame of motion.
    const Imath::M44d ma = a.getMatrix(), mb = b.getMatrix();
    Imath::M44d m;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) m[i][j] = ma[i][j] * (1.0 - br.t) + mb[i][j] * br.t;
    }
    return m;
}

bool findMesh(const Abc::IObject& obj, const std::string& name, bool included, std::vector<AbcG::IXform>& chain,
              AbcG::IPolyMesh& out) {
    included = included || obj.getName() == name;
    if (included && AbcG::IPolyMesh::matches(obj.getHeader())) {
        out = AbcG::IPolyMesh(obj, Abc::kWrapExisting);
        return true;
    }
    const bool isXform = AbcG::IXform::matches(obj.getHeader());
    if (isXform) chain.emplace_back(obj, Abc::kWrapExisting);
    for (std::size_t i = 0; i < obj.getNumChildren(); ++i) {
        if (findMesh(obj.getChild(i), name, included, chain, out)) return true;
    }
    if (isXform) chain.pop_back();
    return false;
}

}  // namespace

struct AlembicMeshReader::Impl {
    std::string path;
    Abc::IArchive archive;
    AbcG::IPolyMesh mesh;
    std::vector<AbcG::IXform> chain;  // root first
};

AlembicMeshReader::AlembicMeshReader(const std::string& path, const std::string& objectName)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = path;
    try {
        impl_->archive = Abc::IArchive(Alembic::AbcCoreOgawa::ReadArchive(), path);
        if (!findMesh(impl_->archive.getTop(), objectName, objectName.empty(), impl_->chain, impl_->mesh)) {
            throw AlembicError("no polygon mesh" + (objectName.empty() ? std::string() : " named '" + objectName + "'"));
        }
    } catch (const AlembicError& e) {
        throw AlembicError("'" + path + "': " + e.what());
    } catch (const std::exception& e) {
        throw AlembicError("cannot read '" + path + "': " + e.what());
    }
}

AlembicMeshReader::~AlembicMeshReader() = default;

const std::string& AlembicMeshReader::meshName() const { return impl_->mesh.getName(); }

bool AlembicMeshReader::animated() const {
    AbcG::IPolyMeshSchema& schema = impl_->mesh.getSchema();
    if (schema.getPositionsProperty().getNumSamples() > 1 && !schema.getPositionsProperty().isConstant()) return true;
    for (const AbcG::IXform& x : impl_->chain) {
        if (!x.getSchema().isConstant()) return true;
    }
    return false;
}

bool AlembicMeshReader::timeRange(double* startSeconds, double* endSeconds) const {
    // The positions' samples, widened by any parent transform that moves.
    double lo = 0.0, hi = 0.0;
    bool any = false;
    auto widen = [&](const Abc::TimeSamplingPtr& sampling, std::size_t n) {
        if (n <= 1) return;
        const double first = sampling->getSampleTime(0), last = sampling->getSampleTime(static_cast<Abc::index_t>(n - 1));
        lo = any ? std::min(lo, first) : first;
        hi = any ? std::max(hi, last) : last;
        any = true;
    };
    AbcG::IPolyMeshSchema& schema = impl_->mesh.getSchema();
    if (!schema.getPositionsProperty().isConstant()) widen(schema.getTimeSampling(), schema.getNumSamples());
    for (const AbcG::IXform& x : impl_->chain) {
        if (!x.getSchema().isConstant()) widen(x.getSchema().getTimeSampling(), x.getSchema().getNumSamples());
    }
    if (!any) return false;
    if (startSeconds) *startSeconds = lo;
    if (endSeconds) *endSeconds = hi;
    return true;
}

std::vector<Vec3f> AlembicMeshReader::pointsAt(double seconds) const {
    try {
        AbcG::IPolyMeshSchema& schema = impl_->mesh.getSchema();
        const TimeBracket br = timeBracket(schema.getTimeSampling(), schema.getNumSamples(), seconds);
        const Abc::P3fArraySamplePtr a = schema.getPositionsProperty().getValue(Abc::ISampleSelector(br.a));
        std::vector<Vec3f> pts(a->size());
        for (std::size_t i = 0; i < pts.size(); ++i) pts[i] = {(*a)[i].x, (*a)[i].y, (*a)[i].z};
        if (br.a != br.b && br.t > 0.0) {
            const Abc::P3fArraySamplePtr b = schema.getPositionsProperty().getValue(Abc::ISampleSelector(br.b));
            if (b->size() == pts.size()) {
                const auto t = static_cast<float>(br.t);
                for (std::size_t i = 0; i < pts.size(); ++i) {
                    const Vec3f q{(*b)[i].x, (*b)[i].y, (*b)[i].z};
                    pts[i] = pts[i] * (1.0f - t) + q * t;
                }
            }
        }

        Imath::M44d world;  // identity
        bool identity = true;
        for (const AbcG::IXform& x : impl_->chain) {
            const Imath::M44d m = xformAt(x, seconds);
            world = x.getSchema().getInheritsXforms() ? m * world : m;
            identity = identity && m == Imath::M44d();
        }
        if (!identity) {
            for (Vec3f& p : pts) {
                Imath::V3d v(p.x, p.y, p.z);
                world.multVecMatrix(v, v);
                p = {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
            }
        }
        return pts;
    } catch (const std::exception& e) {
        throw AlembicError("cannot read '" + impl_->path + "': " + e.what());
    }
}

std::shared_ptr<const ScalpMesh> AlembicMeshReader::meshAt(double seconds) const {
    std::vector<Vec3f> points = pointsAt(seconds);
    try {
        AbcG::IPolyMeshSchema& schema = impl_->mesh.getSchema();
        // The topology of the first sample: a scalp keeps its faces over time (a cache whose faces
        // change is not a scalp a groom can be rooted on).
        const Abc::ISampleSelector first(static_cast<Abc::index_t>(0));
        const Abc::Int32ArraySamplePtr indices = schema.getFaceIndicesProperty().getValue(first);
        const Abc::Int32ArraySamplePtr counts = schema.getFaceCountsProperty().getValue(first);
        // Alembic winds faces clockwise (Maya's AbcExport writes them reversed); the fan is built
        // reversed so the triangles face outward as the normals of a Maya mesh do.
        std::vector<ScalpMesh::Triangle> triangles;
        std::vector<ScalpMesh::UV> uvs;
        AbcG::IV2fGeomParam uvParam = schema.getUVsParam();
        std::vector<ScalpMesh::UV> cornerUVs;  // per face corner as the file lists them, or per vertex
        bool uvsPerVertex = false;
        if (uvParam.valid()) {
            const AbcG::IV2fGeomParam::Sample sample = uvParam.getIndexedValue(first);
            const Abc::V2fArraySamplePtr values = sample.getVals();
            const Abc::UInt32ArraySamplePtr uvIndices = sample.getIndices();
            const std::size_t count = uvIndices ? uvIndices->size() : values->size();
            cornerUVs.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t v = uvIndices ? (*uvIndices)[i] : i;
                if (v >= values->size()) throw AlembicError("a UV index out of range");
                cornerUVs[i] = {(*values)[v].x, (*values)[v].y};
            }
            uvsPerVertex = uvParam.getScope() == AbcG::kVertexScope || uvParam.getScope() == AbcG::kVaryingScope;
            if (!uvsPerVertex && count != indices->size()) cornerUVs.clear();  // not one per corner: left out
            if (uvsPerVertex && count != points.size()) cornerUVs.clear();
        }
        std::size_t corner = 0;
        for (std::size_t f = 0; f < counts->size(); ++f) {
            const std::int32_t n = (*counts)[f];
            if (n < 3) throw AlembicError("a face with fewer than 3 corners");
            if (corner + static_cast<std::size_t>(n) > indices->size()) throw AlembicError("face counts past the indices");
            auto vertex = [&](std::int32_t k) {
                const std::int32_t v = (*indices)[corner + static_cast<std::size_t>(k)];
                if (v < 0 || static_cast<std::size_t>(v) >= points.size()) throw AlembicError("a face index out of range");
                return v;
            };
            auto uvOf = [&](std::int32_t k) -> ScalpMesh::UV {
                if (uvsPerVertex) return cornerUVs[static_cast<std::size_t>(vertex(k))];
                return cornerUVs[corner + static_cast<std::size_t>(k)];
            };
            for (std::int32_t k = 1; k + 1 < n; ++k) {
                triangles.push_back({vertex(0), vertex(k + 1), vertex(k)});
                if (!cornerUVs.empty()) {
                    uvs.push_back(uvOf(0));
                    uvs.push_back(uvOf(k + 1));
                    uvs.push_back(uvOf(k));
                }
            }
            corner += static_cast<std::size_t>(n);
        }
        auto mesh = std::make_shared<ScalpMesh>(std::move(points), std::move(triangles));
        if (!uvs.empty()) mesh->setCornerUVs(std::move(uvs));
        return mesh;
    } catch (const AlembicError&) {
        throw;
    } catch (const std::exception& e) {
        throw AlembicError("cannot read '" + impl_->path + "': " + e.what());
    }
}

// ---------------------------------------------------------------------------
// Mesh writer (scalp caches)
// ---------------------------------------------------------------------------

struct AlembicMeshWriter::Impl {
    std::filesystem::path target, tmp;
    Abc::OArchive archive;
    AbcG::OXform root;
    AbcG::OPolyMesh mesh;
    std::uint32_t ts = 0;
    std::size_t frames = 0;
    std::size_t pointCount = 0;
    std::string path;
};

AlembicMeshWriter::AlembicMeshWriter(const std::string& path, const std::string& name, double startSeconds,
                                     double secondsPerFrame)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = path;
    try {
        impl_->target = std::filesystem::u8path(path);
        impl_->tmp = tempPathFor(impl_->target);
        impl_->archive = Abc::OArchive(Alembic::AbcCoreOgawa::WriteArchive(), impl_->tmp.u8string());
        impl_->ts = impl_->archive.addTimeSampling(AbcG::TimeSampling(secondsPerFrame, startSeconds));
        impl_->root = AbcG::OXform(impl_->archive.getTop(), name, impl_->ts);
        impl_->mesh = AbcG::OPolyMesh(impl_->root, name + "Shape", impl_->ts);
    } catch (const std::exception& e) {
        std::error_code ec;
        std::filesystem::remove(impl_->tmp, ec);
        throw AlembicError("cannot create '" + path + "': " + e.what());
    }
}

AlembicMeshWriter::~AlembicMeshWriter() {
    try {
        close();
    } catch (...) {
        // Destructors must not throw; close() explicitly to see errors.
    }
}

std::size_t AlembicMeshWriter::frameCount() const { return impl_ ? impl_->frames : 0; }

void AlembicMeshWriter::writeFrame(const ScalpMesh& mesh) {
    if (!impl_) throw AlembicError("the mesh writer is closed");
    Impl& d = *impl_;
    if (d.frames > 0 && mesh.pointCount() != d.pointCount) {
        throw AlembicError("frame " + std::to_string(d.frames) + " has " + std::to_string(mesh.pointCount()) +
                           " points; the first frame has " + std::to_string(d.pointCount));
    }
    try {
        std::vector<Imath::V3f> pts(mesh.pointCount());
        for (std::size_t i = 0; i < pts.size(); ++i) pts[i] = {mesh.points()[i].x, mesh.points()[i].y, mesh.points()[i].z};
        if (d.frames == 0) {
            // The topology once, with the first frame: the triangles reversed as AbcExport writes
            // a Maya mesh's faces (meshAt's reversed fan brings them back facing as they do here).
            d.pointCount = mesh.pointCount();
            std::vector<std::int32_t> indices, counts;
            indices.reserve(mesh.triangleCount() * 3);
            counts.assign(mesh.triangleCount(), 3);
            for (const ScalpMesh::Triangle& t : mesh.triangles()) {
                indices.push_back(t[0]);
                indices.push_back(t[2]);
                indices.push_back(t[1]);
            }
            const Abc::P3fArraySample pointsSample(pts);
            const Abc::Int32ArraySample indicesSample(indices), countsSample(counts);
            AbcG::OPolyMeshSchema::Sample sample(pointsSample, indicesSample, countsSample);
            std::vector<Imath::V2f> uvs;
            if (mesh.hasUVs()) {
                // Per corner, in the reversed order the indices take.
                uvs.reserve(mesh.cornerUVs().size());
                for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
                    const ScalpMesh::UV& a = mesh.cornerUVs()[3 * t];
                    const ScalpMesh::UV& b = mesh.cornerUVs()[3 * t + 1];
                    const ScalpMesh::UV& c = mesh.cornerUVs()[3 * t + 2];
                    uvs.push_back({a[0], a[1]});
                    uvs.push_back({c[0], c[1]});
                    uvs.push_back({b[0], b[1]});
                }
                const Abc::V2fArraySample uvValues(uvs);
                const AbcG::OV2fGeomParam::Sample uvSample(uvValues, AbcG::kFacevaryingScope);
                sample.setUVs(uvSample);
            }
            d.mesh.getSchema().set(sample);
        } else {
            const Abc::P3fArraySample pointsSample(pts);
            AbcG::OPolyMeshSchema::Sample sample;
            sample.setPositions(pointsSample);
            d.mesh.getSchema().set(sample);
        }
        ++d.frames;
    } catch (const AlembicError&) {
        throw;
    } catch (const std::exception& e) {
        throw AlembicError("cannot write '" + d.path + "': " + e.what());
    }
}

void AlembicMeshWriter::close() {
    if (!impl_) return;
    std::unique_ptr<Impl> d = std::move(impl_);
    d->mesh = AbcG::OPolyMesh();
    d->root = AbcG::OXform();
    d->archive = Abc::OArchive();  // closes the file
    try {
        replaceFile(d->tmp, d->target);
    } catch (const std::runtime_error& e) {
        throw AlembicError(e.what());
    }
}

}  // namespace groomlab
