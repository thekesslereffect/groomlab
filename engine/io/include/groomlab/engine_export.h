// SPDX-License-Identifier: Apache-2.0
#pragma once

// A groom written for a game engine's hair importer, frame by frame, as every host writes it
// (Maya's groomLabExportAlembic -unreal / -unity, the standalone app's Export panel): Unreal's
// groom (UnrealGroomWriter: the strands, the primary guides it simulates, groom groups) or
// Unity's hair package (AlembicCurvesWriter: UVs per point, every root width above 0, a curves
// object per groom group of at most its strand limit). One options struct, so a host cannot
// leave an option out.
//
// Each frame's strands are made lighter (thinBarbs: fewer, wider barbs) and lose the feathers'
// shafts when those are geometry (a rachis mesh: hasRachis), then split into groom groups by a
// channel (splitGroomGroups, featherGroupNames naming the feather groups). Unreal: the hair's
// first group simulates on the primary guides, and with featherGuides a feather group on its
// shafts. Unity: each group its own curves object, a feather group's strands resampled to
// barbPoints.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "groomlab/export_view.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"

namespace groomlab {

struct EngineExportOptions {
    enum class Engine { Unreal, Unity };
    Engine engine = Engine::Unreal;
    // Groom groups: "" one object (Unreal still reads groom_group_id from the groupId channel);
    // a channel's name (kGroupIdChannel: a host's "auto") an object per value of it.
    std::string groups;
    // The feathers' shafts as strands; unset: left out when the feathers have a rachis mesh.
    std::optional<bool> shafts;
    bool featherGuides = false;  // Unreal: a feather group's guides are its shafts
    bool iridescenceAo = false;  // Unreal: FeatherColor's iridescence as groom_ao too (UnrealGroomWriter::setIridescenceAsAo)
    // Unreal: the groom's space, written in Unreal's so the importer's defaults (Rotation 0, Scale
    // 1) line the groom up with an FBX of the same scene (UnrealGroomWriter::setSourceSpace): the
    // points' up axis ("Y": Maya, the app, the Blender helper's groom space; "Z": a Z-up Maya
    // scene) and their unit in centimeters.
    std::string upAxis = "Y";
    double centimetersPerUnit = 1.0;
    float barbDensity = 1.0f;    // barbs kept (0 to 1, by ID), each widened by barbWidthScale
    float barbWidthScale = 1.0f;
    // Unity: the curves scaled (scene units to the engine's) and resampled to pointsPerStrand
    // (0: as they are; else 2 to 4096); a feather group's strands to barbPoints (0: as the rest);
    // a group past maxStrandsPerGroup in parts (0: no limit).
    float scale = 1.0f;
    std::size_t pointsPerStrand = 0;
    std::size_t barbPoints = 0;
    std::size_t maxStrandsPerGroup = kUnityMaxStrandsPerGroup;

    // Why the options cannot be written ("" when they can): a density outside (0, 1], a width
    // scale or a scale of 0 or less, a point count of 1 or past 4096, an up axis not "Y" or "Z",
    // centimeters per unit of 0 or less.
    std::string problem() const;
};

// A lighter feather groom for heavy shots (every host's: Maya's game.py, the Blender helper, the
// app): the barbs kept, the lod's share ("high" 1, "mid" 0.5, "low" 0.25; "" none) unless `density`
// is given, and each kept one widened by `widthScale`, else by 1 / sqrt(share) so the vanes keep
// their look. Throws std::invalid_argument for another lod, a share outside (0, 1] or a width scale
// of 0 or less.
void setBarbLod(EngineExportOptions& options, const std::string& lod, std::optional<double> density = std::nullopt,
                std::optional<double> widthScale = std::nullopt);
// The width scale that keeps a share of barbs looking as the whole: 1 / sqrt(share).
double barbWidthScaleFor(double density);

class EngineGroomWriter {
public:
    // Throws std::invalid_argument (EngineExportOptions::problem) or AlembicError.
    EngineGroomWriter(const std::string& path, const std::string& name, double startSeconds, double secondsPerFrame,
                      const EngineExportOptions& options);
    ~EngineGroomWriter();
    EngineGroomWriter(const EngineGroomWriter&) = delete;
    EngineGroomWriter& operator=(const EngineGroomWriter&) = delete;

    // One frame: the strands as evaluated, the primary guides Unreal simulates (null or empty:
    // none; Unity takes none) and the scalp (root UVs; may be null).
    void writeFrame(const StrandSet& strands, const StrandSet* guides, const ScalpMesh* scalp);

    // What the first frame wrote (it decides the objects): Unity's curves objects with their
    // groups, Unreal's groups.
    struct Object {
        std::string path;   // the curves object (Unity), "" for Unreal's
        std::string group;  // the groom group's name
        int groupId = 0;
        bool feathers = false;
        std::size_t strands = 0;
        std::size_t pointsPerStrand = 0;  // what they were resampled to; 0: as they are
        std::size_t minPoints = 0, maxPoints = 0;
    };
    const std::vector<Object>& objects() const;
    std::size_t frameCount() const;
    void close();  // called by the destructor if needed

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace groomlab
