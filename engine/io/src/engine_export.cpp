// SPDX-License-Identifier: Apache-2.0
#include "groomlab/engine_export.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "groomlab/alembic_io.h"
#include "groomlab/engine_cards.h"
#include "groomlab/engine_meshes.h"
#include "groomlab/fbx_writer.h"
#include "groomlab/feathers.h"

namespace groomlab {

double barbWidthScaleFor(double density) { return density > 0.0 ? 1.0 / std::sqrt(density) : 1.0; }

void setBarbLod(EngineExportOptions& options, const std::string& lod, std::optional<double> density, std::optional<double> widthScale) {
    double share = 1.0;
    bool given = false;
    if (!lod.empty()) {
        if (lod == "high") share = 1.0;
        else if (lod == "mid") share = 0.5;
        else if (lod == "low") share = 0.25;
        else throw std::invalid_argument("lod is high, mid or low");
        given = true;
    }
    if (density) {
        share = *density;
        given = true;
    }
    if (!(share > 0.0 && share <= 1.0)) throw std::invalid_argument("barb density must be above 0 and at most 1");
    const double width = widthScale ? *widthScale : (given ? barbWidthScaleFor(share) : 1.0);
    if (!(width > 0.0)) throw std::invalid_argument("barb width scale must be above 0");
    options.barbDensity = static_cast<float>(share);
    options.barbWidthScale = static_cast<float>(width);
}

std::string EngineExportOptions::problem() const {
    if (!(barbDensity > 0.0f && barbDensity <= 1.0f)) return "the barb density must be above 0 and at most 1";
    if (!(barbWidthScale > 0.0f)) return "the barb width scale must be positive";
    if (!(scale > 0.0f)) return "the scale must be positive";
    if (engine == Engine::Unity) {
        auto outside = [](std::size_t n) { return n != 0 && (n < kUnityMinStrandPoints || n > kUnityMaxStrandPoints); };
        if (outside(pointsPerStrand)) return "points per strand must be 3 to 128 (Unity's strand particle counts), or 0: as they are";
        if (outside(barbPoints)) return "barb points must be 3 to 128 (Unity's strand particle counts), or 0: as they are";
    }
    if (upAxis != "Y" && upAxis != "Z") return "the up axis must be Y or Z";
    if (!(centimetersPerUnit > 0.0) || !std::isfinite(centimetersPerUnit)) return "the centimeters per unit must be positive";
    return {};
}

struct EngineGroomWriter::Impl {
    EngineExportOptions options;
    std::string name;
    std::unique_ptr<UnrealGroomWriter> unreal;
    std::unique_ptr<AlembicCurvesWriter> unity;
    std::vector<Object> objects;
    std::size_t frames = 0;

    // The strands an engine gets: fewer, wider barbs (a lighter groom); without the shafts when
    // they are geometry (a rachis mesh) or the options say so.
    bool dropShafts(const StrandSet& evaluated) const {
        return options.shafts ? !*options.shafts : hasRachis(evaluated.featherLayers());
    }

    void noteObjects(const std::vector<GroomGroup>& groups, const std::vector<std::string>& paths) {
        if (frames != 0) return;  // the first frame decides the objects
        for (std::size_t i = 0; i < groups.size(); ++i) {
            const GroomGroup& g = groups[i];
            Object o;
            o.path = i < paths.size() ? paths[i] : std::string();
            o.group = g.name;
            o.groupId = g.id;
            o.feathers = g.feathers;
            o.strands = g.strands.strandCount();
            const std::size_t own = g.feathers && options.barbPoints ? options.barbPoints : options.pointsPerStrand;
            o.pointsPerStrand = own >= 2 ? own : 0;
            if (own >= 2) {
                o.minPoints = o.maxPoints = own;
            } else {  // as they are
                for (std::size_t s = 0; s < g.strands.strandCount(); ++s) {
                    const std::size_t n = g.strands.strandPointCount(s);
                    o.minPoints = s == 0 ? n : std::min(o.minPoints, n);
                    o.maxPoints = std::max(o.maxPoints, n);
                }
            }
            objects.push_back(std::move(o));
        }
    }

