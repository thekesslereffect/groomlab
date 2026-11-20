// SPDX-License-Identifier: Apache-2.0
#include "groomlab/export_view.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>

#include "groomlab/curve.h"
#include "groomlab/hair_color.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/usd_writer.h"

namespace groomlab {

namespace fs = std::filesystem;

const std::vector<float>* wetnessChannel(const StrandSet& strands, bool* perPoint) {
    *perPoint = true;
    const std::vector<float>* points = strands.pointChannel(kWetnessChannel);
    if (points && points->size() == strands.pointCount()) return points;
    *perPoint = false;
    const std::vector<float>* each = strands.strandChannel(kWetnessChannel);
    return each && each->size() == strands.strandCount() ? each : nullptr;
}

const std::vector<float>* ExportView::strandChannel(const std::string& name) const {
    const std::vector<float>* values = strands ? strands->strandChannel(name) : nullptr;
    return values && values->size() == strandCount() ? values : nullptr;
}

const std::vector<float>* ExportView::pointChannel(const std::string& name) const {
    const std::vector<float>* values = strands ? strands->pointChannel(name) : nullptr;
    return values && values->size() == pointCount() ? values : nullptr;
}

std::vector<std::array<float, 3>> strandColors(const StrandSet& strands) {
    std::vector<std::array<float, 3>> colors;
    const std::size_t count = strands.strandCount();
    // The RGB first: HairColor's channels, all three the right size.
    std::array<const std::vector<float>*, 3> rgb{};
    bool explicitColor = true;
    for (int k = 0; k < 3; ++k) {
        rgb[k] = strands.strandChannel(kColorChannels[k]);
        if (!rgb[k] || rgb[k]->size() != count) explicitColor = false;
    }
    if (explicitColor) {
        colors.resize(count);
        for (std::size_t s = 0; s < count; ++s) colors[s] = {(*rgb[0])[s], (*rgb[1])[s], (*rgb[2])[s]};
        return colors;
    }
    const std::vector<float>* melanin = strands.strandChannel(kMelaninChannel);
    if (!melanin || melanin->size() != strands.strandCount()) return colors;
    const std::vector<float>* redness = strands.strandChannel(kMelaninRednessChannel);
    if (redness && redness->size() != melanin->size()) redness = nullptr;
    colors.resize(melanin->size());
    for (std::size_t s = 0; s < colors.size(); ++s) {
        colors[s] = melaninColor((*melanin)[s], redness ? (*redness)[s] : kDefaultMelaninRedness);
    }
    return colors;
}

std::vector<std::array<float, 3>> pointColors(const StrandSet& strands) {
    std::vector<std::array<float, 3>> colors;
    const std::size_t count = strands.pointCount();
    std::array<const std::vector<float>*, 3> rgb{};
    for (int k = 0; k < 3; ++k) {
        rgb[k] = strands.pointChannel(kColorChannels[k]);
        if (!rgb[k] || rgb[k]->size() != count) return colors;
    }
    colors.resize(count);
    for (std::size_t i = 0; i < count; ++i) colors[i] = {(*rgb[0])[i], (*rgb[1])[i], (*rgb[2])[i]};
    return colors;
}

namespace {

std::vector<std::array<float, 3>> appearanceOf(std::vector<std::array<float, 3>> colors) {
    for (std::array<float, 3>& c : colors) c = hairAppearanceColor(c);
    return colors;
}

}  // namespace

std::vector<std::array<float, 3>> strandAppearanceColors(const StrandSet& strands) { return appearanceOf(strandColors(strands)); }

std::vector<std::array<float, 3>> pointAppearanceColors(const StrandSet& strands) { return appearanceOf(pointColors(strands)); }

ExportView describeExport(const StrandSet& strands, const ScalpMesh* scalp, unsigned parts) {
    ExportView v;
    v.strands = &strands;
    const std::size_t strandCount = strands.strandCount();
    v.counts.resize(strandCount);
    for (std::size_t s = 0; s < strandCount; ++s) v.counts[s] = static_cast<std::int32_t>(strands.strandPointCount(s));

    if (scalp && scalp->hasUVs()) {
        v.rootUVs.assign(strandCount, {0.0f, 0.0f});
        const std::vector<StrandRoot>& roots = strands.roots();
        for (std::size_t s = 0; s < strandCount && s < roots.size(); ++s) {
            if (scalp->contains(roots[s])) v.rootUVs[s] = scalp->uvAt(roots[s]);
        }
    }
    const bool appearance = (parts & kExportAppearanceColors) != 0;
    v.colors = appearance ? strandAppearanceColors(strands) : strandColors(strands);
    if (parts & kExportPointColors) v.pointColors = appearance ? pointAppearanceColors(strands) : pointColors(strands);
    v.strandChannels = strands.strandChannelNames();
    v.pointChannels = strands.pointChannelNames();

    const std::size_t pointCount = strands.pointCount();
    if ((parts & kExportBounds) && pointCount > 0) {
        const float big = std::numeric_limits<float>::max();
        Vec3f lo{big, big, big}, hi{-big, -big, -big};
        for (std::size_t i = 0; i < pointCount; ++i) {
            const Vec3f& p = strands.positions()[i];
            const float r = v.radius(i);
            lo = {std::min(lo.x, p.x - r), std::min(lo.y, p.y - r), std::min(lo.z, p.z - r)};
            hi = {std::max(hi.x, p.x + r), std::max(hi.y, p.y + r), std::max(hi.z, p.z + r)};
        }
        v.boundsLo = lo;
        v.boundsHi = hi;
    }
    return v;
}

std::string exportObjectName(const std::string& wanted) {
    std::string name;
    for (const char c : wanted) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        name += plain ? c : '_';
    }
    if (name.empty() || (name[0] >= '0' && name[0] <= '9')) name = "groom" + name;
    return name;
}

