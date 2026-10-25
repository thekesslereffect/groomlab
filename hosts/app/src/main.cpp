// SPDX-License-Identifier: Apache-2.0
// The standalone GroomLab app: a lookdev viewer over the core without Maya (docs/DEVELOPING.md,
// "Standalone app").
//
//   GroomLab [file.groom | mesh.obj | mesh.abc] [--density 0.5] [--point-budget K] [--set node.param=value]...
//            [--material preset] [--lighting preset] [--no-lookdev]
//            [--scalp-material preset] [--prop mesh.obj [--prop-material preset]]...
//            [--eye cx,cy,cz,fx,fy,fz,r [--eye-material preset]]... [--occlusion-rays N]
//            [--brush name=value]... [--stroke "x,y x,y ..."]... [--tool camera|brush|select]
//            [--time frame] [--follow-scalp on|off] [--show-cards] [--simulate frame] [--bake-simulation frame]
//            [--export-alembic out.abc] [--export-usd out.usda] [--export-unreal out.abc]
//            [--export-unity out.abc] [--export-vrscene out.vrscene] [--export-cards out.glb|out.gltf] [--export-maps folder/name]
//            [--map-preset name] [--map-format tga|png|tiff|exr] [--map-bit-depth N] [--map-supersample N]
//            [--export-frames a-b[:step]] [--export-density d] [--points-per-strand N] [--export-scale s]
//            [--cards-resolution N] [--bake-cards dir [--bake-name name]]
//            [--screenshot out.png [--size WxH] [--frames N]] [--benchmark N] [--live-benchmark N [--live-mode orbit|hover|stroke|param]]
//            [--ui-screenshot out.png [--ui-mode groom|look|cards|export] [--ui-graph] [--ui-panel name]
//                                     [--ui-hover rail|panels|all|labels] [--ui-preset name] [--ui-search text]]
//
// --screenshot renders the scene without a window (an offscreen device) and quits: a check
// without anyone at the window; --frames N accumulates N refined frames first. --benchmark N
// renders N frames the same way and prints the time per frame. --brush and --stroke run in
// the order given, after the camera is placed: each stroke goes through the brush session as
// the mouse would, in pixels of --size's view (y down from the top, as in the screenshot), so
// a scripted run grooms and shows the result ("ctrl:" or "shift:" first: with that key held).
// --export-* and --bake-cards write the groom (at --time, or over --export-frames) without a
// window and quit unless a screenshot is asked for too: the scripted exports the tests run.
// --render-vray renders the frame in V-Ray Standalone at --size (--vray-turntable N: N images
// around the groom) the same way (vray_render.h); --render-prman in RenderMan (--prman-turntable N;
// prman_render.h).
// --ui-screenshot shows the window at --size, in a UI mode, with the node graph or a panel
// open (--ui-node selects a node in the graph, --ui-look-tab picks the look card's tab, --ui-search
// opens the graph's Add Node search with the text typed; --ui-panel shortcuts: the shortcut sheet), grabs
// it with its floating UI after the first frame and quits (no dialog ever opens). --display
// sets the display options (strand colors, the map on the scalp, the guides, the card view).
// --theme picks this run's theme (View > Theme saves one for every start; scripted runs use the
// default, Charcoal, unless --theme says otherwise).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <QApplication>
#include <QIcon>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QElapsedTimer>
#include <QImage>
#include <QStandardPaths>
#include <QSysInfo>
#include <QPointF>
#include <QTimer>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QSurfaceFormat>
#include <QVector3D>

#include "groomlab/alembic_operators.h"
#include "groomlab/card_maps.h"
#include "groomlab/graph.h"
#include "groomlab/version.h"
#include "main_window.h"
#include "glass.h"
#include "theme.h"

namespace {

// The log of a window someone sits at: Qt's messages (the graphics device, fonts, the network)
// appended to %LOCALAPPDATA%/GroomLab/logs/GroomLab.log, the run before kept as GroomLab.previous.log,
// and passed on to Qt's own handler as before. Scripted runs (the tests, the exports) don't log.
QFile* gLog = nullptr;
std::mutex gLogMutex;
QtMessageHandler gQtHandler = nullptr;

void logMessage(QtMsgType type, const QMessageLogContext& context, const QString& text) {
    static const char* const kinds[] = {"debug", "warning", "critical", "fatal", "info"};
    const char* kind = type >= QtDebugMsg && type <= QtInfoMsg ? kinds[type] : "message";
    const QByteArray line =
        QStringLiteral("%1 %2: %3\n").arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), QLatin1String(kind), text).toUtf8();
    {
        const std::lock_guard<std::mutex> lock(gLogMutex);
        if (gLog) {
            gLog->write(line);
            gLog->flush();
        }
    }
    if (gQtHandler) gQtHandler(type, context, text);
}

