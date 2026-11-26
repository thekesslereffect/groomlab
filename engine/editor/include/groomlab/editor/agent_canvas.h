// SPDX-License-Identifier: Apache-2.0
#pragma once

// A small RGB canvas the agent tools draw their pictures on (agent_photos.h: a photo with a pixel
// grid, a mesh with numbered points, the mesh lined up over a photo): lines, dots, labels in a
// built-in pixel font (digits and a few signs), a mesh shaded through a pinhole camera, and the PNG
// the agent sees. Deterministic: the same calls give the same pixels everywhere.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "groomlab/reference_guides.h"

namespace groomlab::agent {

using Rgb = std::array<float, 3>;  // 0-1, as shown (sRGB)

class Canvas {
public:
    Canvas(int width, int height, const Rgb& background = {0.05f, 0.05f, 0.06f});
    // A photo as the canvas (gray or RGB, 0-1 as stored), scaled by `scale`.
    static Canvas ofImage(const RefImage& image, float scale = 1.0f);

    int width() const { return w_; }
    int height() const { return h_; }
    Rgb at(int x, int y) const;
    void set(int x, int y, const Rgb& c, float alpha = 1.0f);
    // Lines and shapes in pixels (x right, y down), `width` pixels wide.
    void line(float x0, float y0, float x1, float y1, const Rgb& c, float width = 1.0f, float alpha = 1.0f);
    void dot(float x, float y, float radius, const Rgb& c, float alpha = 1.0f);
    void ring(float x, float y, float radius, const Rgb& c, float width = 1.0f);
    // Text in the pixel font (0-9 - . , : x, and letters A-Z drawn as capitals) `scale` pixels a
    // font pixel, its top-left at (x, y), on a dark box when `boxed`. Returns its width.
    int text(int x, int y, const std::string& s, const Rgb& c, int scale = 1, bool boxed = true);
    static int textWidth(const std::string& s, int scale = 1);
    // The mesh shaded through `camera` (its pixels times `scale`), a simple key light from the
    // camera; behind-camera triangles left out. `depth` (canvas-sized, filled with +inf when
    // empty) is the z-buffer, kept for what is drawn after.
    void mesh(const ScalpMesh& mesh, const RefCamera& camera, const Rgb& color, std::vector<float>& depth, float alpha = 1.0f,
              float scale = 1.0f);
    // The PNG (8-bit RGB).
    std::vector<std::uint8_t> png() const;

private:
    int w_ = 0, h_ = 0;
    std::vector<Rgb> pixels_;
};

}  // namespace groomlab::agent