std::vector<std::string> usdIdentifiers(const std::vector<std::string>& names, const std::vector<std::string>& reserved) {
    std::set<std::string> taken(reserved.begin(), reserved.end());
    std::vector<std::string> out;
    out.reserve(names.size());
    for (const std::string& original : names) {
        const std::string name = usdIdentifier(original);
        std::string candidate = name;
        for (int n = 2; taken.count(candidate); ++n) candidate = name + "_" + std::to_string(n);
        taken.insert(candidate);
        out.push_back(candidate);
    }
    return out;
}

std::vector<std::string> usdChannelNames(const std::vector<std::string>& channels) {
    return usdIdentifiers(channels, {"st", "strandId", "displayColor", "displayOpacity", "widths", "points", "normals"});
}

StrandSet exportCopy(const StrandSet& strands, float scale, std::size_t pointsPerStrand) {
    const std::size_t count = strands.strandCount();
    const bool resample = pointsPerStrand >= 2;
    StrandSet out;
    if (!resample) {
        out = strands;
    } else {
        out.resize(std::vector<std::size_t>(count, pointsPerStrand));
        out.mutableIds() = strands.ids();
        out.mutableRoots() = strands.roots();
        for (const std::string& name : strands.strandChannelNames()) {
            out.addStrandChannel(name, strands.strandChannelDefault(name)) = *strands.strandChannel(name);
        }
        // Each channel's arrays found once (in and out), then every strand on its own.
        std::vector<std::pair<const float*, float*>> channels;
        for (const std::string& name : strands.pointChannelNames()) {
            std::vector<float>& to = out.addPointChannel(name, strands.pointChannelDefault(name));
            const std::vector<float>& values = *strands.pointChannel(name);
            if (values.size() == strands.pointCount()) channels.emplace_back(values.data(), to.data());
        }
        const bool widths = strands.widths().size() == strands.pointCount();
        const float* inWidths = strands.widths().data();
        float* outWidths = out.mutableWidths().data();
        Vec3f* outPoints = out.mutablePositions().data();
        parallelFor(0, count, [&](std::size_t s) {
            const Vec3f* src = strands.strandPoints(s);
            const std::size_t n = strands.strandPointCount(s), begin = strands.strandBegin(s), to = out.strandBegin(s);
            if (n == 0) return;
            resampleByArcLengthInto(src, n, outPoints + to, pointsPerStrand);
            if (widths) resampleValuesByArcLength(src, n, inWidths + begin, n, outWidths + to, pointsPerStrand);
            for (const auto& [in, into] : channels) resampleValuesByArcLength(src, n, in + begin, n, into + to, pointsPerStrand);
        }, 64);
    }
    // Widths as every writer reads them (1 where the set has none), so they scale with the points.
    if (out.widths().size() != out.pointCount()) out.mutableWidths().assign(out.pointCount(), 1.0f);
    if (scale != 1.0f) {
        for (Vec3f& p : out.mutablePositions()) p *= scale;
        for (float& w : out.mutableWidths()) w *= scale;
    }
    return out;
}

