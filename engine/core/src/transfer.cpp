// SPDX-License-Identifier: Apache-2.0
#include "groomlab/portable_math.h"
#include "groomlab/transfer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "groomlab/frames.h"

namespace groomlab {

using json = nlohmann::json;

namespace {

Aabb boxOf(const ScalpMesh& mesh) {
    Aabb b;
    for (const Vec3f& p : mesh.points()) b.expand(p);
    return b;
}

// ---- Small dense linear algebra (doubles) ------------------------------------------------

using Mat3d = std::array<std::array<double, 3>, 3>;
using Mat4d = std::array<std::array<double, 4>, 4>;

// Eigenvalues (descending) and eigenvectors (columns of `vectors`, matching) of a symmetric N x N
// matrix, by cyclic Jacobi rotations.
template <std::size_t N>
void symmetricEigen(std::array<std::array<double, N>, N> a, std::array<double, N>* values,
                    std::array<std::array<double, N>, N>* vectors) {
    std::array<std::array<double, N>, N> v{};
    for (std::size_t i = 0; i < N; ++i) v[i][i] = 1.0;
    for (int sweep = 0; sweep < 64; ++sweep) {
        double off = 0.0;
        for (std::size_t p = 0; p < N; ++p) {
            for (std::size_t q = p + 1; q < N; ++q) off += a[p][q] * a[p][q];
        }
        if (off < 1e-30) break;
        for (std::size_t p = 0; p < N; ++p) {
            for (std::size_t q = p + 1; q < N; ++q) {
                if (std::abs(a[p][q]) < 1e-300) continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (std::size_t k = 0; k < N; ++k) {  // A <- A J
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (std::size_t k = 0; k < N; ++k) {  // A <- J^T A
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (std::size_t k = 0; k < N; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
    std::array<std::size_t, N> order{};
    for (std::size_t i = 0; i < N; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) { return a[x][x] > a[y][y]; });
    for (std::size_t i = 0; i < N; ++i) {
        (*values)[i] = a[order[i]][order[i]];
        for (std::size_t k = 0; k < N; ++k) (*vectors)[k][i] = v[k][order[i]];
    }
}

// How the points spread: the square roots of their covariance's eigenvalues, largest first.
std::array<double, 3> spread(const std::vector<Vec3f>& points) {
    std::array<double, 3> mean{};
    for (const Vec3f& p : points) {
        mean[0] += p.x;
        mean[1] += p.y;
        mean[2] += p.z;
    }
    for (double& m : mean) m /= static_cast<double>(std::max<std::size_t>(points.size(), 1));
    Mat3d c{};
    for (const Vec3f& p : points) {
        const double d[3] = {p.x - mean[0], p.y - mean[1], p.z - mean[2]};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) c[i][j] += d[i] * d[j];
        }
    }
    std::array<double, 3> values{};
    Mat3d vectors{};
    symmetricEigen<3>(c, &values, &vectors);
    for (double& v : values) v = std::sqrt(std::max(v, 0.0));
    return values;
}

Mat3f toMat3f(const Mat3d& m) {
    Mat3f out;
    for (int i = 0; i < 3; ++i) {
        out.rows[i] = Vec3f(static_cast<float>(m[i][0]), static_cast<float>(m[i][1]), static_cast<float>(m[i][2]));
    }
    return out;
}

Mat3d toMat3d(const Mat3f& m) {
    Mat3d out{};
    for (int i = 0; i < 3; ++i) out[i] = {m.rows[i].x, m.rows[i].y, m.rows[i].z};
    return out;
}

Mat3f multiply(const Mat3f& a, const Mat3f& b) {
    Mat3f out;
    for (int i = 0; i < 3; ++i) {
        const Vec3f& r = a.rows[i];
        out.rows[i] = Vec3f(r.x * b.rows[0].x + r.y * b.rows[1].x + r.z * b.rows[2].x,
                            r.x * b.rows[0].y + r.y * b.rows[1].y + r.z * b.rows[2].y,
                            r.x * b.rows[0].z + r.y * b.rows[1].z + r.z * b.rows[2].z);
    }
    return out;
}

Mat3f transpose(const Mat3f& m) {
    Mat3f out;
    out.rows[0] = {m.rows[0].x, m.rows[1].x, m.rows[2].x};
    out.rows[1] = {m.rows[0].y, m.rows[1].y, m.rows[2].y};
    out.rows[2] = {m.rows[0].z, m.rows[1].z, m.rows[2].z};
    return out;
}

// The matrix of the least rotation taking unit `from` onto unit `to` (rotateBetween's).
Mat3f rotationBetween(const Vec3f& from, const Vec3f& to) {
    const Vec3f x = rotateBetween({1, 0, 0}, from, to), y = rotateBetween({0, 1, 0}, from, to),
                z = rotateBetween({0, 0, 1}, from, to);
    Mat3f m;  // columns are where the axes go
    m.rows[0] = {x.x, y.x, z.x};
    m.rows[1] = {x.y, y.y, z.y};
    m.rows[2] = {x.z, y.z, z.z};
    return m;
}

// The matrix whose columns are a, b, c.
Mat3f columns(const Vec3f& a, const Vec3f& b, const Vec3f& c) {
    Mat3f m;
    m.rows[0] = {a.x, b.x, c.x};
    m.rows[1] = {a.y, b.y, c.y};
    m.rows[2] = {a.z, b.z, c.z};
    return m;
}

double determinant(const Mat3d& m) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

bool invert(const Mat3d& m, Mat3d* out) {
    const double det = determinant(m);
    if (!(std::abs(det) > 1e-300)) return false;
    const double k = 1.0 / det;
    (*out)[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * k;
    (*out)[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k;
    (*out)[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k;
    (*out)[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * k;
    (*out)[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k;
    (*out)[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k;
    (*out)[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * k;
    (*out)[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k;
    (*out)[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k;
    return true;
}

// Solves a x = b for a dense square system (Gaussian elimination, partial pivoting), several
// right-hand sides at once. False when it is singular.
bool solve(std::vector<std::vector<double>> a, std::vector<std::array<double, 3>>& b) {
    const std::size_t n = a.size();
    for (std::size_t col = 0; col < n; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < n; ++r) {
            if (std::abs(a[r][col]) > std::abs(a[pivot][col])) pivot = r;
        }
        if (!(std::abs(a[pivot][col]) > 1e-14)) return false;
        std::swap(a[pivot], a[col]);
        std::swap(b[pivot], b[col]);
        for (std::size_t r = col + 1; r < n; ++r) {
            const double f = a[r][col] / a[col][col];
            if (f == 0.0) continue;
            for (std::size_t k = col; k < n; ++k) a[r][k] -= f * a[col][k];
            for (int k = 0; k < 3; ++k) b[r][k] -= f * b[col][k];
        }
    }
    for (std::size_t i = n; i-- > 0;) {
        for (int k = 0; k < 3; ++k) {
            double s = b[i][k];
            for (std::size_t j = i + 1; j < n; ++j) s -= a[i][j] * b[j][k];
            b[i][k] = s / a[i][i];
        }
    }
    return true;
}

// Two markers at one point of a scalp: the mapping cannot tell where that point goes.
void requireDistinct(const std::vector<Vec3f>& points, const char* side) {
    float size = 0.0f;
    for (const Vec3f& p : points) {
        for (const Vec3f& q : points) size = std::max(size, (p - q).length());
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        for (std::size_t j = i + 1; j < points.size(); ++j) {
            if ((points[i] - points[j]).length() <= 1e-4f * size) {
                throw std::invalid_argument("markers " + std::to_string(i + 1) + " and " + std::to_string(j + 1) +
                                            " are at one point of the " + side + " scalp: move or remove one");
            }
        }
    }
}

// ---- UV lookup: a grid of a mesh's triangles over UV space ---------------------------------

class UVIndex {
public:
    explicit UVIndex(const ScalpMesh& mesh) : mesh_(&mesh) {
        const auto& uvs = mesh.cornerUVs();
        const std::size_t tris = mesh.triangleCount();
        if (uvs.size() < tris * 3 || tris == 0) return;
        lo_ = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        hi_ = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
        for (const auto& uv : uvs) {
            lo_[0] = std::min(lo_[0], uv[0]);
            lo_[1] = std::min(lo_[1], uv[1]);
            hi_[0] = std::max(hi_[0], uv[0]);
            hi_[1] = std::max(hi_[1], uv[1]);
        }
        cells_ = std::clamp(static_cast<int>(std::sqrt(static_cast<double>(tris))), 1, 1024);
        const float w = std::max(hi_[0] - lo_[0], 1e-12f), h = std::max(hi_[1] - lo_[1], 1e-12f);
        scale_ = {cells_ / w, cells_ / h};
        grid_.assign(static_cast<std::size_t>(cells_) * cells_, {});
        for (std::size_t f = 0; f < tris; ++f) {
            const auto& a = uvs[f * 3];
            const auto& b = uvs[f * 3 + 1];
            const auto& c = uvs[f * 3 + 2];
            const float area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
            if (std::abs(area) < 1e-14f) continue;  // no UV area: nothing maps into it
            const int x0 = cell(std::min({a[0], b[0], c[0]}), 0), x1 = cell(std::max({a[0], b[0], c[0]}), 0);
            const int y0 = cell(std::min({a[1], b[1], c[1]}), 1), y1 = cell(std::max({a[1], b[1], c[1]}), 1);
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) grid_[static_cast<std::size_t>(y) * cells_ + x].push_back(static_cast<std::int32_t>(f));
            }
        }
    }
    bool usable() const { return !grid_.empty(); }

    // The root of the mesh at this UV (the triangle holding it, else the nearest within a small
    // tolerance, for a UV on an edge); face -1 when none holds it.
    StrandRoot find(const ScalpMesh::UV& uv) const {
        StrandRoot none;
        if (grid_.empty()) return none;
        const float tol = 1e-4f;
        if (uv[0] < lo_[0] - tol || uv[0] > hi_[0] + tol || uv[1] < lo_[1] - tol || uv[1] > hi_[1] + tol) return none;
        const auto& uvs = mesh_->cornerUVs();
        StrandRoot best;
        float bestOutside = tol;
        for (const std::int32_t f : grid_[static_cast<std::size_t>(cell(uv[1], 1)) * cells_ + cell(uv[0], 0)]) {
            const auto& a = uvs[static_cast<std::size_t>(f) * 3];
            const auto& b = uvs[static_cast<std::size_t>(f) * 3 + 1];
            const auto& c = uvs[static_cast<std::size_t>(f) * 3 + 2];
            const float e1x = b[0] - a[0], e1y = b[1] - a[1], e2x = c[0] - a[0], e2y = c[1] - a[1];
            const float det = e1x * e2y - e2x * e1y;
            const float px = uv[0] - a[0], py = uv[1] - a[1];
            const float u = (px * e2y - e2x * py) / det;
            const float v = (e1x * py - px * e1y) / det;
            const float outside = std::max({-u, -v, u + v - 1.0f, 0.0f});
            if (outside == 0.0f) return {f, u, v};
            if (outside < bestOutside) {
                bestOutside = outside;
                float cu = std::max(u, 0.0f), cv = std::max(v, 0.0f);
                const float s = cu + cv;
                if (s > 1.0f) {
                    cu /= s;
                    cv /= s;
                }
                best = {f, cu, cv};
            }
        }
        return best;
    }

private:
    int cell(float value, int axis) const {
        return std::clamp(static_cast<int>((value - lo_[axis]) * scale_[axis]), 0, cells_ - 1);
    }
    const ScalpMesh* mesh_;
    std::array<float, 2> lo_{}, hi_{}, scale_{};
    int cells_ = 0;
    std::vector<std::vector<std::int32_t>> grid_;
};

// A mesh's UV tangent frame at a root (columns: tangent along u, bitangent, normal) and its
// length per UV unit there. False on a face without UV area.
bool uvFrame(const ScalpMesh& mesh, const StrandRoot& root, Mat3f* frame, float* density) {
    const auto& uvs = mesh.cornerUVs();
    const std::size_t f = static_cast<std::size_t>(root.face);
    if (!mesh.contains(root) || uvs.size() < (f + 1) * 3) return false;
    const auto& t = mesh.triangles()[f];
    const Vec3f e1 = mesh.points()[t[1]] - mesh.points()[t[0]];
    const Vec3f e2 = mesh.points()[t[2]] - mesh.points()[t[0]];
    const float d1u = uvs[f * 3 + 1][0] - uvs[f * 3][0], d1v = uvs[f * 3 + 1][1] - uvs[f * 3][1];
    const float d2u = uvs[f * 3 + 2][0] - uvs[f * 3][0], d2v = uvs[f * 3 + 2][1] - uvs[f * 3][1];
    const float det = d1u * d2v - d2u * d1v;
    const float area = e1.cross(e2).length();
    if (std::abs(det) < 1e-14f || area <= 0.0f) return false;
    const Vec3f dPdu = (e1 * d2v - e2 * d1v) / det;
    const Vec3f n = mesh.normal(root).normalized();
    Vec3f tangent = dPdu - n * dPdu.dot(n);
    if (tangent.length() <= 1e-12f) return false;
    tangent = tangent.normalized();
    *frame = columns(tangent, n.cross(tangent), n);
    *density = std::sqrt(area / std::abs(det));
    return true;
}

}  // namespace

// ---- ScalpFit ------------------------------------------------------------------------------

ScalpFit ScalpFit::boundingBoxes(const ScalpMesh& from, const ScalpMesh& to) {
    if (from.pointCount() == 0 || to.pointCount() == 0) return {};
    const Aabb a = boxOf(from);
    const Aabb b = boxOf(to);
    ScalpFit fit;
    fit.fromCenter = a.center();
    fit.toCenter = b.center();
    fit.scale = a.diagonal() > 0.0f ? b.diagonal() / a.diagonal() : 1.0f;
    return fit;
}

// ---- Methods and markers ---------------------------------------------------------------------

const char* transferMethodName(TransferMethod method) {
    switch (method) {
        case TransferMethod::Closest: return "closest";
        case TransferMethod::Bounds: return "bounds";
        case TransferMethod::MarkersAlign: return "markersAlign";
        case TransferMethod::MarkersWarp: return "markersWarp";
        case TransferMethod::UV: return "uv";
    }
    return "closest";
}

TransferMethod transferMethodFromName(const std::string& name) {
    for (TransferMethod m : {TransferMethod::Closest, TransferMethod::Bounds, TransferMethod::MarkersAlign,
                             TransferMethod::MarkersWarp, TransferMethod::UV}) {
        if (name == transferMethodName(m)) return m;
    }
    throw std::invalid_argument("transfer method '" + name + "': closest, bounds, markersAlign, markersWarp or uv");
}

bool usesMarkers(TransferMethod method) {
    return method == TransferMethod::MarkersAlign || method == TransferMethod::MarkersWarp;
}

Vec3f markerOldPosition(const TransferMarker& marker, const ScalpMesh& from) {
    return from.contains(marker.oldRoot) ? from.position(marker.oldRoot) : marker.oldPoint;
}

Vec3f markerNewPosition(const TransferMarker& marker, const ScalpMesh& to) {
    return to.contains(marker.newRoot) ? to.position(marker.newRoot) : marker.newPoint;
}

std::vector<TransferMarker> snapTransferMarkers(const std::vector<TransferMarker>& markers, const ScalpMesh& from,
                                                const ScalpMesh& to) {
    std::vector<TransferMarker> out = markers;
    for (TransferMarker& m : out) {
        if (!from.contains(m.oldRoot)) m.oldRoot = from.closestPoint(m.oldPoint).root;
        if (from.contains(m.oldRoot)) m.oldPoint = from.position(m.oldRoot);
        if (!to.contains(m.newRoot)) m.newRoot = to.closestPoint(m.newPoint).root;
        if (to.contains(m.newRoot)) m.newPoint = to.position(m.newRoot);
    }
    return out;
}

namespace {

json sideJson(const StrandRoot& root, const Vec3f& point) {
    json side = {{"point", {point.x, point.y, point.z}}};
    if (root.face >= 0) {
        side["face"] = root.face;
        side["u"] = root.u;
        side["v"] = root.v;
    }
    return side;
}

void readSide(const json& side, StrandRoot* root, Vec3f* point) {
    *root = StrandRoot{};
    if (side.contains("face")) *root = {side.at("face").get<std::int32_t>(), side.value("u", 0.0f), side.value("v", 0.0f)};
    if (side.contains("point")) {
        const json& p = side.at("point");
        *point = Vec3f(p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>());
    } else if (root->face < 0) {
        throw std::invalid_argument("a marker side has neither a face nor a point");
    }
}

}  // namespace

std::string transferMarkersToJson(const std::vector<TransferMarker>& markers) {
    json list = json::array();
    for (const TransferMarker& m : markers) list.push_back({{"old", sideJson(m.oldRoot, m.oldPoint)}, {"new", sideJson(m.newRoot, m.newPoint)}});
    return list.dump();
}

std::vector<TransferMarker> transferMarkersFromJson(const std::string& text) {
    std::vector<TransferMarker> out;
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return out;
    try {
        const json list = json::parse(text);
        if (!list.is_array()) throw std::invalid_argument("not a list of marker pairs");
        for (const json& pair : list) {
            TransferMarker m;
            readSide(pair.at("old"), &m.oldRoot, &m.oldPoint);
            readSide(pair.at("new"), &m.newRoot, &m.newPoint);
            out.push_back(m);
        }
    } catch (const json::exception& e) {
        throw std::invalid_argument(std::string("transfer markers: ") + e.what());
    } catch (const std::invalid_argument& e) {
        throw std::invalid_argument(std::string("transfer markers: ") + e.what());
    }
    return out;
}

// ---- Similarity ------------------------------------------------------------------------------

Similarity Similarity::inverse() const {
    Similarity inv;
    inv.rotation = transpose(rotation);
    inv.scale = scale != 0.0f ? 1.0f / scale : 1.0f;
    inv.translation = -(inv.rotation * translation) * inv.scale;
    return inv;
}

Similarity fitSimilarity(const std::vector<Vec3f>& from, const std::vector<Vec3f>& to) {
    if (from.size() != to.size()) throw std::invalid_argument("fitSimilarity: as many points on each side");
    if (from.size() < 3) {
        throw std::invalid_argument("aligning by markers needs at least 3 marker pairs (" + std::to_string(from.size()) + " given)");
    }
    for (const auto* side : {&from, &to}) {
        const std::array<double, 3> s = spread(*side);
        if (!(s[1] > 1e-3 * s[0])) {
            throw std::invalid_argument(std::string("the markers on the ") + (side == &from ? "old" : "new") +
                                        " scalp lie on one line: place at least 3 apart (front, side, back)");
        }
    }
    const double n = static_cast<double>(from.size());
    std::array<double, 3> ma{}, mb{};
    for (std::size_t i = 0; i < from.size(); ++i) {
        ma[0] += from[i].x / n, ma[1] += from[i].y / n, ma[2] += from[i].z / n;
        mb[0] += to[i].x / n, mb[1] += to[i].y / n, mb[2] += to[i].z / n;
    }
    Mat3d s{};  // S_jk = sum a_j b_k
    double aa = 0.0;
    for (std::size_t i = 0; i < from.size(); ++i) {
        const double a[3] = {from[i].x - ma[0], from[i].y - ma[1], from[i].z - ma[2]};
        const double b[3] = {to[i].x - mb[0], to[i].y - mb[1], to[i].z - mb[2]};
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k) s[j][k] += a[j] * b[k];
        }
        aa += a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
    }
    // Horn (1987): the unit quaternion maximizing sum b . R a is the top eigenvector of N.
    const double sxx = s[0][0], sxy = s[0][1], sxz = s[0][2], syx = s[1][0], syy = s[1][1], syz = s[1][2],
                 szx = s[2][0], szy = s[2][1], szz = s[2][2];
    const Mat4d nm = {{{sxx + syy + szz, syz - szy, szx - sxz, sxy - syx},
                       {syz - szy, sxx - syy - szz, sxy + syx, szx + sxz},
                       {szx - sxz, sxy + syx, -sxx + syy - szz, syz + szy},
                       {sxy - syx, szx + sxz, syz + szy, -sxx - syy + szz}}};
    std::array<double, 4> values{};
    Mat4d vectors{};
    symmetricEigen<4>(nm, &values, &vectors);
    double w = vectors[0][0], x = vectors[1][0], y = vectors[2][0], z = vectors[3][0];
    const double len = std::sqrt(w * w + x * x + y * y + z * z);
    w /= len, x /= len, y /= len, z /= len;
    const Mat3d r = {{{w * w + x * x - y * y - z * z, 2 * (x * y - w * z), 2 * (x * z + w * y)},
                      {2 * (x * y + w * z), w * w - x * x + y * y - z * z, 2 * (y * z - w * x)},
                      {2 * (x * z - w * y), 2 * (y * z + w * x), w * w - x * x - y * y + z * z}}};
    // Umeyama's scale: sum b . R a / sum |a|^2 (the least squares scale for this rotation).
    double bra = 0.0;
    for (int j = 0; j < 3; ++j) {
        for (int k = 0; k < 3; ++k) bra += r[k][j] * s[j][k];
    }
    Similarity out;
    out.rotation = toMat3f(r);
    out.scale = static_cast<float>(aa > 0.0 ? bra / aa : 1.0);
    const Vec3f mA(static_cast<float>(ma[0]), static_cast<float>(ma[1]), static_cast<float>(ma[2]));
    const Vec3f mB(static_cast<float>(mb[0]), static_cast<float>(mb[1]), static_cast<float>(mb[2]));
    out.translation = mB - out.rotation * mA * out.scale;
    return out;
}

// ---- Spline warp ---------------------------------------------------------------------------

SplineWarp::SplineWarp(const std::vector<Vec3f>& from, const std::vector<Vec3f>& to, double smoothing) {
    if (from.size() != to.size()) throw std::invalid_argument("SplineWarp: as many points on each side");
    if (from.size() < 4) {
        throw std::invalid_argument("a warp by markers needs at least 4 marker pairs (" + std::to_string(from.size()) + " given)");
    }
    const std::array<double, 3> s = spread(from);
    if (!(s[2] > 1e-2 * s[0])) throw std::invalid_argument("the markers on the old scalp lie on one plane");
    const std::size_t n = from.size();
    for (const Vec3f& p : from) {
        origin_[0] += p.x / static_cast<double>(n);
        origin_[1] += p.y / static_cast<double>(n);
        origin_[2] += p.z / static_cast<double>(n);
    }
    size_ = 0.0;
    for (const Vec3f& p : from) {
        size_ = std::max(size_, std::sqrt((p.x - origin_[0]) * (p.x - origin_[0]) + (p.y - origin_[1]) * (p.y - origin_[1]) +
                                          (p.z - origin_[2]) * (p.z - origin_[2])));
    }
    if (!(size_ > 0.0)) throw std::invalid_argument("the markers on the old scalp are all at one point");
    for (const Vec3f& p : from) centers_.push_back({(p.x - origin_[0]) / size_, (p.y - origin_[1]) / size_, (p.z - origin_[2]) / size_});
    // [K + smoothing I, P; P^T, 0] [w; a] = [y; 0], K_ij = |c_i - c_j|^3, P_i = (1, c_i).
    const std::size_t m = n + 4;
    std::vector<std::vector<double>> a(m, std::vector<double>(m, 0.0));
    std::vector<std::array<double, 3>> b(m, {0.0, 0.0, 0.0});
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            const double dx = centers_[i][0] - centers_[j][0], dy = centers_[i][1] - centers_[j][1], dz = centers_[i][2] - centers_[j][2];
            const double r = std::sqrt(dx * dx + dy * dy + dz * dz);
            a[i][j] = r * r * r;
        }
        a[i][i] += smoothing;
        a[i][n] = a[n][i] = 1.0;
        for (int k = 0; k < 3; ++k) a[i][n + 1 + k] = a[n + 1 + k][i] = centers_[i][k];
        b[i] = {to[i].x, to[i].y, to[i].z};
    }
    if (!solve(std::move(a), b)) throw std::invalid_argument("the markers do not make a warp (degenerate placement)");
    weights_.assign(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n));
    for (int k = 0; k < 3; ++k) {
        for (int c = 0; c < 4; ++c) affine_[k][c] = b[n + c][k];
    }
}

Vec3f SplineWarp::apply(const Vec3f& p) const {
    if (centers_.empty()) return p;
    const double x[3] = {(p.x - origin_[0]) / size_, (p.y - origin_[1]) / size_, (p.z - origin_[2]) / size_};
    double out[3];
    for (int k = 0; k < 3; ++k) out[k] = affine_[k][0] + affine_[k][1] * x[0] + affine_[k][2] * x[1] + affine_[k][3] * x[2];
    for (std::size_t i = 0; i < centers_.size(); ++i) {
        const double dx = x[0] - centers_[i][0], dy = x[1] - centers_[i][1], dz = x[2] - centers_[i][2];
        const double r = std::sqrt(dx * dx + dy * dy + dz * dz);
        const double phi = r * r * r;
        for (int k = 0; k < 3; ++k) out[k] += weights_[i][k] * phi;
    }
    return {static_cast<float>(out[0]), static_cast<float>(out[1]), static_cast<float>(out[2])};
}

Mat3f SplineWarp::jacobian(const Vec3f& p) const {
    if (centers_.empty()) return Mat3f::identity();
    const double x[3] = {(p.x - origin_[0]) / size_, (p.y - origin_[1]) / size_, (p.z - origin_[2]) / size_};
    Mat3d j{};
    for (int k = 0; k < 3; ++k) {
        for (int c = 0; c < 3; ++c) j[k][c] = affine_[k][1 + c];
    }
    for (std::size_t i = 0; i < centers_.size(); ++i) {
        const double d[3] = {x[0] - centers_[i][0], x[1] - centers_[i][1], x[2] - centers_[i][2]};
        const double r = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; ++k) {
            for (int c = 0; c < 3; ++c) j[k][c] += weights_[i][k] * 3.0 * r * d[c];  // grad r^3 = 3 r d
        }
    }
    for (auto& row : j) {
        for (double& v : row) v /= size_;  // d x_normalized / d x = 1 / size
    }
    return toMat3f(j);
}

bool rotationAndScale(const Mat3f& m, Mat3f* rotation, float* scale) {
    Mat3d r = toMat3d(m);
    const double det = determinant(r);
    if (!(det > 1e-300) || !std::isfinite(det)) return false;
    // Newton's iteration for the orthogonal polar factor: R <- (R + R^-T) / 2.
    for (int it = 0; it < 30; ++it) {
        Mat3d inv{};
        if (!invert(r, &inv)) return false;
        double change = 0.0;
        for (int i = 0; i < 3; ++i) {
            for (int k = 0; k < 3; ++k) {
                const double next = 0.5 * (r[i][k] + inv[k][i]);
                change = std::max(change, std::abs(next - r[i][k]));
                r[i][k] = next;
            }
        }
        if (change < 1e-12) break;
    }
    *rotation = toMat3f(r);
    *scale = static_cast<float>(pm::cbrt(det));
    return true;
}

// ---- ScalpMapping ----------------------------------------------------------------------------

struct ScalpMapping::Impl {
    Similarity similarity, inverse;  // align (and warp's fallback where its Jacobian folds)
    SplineWarp warp, warpBack;
    std::unique_ptr<UVIndex> toUVs, fromUVs;
    mutable std::atomic<std::size_t> uvMisses{0};
};

ScalpMapping::ScalpMapping(const ScalpMesh& from, const ScalpMesh& to)
    : from_(&from), to_(&to), impl_(std::make_unique<Impl>()) {}

ScalpMapping::ScalpMapping(const ScalpMesh& from, const ScalpMesh& to, const TransferOptions& options,
                           std::vector<std::string>* warnings)
    : ScalpMapping(from, to) {
    options_ = options;
    build(warnings);
}

ScalpMapping::ScalpMapping(const ScalpMesh& from, const ScalpMesh& to, const ScalpFit& fit) : ScalpMapping(from, to) {
    const bool identity = fit.scale == 1.0f && fit.fromCenter == fit.toCenter;
    options_.method = identity ? TransferMethod::Closest : TransferMethod::Bounds;
    method_ = options_.method;
    fit_ = fit;
}

ScalpMapping::~ScalpMapping() = default;

ScalpMapping::ScalpMapping(const ScalpMapping& other) : ScalpMapping(*other.from_, *other.to_) {
    options_ = other.options_;
    method_ = other.method_;
    fit_ = other.fit_;
    impl_->similarity = other.impl_->similarity;
    impl_->inverse = other.impl_->inverse;
    impl_->warp = other.impl_->warp;
    impl_->warpBack = other.impl_->warpBack;
    if (method_ == TransferMethod::UV) {
        impl_->toUVs = std::make_unique<UVIndex>(*to_);
        impl_->fromUVs = std::make_unique<UVIndex>(*from_);
    }
}

ScalpMapping ScalpMapping::onMeshes(const ScalpMesh& from, const ScalpMesh& to) const {
    ScalpMapping out(from, to);
    out.options_ = options_;
    if (byNormals()) {
        out.method_ = method_;
        out.fit_ = fit_;  // as given (a host's fit), on any pose
        return out;
    }
    out.build(nullptr);
    return out;
}

void ScalpMapping::build(std::vector<std::string>* warnings) {
    method_ = options_.method;
    if (from_->triangleCount() == 0) throw std::invalid_argument("transfer: the old scalp has no faces");
    if (to_->triangleCount() == 0) throw std::invalid_argument("transfer: the new scalp has no faces");
    switch (method_) {
        case TransferMethod::Closest: fit_ = ScalpFit::identity(); return;
        case TransferMethod::Bounds: fit_ = ScalpFit::boundingBoxes(*from_, *to_); return;
        case TransferMethod::UV: {
            if (!from_->hasUVs()) throw std::invalid_argument("transfer by UVs: the old scalp has no UVs");
            if (!to_->hasUVs()) throw std::invalid_argument("transfer by UVs: the new scalp has no UVs");
            impl_->toUVs = std::make_unique<UVIndex>(*to_);
            impl_->fromUVs = std::make_unique<UVIndex>(*from_);
            return;
        }
        case TransferMethod::MarkersAlign:
        case TransferMethod::MarkersWarp: break;
    }
    std::vector<Vec3f> a, b;
    for (const TransferMarker& m : options_.markers) {
        a.push_back(markerOldPosition(m, *from_));
        b.push_back(markerNewPosition(m, *to_));
    }
    if (a.size() < 3) {
        throw std::invalid_argument(std::string("transfer by markers needs at least 3 marker pairs (") + std::to_string(a.size()) +
                                    " given): add pairs, a point on the old scalp and the same point on the new one");
    }
    requireDistinct(a, "old");
    requireDistinct(b, "new");
    impl_->similarity = fitSimilarity(a, b);
    impl_->inverse = impl_->similarity.inverse();
    if (method_ != TransferMethod::MarkersWarp) return;
    try {
        impl_->warp = SplineWarp(a, b);
        impl_->warpBack = SplineWarp(b, a);
    } catch (const std::invalid_argument& e) {
        method_ = TransferMethod::MarkersAlign;
        impl_->warp = impl_->warpBack = SplineWarp();
        if (warnings) {
            warnings->push_back(std::string("the warp needs 4 or more marker pairs spread off one plane (") + e.what() +
                                "): aligned by the markers instead");
        }
    }
}

std::size_t ScalpMapping::uvMisses() const { return impl_->uvMisses.load(); }

Vec3f ScalpMapping::apply(const Vec3f& p) const {
    switch (method_) {
        case TransferMethod::Closest:
        case TransferMethod::Bounds: return fit_.apply(p);
        case TransferMethod::MarkersAlign: return impl_->similarity.apply(p);
        case TransferMethod::MarkersWarp: return impl_->warp.apply(p);
        case TransferMethod::UV: return p;
    }
    return p;
}

Vec3f ScalpMapping::unapply(const Vec3f& p) const {
    switch (method_) {
        case TransferMethod::Closest:
        case TransferMethod::Bounds: return fit_.unapply(p);
        case TransferMethod::MarkersAlign: return impl_->inverse.apply(p);
        case TransferMethod::MarkersWarp: return impl_->warpBack.apply(p);
        case TransferMethod::UV: return p;
    }
    return p;
}

MappedPoint ScalpMapping::mapPoint(const Vec3f& oldPosition) const {
    return map(from_->closestPoint(oldPosition).root);
}

MappedPoint ScalpMapping::map(const StrandRoot& oldRoot) const {
    MappedPoint out;
    if (!from_->contains(oldRoot)) return out;
    const Vec3f p = from_->position(oldRoot);
    const Vec3f n0 = from_->normal(oldRoot).normalized();
    Mat3f turn = Mat3f::identity();
    float scale = 1.0f;
    switch (method_) {
        case TransferMethod::Closest:
        case TransferMethod::Bounds:
            out.mapped = fit_.apply(p);
            out.target = to_->closestPoint(out.mapped);
            out.rotation = rotationBetween(n0, out.target.normal.normalized());
            out.scale = fit_.scale;
            return out;  // byNormals: no further correction
        case TransferMethod::MarkersAlign:
            out.mapped = impl_->similarity.apply(p);
            turn = impl_->similarity.rotation;
            scale = impl_->similarity.scale;
            out.target = to_->closestPoint(out.mapped);
            break;
        case TransferMethod::MarkersWarp:
            out.mapped = impl_->warp.apply(p);
            if (!rotationAndScale(impl_->warp.jacobian(p), &turn, &scale)) {
                turn = impl_->similarity.rotation;  // folded here: the overall turn
                scale = impl_->similarity.scale;
            }
            out.target = to_->closestPoint(out.mapped);
            break;
        case TransferMethod::UV: {
            const StrandRoot hit = impl_->toUVs ? impl_->toUVs->find(from_->uvAt(oldRoot)) : StrandRoot{};
            Mat3f f0, f1;
            float d0 = 0.0f, d1 = 0.0f;
            if (hit.face >= 0 && uvFrame(*from_, oldRoot, &f0, &d0) && uvFrame(*to_, hit, &f1, &d1)) {
                out.target.root = hit;
                out.target.position = to_->position(hit);
                out.target.normal = to_->normal(hit);
                out.mapped = out.target.position;
                turn = multiply(f1, transpose(f0));
                scale = d0 > 0.0f ? d1 / d0 : 1.0f;
            } else {
                // No UV match: the closest point, turned by the normals.
                impl_->uvMisses.fetch_add(1);
                out.mapped = p;
                out.target = to_->closestPoint(p);
                out.rotation = rotationBetween(n0, out.target.normal.normalized());
                out.scale = 1.0f;
                return out;
            }
            break;
        }
    }
    // What stood on the old normal stands on the new one: the least turn from where the mapping
    // took the normal to the new scalp's normal there (none for an exact similarity).
    const Vec3f turned = (turn * n0).normalized();
    out.rotation = multiply(rotationBetween(turned, out.target.normal.normalized()), turn);
    out.scale = scale;
    return out;
}

SurfacePoint ScalpMapping::back(const Vec3f& newPosition, const StrandRoot& newRoot) const {
    if (method_ == TransferMethod::UV) {
        const StrandRoot root = to_->contains(newRoot) ? newRoot : to_->closestPoint(newPosition).root;
        const StrandRoot hit = impl_->fromUVs && to_->contains(root) ? impl_->fromUVs->find(to_->uvAt(root)) : StrandRoot{};
        if (hit.face >= 0) return SurfacePoint{hit, from_->position(hit), from_->normal(hit)};
        impl_->uvMisses.fetch_add(1);
        return from_->closestPoint(newPosition);
    }
    return from_->closestPoint(unapply(newPosition));
}

}  // namespace groomlab
