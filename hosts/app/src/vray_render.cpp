// SPDX-License-Identifier: Apache-2.0
// V-Ray Standalone renders of the groom: the stage's scene, vray.exe's arguments and console
// (vray_render.h; the process and the window: render_job.h).

#include "vray_render.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

namespace groomlab::app {

namespace fs = std::filesystem;

namespace {

constexpr float kPi = 3.14159265358979f;
const char* const kVraySetting = "render/vrayStandalone";

V3 v3(const QVector3D& v) { return {v.x(), v.y(), v.z()}; }
std::string num(double v) { return sceneNumber(v); }
std::string vec(const V3& v) { return "Vector(" + num(v[0]) + ", " + num(v[1]) + ", " + num(v[2]) + ")"; }
// A V-Ray Transform: the images of the axes (the matrix's columns), then the translation.
std::string transform(const V3& x, const V3& y, const V3& z, const V3& origin) {
    return "Transform(Matrix(" + vec(x) + ", " + vec(y) + ", " + vec(z) + "), " + vec(origin) + ")";
}
std::string color(const RGB& c, float scale = 1.0f) {
    return "Color(" + num(c[0] * scale) + ", " + num(c[1] * scale) + ", " + num(c[2] * scale) + ")";
}
std::string scenePathText(const std::string& path) {
    std::string s = fs::absolute(fs::u8path(path)).generic_u8string();
    return s;
}

// The camera looking from `eye` at `target`, Y up: V-Ray's camera looks down its -Z.
std::string cameraTransform(const V3& eye, const V3& target) {
    const CameraAxes a = lookAt(eye, target);
    return transform(a.right, a.up, {-a.forward[0], -a.forward[1], -a.forward[2]}, eye);
}

V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
V3 normalized(const V3& v) {
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return n > 0.0f ? V3{v[0] / n, v[1] / n, v[2] / n} : V3{0.0f, 0.0f, 1.0f};
}

}  // namespace

void writeVrayScene(const std::string& scenePath, const std::string& groomPath, const RenderStage& stage) {
    const LightingSetup& l = stage.lighting;
    const int width = std::max(stage.width, 16), height = std::max(stage.height, 16);
    std::ostringstream s;
    s << "// GroomLab: the app's stage around the groom's V-Ray scene (vray_render.h)\n";
    s << "#include \"" << scenePathText(groomPath) << "\"\n\n";
    s << "SettingsOutput output {\n  img_width=" << width << ";\n  img_height=" << height << ";\n  img_noAlpha=1;\n}\n\n";
    // Brute-force GI (BRDFHair4's own bounces, its trace_depth, carry most of the light in the hair).
    s << "SettingsGI gi {\n  on=1;\n  primary_engine=2;\n  secondary_engine=2;\n}\n\n";
    s << "SettingsDMCGI dmcgi {\n  depth=" << stage.giDepth << ";\n}\n\n";
    s << "SettingsImageSampler sampler {\n  type=1;\n  dmc_minSubdivs=1;\n  dmc_maxSubdivs=" << stage.maxSubdivs
      << ";\n  dmc_threshold=" << num(stage.threshold) << ";\n}\n\n";
    s << "SettingsOptions options {\n  light_doDefaultLights=0;\n}\n\n";
    s << "FilterGaussian filter {\n  size=1.5;\n}\n\n";
    const double exposure = std::pow(2.0, static_cast<double>(l.exposure));
    s << "SettingsColorMapping color_mapping {\n  type=0;\n  exposure=Color(" << num(exposure) << ", " << num(exposure) << ", "
      << num(exposure) << ");\n}\n\n";

    // The camera: V-Ray's fov is the horizontal one, in radians.
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float fovH = 2.0f * std::atan(std::tan(stage.fovDegrees * kPi / 180.0f * 0.5f) * aspect);
    const V3 eye = v3(stage.eye), target = v3(stage.target);
    s << "RenderView camera {\n  transform=";
    if (stage.turntableFrames > 0) {
        s << "interpolate(";
        for (int k = 0; k < stage.turntableFrames; ++k) {
            const float degrees = 360.0f * static_cast<float>(k) / static_cast<float>(stage.turntableFrames);
            s << (k ? ", " : "") << "(" << (k + 1) << ", " << cameraTransform(orbitEye(eye, target, degrees), target) << ")";
        }
        s << ")";
    } else {
        s << cameraTransform(eye, target);
    }
    s << ";\n  fov=" << num(fovH) << ";\n  clipping=0;\n}\n\n";

    // The lights: LightDirect shines down its -Z (Z toward the light), pi x its intensity as irradiance.
    int n = 0;
    for (const DirectionalLight& light : l.lights) {
        if (!light.enabled || !(light.intensity > 0.0f)) continue;
        const V3 z = normalized(light.direction);
        const V3 up = std::abs(z[1]) > 0.9f ? V3{1.0f, 0.0f, 0.0f} : V3{0.0f, 1.0f, 0.0f};
        const V3 x = normalized(cross(up, z));
        const V3 y = cross(z, x);
        s << "LightDirect light_" << n++ << " {\n  transform=" << transform(x, y, z, {0.0f, 0.0f, 0.0f}) << ";\n  color=AColor("
          << num(light.color[0]) << ", " << num(light.color[1]) << ", " << num(light.color[2]) << ", 1);\n  intensity="
          << num(light.intensity / kPi) << ";\n  shadows=" << (light.shadows ? 1 : 0) << ";\n}\n\n";
    }
    // The environment: a lat-long .hdr (the app's direction convention; uvw_matrix turns V-Ray's Z-up
    // lat-long to it) as an invisible dome, the background from SettingsEnvironment.
    const bool environment = l.environment != Environment::None && l.environmentIntensity > 0.0f;
    if (environment) {
        const fs::path scene = fs::u8path(scenePath);
        const fs::path hdr = scene.parent_path() / (scene.stem().u8string() + "_env.hdr");
        writeHdr(hdr.u8string(), stageEnvironment(l));
        s << "BitmapBuffer env_bitmap {\n  file=\"" << scenePathText(hdr.u8string()) << "\";\n  transfer_function=0;\n  filter_type=0;\n}\n\n";
        s << "UVWGenEnvironment env_uvw {\n  mapping_type=\"spherical\";\n"
             "  uvw_matrix=Matrix(Vector(0, 1, 0), Vector(0, 0, 1), Vector(1, 0, 0));\n}\n\n";
        s << "TexBitmap env_tex {\n  bitmap=env_bitmap;\n  uvwgen=env_uvw;\n}\n\n";
        s << "LightDome environment {\n  use_dome_tex=1;\n  dome_tex=env_tex;\n  intensity=" << num(l.environmentIntensity)
          << ";\n  dome_spherical=1;\n  invisible=1;\n}\n\n";
    }
    s << "SettingsEnvironment background {\n";
    if (environment && l.background == Background::Environment) {
        s << "  bg_tex=env_tex;\n  bg_tex_mult=" << num(l.environmentIntensity * l.backgroundIntensity) << ";\n";
    } else {
        s << "  bg_color=" << color(l.backgroundColor) << ";\n";
    }
    s << "}\n\n";

    // The scalp: its triangles, smooth normals, a diffuse of its material's color.
    if (stage.scalp && stage.showScalp && stage.scalp->triangleCount() > 0) {
        const ScalpMesh& m = *stage.scalp;
        s << "GeomStaticMesh scalp@mesh {\n  vertices=ListVector(";
        for (std::size_t i = 0; i < m.points().size(); ++i) {
            const Vec3f& p = m.points()[i];
            s << (i ? ", " : "") << vec({p.x, p.y, p.z});
        }
        s << ");\n  faces=ListInt(";
        for (std::size_t i = 0; i < m.triangles().size(); ++i) {
            const auto& t = m.triangles()[i];
            s << (i ? ", " : "") << t[0] << ", " << t[1] << ", " << t[2];
        }
        s << ");\n  compute_normals=1;\n  smooth_angle=180;\n}\n\n";
        s << "BRDFDiffuse scalp@brdf {\n  color=" << color(stage.scalpColor) << ";\n}\n\n";
        s << "MtlSingleBRDF scalp@mtl {\n  brdf=scalp@brdf;\n}\n\n";
        s << "Node scalp@node {\n  transform=" << transform({1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0})
          << ";\n  geometry=scalp@mesh;\n  material=scalp@mtl;\n}\n\n";
    }

    const std::string text = s.str();
    std::ofstream out(fs::u8path(scenePath), std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + scenePath);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) throw std::runtime_error("cannot write " + scenePath);
}

