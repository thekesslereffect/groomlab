// SPDX-License-Identifier: Apache-2.0
// Brush alphas (brush_alpha.h): the image as the brush weighs it, turned, its mip levels and
// thumbnail, and the library's folders.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "groomlab/brush_alpha.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/texture.h"

namespace groomlab {
namespace {

namespace fs = std::filesystem;

constexpr float kPi = 3.14159265358979f;

fs::path freshFolder(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / ("groomlab_brush_alpha_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}

// White on the image's left half, black on its right.
BrushAlpha leftHalf(int size = 8) {
    std::vector<float> values(static_cast<std::size_t>(size * size));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) values[static_cast<std::size_t>(y * size + x)] = x < size / 2 ? 1.0f : 0.0f;
    }
    return BrushAlpha(size, size, std::move(values));
}

TEST(BrushAlpha, SamplesTheSquareAroundTheCircle) {
    // A 2 x 2 image: top left 1, top right 0.5, bottom left 0.25, bottom right 0 (rows top-down).
    const BrushAlpha a(2, 2, {1.0f, 0.5f, 0.25f, 0.0f});
    EXPECT_FLOAT_EQ(a.sample(-0.5f, 0.5f, 1.0f), 1.0f);    // the top left texel's center
    EXPECT_FLOAT_EQ(a.sample(0.5f, -0.5f, 1.0f), 0.0f);    // bottom right
    EXPECT_FLOAT_EQ(a.sample(0.0f, 0.0f, 1.0f), 0.4375f);  // between all four
    EXPECT_FLOAT_EQ(a.sample(-1.0f, 1.0f, 1.0f), 1.0f);    // the corner: the edge texel
    EXPECT_EQ(a.sample(1.01f, 0.0f, 1.0f), 0.0f);          // outside the square
    EXPECT_EQ(a.sample(0.0f, -1.5f, 1.0f), 0.0f);
    EXPECT_THROW(BrushAlpha(2, 2, {1.0f}), std::invalid_argument);
}

TEST(BrushAlpha, AlphaChannelOrLuminance) {
    // Opaque: the luminance. A shape on a transparent background: its alpha channel.
    const Texture gray(1, 1, {0.2f, 0.4f, 0.6f, 1.0f});
    EXPECT_NEAR(BrushAlpha(gray).sample(0, 0, 1), 0.2126f * 0.2f + 0.7152f * 0.4f + 0.0722f * 0.6f, 1e-6f);
    const Texture shape(2, 1, {0.0f, 0.0f, 0.0f, 0.75f, 0.0f, 0.0f, 0.0f, 0.0f});
    EXPECT_FLOAT_EQ(BrushAlpha(shape).sample(-0.5f, 0, 1), 0.75f);
}

TEST(BrushAlpha, MipLevelsFollowTheBrushSize) {
    // A checker of single texels: averaged to gray on the level a small brush reads.
    const int size = 64;
    std::vector<float> values(static_cast<std::size_t>(size * size));
    for (int i = 0; i < size * size; ++i) values[static_cast<std::size_t>(i)] = ((i % size) + (i / size)) % 2 ? 1.0f : 0.0f;
    const BrushAlpha a(size, size, values);
    EXPECT_EQ(a.levelCount(), 7);  // 64, 32, 16, 8, 4, 2, 1
    EXPECT_EQ(a.levelFor(32.0f), 0);  // a texel a pixel
    EXPECT_EQ(a.levelFor(100.0f), 0);
    EXPECT_EQ(a.levelFor(8.0f), 2);  // 4 texels a pixel
    EXPECT_EQ(a.levelFor(0.01f), 6);
    EXPECT_FLOAT_EQ(a.sample(0.1f, 0.3f, 8.0f), 0.5f);
    // The thumbnail: square, white where the image is.
    const std::vector<std::uint8_t> thumb = leftHalf(32).thumbnail(16);
    ASSERT_EQ(thumb.size(), 256u);
    EXPECT_EQ(thumb[0], 255);
    EXPECT_EQ(thumb[15], 0);
}

TEST(BrushAlpha, StampTurnsAndInverts) {
    AlphaStamp stamp{std::make_shared<const BrushAlpha>(leftHalf()), 0.0f, false};
    EXPECT_FLOAT_EQ(stamp.at(-30, 0, 60), 1.0f);  // left of the center
    EXPECT_FLOAT_EQ(stamp.at(30, 0, 60), 0.0f);
    stamp.angle = kPi;  // half a turn: the white half on the right
    EXPECT_NEAR(stamp.at(30, 0, 60), 1.0f, 1e-5f);
    EXPECT_NEAR(stamp.at(-30, 0, 60), 0.0f, 1e-5f);
    stamp.angle = 0.5f * kPi;  // a quarter turn counterclockwise: the white half below
    EXPECT_NEAR(stamp.at(0, -30, 60), 1.0f, 1e-5f);
    EXPECT_NEAR(stamp.at(0, 30, 60), 0.0f, 1e-5f);
    stamp.angle = 0.0f;
    stamp.invert = true;
    EXPECT_FLOAT_EQ(stamp.at(30, 0, 60), 1.0f);
    EXPECT_FLOAT_EQ(AlphaStamp{}.at(5, 5, 10), 1.0f);  // none: every point weighs 1
}

TEST(BrushAlpha, LibraryFoldersAndAdding) {
    const fs::path user = freshFolder("user"), shipped = freshFolder("shipped"), outside = freshFolder("outside");
    setEnv("GROOMLAB_USER_ALPHAS", user.u8string());
    setEnv("GROOMLAB_ALPHA_PATH", "");
    setShippedBrushAlphaDir(shipped.generic_u8string());
    const std::vector<std::uint8_t> pixels(16, 200);
    writePng((shipped / "Speckle.png").u8string(), 4, 4, 1, pixels);
    writePng((shipped / "Cloud.png").u8string(), 4, 4, 1, pixels);
    writePng((outside / "speckle.png").u8string(), 4, 4, 1, pixels);
    writePng((outside / "Mine.png").u8string(), 4, 4, 1, pixels);
    {
        std::ofstream((shipped / "notes.txt").string()) << "not an image";
    }
    std::vector<BrushAlphaFile> list = listBrushAlphas();
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].name, "Cloud");
    EXPECT_TRUE(list[0].shipped);
    // Added: copied into the user's folder; a name a shipped alpha has gets a number (no hiding).
    const std::string mine = addBrushAlpha((outside / "Mine.png").u8string());
    EXPECT_EQ(fs::u8path(mine).parent_path(), fs::u8path(user.generic_u8string()));
    const std::string speckle = addBrushAlpha((outside / "speckle.png").u8string());
    EXPECT_EQ(fs::u8path(speckle).filename().u8string(), "speckle 2.png");
    EXPECT_EQ(addBrushAlpha(speckle), speckle);  // in the library already
    list = listBrushAlphas();
    ASSERT_EQ(list.size(), 4u);
    EXPECT_EQ(list[0].name, "Mine");  // the user's first
    EXPECT_FALSE(list[0].shipped);
    EXPECT_EQ(list[1].name, "speckle 2");
    EXPECT_EQ(list[2].name, "Cloud");
    EXPECT_THROW(addBrushAlpha((shipped / "notes.txt").u8string()), std::runtime_error);
    EXPECT_FLOAT_EQ(loadBrushAlpha(mine)->sample(0, 0, 4), 200.0f / 255.0f);
    setShippedBrushAlphaDir("");
    setEnv("GROOMLAB_USER_ALPHAS", "");
}

}  // namespace
}  // namespace groomlab
