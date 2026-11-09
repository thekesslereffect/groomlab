// SPDX-License-Identifier: Apache-2.0
// RenderMan renders of the groom: the stage's RIB, prman's arguments and console, finding prman and
// GroomLab's procedural (prman_render.h; the process and the window: render_job.h).

#include "prman_render.h"
#include "graph_presets.h"  // appResourceDir

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

namespace groomlab::app {

namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979;
const char* const kPrmanSetting = "render/prman";
#ifdef Q_OS_WIN
const char* const kPrmanExe = "prman.exe";
const char* const kProceduralFile = "groomlab_renderman.dll";
#else
const char* const kPrmanExe = "prman";
const char* const kProceduralFile = "groomlab_renderman.so";
#endif

V3 v3(const QVector3D& v) { return {v.x(), v.y(), v.z()}; }
std::string num(double v) { return sceneNumber(v); }
float dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
std::string path(const std::string& p) { return "\"" + fs::absolute(fs::u8path(p)).generic_u8string() + "\""; }
std::string rgb(const RGB& c, double scale = 1.0) {
    return "[" + num(c[0] * scale) + " " + num(c[1] * scale) + " " + num(c[2] * scale) + "]";
}
// A matrix as RIB writes it: rows (the images of the axes), the translation last.
std::string matrix(const V3& x, const V3& y, const V3& z, const V3& t) {
    return "[" + num(x[0]) + " " + num(x[1]) + " " + num(x[2]) + " 0 " + num(y[0]) + " " + num(y[1]) + " " + num(y[2]) +
           " 0 " + num(z[0]) + " " + num(z[1]) + " " + num(z[2]) + " 0 " + num(t[0]) + " " + num(t[1]) + " " +
           num(t[2]) + " 1]";
}
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
V3 normalized(const V3& v) {
    const float n = std::sqrt(dot(v, v));
    return n > 0.0f ? V3{v[0] / n, v[1] / n, v[2] / n} : V3{0.0f, 0.0f, 1.0f};
}

// World to camera for a camera at `eye` looking at `target` (Y up): the inverse of the camera's
// matrix whose axes are right, up and back (a camera looking down its -Z), as RIB's ConcatTransform.
std::string worldToCamera(const V3& eye, const V3& target) {
    const CameraAxes a = lookAt(eye, target);
    const V3 back{-a.forward[0], -a.forward[1], -a.forward[2]};
    return matrix({a.right[0], a.up[0], back[0]}, {a.right[1], a.up[1], back[1]}, {a.right[2], a.up[2], back[2]},
                  {-dot(a.right, eye), -dot(a.up, eye), -dot(back, eye)});
}

// The procedural's hair network (hosts/renderman/examples/groom.rib), PxrMarschnerHair's
// specularOffset minus the tilt.
std::string hairNetwork(float tilt) {
    std::ostringstream s;
    const auto primvar = [&s](const char* name, const char* var, const char* type, const char* extra = "") {
        s << "  Pattern \"PxrPrimvar\" \"" << name << "\" \"string varname\" [\"" << var << "\"] \"string type\" [\"" << type
          << "\"]" << extra << "\n";
    };
    primvar("pv_melanin", "rman_melanin", "float");
    primvar("pv_redness", "rman_redness", "float");
    primvar("pv_dye", "rman_dye", "color", " \"color defaultColor\" [1 1 1]");
    primvar("pv_color", "rman_color", "color");
    primvar("pv_share", "rman_rgbShare", "float");
    primvar("pv_gainR", "rman_specularGainR", "float", " \"float defaultFloat\" [1.75]");
    primvar("pv_coneR", "rman_coneAngleR", "float", " \"float defaultFloat\" [8]");
    primvar("pv_coneTT", "rman_coneAngleTT", "float", " \"float defaultFloat\" [4]");
    primvar("pv_coneTRT", "rman_coneAngleTRT", "float", " \"float defaultFloat\" [16]");
    primvar("pv_opacity", "opacity", "float", " \"float defaultFloat\" [1]");
    s << "  Pattern \"PxrHairColor\" \"pigment\" \"int mode\" [0] \"reference float melanin\" [\"pv_melanin:resultF\"]"
         " \"reference float redness\" [\"pv_redness:resultF\"] \"reference color dye\" [\"pv_dye:resultRGB\"]"
         " \"float randomMelanin\" [0] \"float strayDensity\" [0] \"string hairIndexPrimvar\" [\"hair_index\"]\n";
    s << "  Pattern \"PxrHairColor\" \"rgb\" \"int mode\" [1] \"reference color Color\" [\"pv_color:resultRGB\"]"
         " \"color randomColor\" [0 0 0] \"float strayDensity\" [0]\n";
    for (const char* lobe : {"Diff", "TT", "TRT"}) {
        std::string lower(lobe);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        s << "  Pattern \"PxrMix\" \"mix_" << lower << "\" \"reference color color1\" [\"pigment:result" << lobe
          << "\"] \"reference color color2\" [\"rgb:result" << lobe << "\"] \"reference float mixer\" [\"pv_share:resultF\"]\n";
    }
    s << "  Bxdf \"PxrMarschnerHair\" \"hair\" \"reference color diffuseColor\" [\"mix_diff:resultRGB\"]"
         " \"reference color specularColorTT\" [\"mix_tt:resultRGB\"] \"reference color specularColorTRT\" [\"mix_trt:resultRGB\"]"
         " \"reference float specularGainR\" [\"pv_gainR:resultF\"] \"float specularGainTT\" [2] \"float specularGainTRT\" [1.2]"
         " \"reference float specularConeAngleR\" [\"pv_coneR:resultF\"] \"reference float specularConeAngleTT\" [\"pv_coneTT:resultF\"]"
         " \"reference float specularConeAngleTRT\" [\"pv_coneTRT:resultF\"] \"float specularOffset\" ["
      << num(-tilt) << "] \"float specularIor\" [1.55] \"reference float presence\" [\"pv_opacity:resultF\"]\n";
    return s.str();
}

}  // namespace

