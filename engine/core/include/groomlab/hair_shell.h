// SPDX-License-Identifier: Apache-2.0
#pragma once

// The hair's outer surface round a head, by direction from the head's middle (the lock tools:
// lock_guides.h lifts strokes drawn on photos onto it, combs guides riding it and fills guides in).
//
// The head is a radius per direction (the outermost surface of its mesh, as a ray from far out
// meets it, its bumps evened out: half the largest radius within 12 degrees, half their mean). The
// hair over it comes from the lined-up photos' hair masks: along each direction, from the head out,
// a point is hair until a photo that sees it (in its frame, not behind the head) shows backdrop or
// skin there (a visual hull carved per direction, so the hair is one piece from the head out);
// stretches no photo sees take a guessed volume (fuller on top). The thickness is smoothed over a
// few degrees. Without photos the whole shell is that guess: `volume` over the head.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "groomlab/reference_guides.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

// A head's middle and directions: `center` and `radius` a sphere fitted to the hair region (the
// cranium's), `up` the world's up, `front` the way the face looks, `left` the character's left.
struct HeadFrame {
    Vec3f center;
    float radius = 1.0f;
    Vec3f up{0.0f, 1.0f, 0.0f}, front{0.0f, 0.0f, 1.0f}, left{1.0f, 0.0f, 0.0f};
    // A world direction in the frame: x left, y up, z front.
    Vec3f local(const Vec3f& d) const { return {d.dot(left), d.dot(up), d.dot(front)}; }
};

// A sphere fitted to `region`'s points (area-weighted least squares) and the frame's directions
// (`front` made square to `up`, left = up x front). Throws std::invalid_argument for an empty mesh.
HeadFrame headFrameOf(const ScalpMesh& region, const Vec3f& up, const Vec3f& front);

// The part of the head a direction from its middle points at (`d` any length, in the world):
// "nape" (low at the back), "front", "sides", "back" or "top" (the rest, high; low: "sides").
std::string headRegion(const HeadFrame& frame, const Vec3f& d);
const std::vector<std::string>& headRegionNames();

struct HairShellOptions {
    int thetaSteps = 72;          // directions from up to down
    int phiSteps = 144;           // directions round
    float reach = 0.0f;           // how far past the head hair may reach (0: 2.5 head radii)
    float sampleStep = 0.0f;      // samples along a direction (0: a 40th of the head's radius)
    int maskGrow = 2;             // pixels the photos' hair masks are grown by (a ragged edge carves nothing)
    float smoothDegrees = 4.0f;   // the thickness smoothed over this many degrees
    float volume = 0.0f;          // the guessed hair over the head where no photo sees it (0: 0.3 head radii)
};

// A photo's hair as the shell reads it: its camera (at its size), its hair mask (1 hair), rows top
// to bottom.
struct ShellPhoto {
    RefCamera camera;
    int width = 0, height = 0;
    std::vector<std::uint8_t> hair;
};

class HairShell {
public:
    HairShell() = default;
    // The head alone, `options.volume` over it (fuller on top, close at the nape).
    static HairShell fromVolume(const ScalpMesh& head, const HeadFrame& frame, const HairShellOptions& options = {});
    // Carved from lined-up photos: `views` (camera, image, mask touch-ups), their hair masks found
    // over `hairScalp` (where hair grows) with `viewOptions` (its occluders: the whole head).
    static HairShell fromPhotos(const ScalpMesh& head, const ScalpMesh& hairScalp, const HeadFrame& frame,
                                const std::vector<ReferenceView>& views, const ReferenceGuideOptions& viewOptions,
                                const HairShellOptions& options = {});
    // Carved from hair masks given (fromPhotos finds them: photoHairMask).
    static HairShell fromMasks(const ScalpMesh& head, const HeadFrame& frame, const std::vector<ShellPhoto>& photos,
                               const HairShellOptions& options = {});

    bool empty() const { return head_.empty(); }
    const HeadFrame& frame() const { return frame_; }
    int photoCount() const { return static_cast<int>(views_.size()); }

    // By a world direction from the frame's center (any length): the head's radius, the hair's.
    float head(const Vec3f& d) const;
    float outer(const Vec3f& d) const;
    float thickness(const Vec3f& d) const { return outer(d) - head(d); }
    float maxOuter() const { return maxOuter_; }
    // The guessed volume's thickness in a direction (what fromVolume uses).
    float guessedVolume(const Vec3f& d) const;

    // Whether a photo shows a point is not hair: one sees it (in its frame, not behind the head)
    // and its hair mask is off there. False without photos.
    bool seenBare(const Vec3f& p) const;
    // Whether a photo shows hair at the point (one sees it and its mask is on), none says bare.
    bool seenHair(const Vec3f& p) const;

    // The first point of a ray (unit `d`) in the shell (hit), else the point it passes nearest it
    // (no deeper along the ray than the head's middle), and how far outside that is.
    struct Hit {
        Vec3f point;
        float t = 0.0f;
        bool hit = false;
        float gap = 0.0f;
    };
    Hit raycast(const Vec3f& origin, const Vec3f& d) const;

    // The grid (stats, pictures): directions and radii by cell.
    int thetaSteps() const { return nt_; }
    int phiSteps() const { return np_; }
    Vec3f direction(int i, int j) const;
    float headAt(int i, int j) const { return head_[cell(i, j)]; }
    float outerAt(int i, int j) const { return outer_[cell(i, j)]; }

private:
    struct View {
        RefCamera camera;  // at the photo's size
        int width = 0, height = 0;
        std::vector<std::uint8_t> hair;  // grown
        std::vector<float> depth;        // the head's
    };
    std::size_t cell(int i, int j) const { return static_cast<std::size_t>(i) * static_cast<std::size_t>(np_) + static_cast<std::size_t>(j); }
    float lookup(const std::vector<float>& grid, const Vec3f& d) const;
    int say(const View& v, const Vec3f& p) const;  // -1 unseen, 0 bare, 1 hair
    void prepare(const ScalpMesh& head, const HeadFrame& frame, const HairShellOptions& options);

    HeadFrame frame_;
    HairShellOptions options_;
    int nt_ = 0, np_ = 0;
    std::vector<float> head_, outer_;
    float maxOuter_ = 0.0f;
    std::vector<View> views_;
};

// The shell's thickness over the hair region by part of the head (headRegion), scene units: the
// mean and 90th percentile over the directions whose head point lies on `hairScalp` (within a
// tenth of the head's radius), and the share of those with hair over them (thicker than a 20th of
// the head's radius).
struct ShellRegionStats {
    std::string region;
    int directions = 0;
    float mean = 0.0f, p90 = 0.0f, max = 0.0f;
    float covered = 0.0f;
};
std::vector<ShellRegionStats> shellStats(const HairShell& shell, const ScalpMesh& hairScalp);

}  // namespace groomlab
