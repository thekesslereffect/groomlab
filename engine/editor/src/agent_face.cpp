// SPDX-License-Identifier: Apache-2.0
#include "groomlab/editor/agent_face.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace groomlab::agent {

namespace {

constexpr float kNone = -std::numeric_limits<float>::infinity();

// The head's own coordinates about its middle: a (toward its left), b (up), c (toward its front).
struct Frame {
    Vec3f m, left, up, front;
    Vec3f world(float a, float b, float c) const { return m + left * a + up * b + front * c; }
    float a(const Vec3f& p) const { return (p - m).dot(left); }
    float b(const Vec3f& p) const { return (p - m).dot(up); }
    float c(const Vec3f& p) const { return (p - m).dot(front); }
};

// A depth map: the mesh seen along -`look` over a grid of (s, t) (two of a, b, c), each cell the
// nearest surface's coordinate along `look` (kNone: missed) and the hit.
struct DepthMap {
    int ns = 0, nt = 0;
    float s0 = 0, t0 = 0, cell = 1;
    std::vector<float> depth;
    std::vector<SurfacePoint> hits;
    float at(int i, int j) const {
        if (i < 0 || j < 0 || i >= ns || j >= nt) return kNone;
        return depth[static_cast<std::size_t>(j) * static_cast<std::size_t>(ns) + static_cast<std::size_t>(i)];
    }
    const SurfacePoint& hit(int i, int j) const { return hits[static_cast<std::size_t>(j) * static_cast<std::size_t>(ns) + static_cast<std::size_t>(i)]; }
    int si(float s) const { return static_cast<int>(std::floor((s - s0) / cell)); }
    int tj(float t) const { return static_cast<int>(std::floor((t - t0) / cell)); }
    float s(int i) const { return s0 + (static_cast<float>(i) + 0.5f) * cell; }
    float t(int j) const { return t0 + (static_cast<float>(j) + 0.5f) * cell; }
};

// `origin(s, t)` a point on the far side, `dir` the way the rays go, `depthOf(hit)` the coordinate kept.
template <class Origin, class DepthOf>
DepthMap depthMap(const ScalpMesh& mesh, float s0, float s1, float t0, float t1, float cell, const Vec3f& dir, Origin origin, DepthOf depthOf) {
    DepthMap d;
    d.s0 = s0;
    d.t0 = t0;
    d.cell = cell;
    d.ns = std::max(1, static_cast<int>(std::ceil((s1 - s0) / cell)));
    d.nt = std::max(1, static_cast<int>(std::ceil((t1 - t0) / cell)));
    d.depth.assign(static_cast<std::size_t>(d.ns) * static_cast<std::size_t>(d.nt), kNone);
    d.hits.resize(d.depth.size());
    for (int j = 0; j < d.nt; ++j) {
        for (int i = 0; i < d.ns; ++i) {
            SurfacePoint hit;
            if (!mesh.intersectRay(origin(d.s(i), d.t(j)), dir, &hit)) continue;
            const std::size_t k = static_cast<std::size_t>(j) * static_cast<std::size_t>(d.ns) + static_cast<std::size_t>(i);
            d.depth[k] = depthOf(hit.position);
            d.hits[k] = hit;
        }
    }
    return d;
}

float smoothstep(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

}  // namespace

const std::vector<std::string>& faceLandmarkNames() {
    static const std::vector<std::string> names = {
        "nose_tip",         "chin",           "left_eye",          "right_eye",          "left_eye_inner",  "left_eye_outer",
        "right_eye_inner",  "right_eye_outer", "left_mouth_corner", "right_mouth_corner", "left_ear_top",    "left_earlobe",
        "right_ear_top",    "right_earlobe",  "hairline_middle",   "crown",              "nape"};
    return names;
}

const FaceLandmark* FaceLandmarks::find(const std::string& name) const {
    for (const FaceLandmark& l : points) {
        if (l.name == name) return &l;
    }
    return nullptr;
}

FaceLandmarks faceLandmarks(const ScalpMesh& mesh, const std::string& upAxis) {
    FaceLandmarks out;
    if (mesh.triangleCount() == 0) {
        out.why = "the mesh has no triangles";
        return out;
    }
    const ScalpLandmarks marks = scalpLandmarks(mesh, upAxis);
    Frame f{marks.middle, marks.frame.left, marks.frame.up, marks.frame.front};
    float aLo = 1e30f, aHi = -1e30f, bLo = 1e30f, bHi = -1e30f, cLo = 1e30f, cHi = -1e30f;
    for (const Vec3f& p : mesh.points()) {
        aLo = std::min(aLo, f.a(p)), aHi = std::max(aHi, f.a(p));
        bLo = std::min(bLo, f.b(p)), bHi = std::max(bHi, f.b(p));
        cLo = std::min(cLo, f.c(p)), cHi = std::max(cHi, f.c(p));
    }
    const float size = std::max({aHi - aLo, bHi - bLo, cHi - cLo, 1e-6f});
    const float depthAll = std::max(cHi - cLo, 1e-6f);
    const float cell = size / 160.0f;
    const float margin = 0.05f * size;

    // The face seen from the front: each cell's most forward surface.
    const DepthMap front = depthMap(
        mesh, aLo, aHi, bLo, bHi, cell, -f.front, [&](float a, float b) { return f.world(a, b, cHi + margin); },
        [&](const Vec3f& p) { return f.c(p); });
    // The middle line's profile: the most forward cell near a = 0 at each height.
    const int i0 = front.si(0.0f);
    const int band = std::max(1, static_cast<int>(0.02f * size / cell));
    std::vector<float> profile(static_cast<std::size_t>(front.nt), kNone);
    std::vector<int> profileCol(static_cast<std::size_t>(front.nt), i0);
    for (int j = 0; j < front.nt; ++j) {
        for (int i = i0 - band; i <= i0 + band; ++i) {
            const float c = front.at(i, j);
            if (c > profile[static_cast<std::size_t>(j)]) profile[static_cast<std::size_t>(j)] = c, profileCol[static_cast<std::size_t>(j)] = i;
        }
    }
    int nose = -1;
    for (int j = 0; j < front.nt; ++j) {
        if (profile[static_cast<std::size_t>(j)] > kNone && (nose < 0 || profile[static_cast<std::size_t>(j)] > profile[static_cast<std::size_t>(nose)])) nose = j;
    }
    if (nose < 0) {
        out.why = "the mesh has no front";
        return out;
    }
    const float noseC = profile[static_cast<std::size_t>(nose)];
    // A nose stands out of the cheeks either side of it (a ball's front only curves away a little).
    const float halfWidth = 0.5f * (aHi - aLo);
    const int side = std::max(1, static_cast<int>(0.24f * halfWidth / cell));
    const float cheeks = std::max(front.at(i0 - side, nose), front.at(i0 + side, nose));
    const float prominence = cheeks > kNone ? noseC - cheeks : 0.0f;
    if (prominence < 0.08f * halfWidth) {
        out.why = "no face: nothing stands out of the front's middle as a nose does (a scalp cap or another shape)";
        return out;
    }
    // The chin: walking down the profile from the nose, the lowest point still near the face's front
    // (the neck falls back below it).
    const float near = noseC - 0.18f * depthAll;
    int chin = nose, gap = 0;
    for (int j = nose - 1; j >= 0 && gap <= 2; --j) {
        if (profile[static_cast<std::size_t>(j)] >= near) {
            chin = j;
            gap = 0;
        } else {
            ++gap;
        }
    }
    const float noseB = front.t(nose), chinB = front.t(chin);
    const float third = noseB - chinB;
    if (third < 0.1f * size || third > 0.5f * size) {
        out.why = "no face: the nose and the chin are not a face's distance apart";
        return out;
    }
    float chinDrop = 0.0f;  // how far the profile falls back below the chin
    for (int j = chin - 1; j >= 0 && front.t(j) > chinB - 0.4f * third; --j) {
        if (profile[static_cast<std::size_t>(j)] > kNone) chinDrop = std::max(chinDrop, profile[static_cast<std::size_t>(chin)] - profile[static_cast<std::size_t>(j)]);
    }
    // The head's width across the forehead (above the ears).
    float width = 0.0f;
    {
        const int j = front.tj(noseB + 1.3f * third);
        int lo = front.ns, hi = -1;
        for (int i = 0; i < front.ns; ++i) {
            if (front.at(i, j) > kNone) lo = std::min(lo, i), hi = std::max(hi, i);
        }
        width = hi >= lo ? static_cast<float>(hi - lo + 1) * cell : 2.0f * halfWidth;
    }
    out.head = true;
    out.third = third;
    out.width = width;
    auto put = [&](const std::string& name, const SurfacePoint& at, float confidence) {
        out.points.push_back({name, at.position, at.normal.normalized(), std::clamp(confidence, 0.0f, 1.0f)});
    };
    // A point of the face's front at (a, b): its cell's hit, else the nearest surface.
    auto frontAt = [&](float a, float b) {
        const int i = std::clamp(front.si(a), 0, front.ns - 1), j = std::clamp(front.tj(b), 0, front.nt - 1);
        if (front.at(i, j) > kNone) return front.hit(i, j);
        return mesh.closestPoint(f.world(a, b, noseC));
    };
    put("nose_tip", front.hit(profileCol[static_cast<std::size_t>(nose)], nose), std::min(1.0f, prominence / (0.2f * halfWidth)));
    put("chin", front.hit(profileCol[static_cast<std::size_t>(chin)], chin), chinDrop > 0.15f * depthAll ? 0.85f : 0.55f);

    // The eyes: the sockets' deepest point against the face around them, near the usual height and
    // spacing.
    const float eyeB = noseB + 0.45f * third;
    float eyeConfidence[2] = {0.0f, 0.0f};
    Vec3f eyeAt[2];
    float eyeA[2] = {0.0f, 0.0f}, eyeBs[2] = {eyeB, eyeB};
    const int ring = std::max(2, static_cast<int>(0.12f * width / cell));
    for (int k = 0; k < 2; ++k) {
        const float sign = k == 0 ? 1.0f : -1.0f;  // left, right
        const float a0 = sign * 0.22f * width;
        float best = 1e30f;
        int bi = front.si(a0), bj = front.tj(eyeB);
        for (int j = front.tj(eyeB - 0.2f * third); j <= front.tj(eyeB + 0.2f * third); ++j) {
            for (int i = front.si(a0 - 0.08f * width); i <= front.si(a0 + 0.08f * width); ++i) {
                const float c = front.at(i, j);
                if (!(c > kNone)) continue;
                float sum = 0.0f;
                int n = 0;
                for (int dj = -ring; dj <= ring; dj += 2) {
                    for (int di = -ring; di <= ring; di += 2) {
                        const float q = front.at(i + di, j + dj);
                        if (q > kNone) sum += q, ++n;
                    }
                }
                if (n < 4) continue;
                const float residual = c - sum / static_cast<float>(n);
                if (residual < best) best = residual, bi = i, bj = j;
            }
        }
        eyeA[k] = front.s(bi);
        eyeBs[k] = front.t(bj);
        eyeConfidence[k] = best < 1e29f ? 0.85f * std::clamp(-best / (0.1f * third), 0.0f, 1.0f) : 0.0f;
        const SurfacePoint at = frontAt(eyeA[k], eyeBs[k]);
        eyeAt[k] = at.position;
        put(k == 0 ? "left_eye" : "right_eye", at, eyeConfidence[k]);
    }
    for (int k = 0; k < 2; ++k) {
        const float sign = k == 0 ? 1.0f : -1.0f;
        const std::string prefix = k == 0 ? "left_eye" : "right_eye";
        put(prefix + "_inner", frontAt(eyeA[k] - sign * 0.105f * width, eyeBs[k]), 0.5f * eyeConfidence[k]);
        put(prefix + "_outer", frontAt(eyeA[k] + sign * 0.105f * width, eyeBs[k]), 0.5f * eyeConfidence[k]);
    }
    // The mouth's corners: its usual height and width.
    const float mouthB = noseB - 0.33f * third;
    put("left_mouth_corner", frontAt(0.18f * width, mouthB), 0.35f);
    put("right_mouth_corner", frontAt(-0.18f * width, mouthB), 0.35f);

    // The ears: from each side, what sticks out of the head's side around the ears' height.
    const float brow = 0.5f * (eyeBs[0] + eyeBs[1]) + 0.2f * third;
    for (int k = 0; k < 2; ++k) {
        const float sign = k == 0 ? 1.0f : -1.0f;
        const Vec3f outward = f.left * sign;
        const DepthMap sideMap = depthMap(
            mesh, cLo, cHi, noseB - 0.9f * third, brow + 0.6f * third, cell, -outward,
            [&](float c, float b) { return f.world(sign * (std::max(std::abs(aLo), std::abs(aHi)) + margin), b, c); },
            [&](const Vec3f& p) { return sign * f.a(p); });
        // The side's broad shape (a wide average), and each cell's bump above it.
        const int r = std::max(2, static_cast<int>(0.3f * third / cell));
        std::vector<float> bump(sideMap.depth.size(), 0.0f);
        float strongest = 0.0f;
        int si = -1, sj = -1;
        const float cMin = -0.45f * depthAll, cMax = 0.15f * depthAll;
        for (int j = 0; j < sideMap.nt; ++j) {
            for (int i = 0; i < sideMap.ns; ++i) {
                const float d = sideMap.at(i, j);
                if (!(d > kNone)) continue;
                float sum = 0.0f;
                int n = 0;
                for (int dj = -r; dj <= r; dj += 2) {
                    for (int di = -r; di <= r; di += 2) {
                        const float q = sideMap.at(i + di, j + dj);
                        if (q > kNone) sum += q, ++n;
                    }
                }
                if (n < 4) continue;
                const float b = d - sum / static_cast<float>(n);
                bump[static_cast<std::size_t>(j) * static_cast<std::size_t>(sideMap.ns) + static_cast<std::size_t>(i)] = b;
                const float cc = sideMap.s(i);
                if (cc >= cMin && cc <= cMax && b > strongest) strongest = b, si = i, sj = j;
            }
        }
        const std::string name = k == 0 ? "left_ear" : "right_ear";
        const std::string lobe = k == 0 ? "left_earlobe" : "right_earlobe";
        if (si >= 0 && strongest > 0.08f * third) {
            // The bump's cells joined to the strongest (over a third of its height), top and bottom.
            const float threshold = 0.35f * strongest;
            std::vector<char> seen(bump.size(), 0);
            std::vector<std::array<int, 2>> stack{{si, sj}};
            int top = sj, bottom = sj, topI = si, bottomI = si, cells = 0;
            while (!stack.empty()) {
                const auto [i, j] = stack.back();
                stack.pop_back();
                if (i < 0 || j < 0 || i >= sideMap.ns || j >= sideMap.nt) continue;
                const std::size_t q = static_cast<std::size_t>(j) * static_cast<std::size_t>(sideMap.ns) + static_cast<std::size_t>(i);
                if (seen[q] || bump[q] < threshold) continue;
                seen[q] = 1;
                ++cells;
                if (j > top) top = j, topI = i;
                if (j < bottom) bottom = j, bottomI = i;
                stack.push_back({i + 1, j});
                stack.push_back({i - 1, j});
                stack.push_back({i, j + 1});
                stack.push_back({i, j - 1});
            }
            const float confidence = std::min(1.0f, strongest / (0.25f * third)) * std::min(1.0f, static_cast<float>(cells) * cell * cell /
                                                                                                 (0.2f * third * third));
            put(name + "_top", sideMap.hit(topI, top), 0.9f * confidence);
            put(lobe, sideMap.hit(bottomI, bottom), 0.9f * confidence);
        } else {
            put(name + "_top", mesh.closestPoint(f.world(sign * halfWidth, brow, -0.05f * depthAll)), 0.15f);
            put(lobe, mesh.closestPoint(f.world(sign * halfWidth, noseB - 0.15f * third, -0.05f * depthAll)), 0.15f);
        }
    }
    // The hairline at the forehead's middle: the face's thirds (brow to hairline as nose to chin).
    put("hairline_middle", frontAt(0.0f, 0.5f * (eyeBs[0] + eyeBs[1]) + 0.95f * third), 0.35f);
    for (const char* name : {"crown", "nape"}) {
        const Landmark* l = marks.find(name);
        put(name, mesh.closestPoint(l->point), 0.5f);
    }
    return out;
}

float hairlineHeight(const FaceLandmarks& face, const ScalpLandmarks& marks, float angleDegrees) {
    const Frame f{marks.middle, marks.frame.left, marks.frame.up, marks.frame.front};
    const FaceLandmark* hair = face.find("hairline_middle");
    const int k = angleDegrees >= 0.0f ? 0 : 1;  // the side
    const FaceLandmark* top = face.find(k == 0 ? "left_ear_top" : "right_ear_top");
    const FaceLandmark* lobe = face.find(k == 0 ? "left_earlobe" : "right_earlobe");
    if (!face.head || !hair || !top || !lobe) return std::numeric_limits<float>::infinity();
    const float T = face.third;
    const float hb = f.b(hair->point), earTop = f.b(top->point), earLobe = f.b(lobe->point);
    const float theta = std::min(180.0f, std::abs(angleDegrees));
    struct Knot {
        float angle, height;
    };
    const Knot knots[] = {{0.0f, hb},
                          {35.0f, hb - 0.05f * T},
                          {70.0f, earTop + 0.2f * T},
                          {95.0f, earTop + 0.1f * T},
                          {115.0f, earLobe},
                          {180.0f, earLobe - 0.45f * T}};
    float h = knots[5].height;
    for (int i = 1; i < 6; ++i) {
        if (theta <= knots[i].angle) {
            const float t = (theta - knots[i - 1].angle) / (knots[i].angle - knots[i - 1].angle);
            h = knots[i - 1].height + (knots[i].height - knots[i - 1].height) * t;
            break;
        }
    }
    return h;
}

float hairRegionWeight(const FaceLandmarks& face, const ScalpLandmarks& marks, const Vec3f& point, float softness) {
    if (!face.head) return 1.0f;
    const Frame f{marks.middle, marks.frame.left, marks.frame.up, marks.frame.front};
    const float a = f.a(point), b = f.b(point), c = f.c(point);
    const float h = hairlineHeight(face, marks, std::atan2(a, c) * 57.29578f);
    if (!std::isfinite(h)) return 1.0f;
    return smoothstep((b - h) / std::max(1e-6f, softness * face.third) + 0.5f);
}

Vec3f hairlinePoint(const ScalpMesh& mesh, const FaceLandmarks& face, const ScalpLandmarks& marks, float angleDegrees, Vec3f* normal) {
    const Frame f{marks.middle, marks.frame.left, marks.frame.up, marks.frame.front};
    const float h = hairlineHeight(face, marks, angleDegrees);
    const float t = angleDegrees / 57.29578f;
    const Vec3f out = f.front * std::cos(t) + f.left * std::sin(t);
    const Vec3f axis = f.m + f.up * (std::isfinite(h) ? h : 0.0f);
    SurfacePoint hit;
    if (!mesh.intersectRay(axis + out * (4.0f * marks.size), -out, &hit)) hit = mesh.closestPoint(axis + out * marks.size);
    if (normal) *normal = hit.normal.normalized();
    return hit.position;
}

}  // namespace groomlab::agent