namespace {

constexpr const char* kFeatherIdName = "featherId";
constexpr const char* kIsShaftName = "isShaft";

// The feather channels of a set, looked up once for a walk over its strands.
struct FeatherChannels {
    const std::vector<float>* feathers;
    const std::vector<float>* shafts;
    explicit FeatherChannels(const StrandSet& s) : feathers(s.strandChannel(kFeatherIdName)), shafts(s.strandChannel(kIsShaftName)) {}
    bool shaft(std::size_t s) const { return shafts && s < shafts->size() && (*shafts)[s] > 0.5f; }
    bool barb(std::size_t s) const { return feathers && s < feathers->size() && (*feathers)[s] >= 0.0f && !shaft(s); }
};

// The strands that are feathers' shafts (`shafts`), or the others, in order.
std::vector<std::size_t> shaftsOrNot(const StrandSet& strands, bool shafts) {
    const FeatherChannels c(strands);
    std::vector<std::size_t> picked;
    picked.reserve(strands.strandCount());
    for (std::size_t s = 0; s < strands.strandCount(); ++s) {
        if (c.shaft(s) == shafts) picked.push_back(s);
    }
    return picked;
}

}  // namespace

bool isBarb(const StrandSet& strands, std::size_t s) { return FeatherChannels(strands).barb(s); }

bool hasBarbs(const StrandSet& strands) {
    const FeatherChannels c(strands);
    if (!c.feathers) return false;
    for (std::size_t s = 0; s < strands.strandCount(); ++s) {
        if (c.barb(s)) return true;
    }
    return false;
}

StrandSet withoutShafts(const StrandSet& strands) {
    if (!strands.strandChannel(kIsShaftName)) return strands;
    return strands.subset(shaftsOrNot(strands, false));
}

StrandSet onlyShafts(const StrandSet& strands) { return strands.subset(shaftsOrNot(strands, true)); }

std::vector<std::array<float, 3>> rachisColors(const StrandSet& strands, const RachisMesh& mesh, const std::array<float, 3>& fallback) {
    std::map<long, std::array<float, 3>> shaftColor;
    const std::vector<float>* isShaft = strands.strandChannel(kIsShaftName);
    const std::vector<float>* featherId = strands.strandChannel("featherId");
    if (isShaft && featherId) {
        std::array<const std::vector<float>*, 3> perPoint{}, perStrand{};
        bool pointColors = true, strandColors = true;
        for (int c = 0; c < 3; ++c) {
            perPoint[c] = strands.pointChannel(kColorChannels[c]);
            perStrand[c] = strands.strandChannel(kColorChannels[c]);
            pointColors = pointColors && perPoint[c] && perPoint[c]->size() == strands.pointCount();
            strandColors = strandColors && perStrand[c] && perStrand[c]->size() == strands.strandCount();
        }
        for (std::size_t k = 0; k < strands.strandCount() && (pointColors || strandColors); ++k) {
            if ((*isShaft)[k] < 0.5f) continue;
            std::array<float, 3> rgb{};
            for (int c = 0; c < 3; ++c) rgb[c] = pointColors ? (*perPoint[c])[strands.strandBegin(k)] : (*perStrand[c])[k];
            shaftColor[std::lround((*featherId)[k])] = rgb;
        }
    }
    std::vector<std::array<float, 3>> colors(mesh.points.size(), fallback);
    for (std::size_t p = 0; p < colors.size() && p < mesh.featherIndex.size() && !shaftColor.empty(); ++p) {
        const auto own = shaftColor.find(std::lround(mesh.featherIndex[p]));
        if (own != shaftColor.end()) colors[p] = own->second;
    }
    return colors;
}

StrandSet thinBarbs(const StrandSet& strands, float density, float widthScale) {
    density = std::clamp(std::isfinite(density) ? density : 1.0f, 0.0f, 1.0f);
    if (density >= 1.0f && widthScale == 1.0f) return strands;
    std::vector<std::size_t> kept;
    kept.reserve(strands.strandCount());
    const FeatherChannels in(strands);
    for (std::size_t s = 0; s < strands.strandCount(); ++s) {
        if (!in.barb(s) || keepAtDensity(strands.ids()[s], density)) kept.push_back(s);
    }
    StrandSet out = kept.size() == strands.strandCount() ? strands : strands.subset(kept);
    if (widthScale != 1.0f && std::isfinite(widthScale) && widthScale > 0.0f && out.widths().size() == out.pointCount()) {
        std::vector<float>& widths = out.mutableWidths();
        const FeatherChannels scaled(out);
        for (std::size_t s = 0; s < out.strandCount(); ++s) {
            if (!scaled.barb(s)) continue;
            const std::size_t begin = out.strandBegin(s), end = begin + out.strandPointCount(s);
            for (std::size_t k = begin; k < end; ++k) widths[k] *= widthScale;
        }
    }
    return out;
}