#ifdef Q_OS_MACOS
namespace {
// macOS: vray.bin inside the install's VRay.app (Standalone_for_<os>/VRay.app/Contents/MacOS); the
// install folder or VRay.app itself given (the file dialog picks an app bundle as a file) is read as it.
QString vrayBinIn(const QString& path) {
    if (path.isEmpty()) return {};
    if (QFileInfo(path).isFile()) return QFileInfo(path).absoluteFilePath();
    for (const QString& inside : {QStringLiteral("Contents/MacOS/vray.bin"), QStringLiteral("VRay.app/Contents/MacOS/vray.bin")}) {
        const QFileInfo bin(QDir(path).filePath(inside));
        if (bin.isFile()) return bin.absoluteFilePath();
    }
    return {};
}
}  // namespace
#endif

// As Maya's vray_scene.find_vray: the user's choice, GROOMLAB_VRAY, VRAY_PATH, the default
// installs, the PATH.
QString findVrayStandalone() {
    const QStringList given = {QSettings().value(QLatin1String(kVraySetting)).toString(), qEnvironmentVariable("GROOMLAB_VRAY"),
                               qEnvironmentVariable("VRAY_PATH")};
#ifdef Q_OS_MACOS
    for (const QString& g : given) {
        const QString bin = vrayBinIn(g);
        if (!bin.isEmpty()) return bin;
    }
    QDir chaos(QStringLiteral("/Applications/Chaos/V-Ray"));
    QStringList installs = chaos.entryList({QStringLiteral("Standalone_for_*")}, QDir::Dirs, QDir::Name | QDir::Reversed);
    for (const QString& install : installs) {
        const QString bin = vrayBinIn(chaos.filePath(install));
        if (!bin.isEmpty()) return bin;
    }
    return QStandardPaths::findExecutable(QStringLiteral("vray.bin"));
#else
#ifdef Q_OS_WIN
    const QString exe = QStringLiteral("vray.exe");
    const QStringList roots = {qEnvironmentVariable("ProgramFiles", QStringLiteral("C:/Program Files")), QStringLiteral("C:/Program Files")};
#else
    const QString exe = QStringLiteral("vray");
    const QStringList roots = {QStringLiteral("/usr/ChaosGroup"), QStringLiteral("/opt/chaos"), QStringLiteral("/Applications/ChaosGroup")};
#endif
    // vray at `path`: the file, its folder, or an install folder with bin/.
    const auto in = [&](const QString& path) -> QString {
        if (path.isEmpty()) return {};
        if (QFileInfo(path).isFile()) return QFileInfo(path).absoluteFilePath();
        for (const QString& candidate : {QDir(path).filePath(exe), QDir(path).filePath(QStringLiteral("bin/") + exe)}) {
            if (QFileInfo(candidate).isFile()) return QDir::cleanPath(QFileInfo(candidate).absoluteFilePath());
        }
        return {};
    };
    QStringList candidates = given;
    for (const QString& root : roots) {
        for (const QString& vendor : {QStringLiteral("Chaos"), QStringLiteral("Chaos Group")}) {
            candidates << QDir(root).filePath(vendor + QStringLiteral("/V-Ray/Standalone for x64/bin"));
        }
    }
    for (const QString& c : candidates) {
        const QString found = in(c);
        if (!found.isEmpty()) return found;
    }
    return QStandardPaths::findExecutable(QStringLiteral("vray"));
#endif
}