void startLog() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty()) return;
    const QString dir = base + QStringLiteral("/GroomLab/logs");
    if (!QDir().mkpath(dir)) return;
    const QString path = dir + QStringLiteral("/GroomLab.log"), previous = dir + QStringLiteral("/GroomLab.previous.log");
    QFile::remove(previous);
    QFile::rename(path, previous);
    auto* file = new QFile(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        delete file;
        return;
    }
    gLog = file;
    gQtHandler = qInstallMessageHandler(logMessage);
    qInfo("GroomLab %s started (Qt %s, %s)", groomlab::kVersionString, qVersion(), qPrintable(QSysInfo::prettyProductName()));
}

}  // namespace

int main(int argc, char** argv) {
    // A UI screenshot is --size pixels whatever the screen's scale: Qt's high-DPI scaling is off
    // for that run (it is decided before the application exists), so logical and device pixels
    // agree and the window is exactly as big as asked.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ui-screenshot") == 0 && !qEnvironmentVariableIsSet("QT_ENABLE_HIGHDPI_SCALING")) {
            qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
        }
    }
    // GROOMLAB_APP_NO_VSYNC=1 presents without waiting for the display (the live benchmarks'
    // frame costs past the refresh rate); decided before the first window exists.
    if (qEnvironmentVariableIntValue("GROOMLAB_APP_NO_VSYNC") != 0) {
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setSwapInterval(0);
        QSurfaceFormat::setDefaultFormat(format);
    }
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("GroomLab"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/groomlab/icons/app_icon.svg")));  // every window's, the taskbar's
    QApplication::setOrganizationName(QStringLiteral("GroomLab"));
    QApplication::setApplicationVersion(QString::fromLatin1(groomlab::kVersionString));  // About, Check for Updates
    QSettings::setDefaultFormat(QSettings::IniFormat);  // the recent files: an .ini in the user's config folder
    // The ImportCurves node (Alembic guides) as the CLI and Maya register it.
    groomlab::registerAlembicOperators(groomlab::OperatorRegistry::instance());

    QString liveMode = QStringLiteral("orbit");
    QString path, screenshot, material, lighting, graph, quality, interactiveQuality, save, tonemap, background, scalpMaterial;
    // The headless benchmarks: drags of a param (node.param=from,to[,steps]) and an orbit, each
    // step drawn offscreen; --draw-dabs draws every --stroke dab the same way and prints the
    // times; --gpu-preview on|off picks the GPU look preview (on by default where there is one).
    QStringList benchmarkDrags;
    int benchmarkOrbit = 0;
    bool drawDabs = false;
    int gpuPreview = -1;  // -1: the preference
    QString uiScreenshot, uiMode, uiPanel, uiLookTab, uiNode, uiHover, uiPreset, uiMenu, uiPrompt, uiPaintParam, uiSearch, themeName, uiTheme;
    bool hasUiSearch = false;
    bool uiGraph = false;
    QStringList sets, displays;
    // Props (meshes beside the groom) with the preset each is shaded by, and eyes.
    struct Prop {
        QString path, material;
        QStringList sets;  // name=value edits of its material (setSurfaceParam)
    };
    struct Eye {
        QVector3D center, forward;
        float radius = 1.2f;
        QString material;
        QStringList sets;
    };
    std::vector<Prop> props;
    std::vector<Eye> eyes;
    // Brush settings and strokes, in the order given.
    struct GroomStep {
        bool stroke = false;
        QString text;
    };
    std::vector<GroomStep> groomSteps;
    QString tool;
    double density = -1.0, pointBudget = -1.0;
    // Exports (target, path) in the order given, their shared options, and the card bake.
    std::vector<std::pair<QString, QString>> exports;
    groomlab::app::ExportOptions exportOptions;
    exportOptions.mapPreset = groomlab::builtinCardMapPresets().front();  // --export-maps: Unreal's layout by default
    bool exportRange = false;
    QString bakeDir, bakeName;
    QString vrayImage;
    int vrayTurntable = 0;
    QString prmanImage;
    int prmanTurntable = 0;
    bool bake = false;
    int bakeResolution = 0;
    double atFrame = 0.0;
    bool hasFrame = false, showCards = false;
    double simulateFrame = -1.0, bakeSimulationFrame = -1.0;  // --simulate, --bake-simulation
    int followScalp = -1;  // -1 as the file says, 0 off, 1 on
    int frames = 1, benchmark = 0, liveBenchmark = 0, width = 1280, height = 720, occlusionRays = 32;
    float yaw = 0.0f, pitch = 0.0f, dolly = 0.0f, distance = 0.0f;
    QVector3D lookAt;
    bool lookdev = true, hasLookAt = false, showScalp = true;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(arg, "--screenshot") == 0 && hasValue) {
            screenshot = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--density") == 0 && hasValue) {
            density = std::atof(argv[++i]);
        } else if (std::strcmp(arg, "--point-budget") == 0 && hasValue) {
            pointBudget = std::atof(argv[++i]);  // thousands of points, 0: no limit (the benchmarks at full density)
        } else if (std::strcmp(arg, "--set") == 0 && hasValue) {
            sets << QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--bypass") == 0 && hasValue) {
            sets << QStringLiteral("bypass:") + QString::fromLocal8Bit(argv[++i]);  // a node bypassed (the benchmarks without Collide)
        } else if (std::strcmp(arg, "--graph") == 0 && hasValue) {
            graph = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--save") == 0 && hasValue) {
            save = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--material") == 0 && hasValue) {
            material = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--lighting") == 0 && hasValue) {
            lighting = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--quality") == 0 && hasValue) {
            quality = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--interactive-quality") == 0 && hasValue) {
            interactiveQuality = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--tonemap") == 0 && hasValue) {
            tonemap = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--background") == 0 && hasValue) {
            background = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--frames") == 0 && hasValue) {
            frames = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--benchmark") == 0 && hasValue) {
            benchmark = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--live-benchmark") == 0 && hasValue) {
            liveBenchmark = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--live-mode") == 0 && hasValue) {
            liveMode = QString::fromLatin1(argv[++i]);
        } else if (std::strcmp(arg, "--benchmark-drag") == 0 && hasValue) {
            benchmarkDrags << QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--benchmark-orbit") == 0 && hasValue) {
            benchmarkOrbit = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--draw-dabs") == 0) {
            drawDabs = true;
        } else if (std::strcmp(arg, "--gpu-preview") == 0 && hasValue) {
            const QString v = QString::fromLatin1(argv[++i]).toLower();
            gpuPreview = (v == QStringLiteral("on") || v == QStringLiteral("1") || v == QStringLiteral("true")) ? 1 : 0;
        } else if (std::strcmp(arg, "--size") == 0 && hasValue) {
            const QStringList parts = QString::fromLatin1(argv[++i]).split(QLatin1Char('x'));
            if (parts.size() == 2) {
                width = parts[0].toInt();
                height = parts[1].toInt();
            }
        } else if (std::strcmp(arg, "--no-lookdev") == 0) {
            lookdev = false;
        } else if (std::strcmp(arg, "--hide-scalp") == 0) {
            // A prop covers the scalp (a head the scalp was cut from): the scalp is not drawn.
            showScalp = false;
        } else if (std::strcmp(arg, "--prop") == 0 && hasValue) {
            props.push_back({QString::fromLocal8Bit(argv[++i]), QStringLiteral("clay")});
        } else if (std::strcmp(arg, "--prop-material") == 0 && hasValue) {
            if (!props.empty()) props.back().material = QString::fromLocal8Bit(argv[++i]);
            else ++i;
        } else if (std::strcmp(arg, "--prop-set") == 0 && hasValue) {
            // name=value on the last prop's or eye's material, whichever came last.
            const QString set = QString::fromLocal8Bit(argv[++i]);
            if (!eyes.empty()) eyes.back().sets << set;
            else if (!props.empty()) props.back().sets << set;
        } else if (std::strcmp(arg, "--scalp-material") == 0 && hasValue) {
            scalpMaterial = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--eye") == 0 && hasValue) {
            // cx,cy,cz,fx,fy,fz,r: the center, the direction it looks along, the radius.
            const QStringList v = QString::fromLatin1(argv[++i]).split(QLatin1Char(','));
            if (v.size() != 7) {
                std::fprintf(stderr, "GroomLab: --eye takes cx,cy,cz,fx,fy,fz,r\n");
                return 2;
            }
            Eye eye;
            eye.center = QVector3D(v[0].toFloat(), v[1].toFloat(), v[2].toFloat());
            eye.forward = QVector3D(v[3].toFloat(), v[4].toFloat(), v[5].toFloat());
            eye.radius = v[6].toFloat();
            eye.material = QStringLiteral("eye brown");
            eyes.push_back(eye);
        } else if (std::strcmp(arg, "--eye-material") == 0 && hasValue) {
            if (!eyes.empty()) eyes.back().material = QString::fromLocal8Bit(argv[++i]);
            else ++i;
        } else if (std::strcmp(arg, "--occlusion-rays") == 0 && hasValue) {
            occlusionRays = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--orbit") == 0 && i + 2 < argc) {
            yaw = static_cast<float>(std::atof(argv[++i]));
            pitch = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(arg, "--dolly") == 0 && hasValue) {
            dolly = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(arg, "--look-at") == 0 && hasValue) {
            // The camera's target (and --distance how far from it): a close-up on a point.
            const QStringList v = QString::fromLatin1(argv[++i]).split(QLatin1Char(','));
            if (v.size() != 3) {
                std::fprintf(stderr, "GroomLab: --look-at takes x,y,z\n");
                return 2;
            }
            lookAt = QVector3D(v[0].toFloat(), v[1].toFloat(), v[2].toFloat());
            hasLookAt = true;
        } else if (std::strcmp(arg, "--distance") == 0 && hasValue) {
            distance = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(arg, "--brush") == 0 && hasValue) {
            groomSteps.push_back({false, QString::fromLocal8Bit(argv[++i])});
        } else if (std::strcmp(arg, "--stroke") == 0 && hasValue) {
            groomSteps.push_back({true, QString::fromLocal8Bit(argv[++i])});
        } else if (std::strcmp(arg, "--stroke-world") == 0 && hasValue) {
            // "x,y,z x,y,z ...": scene points, projected with the camera as placed (a stroke
            // the benchmark scripts give in world space, as Maya's does).
            groomSteps.push_back({true, QStringLiteral("world: ") + QString::fromLocal8Bit(argv[++i])});
        } else if (std::strcmp(arg, "--tool") == 0 && hasValue) {
            tool = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--ui-screenshot") == 0 && hasValue) {
            uiScreenshot = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--ui-mode") == 0 && hasValue) {
            uiMode = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--ui-panel") == 0 && hasValue) {
            uiPanel = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--ui-graph") == 0) {
            uiGraph = true;
        } else if (std::strcmp(arg, "--ui-look-tab") == 0 && hasValue) {
            uiLookTab = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--ui-node") == 0 && hasValue) {
            uiNode = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--ui-hover") == 0 && hasValue) {
            uiHover = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--ui-preset") == 0 && hasValue) {
            uiPreset = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--ui-menu") == 0 && hasValue) {
            uiMenu = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--ui-prompt") == 0 && hasValue) {
            uiPrompt = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--ui-theme") == 0 && hasValue) {
            uiTheme = QString::fromLocal8Bit(argv[++i]);  // switched live once the window is up (GroomLab > Theme)
        } else if (std::strcmp(arg, "--ui-search") == 0 && hasValue) {
            uiSearch = QString::fromLocal8Bit(argv[++i]);
            hasUiSearch = true;
        } else if (std::strcmp(arg, "--ui-paint-param") == 0 && hasValue) {
            uiPaintParam = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--theme") == 0 && hasValue) {
            themeName = QString::fromLocal8Bit(argv[++i]);  // this run's theme (the saved choice unchanged)
        } else if (std::strcmp(arg, "--display") == 0 && hasValue) {
            displays << QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--time") == 0 && hasValue) {
            atFrame = std::atof(argv[++i]);
            hasFrame = true;
        } else if (std::strcmp(arg, "--follow-scalp") == 0 && hasValue) {
            const QString v = QString::fromLatin1(argv[++i]).toLower();
            followScalp = (v == QStringLiteral("on") || v == QStringLiteral("1") || v == QStringLiteral("true")) ? 1 : 0;
        } else if (std::strcmp(arg, "--show-cards") == 0) {
            showCards = true;
        } else if (std::strcmp(arg, "--simulate") == 0 && hasValue) {
            simulateFrame = std::atof(argv[++i]);
        } else if (std::strcmp(arg, "--bake-simulation") == 0 && hasValue) {
            bakeSimulationFrame = std::atof(argv[++i]);
        } else if (std::strncmp(arg, "--export-", 9) == 0 && hasValue &&
                   (std::strcmp(arg + 9, "alembic") == 0 || std::strcmp(arg + 9, "usd") == 0 || std::strcmp(arg + 9, "unreal") == 0 ||
                    std::strcmp(arg + 9, "unity") == 0 || std::strcmp(arg + 9, "cards") == 0 || std::strcmp(arg + 9, "maps") == 0 ||
                    std::strcmp(arg + 9, "vrscene") == 0)) {
            exports.emplace_back(QString::fromLatin1(arg + 9), QString::fromLocal8Bit(argv[++i]));
        } else if (std::strcmp(arg, "--render-vray") == 0 && hasValue) {
            vrayImage = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--render-prman") == 0 && hasValue) {
            prmanImage = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--prman-turntable") == 0 && hasValue) {
            prmanTurntable = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--vray-turntable") == 0 && hasValue) {
            vrayTurntable = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--export-frames") == 0 && hasValue) {
            // a-b[:step]: the frames every export covers (else the current frame alone).
            const QString text = QString::fromLatin1(argv[++i]);
            const QStringList rangeAndStep = text.split(QLatin1Char(':'));
            const QStringList ab = rangeAndStep[0].split(QLatin1Char('-'), Qt::SkipEmptyParts);
            if (ab.size() != 2) {
                std::fprintf(stderr, "GroomLab: --export-frames takes a-b[:step]\n");
                return 2;
            }
            exportOptions.frames.first = ab[0].toDouble();
            exportOptions.frames.last = ab[1].toDouble();
            exportOptions.frames.step = rangeAndStep.size() > 1 ? rangeAndStep[1].toDouble() : 1.0;
            exportRange = true;
        } else if (std::strcmp(arg, "--export-density") == 0 && hasValue) {
            exportOptions.density = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(arg, "--points-per-strand") == 0 && hasValue) {
            exportOptions.pointsPerStrand = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--export-scale") == 0 && hasValue) {
            exportOptions.scale = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(arg, "--cards-resolution") == 0 && hasValue) {
            exportOptions.resolution = std::atoi(argv[++i]);
            exportOptions.mapSettings.resolution = exportOptions.resolution;
            bakeResolution = exportOptions.resolution;
        } else if (std::strcmp(arg, "--map-preset") == 0 && hasValue) {
            const std::string name = QString::fromLocal8Bit(argv[++i]).toStdString();
            const auto preset = groomlab::findCardMapPreset(name);
            if (!preset) {
                std::fprintf(stderr, "GroomLab: no card map preset named '%s'\n", name.c_str());
                return 2;
            }
            exportOptions.mapPreset = *preset;
        } else if (std::strcmp(arg, "--map-format") == 0 && hasValue) {
            exportOptions.mapSettings.format = argv[++i];
        } else if (std::strcmp(arg, "--map-bit-depth") == 0 && hasValue) {
            exportOptions.mapSettings.bitDepth = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--map-supersample") == 0 && hasValue) {
            exportOptions.mapSettings.supersample = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--bake-cards") == 0 && hasValue) {
            bakeDir = QString::fromLocal8Bit(argv[++i]);
            bake = true;
        } else if (std::strcmp(arg, "--bake-name") == 0 && hasValue) {
            bakeName = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            std::printf("usage: GroomLab [file.groom | mesh.obj | mesh.abc] [--graph graph.json] [--density d] [--point-budget K] [--set node.param=value]... [--bypass node]...\n"
                        "                [--save out.groom]\n"
                        "                [--material groom|black|brown|blonde|red|white|dyed] [--lighting studio|daylight|overcast|sunset|rim]\n"
                        "                [--quality interactive|refine] [--interactive-quality adaptive|full] [--tonemap none|aces|agx] [--background solid|gradient|environment]\n"
                        "                [--scalp-material preset] [--prop mesh.obj [--prop-material preset] [--prop-set name=value]...]...\n"
                        "                [--eye cx,cy,cz,fx,fy,fz,r [--eye-material preset] [--prop-set name=value]...]... [--occlusion-rays N]\n"
                        "                [--no-lookdev] [--hide-scalp] [--orbit yaw pitch] [--dolly f] [--look-at x,y,z] [--distance d]\n"
                        "                [--brush name=value]... [--stroke \"[ctrl:|shift:]x,y x,y ...\"]... [--stroke-world \"x,y,z ...\"]... [--tool camera|brush|select]\n"
                        "                [--time frame] [--follow-scalp on|off] [--show-cards] [--simulate frame] [--bake-simulation frame]\n"
                        "                [--export-alembic out.abc] [--export-usd out.usda] [--export-unreal out.abc] [--export-unity out.abc]\n"
                        "                [--export-vrscene out.vrscene] [--render-vray out.png [--vray-turntable N]]\n"
                        "                [--render-prman out.png [--prman-turntable N]]\n"
                        "                [--export-cards out.glb|out.gltf] [--export-frames a-b[:step]] [--export-density d]\n"
                        "                [--points-per-strand N] [--export-scale s] [--cards-resolution N]\n"
                        "                [--export-maps folder/name [--map-preset name] [--map-format f] [--map-bit-depth N] [--map-supersample N]]\n"
                        "                [--bake-cards dir [--bake-name name]]\n"
                        "                [--screenshot out.png [--size WxH] [--frames N]] [--benchmark N]\n"
                        "                [--live-benchmark N [--live-mode orbit|hover|stroke|param|param-commit]]\n"
                        "                [--gpu-preview on|off] [--benchmark-drag node.param=from,to[,steps]]... [--benchmark-orbit N] [--draw-dabs]\n"
                        "                [--display name=value]... [--theme charcoal|playful|polar|studio]\n"
                        "                [--ui-screenshot out.png [--ui-mode groom|look|cards|export] [--ui-graph] [--ui-panel name]\n"
                        "                                         [--ui-look-tab material|lighting|render[,more]] [--ui-node name]\n"
                        "                                         [--ui-hover rail|panels|all|labels] [--ui-preset name] [--ui-menu presets|add|edit|node|groups|editguides|maps|convert|correctives|marking|bar:Menu/Submenu]\n"
                        "                                         [--ui-prompt unsaved|reload|info|error|autosave] [--ui-theme name]\n"
                        "                                         [--ui-paint-param node.param]]\n"
                        "surface presets: clay, scalp, plastic, metal, skin, skin light, skin dark, eye brown, eye blue, eye green\n"
                        "display options: colorMode (solid, colorset, attribute, random, clump, density), colorSource, colorMin,\n"
                        "                colorMax, guides, tubes, guidePoints (0-2), scalpMap, scalpMapChannel (rgb, r, g, b, a), cardsView,\n"
                        "                lookdev, scalp, widthCoverage, coverageDensity, clumpLod, clumpLodError\n"
                        "brush settings: mode (comb, smooth, slide, lift, length, cut, curl, twist, straighten, attract,\n"
                        "                clump, paint, tube, erase, place, draw, part, flow, freeze), radius (px), strength, hardness,\n"
                        "                symmetry (x|y|z|none), symmetryCenter, level, group, pressure, maskByScalp, selectedOnly,\n"
                        "                collide, stretch, sculptDensity, length, spacing, attribute, value,\n"
                        "                tubeChannel (width|flatness|twist), select (all|none|guide IDs: 3,4,9)\n");
            return 0;
        } else if (arg[0] == '-') {
            std::fprintf(stderr, "GroomLab: unknown option %s\n", arg);
            return 2;
        } else {
            path = QString::fromLocal8Bit(arg);
        }
    }

    const bool writes = !exports.empty() || bake || !vrayImage.isEmpty() || !prmanImage.isEmpty();
    const bool headless = !screenshot.isEmpty() || benchmark > 0 || !benchmarkDrags.isEmpty() || benchmarkOrbit > 0 || drawDabs ||
                          (writes && uiScreenshot.isEmpty());
    const bool scripted = headless || !uiScreenshot.isEmpty();  // a run no one sits at: errors end it
    // The settings before anything reads them: GROOMLAB_SETTINGS_DIR's folder, else a run no one
    // sits at gets a throwaway one (never the user's recent files, theme or autosaves; main_window.h).
    const groomlab::app::RunSettings runSettings(scripted || liveBenchmark > 0);
    if (!runSettings.valid()) {
        std::fprintf(stderr, "GroomLab: no folder for this run's settings under %s\n", qPrintable(QDir::tempPath()));
        return 1;
    }
    if (!scripted && liveBenchmark <= 0) startLog();
    // The theme, before any widget takes its colors: --theme's, else the saved choice (View >
    // Theme) for a window someone sits at; scripted runs stay on the default unless asked.
    if (!themeName.isEmpty()) {
        if (!groomlab::app::theme::setTheme(themeName)) {
            std::fprintf(stderr, "GroomLab: no theme named %s (themes: %s)\n", qPrintable(themeName),
                         qPrintable(groomlab::app::theme::themeNames().join(QStringLiteral(", "))));
            return 2;
        }
    } else if (!scripted && liveBenchmark <= 0) {
        groomlab::app::theme::setTheme(groomlab::app::theme::savedTheme());
    }
    QElapsedTimer creation;
    creation.start();
    groomlab::app::MainWindow window;
    if (gpuPreview >= 0) window.setGpuPreview(gpuPreview == 1);
    if (density > 0.0) window.setPreviewDensity(density);
    if (pointBudget >= 0.0) window.setPointBudget(pointBudget);
    if (!material.isEmpty()) window.applyMaterialPreset(material);
    if (!lighting.isEmpty()) window.applyLightingPreset(lighting);
    // A screenshot of several frames accumulates them: Refine, unless asked otherwise (the app's
    // own default is Interactive).
    if (quality.isEmpty() && frames > 1) quality = QStringLiteral("refine");
    if (!quality.isEmpty()) window.setQuality(quality);
    if (!interactiveQuality.isEmpty()) window.setInteractiveQuality(interactiveQuality);
    if (!tonemap.isEmpty()) window.setToneMap(tonemap);
    if (!background.isEmpty()) window.setBackground(background);
    window.setLookdev(lookdev);
    window.setShowScalp(showScalp);
    if (!uiScreenshot.isEmpty() || liveBenchmark > 0) {
        // The window's central widget takes --size: the frame comes on top of it.
        window.resize(std::max(width, 320), std::max(height, 240));
    }
    // The window someone sits at (and the live benchmark, which measures it) evaluates commits
    // off the UI thread; scripted runs keep every evaluation done before the next step.
    // GROOMLAB_APP_ASYNC_COMMITS=0 keeps them on the UI thread (to compare).
    window.setAsyncCommits(((!scripted && !headless) || liveBenchmark > 0) && qEnvironmentVariable("GROOMLAB_APP_ASYNC_COMMITS") != QStringLiteral("0"));
    // A CPU stroke's dabs the same way (GROOMLAB_APP_ASYNC_DABS=0 keeps them on the UI thread).
    window.setAsyncDabs(((!scripted && !headless) || liveBenchmark > 0) && qEnvironmentVariable("GROOMLAB_APP_ASYNC_DABS") != QStringLiteral("0"));
    // A window someone sits at: its default size within the screen (a 1366x768 laptop, or 1920x1080
    // with 125-150% text, is smaller than it), centered.
    // The docks as that window left them last time (its size and place too).
    if (!headless && uiScreenshot.isEmpty() && liveBenchmark <= 0) {
        groomlab::app::fitWindowToScreen(&window, window.size());
        window.restoreLayout();
    }
    // Someone sits at it: unsaved edits are asked about before they go, info messages toast.
    window.setInteractive(!scripted && liveBenchmark <= 0);
    if (!scripted && liveBenchmark <= 0) window.restoreRailLabels();
    if (!headless) window.show();
    if (!uiMode.isEmpty() && !window.setUiMode(uiMode) && scripted) return 1;
    if (!path.isEmpty() && !window.openPath(path) && headless) return 1;
    // A groom left with unsaved changes when GroomLab last closed (a crash): its autosave offered.
    if (!scripted && liveBenchmark <= 0 && path.isEmpty()) window.offerAutosave();
    if (!graph.isEmpty() && !window.loadGraphFile(graph) && headless) return 1;
    for (const QString& set : sets) {
        if (set.startsWith(QStringLiteral("bypass:"))) {
            if (!window.setBypassText(set.mid(7)) && headless) return 1;
        } else if (!window.setParamText(set) && headless) {
            return 1;
        }
    }
    // Creation: the groom opened (or made on its mesh with its graph) and evaluated, to the
    // first drawn frame below (headless runs print both).
    const double creationMs = creation.nsecsElapsed() / 1e6;
    if (followScalp >= 0 && !window.setFollowScalp(followScalp == 1) && headless) return 1;
    // The simulation first (Simulate Range, or a bake beside the groom), then the frame shown.
    if (simulateFrame >= 0.0 && !window.simulateTo(simulateFrame) && headless) return 1;
    if (bakeSimulationFrame >= 0.0 && !window.bakeSimulation(bakeSimulationFrame) && headless) return 1;
    if (hasFrame && !window.setFrame(atFrame) && headless) return 1;
    if (showCards) window.setShowCards(true);
    if (!scalpMaterial.isEmpty()) window.setScalpMaterial(scalpMaterial);
    for (const Prop& prop : props) {
        if (!window.addProp(prop.path, prop.material, occlusionRays) && headless) return 1;
        for (const QString& set : prop.sets) {
            if (!window.setPropParam(set) && headless) return 1;
        }
    }
    for (const Eye& eye : eyes) {
        if (!window.addEye(eye.center, eye.forward, eye.radius, eye.material, occlusionRays) && headless) return 1;
        for (const QString& set : eye.sets) {
            if (!window.setPropParam(set) && headless) return 1;
        }
    }
    if (hasLookAt || distance > 0.0f) window.lookAt(hasLookAt ? lookAt : QVector3D(), distance);
    if (yaw != 0.0f || pitch != 0.0f || dolly > 0.0f) window.adjustCamera(yaw, pitch, dolly);
    if (!tool.isEmpty() && !window.setToolMode(tool) && headless) return 1;
    const QSize viewSize(std::max(width, 16), std::max(height, 16));
    for (const GroomStep& step : groomSteps) {
        if (!step.stroke) {
            if (!window.setBrushText(step.text) && headless) return 1;
            continue;
        }
        // "x,y x,y ...": pixels of the screenshot's view, y down; a "ctrl:" or "shift:" word first.
        QStringList words = step.text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        bool ctrl = false, shift = false, world = false;
        while (!words.isEmpty() && (words.first() == QStringLiteral("ctrl:") || words.first() == QStringLiteral("shift:") ||
                                    words.first() == QStringLiteral("world:"))) {
            if (words.first() == QStringLiteral("ctrl:")) ctrl = true;
            else if (words.first() == QStringLiteral("shift:")) shift = true;
            else world = true;
            words.removeFirst();
        }
        std::vector<QPointF> points;
        for (const QString& word : words) {
            const QStringList xy = word.split(QLatin1Char(','));
            if (world && xy.size() == 3) {
                QPointF at;
                if (!window.projectToView(QVector3D(xy[0].toFloat(), xy[1].toFloat(), xy[2].toFloat()), viewSize, &at)) {
                    std::fprintf(stderr, "GroomLab: --stroke-world: %s is behind the camera\n", qPrintable(word));
                    return 2;
                }
                points.push_back(at);
                continue;
            }
            if (xy.size() != 2) {
                std::fprintf(stderr, "GroomLab: --stroke takes \"x,y x,y ...\", got %s\n", qPrintable(word));
                return 2;
            }
            points.emplace_back(xy[0].toDouble(), xy[1].toDouble());
        }
        if (!window.applyStroke(points, viewSize, ctrl, shift, drawDabs) && headless) return 1;
    }
    for (const QString& drag : benchmarkDrags) {
        // node.param=from,to[,steps]
        const int dot = drag.indexOf(QLatin1Char('.')), equals = drag.indexOf(QLatin1Char('='));
        const QStringList v = equals > 0 ? drag.mid(equals + 1).split(QLatin1Char(',')) : QStringList();
        if (dot <= 0 || equals <= dot + 1 || v.size() < 2) {
            std::fprintf(stderr, "GroomLab: --benchmark-drag takes node.param=from,to[,steps], got %s\n", qPrintable(drag));
            return 2;
        }
        const int steps = v.size() > 2 ? std::max(v[2].toInt(), 1) : 7;
        if (!window.benchmarkDrag(drag.left(dot), drag.mid(dot + 1, equals - dot - 1), v[0].toDouble(), v[1].toDouble(), steps, viewSize)) return 1;
    }
    if (benchmarkOrbit > 0) window.benchmarkOrbit(viewSize, benchmarkOrbit);
    if (!save.isEmpty() && !window.saveTo(save) && headless) return 1;
    for (const QString& display : displays) {
        if (!window.setDisplayText(display) && scripted) return 1;
    }
    // The bake, then every export, at the frame (or over the range) asked for.
    if (bake && !window.bakeCardsTo(bakeDir, bakeName, bakeResolution) && scripted) return 1;
    for (const auto& [target, out] : exports) {
        groomlab::app::ExportOptions options = exportOptions;
        if (!exportRange && window.document()) options.frames.first = options.frames.last = window.document()->frame();
        if (!window.exportTo(target, out, options) && scripted) return 1;
    }
    // A V-Ray Standalone render of the frame (or a turntable) at --size, as the window frames it.
    if (!vrayImage.isEmpty() && !window.renderVray(vrayImage, vrayTurntable, QSize(width, height), true) && scripted) return 1;
    // And in RenderMan (prman, GroomLab's procedural evaluating the groom).
    if (!prmanImage.isEmpty() && !window.renderPrman(prmanImage, prmanTurntable, QSize(width, height), true) && scripted) return 1;
    if (liveBenchmark > 0) {
        if (uiGraph) window.setGraphShown(true);
        window.benchmarkLive(liveBenchmark, liveMode, [](const QString& line) {
            std::printf("GroomLab: %s\n", qPrintable(line));
            QApplication::quit();
        });
        QTimer::singleShot(120000, &app, [] { QApplication::quit(); });
        return QApplication::exec();
    }
    if (!uiScreenshot.isEmpty()) {
        if (uiGraph) window.setGraphShown(true);
        if (!uiPreset.isEmpty() && !window.applyPreset(uiPreset)) return 1;
        if (!uiNode.isEmpty() && !window.selectUiNode(uiNode)) return 1;
        if (!uiPaintParam.isEmpty()) {
            const int dot = uiPaintParam.indexOf(QLatin1Char('.'));
            if (dot <= 0 || !window.paintParam(uiPaintParam.left(dot), uiPaintParam.mid(dot + 1))) return 1;
        }
        if (!uiPanel.isEmpty() && !window.showUiPanel(uiPanel)) return 1;
        if (!uiHover.isEmpty() && !window.setUiHover(uiHover)) return 1;
        if (!uiMenu.isEmpty() && !window.setUiMenu(uiMenu)) return 1;
        if (!uiTheme.isEmpty() && !window.chooseTheme(uiTheme)) return 1;
        if (!uiPrompt.isEmpty() && !window.setUiPrompt(uiPrompt)) return 1;
        if (!uiLookTab.isEmpty() && !window.setUiLookTab(uiLookTab)) return 1;
        if (hasUiSearch && !window.setUiSearch(uiSearch)) return 1;
        int code = 1;
        window.captureUi([&](const QImage& image) {
            if (image.isNull()) {
                std::fprintf(stderr, "GroomLab: no pixels to write for the UI screenshot\n");
            } else if (!image.save(uiScreenshot)) {
                std::fprintf(stderr, "GroomLab: could not write %s\n", qPrintable(uiScreenshot));
            } else {
                std::printf("GroomLab: wrote %s (%dx%d, the window with its UI)\n", qPrintable(uiScreenshot), image.width(), image.height());
                code = 0;
            }
            QApplication::quit();
        });
        // No frame within a few seconds (no device): the run ends rather than hangs.
        QTimer::singleShot(15000, &app, [] { QApplication::quit(); });
        QApplication::exec();
        return code;
    }
    if (headless && screenshot.isEmpty() && benchmark <= 0) return 0;  // the exports were the run
    if (headless) {
        if (screenshot.isEmpty() && benchmark == 0) return 0;  // the benchmarks above were the run
        double ms = 0.0, firstMs = 0.0;
        QString api;
        const int count = benchmark > 0 ? benchmark : std::max(frames, 1);
        std::printf("GroomLab: %s\n", qPrintable(window.cameraDescription()));
        double gpuMs = 0.0;
        const QImage image = window.renderOffscreen(viewSize, count, &ms, &api, nullptr, &firstMs, &gpuMs);
        if (benchmark > 0) {
            std::printf("GroomLab: %d frames at %dx%d on %s: %.2f ms/frame (GPU %.2f ms); creation (open, evaluate) %.1f ms, the first frame %.1f ms\n",
                        count, width, height, qPrintable(api), ms, gpuMs, creationMs, firstMs);
        }
        if (!screenshot.isEmpty()) {
            if (image.isNull()) {
                std::fprintf(stderr, "GroomLab: no pixels to write (%s)\n", qPrintable(api));
                return 1;
            }
            if (!image.save(screenshot)) {
                std::fprintf(stderr, "GroomLab: could not write %s\n", qPrintable(screenshot));
                return 1;
            }
            std::printf("GroomLab: wrote %s (%dx%d, %d frames, %.2f ms/frame on %s)\n", qPrintable(screenshot), image.width(),
                        image.height(), count, ms, qPrintable(api));
        }
        return 0;
    }
    return QApplication::exec();
}