void writePrmanScene(const std::string& scenePath, const PrmanScene& scene, const RenderStage& stage) {
    const LightingSetup& l = stage.lighting;
    const int width = std::max(stage.width, 16), height = std::max(stage.height, 16);
    const double exposure = std::pow(2.0, static_cast<double>(l.exposure));
    const bool environment = !scene.environmentTexture.empty() && l.environment != Environment::None &&
                             l.environmentIntensity > 0.0f;
    const bool domeSeen = environment && l.background == Background::Environment;
    // PxrPerspective's fov spans the shorter side; the viewport's is the vertical one.
    const double tanHalf = std::tan(stage.fovDegrees * kPi / 360.0);
    const double fov = width >= height ? stage.fovDegrees : 2.0 * std::atan(tanHalf * width / height) * 180.0 / kPi;
    const double solidAngle = 2.0 * kPi * (1.0 - std::cos(kPrmanDistantAngle * kPi / 360.0));
    const V3 eye = v3(stage.eye), target = v3(stage.target);
    const int frames = std::max(stage.turntableFrames, 1);

    std::ostringstream s;
    s << "##RenderMan RIB\n# GroomLab: the app's stage around the groom (prman_render.h)\nversion 3.04\n";
    if (!scene.procedurals.empty()) {
        s << "Option \"searchpath\" \"string procedural\" [\"" << fs::absolute(fs::u8path(scene.procedurals)).generic_u8string()
          << ":@\"]\n";
    }
    for (int k = 1; k <= frames; ++k) {
        const QString image = renderFrameImage(QString::fromStdString(scene.imagePath), k, stage.turntableFrames);
        s << "FrameBegin " << k << "\n";
        s << "Format " << width << " " << height << " 1\n";
        s << "Display " << path(image.toStdString()) << " \"png\" \"rgba\"\n";
        s << "Hider \"raytrace\" \"int maxsamples\" [" << stage.maxSamples << "] \"int minsamples\" [" << stage.minSamples
          << "] \"int incremental\" [0]\n";
        s << "PixelVariance " << num(stage.pixelVariance) << "\nPixelFilter \"gaussian\" 2 2\n";
        s << "Integrator \"PxrPathTracer\" \"groomlab_pt\" \"int maxIndirectBounces\" [" << stage.bounces << "]\n";
        if (!domeSeen) {
            s << "DisplayFilter \"PxrBackgroundDisplayFilter\" \"background\" \"color backgroundColor\" "
              << rgb(l.backgroundColor, exposure) << "\n";
        }
        s << "Projection \"PxrPerspective\" \"float fov\" [" << num(fov) << "]\n";
        const float degrees = stage.turntableFrames > 0 ? 360.0f * static_cast<float>(k - 1) / static_cast<float>(frames) : 0.0f;
        // RenderMan's camera looks down its +Z: the camera's own (looking down -Z), Z flipped.
        s << "Scale 1 1 -1\nConcatTransform " << worldToCamera(degrees != 0.0f ? orbitEye(eye, target, degrees) : eye, target)
          << "\n";
        s << "WorldBegin\n";
        int n = 0;
        for (const DirectionalLight& light : l.lights) {
            if (!light.enabled || !(light.intensity > 0.0f)) continue;
            // PxrDistantLight shines down its +Z: away from the light.
            const V3 z = normalized({-light.direction[0], -light.direction[1], -light.direction[2]});
            const V3 up = std::abs(z[1]) > 0.9f ? V3{1.0f, 0.0f, 0.0f} : V3{0.0f, 1.0f, 0.0f};
            const V3 x = normalized(cross(up, z));
            const V3 y = cross(z, x);
            s << "AttributeBegin\n  ConcatTransform " << matrix(x, y, z, {0.0f, 0.0f, 0.0f}) << "\n  Light \"PxrDistantLight\" \"light_"
              << n++ << "\" \"float intensity\" [" << num(light.intensity * exposure / solidAngle) << "] \"color lightColor\" "
              << rgb(light.color) << " \"float angleExtent\" [" << num(kPrmanDistantAngle) << "] \"int enableShadows\" ["
              << (light.shadows ? 1 : 0) << "]\nAttributeEnd\n";
        }
        if (environment) {
            // The app's lat-long (EnvironmentMap::direction) as PxrDomeLight's, which is Z up
            // (tests/data/app/renderman_compare.py checked the turn against Arnold's skydome).
            s << "AttributeBegin\n  Attribute \"visibility\" \"int camera\" [" << (domeSeen ? 1 : 0)
              << "]\n  Rotate -90 1 0 0\n  Rotate 90 0 0 1\n  Light \"PxrDomeLight\" \"environment\" \"float intensity\" ["
              << num(l.environmentIntensity * exposure) << "] \"string lightColorMap\" [" << path(scene.environmentTexture)
              << "] \"int enableTemperature\" [0]\nAttributeEnd\n";
        }
        if (stage.scalp && stage.showScalp && stage.scalp->triangleCount() > 0) {
            const ScalpMesh& m = *stage.scalp;
            s << "AttributeBegin\n  Attribute \"identifier\" \"string name\" [\"scalp\"]\n  Bxdf \"PxrSurface\" \"scalp\" "
                 "\"color diffuseColor\" " << rgb(stage.scalpColor)
              << " \"color specularFaceColor\" [0 0 0] \"color specularEdgeColor\" [0 0 0]\n  PointsPolygons [";
            for (std::size_t i = 0; i < m.triangles().size(); ++i) s << (i ? " 3" : "3");
            s << "]\n  [";
            for (std::size_t i = 0; i < m.triangles().size(); ++i) {
                const auto& t = m.triangles()[i];
                s << (i ? " " : "") << t[0] << " " << t[1] << " " << t[2];
            }
            s << "]\n  \"P\" [";
            for (std::size_t i = 0; i < m.points().size(); ++i) {
                const Vec3f& p = m.points()[i];
                s << (i ? " " : "") << num(p.x) << " " << num(p.y) << " " << num(p.z);
            }
            s << "]";
            const auto& normals = m.vertexNormals();
            if (normals.size() == m.points().size()) {
                s << "\n  \"N\" [";
                for (std::size_t i = 0; i < normals.size(); ++i) {
                    s << (i ? " " : "") << num(normals[i].x) << " " << num(normals[i].y) << " " << num(normals[i].z);
                }
                s << "]";
            }
            s << "\nAttributeEnd\n";
        }
        s << "AttributeBegin\n  Attribute \"identifier\" \"string name\" [\"groom\"]\n" << hairNetwork(scene.hairTilt);
        s << "  Procedural2 \"DynamicLoad\" \"DSOBound\" \"string dsoname\" [\"groomlab_renderman\"] \"string filename\" ["
          << path(scene.groomPath) << "] \"float frame\" [" << num(scene.frame) << "] \"string name\" [\"groom\"]";
        // The render overrides, as Maya's Arnold procedural takes them.
        if (scene.density != 1.0f) s << " \"float density\" [" << num(scene.density) << "]";
        if (scene.widthScale != 1.0f) s << " \"float widthScale\" [" << num(scene.widthScale) << "]";
        if (scene.lengthScale != 1.0f) s << " \"float lengthScale\" [" << num(scene.lengthScale) << "]";
        s << "\n";
        s << "AttributeEnd\nWorldEnd\nFrameEnd\n";
    }

    const std::string text = s.str();
    std::ofstream out(fs::u8path(scenePath), std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + scenePath);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) throw std::runtime_error("cannot write " + scenePath);
}