std::vector<GroomGroup> splitGroomGroups(const StrandSet& strands, const std::string& channel,
                                         const std::string& baseName, const std::map<int, std::string>& names) {
    std::map<int, std::vector<std::size_t>> members;
    const std::vector<float>* values = strands.strandChannel(channel);
    for (std::size_t s = 0; s < strands.strandCount(); ++s) {
        int id = 0;
        if (values && s < values->size() && std::isfinite((*values)[s])) {
            id = std::max(0, static_cast<int>(std::lround((*values)[s])));
        }
        members[id].push_back(s);
    }
    if (members.empty()) members[0];  // no strands: one empty group, as a groom without them
    std::vector<GroomGroup> groups;
    for (auto& [id, list] : members) {
        GroomGroup g;
        g.id = id;
        const auto named = names.find(id);
        if (id == 0) g.name = baseName;
        else if (named != names.end()) g.name = named->second;
        else g.name = (channel == kGroupIdChannel ? std::string("group") : channel) + std::to_string(id);
        g.strands = list.size() == strands.strandCount() ? strands : strands.subset(list);
        g.feathers = hasBarbs(g.strands);
        groups.push_back(std::move(g));
    }
    return groups;
}

std::vector<GroomGroup> limitGroupSize(std::vector<GroomGroup> groups, std::size_t maxStrands) {
    if (maxStrands == 0) return groups;
    std::vector<GroomGroup> out;
    for (GroomGroup& g : groups) {
        const std::size_t count = g.strands.strandCount();
        if (count <= maxStrands) {
            out.push_back(std::move(g));
            continue;
        }
        const std::size_t parts = (count + maxStrands - 1) / maxStrands;
        std::size_t begin = 0;
        for (std::size_t p = 0; p < parts; ++p) {
            // Even parts: the first `count % parts` take one more.
            const std::size_t size = count / parts + (p < count % parts ? 1 : 0);
            std::vector<std::size_t> indices(size);
            for (std::size_t k = 0; k < size; ++k) indices[k] = begin + k;
            begin += size;
            GroomGroup part;
            part.id = g.id;
            part.name = g.name + "_" + std::to_string(p);
            part.strands = g.strands.subset(indices);
            part.feathers = g.feathers;
            out.push_back(std::move(part));
        }
    }
    return out;
}

std::size_t keepRootWidthsPositive(StrandSet& strands, float fallback) {
    const std::size_t count = strands.strandCount();
    if (strands.widths().size() != strands.pointCount()) return 0;
    auto positive = [](float w) { return std::isfinite(w) && w > 0.0f; };
    const std::vector<float>& widths = strands.widths();
    float firstRoot = 0.0f;
    std::vector<std::size_t> bad;
    for (std::size_t s = 0; s < count; ++s) {
        if (strands.strandPointCount(s) == 0) continue;
        const float root = widths[strands.strandBegin(s)];
        if (positive(root)) {
            if (firstRoot <= 0.0f) firstRoot = root;
        } else {
            bad.push_back(s);
        }
    }
    if (bad.empty()) return 0;
    std::vector<float>& out = strands.mutableWidths();
    for (const std::size_t s : bad) {
        const std::size_t begin = strands.strandBegin(s), end = begin + strands.strandPointCount(s);
        float widest = 0.0f;
        for (std::size_t k = begin; k < end; ++k) {
            if (positive(out[k])) widest = std::max(widest, out[k]);
        }
        out[begin] = widest > 0.0f ? widest : firstRoot > 0.0f ? firstRoot : fallback;
    }
    return bad.size();
}

fs::path tempPathFor(const fs::path& target) {
    fs::path tmp = target;
    tmp += ".tmp";
    return tmp;
}

void replaceFile(const fs::path& tmp, const fs::path& target) {
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (!ec) return;
    // Windows refuses to rename over an existing file when it is open elsewhere,
    // and older runtimes refuse it always: remove the target and try again.
    std::error_code removeError;
    fs::remove(target, removeError);
    fs::rename(tmp, target, ec);
    if (!ec) return;
    std::error_code cleanup;
    fs::remove(tmp, cleanup);
    throw std::runtime_error("cannot replace '" + target.u8string() + "': " + ec.message());
}

}  // namespace groomlab
