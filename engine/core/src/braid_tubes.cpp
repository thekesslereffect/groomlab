// SPDX-License-Identifier: Apache-2.0
// Show Tubes (braid_tubes.h): the names of the Braid nodes whose tubes show, and the mesh a host
// draws for them. The tubes themselves are the Braid operator's (operators/braid_op.cpp).

#include "groomlab/braid_tubes.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "groomlab/curve.h"
#include "groomlab/display.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/tube.h"

namespace groomlab {

std::vector<std::string> braidTubeNames(const std::string& shown) {
    std::vector<std::string> names;
    std::istringstream lines(shown);
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || std::find(names.begin(), names.end(), line) != names.end()) continue;
        names.push_back(std::move(line));
    }
    return names;
}

std::string braidTubesText(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) {
        if (name.empty()) continue;
        if (!text.empty()) text += '\n';
        text += name;
    }
    return text;
}

std::string renamedBraidTubes(const std::string& shown, const std::string& from, const std::string& to) {
    std::vector<std::string> names = braidTubeNames(shown);
    const auto at = std::find(names.begin(), names.end(), from);
    if (at == names.end() || to.empty() || from == to) return shown;
    if (std::find(names.begin(), names.end(), to) != names.end()) names.erase(at);  // (shown already: once)
    else *at = to;
    return braidTubesText(names);
}

std::array<float, 3> braidSectionColor(int section) {
    // Clump Colors' saturation and value (clumpColor), each section's hue a golden-ratio step on.
    const float turn = 0.02f + 0.6180339887f * static_cast<float>(section);
    return hueColor(turn - std::floor(turn), 0.7f, 0.95f);
}

std::array<float, 3> guideTubeColor(std::uint64_t id) {
    return hueColor(unitFloat(hashCombine(id, 0x7B5C01u)), 0.7f, 0.95f);
}

namespace {

// The rings a guide's tube is drawn with (guideTubes): one every 12 degrees of its turning.
std::size_t guideTubeRings(const std::vector<Vec3f>& points) {
    constexpr double kTurnPerRing = 12.0 * 3.14159265358979323846 / 180.0;
    double turned = 0.0;
    Vec3f before;
    bool first = true;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const Vec3f way = points[i] - points[i - 1];
        const float length = way.length();
        if (!(length > 0.0f)) continue;
        const Vec3f along = way / length;
        if (!first) turned += std::acos(std::clamp(static_cast<double>(before.dot(along)), -1.0, 1.0));
        before = along;
        first = false;
    }
    const double wanted = std::ceil(turned / kTurnPerRing) + 1.0;
    return static_cast<std::size_t>(std::clamp(wanted, 24.0, 512.0));
}

}  // namespace

BraidTubes guideTubes(const GuideSet& guides, int around, std::size_t maxPoints) {
    BraidTubes out;
    out.around = std::max(around, 3);
    out.pressed = true;  // (the rings lie on the sections' outlines)
    const auto ringPoints = static_cast<std::size_t>(out.around);
    std::vector<const Guide*> tubed;
    auto take = [&](const Guide& g) {
        if (g.hasTube && g.points.size() >= 2) tubed.push_back(&g);
    };
    for (const Guide& g : guides.guides()) take(g);
    if (guides.levelTubes()) {
        for (const Guide& g : *guides.levelTubes()) take(g);
    }
    if (tubed.empty()) return out;
    std::vector<std::size_t> rings(tubed.size());
    parallelFor(0, tubed.size(), [&](std::size_t i) { rings[i] = guideTubeRings(tubed[i]->points); }, 64);
    std::size_t total = 0;
    for (const std::size_t r : rings) total += r;
    if (maxPoints > 0 && total * ringPoints > maxPoints) {
        const double keep = static_cast<double>(maxPoints) / static_cast<double>(total * ringPoints);
        for (std::size_t& r : rings) r = std::max<std::size_t>(2, static_cast<std::size_t>(static_cast<double>(r) * keep));
    }
    out.tubes.resize(tubed.size());
    std::size_t points = 0, quads = 0;
    for (std::size_t i = 0; i < tubed.size(); ++i) {
        BraidTubes::Tube& tube = out.tubes[i];
        tube.path = static_cast<int>(i);
        tube.root = tubed[i]->root;
        tube.colored = true;
        tube.color = guideTubeColor(tubed[i]->id);
        tube.firstPoint = points;
        tube.firstQuad = quads;
        points += rings[i] * ringPoints;
        quads += (rings[i] - 1) * ringPoints;
    }
    out.points.resize(points);
    out.quads.resize(quads);
    out.quadTube.resize(quads);
    parallelFor(0, tubed.size(), [&](std::size_t i) {
        const Guide& g = *tubed[i];
        BraidTubes::Tube& tube = out.tubes[i];
        const std::size_t n = rings[i];
        // Its shape evenly by arc length, a sample a ring, and its sections along it.
        const std::vector<Vec3f> shape = resampleByArcLength(g.points.data(), g.points.size(), n);
        const TubeSpine spine(shape.data(), shape.size(), g.frameUp, g.tube);
        if (spine.samples() != n) return;
        // Its outline (the root's, carried along the profiles where they change), `around` points evenly round.
        const std::vector<SectionPoint>& outline = g.tube.blended() ? tubeShapeOutline(g.tube).perimeter : tubeOutline(g.tube);
        if (outline.empty()) return;
        std::vector<SectionPoint> ring(ringPoints);
        std::vector<TubeBlendPoint> blends;
        for (std::size_t k = 0; k < ringPoints; ++k) ring[k] = outline[k * outline.size() / ringPoints];
        if (spine.blended()) {
            for (const SectionPoint& p : ring) blends.push_back(g.tube.blendPoint(p.u, p.v));
        }
        Vec3f* p = out.points.data() + tube.firstPoint;
        tube.at.resize(n);
        tube.centers.resize(n);
        for (std::size_t j = 0; j < n; ++j) {
            tube.at[j] = static_cast<float>(j) / static_cast<float>(n - 1);
            tube.centers[j] = spine.center(j);
            for (std::size_t k = 0; k < ringPoints; ++k) {
                p[j * ringPoints + k] = blends.empty() ? spine.atSample(j, ring[k].u, ring[k].v)
                                                       : spine.atSample(j, ring[k].u, ring[k].v, blends[k]);
            }
        }
        // The quads between each two rings, facing out as a Braid's (braid_op.cpp).
        const auto first = static_cast<std::uint32_t>(tube.firstPoint);
        for (std::size_t j = 1; j < n; ++j) {
            const std::uint32_t last = first + static_cast<std::uint32_t>((j - 1) * ringPoints);
            const std::uint32_t now = first + static_cast<std::uint32_t>(j * ringPoints);
            for (std::size_t k = 0; k < ringPoints; ++k) {
                const auto a = static_cast<std::uint32_t>(k), b = static_cast<std::uint32_t>((k + 1) % ringPoints);
                const std::size_t q = tube.firstQuad + (j - 1) * ringPoints + k;
                out.quads[q] = {last + a, now + a, now + b, last + b};
                out.quadTube[q] = static_cast<std::uint32_t>(i);
            }
        }
    }, 4);
    return out;
}