std::string prmanEnvironmentTexture(const QString& prman, const std::string& scenePath, const RenderStage& stage) {
    const LightingSetup& l = stage.lighting;
    if (l.environment == Environment::None || !(l.environmentIntensity > 0.0f)) return {};
    const fs::path scene = fs::u8path(scenePath);
    const fs::path hdr = scene.parent_path() / (scene.stem().u8string() + "_env.hdr");
    const fs::path tex = scene.parent_path() / (scene.stem().u8string() + "_env.tex");
    writeHdr(hdr.u8string(), stageEnvironment(l));
    const QString txmake = QDir(QFileInfo(prman).absolutePath()).filePath(
#ifdef Q_OS_WIN
        QStringLiteral("txmake.exe")
#else
        QStringLiteral("txmake")
#endif
    );
    QProcess run;
    run.setProcessEnvironment(prmanEnvironment(prman));
    run.setProcessChannelMode(QProcess::MergedChannels);
    run.start(txmake, {QStringLiteral("-envlatl"), QString::fromStdString(hdr.u8string()), QString::fromStdString(tex.u8string())});
    if (!run.waitForFinished(120000) || run.exitCode() != 0 || !fs::exists(tex)) {
        throw std::runtime_error("txmake could not make the environment's texture (" + txmake.toStdString() + "): " +
                                 QString::fromLocal8Bit(run.readAll()).trimmed().toStdString());
    }
    return tex.u8string();
}