void setVrayStandalone(const QString& path) {
#ifdef Q_OS_MACOS
    const QString bin = vrayBinIn(path);
    QSettings().setValue(QLatin1String(kVraySetting), bin.isEmpty() ? path : bin);
#else
    QSettings().setValue(QLatin1String(kVraySetting), path);
#endif
}

QStringList vrayArguments(const QString& scenePath, const QString& imagePath, int turntableFrames) {
    QStringList args = {QStringLiteral("-sceneFile=") + QDir::toNativeSeparators(scenePath),
                        QStringLiteral("-imgFile=") + QDir::toNativeSeparators(imagePath),
                        QStringLiteral("-display=0"), QStringLiteral("-autoClose=1"), QStringLiteral("-verboseLevel=3"),
                        QStringLiteral("-showProgress=2"), QStringLiteral("-progressIncrement=5"),
                        QStringLiteral("-progressUseColor=0")};
    if (turntableFrames > 0) {
        args << QStringLiteral("-frames=1-%1").arg(turntableFrames);
    } else {
        args << QStringLiteral("-frames=1") << QStringLiteral("-noFrameNumbers=1");
    }
    return args;
}

bool VrayProgress::feed(const QString& line) {
    static const QRegularExpression starting(QStringLiteral("Starting frame (\\d+)"));
    static const QRegularExpression rendering(QStringLiteral("Rendering image[^:]*: *([0-9.]+)%"));
    static const QRegularExpression writtenLine(QStringLiteral("Successfully written image file \"([^\"]+)\""));
    static const QRegularExpression failure(QStringLiteral("\\berror\\b"), QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m = starting.match(line);
    if (m.hasMatch()) {
        frame = m.captured(1).toInt();
        percent = 0.0;
        return true;
    }
    m = rendering.match(line);
    if (m.hasMatch()) {
        percent = m.captured(1).toDouble();
        return true;
    }
    m = writtenLine.match(line);
    if (m.hasMatch()) {
        written = m.captured(1);
        ++imagesWritten;
        percent = 100.0;
        return true;
    }
    if (error.isEmpty() && failure.match(line).hasMatch()) {
        // V-Ray's lines start with "[date] [memory] ": the message after them.
        const int at = line.lastIndexOf(QLatin1String("] "));
        error = (at >= 0 ? line.mid(at + 2) : line).trimmed();
    }
    return false;
}

}  // namespace groomlab::app