std::vector<std::uint32_t> BraidTubeMesh::triangles() const {
    std::vector<std::uint32_t> out;
    out.reserve(quads.size() * 6);
    for (const std::array<std::uint32_t, 4>& q : quads) out.insert(out.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
    return out;
}

void appendBraidTubes(BraidTubeMesh& mesh, BraidTubes tubes) {
    const std::size_t around = static_cast<std::size_t>(std::max(tubes.around, 0));
    if (around < 3 || tubes.points.empty()) return;
    const std::size_t base = mesh.points.size(), count = tubes.points.size();
    // (The first node's arrays are taken as they are: no copy of a mesh of many braids.)
    if (base == 0) mesh.points = std::move(tubes.points);
    else mesh.points.insert(mesh.points.end(), tubes.points.begin(), tubes.points.end());
    // Round an elliptical section's outline: a ring's sides on it, its corners out (the rim's hair
    // under them). A pressed section's ring stays on its outline.
    const float out = tubes.pressed ? 1.0f : 1.0f / std::cos(3.14159265f / static_cast<float>(around));
    mesh.normals.resize(base + count);
    mesh.colors.resize(base + count);
    mesh.uvs.resize(base + count);
    if (base == 0 && mesh.quads.empty()) {
        mesh.quads = std::move(tubes.quads);
    } else {
        const auto offset = static_cast<std::uint32_t>(base);
        const std::size_t firstQuad = mesh.quads.size();
        mesh.quads.resize(firstQuad + tubes.quads.size());
        parallelFor(0, tubes.quads.size(), [&](std::size_t i) {
            const std::array<std::uint32_t, 4>& q = tubes.quads[i];
            mesh.quads[firstQuad + i] = {q[0] + offset, q[1] + offset, q[2] + offset, q[3] + offset};
        }, 8192);
    }
    // A tube at a time (its points are its own): the color, how far along and round, and each
    // point's normal from the quads round it (the ring before and after, the neighbours round).
    parallelFor(0, tubes.tubes.size(), [&](std::size_t t) {
        const BraidTubes::Tube& tube = tubes.tubes[t];
        const std::size_t first = tube.firstPoint;
        const std::size_t end = t + 1 < tubes.tubes.size() ? tubes.tubes[t + 1].firstPoint : count;
        const std::size_t rings = (end - first) / around;
        const std::array<float, 3> color = tube.colored ? tube.color : braidSectionColor(tube.section);
        Vec3f* p = mesh.points.data() + base + first;
        for (std::size_t j = 0; out != 1.0f && j < rings && j < tube.centers.size(); ++j) {
            const Vec3f& center = tube.centers[j];
            for (std::size_t k = 0; k < around; ++k) p[j * around + k] = center + (p[j * around + k] - center) * out;
        }
        for (std::size_t j = 0; j < rings; ++j) {
            const std::size_t before = j > 0 ? j - 1 : j, after = j + 1 < rings ? j + 1 : j;
            for (std::size_t k = 0; k < around; ++k) {
                const std::size_t at = base + first + j * around + k;
                // Along the tube and round the ring at the point (central differences): the quads
                // face out as (ring before, ring after) x (k, k + 1) winds (BraidTubes::quads).
                const Vec3f along = p[after * around + k] - p[before * around + k];
                const Vec3f round = p[j * around + (k + 1) % around] - p[j * around + (k + around - 1) % around];
                const Vec3f normal = along.cross(round);
                const float length = normal.length();
                mesh.normals[at] = length > 0.0f ? normal * (1.0f / length) : Vec3f(0.0f, 1.0f, 0.0f);
                mesh.colors[at] = color;
                mesh.uvs[at] = {rings > 1 ? static_cast<float>(j) / static_cast<float>(rings - 1) : 0.0f,
                                static_cast<float>(k) / static_cast<float>(around)};
            }
        }
    }, 4);
    mesh.tubes += tubes.tubes.size();
}

}  // namespace groomlab