QString prmanIn(const QString& given) {
    if (given.isEmpty()) return {};
    const QFileInfo info(given);
    if (info.isFile()) return info.absoluteFilePath();
    for (const QString& candidate : {QDir(given).filePath(QLatin1String(kPrmanExe)),
                                     QDir(given).filePath(QStringLiteral("bin/") + QLatin1String(kPrmanExe))}) {
        if (QFileInfo(candidate).isFile()) return QFileInfo(candidate).absoluteFilePath();
    }
    return {};
}

QString findPrman() {
    QString found = prmanIn(qEnvironmentVariable("GROOMLAB_PRMAN"));
    if (found.isEmpty()) found = prmanIn(QSettings().value(QLatin1String(kPrmanSetting)).toString());
    if (found.isEmpty()) found = prmanIn(qEnvironmentVariable("RMANTREE"));
    if (!found.isEmpty()) return found;
#ifdef Q_OS_WIN
    const QDir pixar(QDir(qEnvironmentVariable("ProgramFiles", QStringLiteral("C:/Program Files"))).filePath(QStringLiteral("Pixar")));
#else
    const QDir pixar(QStringLiteral("/opt/pixar"));
#endif
    QStringList installs = pixar.entryList({QStringLiteral("RenderManProServer-*")}, QDir::Dirs, QDir::Name);
    // Newest first: 27.10 after 27.3 (by the numbers, not the text).
    std::sort(installs.begin(), installs.end(), [](const QString& a, const QString& b) {
        const QStringList va = a.mid(a.indexOf(QLatin1Char('-')) + 1).split(QLatin1Char('.'));
        const QStringList vb = b.mid(b.indexOf(QLatin1Char('-')) + 1).split(QLatin1Char('.'));
        for (int i = 0; i < std::max(va.size(), vb.size()); ++i) {
            const int x = i < va.size() ? va[i].toInt() : 0, y = i < vb.size() ? vb[i].toInt() : 0;
            if (x != y) return x > y;
        }
        return false;
    });
    for (const QString& install : installs) {
        found = prmanIn(pixar.filePath(install));
        if (!found.isEmpty()) return found;
    }
    return QStandardPaths::findExecutable(QStringLiteral("prman"));
}