    void writeUnity(const StrandSet& evaluated, const ScalpMesh* scalp) {
        StrandSet strands = thinBarbs(evaluated, options.barbDensity, options.barbWidthScale);
        if (dropShafts(evaluated)) strands = withoutShafts(strands);
        std::vector<GroomGroup> groups;
        if (options.groups.empty()) {
            GroomGroup all;
            all.name = name;
            all.strands = strands;
            all.feathers = hasBarbs(strands);
            groups.push_back(std::move(all));
            unity->writeFrame(strands, scalp);
        } else {
            // A strand group per groom group in Unity; past its strand limit, in parts.
            groups = limitGroupSize(splitGroomGroups(strands, options.groups, name, featherGroupNames(evaluated.featherLayers())),
                                    options.maxStrandsPerGroup);
            std::vector<AlembicCurvesWriter::Group> written;
            for (const GroomGroup& g : groups) written.push_back({g.name, &g.strands, g.feathers ? options.barbPoints : 0});
            unity->writeGroups(written, scalp);
        }
        noteObjects(groups, unity->objectPaths());
    }

    void writeUnreal(const StrandSet& evaluated, const StrandSet* guides, const ScalpMesh* scalp) {
        const StrandSet* primaries = guides && !guides->empty() ? guides : nullptr;
        StrandSet strands = thinBarbs(evaluated, options.barbDensity, options.barbWidthScale);
        const bool drop = dropShafts(evaluated);
        if (options.groups.empty()) {
            if (drop) strands = withoutShafts(strands);
            unreal->writeFrame(strands, primaries, scalp);
            GroomGroup all;
            all.name = name;
            all.strands = strands;
            all.feathers = hasBarbs(strands);
            noteObjects({all}, {});
            return;
        }
        std::vector<GroomGroup> groups = splitGroomGroups(strands, options.groups, name, featherGroupNames(evaluated.featherLayers()));
        std::vector<StrandSet> shafts(groups.size());
        std::vector<UnrealGroomWriter::Group> written;
        bool primariesGiven = false;
        for (std::size_t i = 0; i < groups.size(); ++i) {
            GroomGroup& g = groups[i];
            UnrealGroomWriter::Group w;
            w.id = g.id;
            w.name = g.name;
            if (!g.feathers) {  // the hair simulates on the primary guides (its first group)
                if (!primariesGiven && primaries) w.guides = primaries;
                primariesGiven = true;
            } else if (options.featherGuides) {  // a feather group's guides: its shafts
                shafts[i] = onlyShafts(g.strands);
                if (!shafts[i].empty()) w.guides = &shafts[i];
            }
            if (drop) g.strands = withoutShafts(g.strands);
            w.strands = &g.strands;
            written.push_back(w);
        }
        unreal->writeGroups(written, scalp);
        noteObjects(groups, {});
    }
};

EngineGroomWriter::EngineGroomWriter(const std::string& path, const std::string& name, double startSeconds,
                                     double secondsPerFrame, const EngineExportOptions& options)
    : impl_(std::make_unique<Impl>()) {
    const std::string problem = options.problem();
    if (!problem.empty()) throw std::invalid_argument(problem);
    impl_->options = options;
    impl_->name = name;
    if (options.engine == EngineExportOptions::Engine::Unreal) {
        impl_->unreal = std::make_unique<UnrealGroomWriter>(path, name, name, startSeconds, secondsPerFrame);
        impl_->unreal->setIridescenceAsAo(options.iridescenceAo);
        impl_->unreal->setSourceSpace(options.upAxis, options.centimetersPerUnit);
    } else {
        AlembicCurvesWriter::Options abc;
        abc.scale = options.scale;
        abc.pointsPerStrand = options.pointsPerStrand;
        abc.vertexUVs = true;           // Unity reads curve UVs per point
        abc.positiveRootWidths = true;  // and drops a set's widths for one root at 0
        impl_->unity = std::make_unique<AlembicCurvesWriter>(path, name, startSeconds, secondsPerFrame, abc);
    }
}

EngineGroomWriter::~EngineGroomWriter() = default;

void EngineGroomWriter::writeFrame(const StrandSet& strands, const StrandSet* guides, const ScalpMesh* scalp) {
    if (impl_->unreal) {
        impl_->writeUnreal(strands, guides, scalp);
    } else {
        impl_->writeUnity(strands, scalp);
    }
    ++impl_->frames;
}

const std::vector<EngineGroomWriter::Object>& EngineGroomWriter::objects() const { return impl_->objects; }

std::size_t EngineGroomWriter::frameCount() const { return impl_->frames; }

void EngineGroomWriter::close() {
    if (impl_->unreal) impl_->unreal->close();
    if (impl_->unity) impl_->unity->close();
}

std::vector<std::string> UnityFeathersExport::files() const {
    std::vector<std::string> out{abc};
    if (!fbx.empty()) out.push_back(fbx);
    if (!manifest.empty()) out.push_back(manifest);
    return out;
}

UnityFeathersExport writeUnityFeathers(const StrandSet& strands, const ScalpMesh* scalp, double seconds, double secondsPerFrame,
                                       const std::string& directory, const std::string& name, const UnityFeathersOptions& o) {
    for (const std::size_t p : {o.points, o.barbPoints}) {
        if (p < kUnityMinStrandPoints || p > kUnityMaxStrandPoints) {
            throw std::invalid_argument("points and barb points are 3 to 128 (Unity's strand particle counts)");
        }
    }
    EngineExportOptions e;
    e.engine = EngineExportOptions::Engine::Unity;
    e.groups = kGroupIdChannel;
    e.pointsPerStrand = o.points;
    e.barbPoints = o.barbPoints;
    e.maxStrandsPerGroup = o.maxStrandsPerGroup;
    e.shafts = o.shafts;
    if (!o.lod.empty()) setBarbLod(e, o.lod);  // every host's barb LOD table
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::u8path(directory), ec);
    const std::string dir = fs::u8path(directory).generic_u8string();
    UnityFeathersExport out;
    out.abc = dir + "/" + name + "_strands.abc";
    nlohmann::json objects = nlohmann::json::array();
    {
        EngineGroomWriter writer(out.abc, name, seconds, secondsPerFrame, e);
        writer.writeFrame(strands, nullptr, scalp);
        writer.close();
        for (const EngineGroomWriter::Object& w : writer.objects()) {
            objects.push_back({{"object", w.path}, {"group", w.group}, {"groupId", w.groupId}, {"feathers", w.feathers},
                               {"strands", w.strands}, {"pointsPerStrand", w.pointsPerStrand ? nlohmann::json(w.pointsPerStrand) : nlohmann::json()},
                               {"minPoints", w.minPoints}, {"maxPoints", w.maxPoints}});
        }
    }
    if (o.rachis && hasRachis(strands.featherLayers())) {
        out.fbx = dir + "/" + name + "_rachis.fbx";
        if (o.writeRachis) {
            FbxSettings settings;
            settings.meters = true;  // (Unity's)
            writeFbx(out.fbx, {rachisFbxMesh(buildRachisMesh(strands.featherLayers()), name + "_rachis")}, settings);
        }
    } else if (std::any_of(objects.begin(), objects.end(), [](const nlohmann::json& w) { return w.value("feathers", false); })) {
        out.warnings.push_back("the feathers have no rachis: their shafts go as strands, and there is no mesh for the barbs to ride");
    }
    out.manifest = dir + "/" + name + "_unity_feathers.json";
    const std::string manifest = unityFeathersManifestJson(objects.dump(), name + "_strands.abc", out.fbx.empty() ? std::string() : name + "_rachis.fbx",
                                                           o.lod, e.barbDensity, &out.groups);
    std::ofstream f(fs::u8path(out.manifest), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + out.manifest);
    f << manifest;
    return out;
}

}  // namespace groomlab