void setPrman(const QString& path) { QSettings().setValue(QLatin1String(kPrmanSetting), path); }

QString findRenderManProcedural() {
    const auto holds = [](const QString& dir) { return !dir.isEmpty() && QFileInfo(QDir(dir).filePath(QLatin1String(kProceduralFile))).isFile(); };
    const QString env = qEnvironmentVariable("GROOMLAB_RENDERMAN_PROCEDURAL");
    if (!env.isEmpty()) {
        const QFileInfo info(env);
        const QString dir = info.isFile() ? info.absolutePath() : info.absoluteFilePath();
        if (holds(dir)) return QDir::cleanPath(dir);
    }
    const QString beside = QDir(appResourceDir()).filePath(QStringLiteral("procedurals/renderman"));
    if (holds(beside)) return QDir::cleanPath(beside);
    // An installed GroomLab for Maya (its module's procedurals).
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString module = QDir(documents).filePath(QStringLiteral("maya/modules/GroomLab/procedurals/renderman"));
    if (holds(module)) return QDir::cleanPath(module);
    return {};
}

QStringList prmanArguments(const QString& scenePath) {
    return {QStringLiteral("-Progress"), QDir::toNativeSeparators(scenePath)};
}

QProcessEnvironment prmanEnvironment(const QString& prman) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QDir tree(QFileInfo(prman).absolutePath());
    tree.cdUp();
    env.insert(QStringLiteral("RMANTREE"), QDir::toNativeSeparators(tree.absolutePath()));
    return env;
}

bool PrmanProgress::feed(const QString& line) {
    static const QRegularExpression progressLine(QStringLiteral("R90000\\s+([0-9.]+)%"));
    static const QRegularExpression failure(QStringLiteral("\\{(ERROR|SEVERE)\\}"));
    const QRegularExpressionMatch m = progressLine.match(line);
    if (m.hasMatch()) {
        const double now = m.captured(1).toDouble();
        if (frame == 0 || (percent >= 100.0 && now < 100.0)) ++frame;  // a frame begins
        const bool ended = now >= 100.0 && percent < 100.0;
        percent = now;
        if (ended) ++imagesWritten;  // (prman names no file: the job knows the frame's)
        return true;
    }
    if (error.isEmpty() && failure.match(line).hasMatch()) error = line.trimmed();
    return false;
}

}  // namespace groomlab::app
