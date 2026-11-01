// SPDX-License-Identifier: Apache-2.0
// Guides from reference photos (reference_guides.h): the orientation field and the guides.

#include "bind_common.h"

#include <cmath>
#include <fstream>

#include "groomlab/pin_morph.h"
#include "groomlab/png.h"
#include "groomlab/reference_edit.h"
#include "groomlab/reference_guides.h"

namespace groomlab::py_bind {

namespace {

std::array<float, 3> arr3(const std::array<float, 3>& a) { return a; }

// A camera from a dict: "rotation" (3x3, the camera's axes as columns: X right, Y up, Z back) and
// "position", or "matrix" (Maya's worldMatrix: 16 floats, rows x, y, z axes then position); the
// focal length as "focal_px", or "focal_mm" with "aperture_in" (the horizontal film aperture);
// "width" and "height" (pixels the focal length is for).
RefCamera cameraFrom(const py::dict& d) {
    RefCamera c;
    c.width = d.contains("width") ? d["width"].cast<double>() : 0.0;
    c.height = d.contains("height") ? d["height"].cast<double>() : 0.0;
    if (d.contains("matrix")) {
        const auto m = d["matrix"].cast<std::vector<double>>();
        if (m.size() != 16) throw py::value_error("camera matrix: 16 numbers");
        for (int axis = 0; axis < 3; ++axis) {
            double len = std::sqrt(m[static_cast<std::size_t>(axis * 4)] * m[static_cast<std::size_t>(axis * 4)] +
                                   m[static_cast<std::size_t>(axis * 4 + 1)] * m[static_cast<std::size_t>(axis * 4 + 1)] +
                                   m[static_cast<std::size_t>(axis * 4 + 2)] * m[static_cast<std::size_t>(axis * 4 + 2)]);
            if (len <= 0.0) len = 1.0;
            for (int row = 0; row < 3; ++row) c.pose.rotation[static_cast<std::size_t>(row)][static_cast<std::size_t>(axis)] = m[static_cast<std::size_t>(axis * 4 + row)] / len;
        }
        for (int i = 0; i < 3; ++i) c.pose.position[static_cast<std::size_t>(i)] = m[static_cast<std::size_t>(12 + i)];
    } else {
        const auto r = d["rotation"].cast<std::vector<std::vector<double>>>();
        if (r.size() != 3) throw py::value_error("camera rotation: 3x3");
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t j = 0; j < 3; ++j) c.pose.rotation[i][j] = r.at(i).at(j);
        const auto p = d["position"].cast<std::vector<double>>();
        for (std::size_t i = 0; i < 3; ++i) c.pose.position[i] = p.at(i);
    }
    if (d.contains("focal_px")) {
        c.focalPx = d["focal_px"].cast<double>();
    } else if (d.contains("focal_mm")) {
        c.focalPx = focalMmToPx(d["focal_mm"].cast<double>(), c.width, d["aperture_in"].cast<double>());
    } else {
        throw py::value_error("camera: focal_px, or focal_mm and aperture_in");
    }
    return c;
}

ReferenceView viewFrom(const py::dict& d) {
    ReferenceView v;
    v.camera = cameraFrom(d["camera"].cast<py::dict>());
    if (d.contains("image") && !d["image"].is_none()) v.image = loadRefImage(d["image"].cast<std::string>(), 3);
    if (d.contains("mask") && !d["mask"].is_none()) v.mask = loadRefImage(d["mask"].cast<std::string>(), 1);
    // Flow hints: "hints" (polylines in the photo's pixels, root first: [[u, v], ...], or
    // {"points": [...], "directed": bool}) and "hint_image" (a painted RGBA overlay).
    if (d.contains("hints") && !d["hints"].is_none()) {
        for (py::handle h : d["hints"]) {
            RefHintStroke stroke;
            py::object points = py::reinterpret_borrow<py::object>(h);
            if (py::isinstance<py::dict>(h)) {
                const py::dict hd = h.cast<py::dict>();
                points = hd["points"];
                if (hd.contains("directed")) stroke.directed = hd["directed"].cast<bool>();
            }
            for (py::handle p : points) {
                const auto uv = p.cast<std::vector<float>>();
                if (uv.size() < 2) throw py::value_error("a hint point is (u, v)");
                stroke.points.push_back({uv[0], uv[1]});
            }
            v.hints.push_back(std::move(stroke));
        }
    }
    // Mask touch-ups: [{"points": [[u, v], ...], "radius": px, "include": bool}] (photo pixels).
    if (d.contains("mask_strokes") && !d["mask_strokes"].is_none()) {
        for (py::handle h : d["mask_strokes"]) {
            const py::dict hd = h.cast<py::dict>();
            RefMaskStroke stroke;
            for (py::handle p : hd["points"]) {
                const auto uv = p.cast<std::vector<float>>();
                if (uv.size() < 2) throw py::value_error("a mask stroke point is (u, v)");
                stroke.points.push_back({uv[0], uv[1]});
            }
            if (hd.contains("radius")) stroke.radius = hd["radius"].cast<float>();
            if (hd.contains("include")) stroke.include = hd["include"].cast<bool>();
            v.maskStrokes.push_back(std::move(stroke));
        }
    }
    if (d.contains("hint_image") && !d["hint_image"].is_none()) v.hintOverlay = loadRefImage(d["hint_image"].cast<std::string>(), 4);
    if (v.camera.width <= 0.0) {
        const RefImage& sized = v.image.empty() ? v.mask : v.image;
        v.camera.width = sized.width;
        v.camera.height = sized.height;
    }
    return v;
}

py::dict lookDict(const ReferenceLookFit& f) {
    py::dict d;
    d["ok"] = f.ok;
    d["samples"] = f.samples;
    d["appearance"] = arr3(f.appearance);
    d["base_color"] = arr3(f.baseColor);
    d["melanin"] = f.melanin;
    d["redness"] = f.redness;
    d["melanin_error"] = f.melaninError;
    d["value_randomness"] = f.valueRandomness;
    d["root_appearance"] = arr3(f.rootAppearance);
    d["tip_appearance"] = arr3(f.tipAppearance);
    d["white_balance"] = arr3(f.whiteBalance);
    d["exposure"] = f.exposure;
    d["curly"] = f.curly;
    d["curl_share"] = f.curlShare;
    d["curl_amplitude_deg"] = f.curlAmplitudeDeg;
    d["curl_frequency"] = f.curlFrequency;
    d["curl_radius"] = f.curlRadius;
    d["clumped"] = f.clumped;
    d["clump_spacing"] = f.clumpSpacing;
    d["clump_contrast"] = f.clumpContrast;
    d["clump_regularity"] = f.clumpRegularity;
    d["clump_size"] = f.clumpSize;
    d["clump_strength"] = f.clumpStrength;
    d["stray_density"] = f.strayDensity;
    d["cross_share"] = f.crossShare;
    d["flyaway_fraction"] = f.flyawayFraction;
    d["frizz_fraction"] = f.frizzFraction;
    d["strand_spread"] = f.strandSpread;
    d["coherent_spread"] = f.coherentSpread;
    d["melanin_random"] = f.melaninRandom;
    d["clump_share"] = f.clumpShare;
    d["tip_strength"] = f.tipStrength;
    d["tip_color"] = arr3(f.tipColor);
    d["coverage"] = f.coverage;
    return d;
}


using Pixels = std::vector<std::array<float, 2>>;

Pixels pixelsFrom(py::handle points) {
    Pixels out;
    for (py::handle p : points) {
        const auto uv = p.cast<std::vector<float>>();
        if (uv.size() < 2) throw py::value_error("a photo point is (u, v)");
        out.push_back({uv[0], uv[1]});
    }
    return out;
}

py::list pixelsList(const Pixels& points) {
    py::list out;
    for (const auto& p : points) out.append(py::make_tuple(p[0], p[1]));
    return out;
}

// A landmark side: "photo" or "left" (the photo view), "mesh" or "right" (the mesh view), or a MarkerSide.
MarkerSide landmarkSide(py::handle side) {
    if (py::isinstance<py::str>(side)) {
        const std::string s = side.cast<std::string>();
        if (s == "photo" || s == "left" || s == "image") return kLandmarkPhoto;
        if (s == "mesh" || s == "right") return kLandmarkMesh;
        throw py::value_error("landmark side '" + s + "': photo/left or mesh/right");
    }
    return side.cast<MarkerSide>();
}

// A landmark point: a MarkerPoint, (u, v) on the photo side, (x, y, z) on the mesh side.
MarkerPoint landmarkPoint(MarkerSide side, py::handle p) {
    if (py::isinstance<MarkerPoint>(p)) return p.cast<MarkerPoint>();
    const auto values = p.cast<std::vector<float>>();
    if (side == kLandmarkPhoto) {
        if (values.size() < 2) throw py::value_error("a photo landmark is (u, v)");
        return ReferencePhotoEditor::photoPoint(values[0], values[1]);
    }
    if (values.size() < 3) throw py::value_error("a mesh landmark is (x, y, z)");
    MarkerPoint out;
    out.point = Vec3f(values[0], values[1], values[2]);
    return out;
}

py::dict cameraDict(const RefCamera& c) {
    py::dict d;
    std::vector<std::vector<double>> rot(3, std::vector<double>(3));
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) rot[i][j] = c.pose.rotation[i][j];
    d["rotation"] = rot;
    d["position"] = std::vector<double>(c.pose.position.begin(), c.pose.position.end());
    d["focal_px"] = c.focalPx;
    d["width"] = c.width;
    d["height"] = c.height;
    const CameraVec3 e = eulerXYZDegrees(c.pose.rotation);
    d["rotate_xyz"] = std::vector<double>(e.begin(), e.end());
    return d;
}

py::list strokesList(const ReferencePhotoEditor& e) {
    py::list out;
    for (const RefHintStroke& s : e.strokes()) {
        py::dict d;
        d["points"] = pixelsList(s.points);
        d["directed"] = s.directed;
        out.append(d);
    }
    return out;
}

py::list maskStrokesList(const ReferencePhotoEditor& e) {
    py::list out;
    for (const RefMaskStroke& s : e.maskStrokes()) {
        py::dict d;
        d["points"] = pixelsList(s.points);
        d["radius"] = s.radius;
        d["include"] = s.include;
        out.append(d);
    }
    return out;
}

}  // namespace

void bindReference(py::module_& m) {
    m.def(
        "solve_camera",
        [](const std::vector<std::array<double, 2>>& image, const std::vector<std::array<double, 3>>& world, double width,
           double height, double focalPx, bool fixFocal, py::object initial) {
            if (image.size() != world.size()) throw py::value_error("as many image points as world points");
            std::vector<CameraCorrespondence> pairs;
            for (std::size_t i = 0; i < image.size(); ++i) pairs.push_back({image[i][0], image[i][1], world[i]});
            CameraSolveOptions o;
            o.imageWidth = width;
            o.imageHeight = height;
            o.focalPx = focalPx;
            o.fixFocal = fixFocal;
            if (!initial.is_none()) {
                const RefCamera guess = cameraFrom(initial.cast<py::dict>());
                o.hasInitialPose = true;
                o.initialPose = guess.pose;
            }
            const CameraSolveResult r = solveCamera(pairs, o);
            py::dict d;
            d["ok"] = r.ok;
            d["error"] = r.error;
            std::vector<std::vector<double>> rot(3, std::vector<double>(3));
            for (std::size_t i = 0; i < 3; ++i)
                for (std::size_t j = 0; j < 3; ++j) rot[i][j] = r.pose.rotation[i][j];
            d["rotation"] = rot;
            d["position"] = std::vector<double>(r.pose.position.begin(), r.pose.position.end());
            d["focal_px"] = r.focalPx;
            d["width"] = width;
            d["height"] = height;
            d["rms"] = r.rmsPx;
            d["errors"] = r.errorsPx;
            return d;
        },
        py::arg("image_points"), py::arg("world_points"), py::arg("width"), py::arg("height"), py::arg("focal_px") = 0.0,
        py::arg("fix_focal") = false, py::arg("initial") = py::none(),
        "A pinhole camera from 2D-3D point pairs (camera_solve.h; pixels from the top-left): {ok, rotation, position, "
        "focal_px, width, height, rms, errors} - a camera guides_from_reference takes. `initial`: a camera dict to start from.");

    m.def(
        "morph_to_pins",
        [](const std::vector<std::array<double, 3>>& points, const py::list& pins, const py::list& views, double stiffness,
           bool symmetric, int axis) {
            std::vector<PinView> vs;
            for (const py::handle v : views) {
                const RefCamera c = cameraFrom(v.cast<py::dict>());
                PinView pv;
                pv.pose = c.pose;
                pv.focalPx = c.focalPx;
                pv.width = c.width;
                pv.height = c.height;
                vs.push_back(pv);
            }
            std::vector<MorphPin> ps;
            for (const py::handle h : pins) {
                const py::dict d = h.cast<py::dict>();
                MorphPin pin;
                pin.mesh = d["mesh"].cast<std::array<double, 3>>();
                if (d.contains("sights"))
                    for (const py::handle sight : d["sights"].cast<py::list>()) {
                        const auto t = sight.cast<std::tuple<int, double, double>>();
                        pin.sights.push_back({std::get<0>(t), std::get<1>(t), std::get<2>(t)});
                    }
                ps.push_back(pin);
            }
            PinMorphOptions o;
            o.stiffness = stiffness;
            o.symmetric = symmetric;
            o.axis = axis;
            const PinMorphResult r = morphToPins(std::vector<CameraVec3>(points.begin(), points.end()), ps, vs, o);
            py::dict d;
            d["ok"] = r.ok;
            d["error"] = r.error;
            d["points"] = r.points;
            d["targets"] = r.targets;
            d["rms_before"] = r.rmsBeforePx;
            d["rms_after"] = r.rmsAfterPx;
            return d;
        },
        py::arg("points"), py::arg("pins"), py::arg("views"), py::arg("stiffness") = 0.0,
        py::arg("symmetric") = true, py::arg("axis") = 0,
        "The mesh's points morphed so each pin lands on its photo points (pin_morph.h): pins [{mesh: [x, y, z], "
        "sights: [(view, u, v)]}] (pixels from the top-left), views: camera dicts as solve_camera's (or matrix + "
        "focal_mm + aperture_in). {ok, error, points, targets, rms_before, rms_after}.");

    m.def("density_of_coverage", &densityOfCoverage, py::arg("coverage"),
          "A Scatter density factor (0-1) from the share of the skin hair covers (guides_from_reference's "
          "scalp_coverage).");

    m.def("scalp_looks_symmetric", &scalpLooksSymmetric, py::arg("scalp"),
          "Whether the scalp's topology mirrors (most vertices find a partner): reference guides' auto symmetry.");

    m.def(
        "look_measures",
        [](const std::string& image, py::object mask, int maxSize, py::object picture) {
            RefImage rgb = loadRefImage(image, 3);
            RefImage msk;
            if (!mask.is_none()) msk = loadRefImage(mask.cast<std::string>(), 1);
            if (maxSize > 0 && std::max(rgb.width, rgb.height) > maxSize) {
                const double s = static_cast<double>(maxSize) / std::max(rgb.width, rgb.height);
                rgb = rgb.resized(static_cast<int>(std::lround(rgb.width * s)), static_cast<int>(std::lround(rgb.height * s)));
            }
            if (!msk.empty()) msk = msk.resized(rgb.width, rgb.height);
            ClumpEstimate c;
            StrayEstimate s;
            std::vector<std::uint8_t> pic;
            {
                py::gil_scoped_release release;
                const OrientationField fine = orientationField(rgb.gray(), msk);
                const OrientationField coarse = coarseField(fine, msk, 0.02f * static_cast<float>(std::max(rgb.width, rgb.height)));
                RefImage lum = rgb.gray();
                for (std::size_t i = 0; i < lum.pixels.size(); ++i) {
                    float l = 0.0f;
                    const float k[3] = {0.2126f, 0.7152f, 0.0722f};
                    for (int ch = 0; ch < 3; ++ch) {
                        const float v = rgb.pixels[3 * i + static_cast<std::size_t>(ch)];
                        l += k[ch] * (v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f));
                    }
                    lum.pixels[i] = l;
                }
                c = measureClumps(lum, coarse, msk);
                s = measureStrays(rgb.gray(), coarse, msk, {}, picture.is_none() ? nullptr : &pic);
            }
            if (!picture.is_none()) writeCompressedPng(picture.cast<std::string>(), rgb.width, rgb.height, 3, pic);
            py::dict clumps, strays, d;
            clumps["profiles"] = c.profiles;
            clumps["periodic_share"] = c.periodicShare;
            clumps["spacing_px"] = c.spacingPx;
            clumps["contrast"] = c.contrast;
            clumps["strand_spread"] = c.strandSpread;
            clumps["clump_peak"] = c.clumpPeak;
            clumps["coherent_spread"] = c.coherentSpread;
            strays["outline_px"] = s.outlinePx;
            strays["stray_px"] = s.strayPx;
            strays["stray_density"] = s.strayDensity;
            strays["ridge_share"] = s.ridgeShare;
            strays["cross_share"] = s.crossShare;
            strays["contrast"] = s.contrast;
            d["clumps"] = clumps;
            d["strays"] = strays;
            d["width"] = rgb.width;
            d["height"] = rgb.height;
            return d;
        },
        py::arg("image"), py::arg("mask") = py::none(), py::arg("max_size") = 768, py::arg("picture") = py::none(),
        "The measured look's image measures on one photo (diagnostics: measureClumps, measureStrays): {clumps, strays}; "
        "`picture`: a PNG of the strays (the outline white, strays red, ridges inside green, crossing yellow).");

    m.def(
        "orientation_field",
        [](const std::string& image, py::object mask, int maxSize, int orientations, float wavelength, int scales) {
            RefImage img = loadRefImage(image, 1);
            RefImage msk;
            if (!mask.is_none()) msk = loadRefImage(mask.cast<std::string>(), 1);
            if (maxSize > 0 && std::max(img.width, img.height) > maxSize) {
                const double s = static_cast<double>(maxSize) / std::max(img.width, img.height);
                img = img.resized(static_cast<int>(std::lround(img.width * s)), static_cast<int>(std::lround(img.height * s)));
            }
            OrientationOptions o;
            o.orientations = orientations;
            o.wavelength = wavelength;
            o.scales = scales;
            OrientationField f;
            {
                py::gil_scoped_release release;
                f = orientationField(img, msk, o);
            }
            // The smoothed field: its angle and its strength per pixel, float32 rows top-down.
            std::vector<float> angle(f.angle.size()), strength(f.angle.size());
            for (std::size_t i = 0; i < angle.size(); ++i) {
                const float a = f.tensor[2 * i], b = f.tensor[2 * i + 1];
                angle[i] = std::fmod(0.5f * std::atan2(b, a) + 3.14159265f, 3.14159265f);
                strength[i] = std::sqrt(a * a + b * b);
            }
            py::dict d;
            d["width"] = f.width;
            d["height"] = f.height;
            d["angle"] = py::bytes(reinterpret_cast<const char*>(angle.data()), angle.size() * sizeof(float));
            d["confidence"] = py::bytes(reinterpret_cast<const char*>(strength.data()), strength.size() * sizeof(float));
            return d;
        },
        py::arg("image"), py::arg("mask") = py::none(), py::arg("max_size") = 768, py::arg("orientations") = 32,
        py::arg("wavelength") = 4.0f, py::arg("scales") = 3,
        "An image's orientation field (smoothed): {width, height, angle, confidence}, the last two float32 bytes "
        "(radians from +u toward +v in [0, pi); 0-1), rows top-down.");

    m.def(
        "orientation_field_image",
        [](const std::string& image, py::object mask, const std::string& out, int maxSize, int orientations, float wavelength,
           int scales, bool smoothed) {
            RefImage img = loadRefImage(image, 1);
            RefImage msk;
            if (!mask.is_none()) msk = loadRefImage(mask.cast<std::string>(), 1);
            if (maxSize > 0 && std::max(img.width, img.height) > maxSize) {
                const double s = static_cast<double>(maxSize) / std::max(img.width, img.height);
                img = img.resized(static_cast<int>(std::lround(img.width * s)), static_cast<int>(std::lround(img.height * s)));
            }
            OrientationOptions o;
            o.orientations = orientations;
            o.wavelength = wavelength;
            o.scales = scales;
            OrientationField f;
            {
                py::gil_scoped_release release;
                f = orientationField(img, msk, o);
            }
            writeCompressedPng(out, f.width, f.height, 3, f.colors(smoothed));
            return py::make_tuple(f.width, f.height);
        },
        py::arg("image"), py::arg("mask") = py::none(), py::arg("out"), py::arg("max_size") = 768, py::arg("orientations") = 32,
        py::arg("wavelength") = 4.0f, py::arg("scales") = 3, py::arg("smoothed") = true,
        "Writes a picture of an image's orientation field (hue: direction, brightness: confidence). Returns (width, height).");

    m.def(
        "guides_from_reference",
        [](const ScalpMesh& scalp, const py::list& views, int guideCount, py::object roots, float maxLength, float minLength,
           float step, int points, float scalpOffset, const std::string& symmetry, float symmetryCenter, int maxImageSize,
           py::object occluders, int shellResolution, int inflate, float inflateFactor, float surfacePrior,
           float continuityPrior, float gravityPrior, float orientationWeight, float downBias, float hintRadius, float hintLineRadius, float hintWeight, int refineMask, bool cropToSubject, int subjectSize, int curlFlow, float lengthTrim, py::object up, std::uint64_t seed, bool keepFields, const std::string& lookBalance, bool lookExposure, float lookQuantile, int orientations, float wavelength,
           int scales, py::object fieldsDir) {
            std::vector<ReferenceView> vs;
            for (py::handle v : views) vs.push_back(viewFrom(v.cast<py::dict>()));
            ReferenceGuideOptions o;
            o.guideCount = guideCount;
            if (!roots.is_none())
                for (py::handle r : roots) o.roots.push_back(r.cast<StrandRoot>());
            o.maxLength = maxLength;
            o.minLength = minLength;
            o.step = step;
            o.points = points;
            o.scalpOffset = scalpOffset;
            o.symmetry = referenceSymmetryFromName(symmetry);
            o.symmetryCenter = symmetryCenter;
            o.maxImageSize = maxImageSize;
            if (!occluders.is_none())
                for (py::handle h : occluders) o.occluders.push_back(h.cast<std::shared_ptr<ScalpMesh>>());
            o.shellResolution = shellResolution;
            o.inflate = inflate;
            o.inflateFactor = inflateFactor;
            o.surfacePrior = surfacePrior;
            o.continuityPrior = continuityPrior;
            o.downBias = downBias;
            o.hintRadius = hintRadius;
            o.refineMask = refineMask;
            o.cropToSubject = cropToSubject;
            o.subjectSize = subjectSize;
            o.curlFlow = curlFlow;
            o.lengthTrim = lengthTrim;
            o.hintLineRadius = hintLineRadius;
            o.hintWeight = hintWeight;
            o.gravityPrior = gravityPrior;
            o.orientationWeight = orientationWeight;
            if (!up.is_none()) loadVec3(up, o.up);
            o.seed = seed;
            o.keepFields = keepFields || !fieldsDir.is_none();
            if (lookBalance == "none") o.lookBalance = LookBalance::None;
            else if (lookBalance == "highlights") o.lookBalance = LookBalance::Highlights;
            else if (lookBalance == "gray") o.lookBalance = LookBalance::Gray;
            else if (lookBalance == "auto") o.lookBalance = LookBalance::Auto;
            else throw py::value_error("look_balance is auto, none, gray or highlights");
            o.lookQuantile = std::clamp(lookQuantile, 0.0f, 1.0f);
            o.lookExposure = lookExposure;
            o.orientation.orientations = orientations;
            o.orientation.wavelength = wavelength;
            o.orientation.scales = scales;
            ReferenceGuidesResult r;
            {
                py::gil_scoped_release release;
                r = guidesFromReference(scalp, vs, o);
            }
            py::dict out;
            py::list guides;
            for (const ReferenceGuide& g : r.guides) {
                py::dict d;
                py::list pts;
                for (const Vec3f& p : g.points) pts.append(py::make_tuple(p.x, p.y, p.z));
                d["points"] = pts;
                d["root"] = g.root;
                d["mirror_of"] = g.mirrorOf;
                d["confidence"] = g.confidence;
                d["stop"] = g.stop;
                guides.append(d);
            }
            out["guides"] = guides;
            out["symmetry"] = referenceSymmetryName(r.symmetry);
            out["roots_tried"] = r.rootsTried;
            out["roots_unseen"] = r.rootsUnseen;
            out["roots_bald"] = r.rootsBald;
            out["roots_short"] = r.rootsShort;
            out["step"] = r.step;
            out["max_length"] = r.maxLength;
            out["scalp_offset"] = r.scalpOffset;
            out["shell_fill"] = r.shellFill;
            out["rim_length"] = r.rimLength;
            out["reach_length"] = r.reachLength;
            out["look"] = lookDict(r.look);
            out["scalp_coverage"] = r.scalpCoverage;
            out["seconds"] = r.seconds;
            if (!fieldsDir.is_none()) {
                const std::string dir = fieldsDir.cast<std::string>();
                py::list files;
                for (std::size_t i = 0; i < r.fields.size(); ++i) {
                    const OrientationField& f = r.fields[i];
                    const std::string path = dir + "/field_" + std::to_string(i) + ".png";
                    writeCompressedPng(path, f.width, f.height, 3, f.colors(true));
                    const RefImage& mk = r.masks[i];
                    std::vector<std::uint8_t> gray(mk.pixels.size());
                    for (std::size_t k = 0; k < gray.size(); ++k) gray[k] = static_cast<std::uint8_t>(std::lround(255.0f * std::clamp(mk.pixels[k], 0.0f, 1.0f)));
                    const std::string maskPath = dir + "/mask_" + std::to_string(i) + ".png";
                    writeCompressedPng(maskPath, mk.width, mk.height, 1, gray);
                    files.append(py::make_tuple(path, maskPath, py::make_tuple(r.crops[i][0], r.crops[i][1], r.crops[i][2], r.crops[i][3])));
                }
                out["field_files"] = files;
                // The part of each photo the pictures cover (photo pixels), for the scripts that lay them over it.
                std::ofstream crops(dir + "/crops.json");
                crops << "[";
                for (std::size_t i = 0; i < r.crops.size(); ++i)
                    crops << (i ? ", " : "") << "[" << r.crops[i][0] << ", " << r.crops[i][1] << ", " << r.crops[i][2] << ", "
                          << r.crops[i][3] << "]";
                crops << "]\n";
            }
            return out;
        },
        py::arg("scalp"), py::arg("views"), py::arg("guide_count") = 300, py::arg("roots") = py::none(), py::arg("max_length") = 0.0f,
        py::arg("min_length") = 0.0f, py::arg("step") = 0.0f, py::arg("points") = 16, py::arg("scalp_offset") = 0.0f,
        py::arg("symmetry") = "auto", py::arg("symmetry_center") = 0.0f, py::arg("max_image_size") = 768,
        py::arg("occluders") = py::none(), py::arg("shell_resolution") = 96, py::arg("inflate") = -1, py::arg("inflate_factor") = 1.0f,
        py::arg("surface_prior") = 0.15f, py::arg("continuity_prior") = 0.1f, py::arg("gravity_prior") = 0.15f, py::arg("orientation_weight") = 1.0f, py::arg("down_bias") = 1.0f, py::arg("hint_radius") = 0.08f, py::arg("hint_line_radius") = 0.025f, py::arg("hint_weight") = 3.0f, py::arg("refine_mask") = -1, py::arg("crop_to_subject") = true, py::arg("subject_size") = 512, py::arg("curl_flow") = -1, py::arg("length_trim") = 0.86f, py::arg("up") = py::none(), py::arg("seed") = 1,
        py::arg("keep_fields") = false, py::arg("look_balance") = "auto", py::arg("look_exposure") = true, py::arg("look_quantile") = 0.65f, py::arg("orientations") = 32, py::arg("wavelength") = 4.0f, py::arg("scales") = 3,
        py::arg("fields_dir") = py::none(),
        "Guides grown from reference photos (a prototype). `views`: dicts {image: path, mask: path or None, hints: "
        "[[[u, v], ...] root first, ...] or [{points, directed}], hint_image: an RGBA overlay of painted strokes (dark at "
        "the root end, light at the tip for a way), camera: "
        "{rotation (3x3, camera axes as columns) and position, or matrix (Maya worldMatrix, 16 numbers); focal_px, or "
        "focal_mm and aperture_in; width, height}}. Returns {guides: [{points, root, mirror_of, confidence, stop}], "
        "symmetry, roots_tried, roots_unseen, roots_bald, roots_short, step, max_length, look: {...}, seconds}.");

    // ---- The Reference window's model (reference_edit.h) ----
    py::class_<ReferencePhotoEditor>(m, "ReferencePhotoEditor",
                                     "One photo's landmarks (photo/left and mesh/right lists paired by index, as the "
                                     "Transfer window's markers), flow arrows and mask touch-ups, with an undo stack.")
        .def(py::init<>())
        .def("reset_json", [](ReferencePhotoEditor& e, const std::string& text) {
                 try {
                     e.reset(referencePhotoStateFromJson(text));
                 } catch (const std::invalid_argument& err) {
                     throw py::value_error(err.what());
                 }
             }, py::arg("text"), "The state from JSON (state_json's form); no history.")
        .def("state_json", [](const ReferencePhotoEditor& e) { return referencePhotoStateToJson(e.state()); },
             "{landmarks: [{image: [u, v] or None, mesh: [x, y, z] or None}], strokes: [{points, directed}], "
             "mask_strokes: [{points, radius, include}]} as JSON text.")
        .def("points", [](const ReferencePhotoEditor& e, py::handle side) { return e.landmarks().points(landmarkSide(side)); },
             py::arg("side"), "A side's landmark points in order (MarkerPoint; a photo point's point is (u, v, 0)).")
        .def("count", [](const ReferencePhotoEditor& e, py::handle side) { return e.landmarks().count(landmarkSide(side)); },
             py::arg("side"))
        .def("matched", [](const ReferencePhotoEditor& e, py::handle side, std::size_t i) {
                 return e.landmarks().matched(landmarkSide(side), i);
             }, py::arg("side"), py::arg("index"))
        .def_property_readonly("pair_count", [](const ReferencePhotoEditor& e) { return e.landmarks().pairCount(); })
        .def_property_readonly("pending_count", [](const ReferencePhotoEditor& e) { return e.landmarks().pendingCount(); })
        .def("summary", [](const ReferencePhotoEditor& e, const std::string& none) { return e.landmarks().summary(none.c_str()); },
             py::arg("none") = "No landmarks")
        .def("add_landmark", [](ReferencePhotoEditor& e, py::handle side, py::handle p) {
                 const MarkerSide s = landmarkSide(side);
                 return e.addLandmark(s, landmarkPoint(s, p));
             }, py::arg("side"), py::arg("point"), "A landmark at the end of a side's list ((u, v) on the photo, (x, y, z) "
             "on the mesh); returns its index. One undo step.")
        .def("remove_landmark", [](ReferencePhotoEditor& e, py::handle side, std::size_t i) {
                 try {
                     return e.removeLandmark(landmarkSide(side), i);
                 } catch (const std::out_of_range& err) {
                     throw py::index_error(err.what());
                 }
             }, py::arg("side"), py::arg("index"), "Deletes a landmark and its partner; returns the indices gone.")
        .def("move_landmark", [](ReferencePhotoEditor& e, py::handle side, std::size_t i, py::handle p, bool step) {
                 const MarkerSide s = landmarkSide(side);
                 try {
                     e.moveLandmark(s, i, landmarkPoint(s, p), step);
                 } catch (const std::out_of_range& err) {
                     throw py::index_error(err.what());
                 }
             }, py::arg("side"), py::arg("index"), py::arg("point"), py::arg("new_step") = true,
             "A landmark moved (new_step False: joins the last step, a drag's later moves).")
        .def("clear_landmarks", &ReferencePhotoEditor::clearLandmarks)
        .def("add_stroke", [](ReferencePhotoEditor& e, py::handle points, bool directed) {
                 try {
                     return e.addStroke(pixelsFrom(points), directed);
                 } catch (const std::invalid_argument& err) {
                     throw py::value_error(err.what());
                 }
             }, py::arg("points"), py::arg("directed") = true, "A flow arrow (photo pixels, root first); its index.")
        .def("erase_strokes", [](ReferencePhotoEditor& e, py::handle path, float radius) {
                 return e.eraseStrokes(pixelsFrom(path), radius);
             }, py::arg("path"), py::arg("radius"), "Removes the flow arrows within radius of path; how many went.")
        .def("clear_strokes", &ReferencePhotoEditor::clearStrokes)
        .def_property_readonly("strokes", &strokesList)
        .def("add_mask_stroke", [](ReferencePhotoEditor& e, py::handle points, float radius, bool include) {
                 try {
                     return e.addMaskStroke(pixelsFrom(points), radius, include);
                 } catch (const std::invalid_argument& err) {
                     throw py::value_error(err.what());
                 }
             }, py::arg("points"), py::arg("radius"), py::arg("include") = true)
        .def("clear_mask_strokes", &ReferencePhotoEditor::clearMaskStrokes)
        .def_property_readonly("mask_strokes", &maskStrokesList)
        .def("undo", &ReferencePhotoEditor::undo)
        .def("redo", &ReferencePhotoEditor::redo)
        .def_property_readonly("can_undo", &ReferencePhotoEditor::canUndo)
        .def_property_readonly("can_redo", &ReferencePhotoEditor::canRedo)
        .def_property_readonly("undo_text", &ReferencePhotoEditor::undoText)
        .def_property_readonly("redo_text", &ReferencePhotoEditor::redoText)
        .def_property_readonly("undo_depth", &ReferencePhotoEditor::undoDepth)
        .def_property_readonly("revision", &ReferencePhotoEditor::revision);

    m.def(
        "solve_reference_landmarks",
        [](const ReferencePhotoEditor& e, double width, double height, double focalPx, bool keepFocal, py::object initial,
           bool findBad) {
            CameraPose pose;
            const CameraPose* start = nullptr;
            if (!initial.is_none()) {
                pose = cameraFrom(initial.cast<py::dict>()).pose;
                start = &pose;
            }
            ReferenceLandmarkSolve r;
            {
                py::gil_scoped_release release;
                r = solveReferenceLandmarks(e.landmarks(), width, height, focalPx, keepFocal, start, findBad);
            }
            py::dict d = r.ok ? cameraDict(r.camera) : py::dict();
            d["ok"] = r.ok;
            d["why"] = r.why;
            d["pairs"] = r.pairs;
            d["focal_solved"] = r.focalSolved;
            d["rms"] = r.rms;
            d["errors"] = r.errors;
            py::list projected;
            for (const auto& p : r.projected) projected.append(py::make_tuple(p[0], p[1]));
            d["projected"] = projected;
            d["bad"] = std::vector<bool>(r.bad.begin(), r.bad.end());
            return d;
        },
        py::arg("editor"), py::arg("width"), py::arg("height"), py::arg("focal_px"), py::arg("keep_focal") = false,
        py::arg("initial") = py::none(), py::arg("find_bad") = true,
        "The camera from the editor's complete landmark pairs (4 with the focal length kept, 6 to solve it): {ok, why, "
        "pairs, focal_solved, rotation, position, focal_px, rotate_xyz (Maya xyz degrees), width, height, rms, errors, "
        "projected (each pair's mesh point in pixels), bad (find_bad: the pairs that disagree with the rest)}.");

    m.def(
        "reference_mesh_overlay",
        [](const ScalpMesh& mesh, const py::dict& camera, bool silhouette) {
            const RefCamera c = cameraFrom(camera);
            std::vector<std::array<float, 4>> lines;
            {
                py::gil_scoped_release release;
                lines = referenceMeshOverlay(mesh, c, silhouette);
            }
            py::list out;
            for (const auto& l : lines) out.append(py::make_tuple(l[0], l[1], l[2], l[3]));
            return out;
        },
        py::arg("mesh"), py::arg("camera"), py::arg("silhouette") = true,
        "The mesh over the photo through a camera (a camera dict): segments (x0, y0, x1, y1) in the photo's pixels, its "
        "outline or (silhouette False) the wireframe of its faces toward the camera.");

    m.def(
        "reference_arrow",
        [](py::handle points, float headSize) {
            const ReferenceArrow a = referenceArrow(pixelsFrom(points), headSize);
            py::dict d;
            d["line"] = pixelsList(a.line);
            if (a.hasHead) d["head"] = pixelsList(Pixels(a.head.begin(), a.head.end()));
            else d["head"] = py::none();
            return d;
        },
        py::arg("points"), py::arg("head_size"),
        "A flow arrow as the windows draw it: {line: the stroke smoothed, head: (side, tip, side) or None}.");

    m.def(
        "preview_reference_view",
        [](const ScalpMesh& scalp, const py::dict& view, float lineSpacing, float maxLength, int maxImageSize, int subjectSize,
           bool cropToSubject, int refineMask) {
            const ReferenceView v = viewFrom(view);
            ReferenceGuideOptions o;
            o.maxLength = maxLength;
            o.maxImageSize = maxImageSize;
            o.subjectSize = subjectSize;
            o.cropToSubject = cropToSubject;
            o.refineMask = refineMask;
            ReferenceViewPreview p;
            {
                py::gil_scoped_release release;
                p = previewReferenceView(scalp, v, o, lineSpacing);
            }
            std::string mask(p.mask.pixels.size(), '\0');
            for (std::size_t i = 0; i < mask.size(); ++i)
                mask[i] = static_cast<char>(static_cast<unsigned char>(std::lround(255.0f * std::clamp(p.mask.pixels[i], 0.0f, 1.0f))));
            py::dict d;
            d["mask"] = py::bytes(mask);
            d["width"] = p.mask.width;
            d["height"] = p.mask.height;
            d["crop"] = py::make_tuple(p.crop[0], p.crop[1], p.crop[2], p.crop[3]);
            py::list lines;
            for (const auto& l : p.flowLines) lines.append(py::make_tuple(l[0], l[1], l[2], l[3]));
            d["lines"] = lines;
            return d;
        },
        py::arg("scalp"), py::arg("view"), py::arg("line_spacing") = 0.0f, py::arg("max_length") = 0.0f,
        py::arg("max_image_size") = 768, py::arg("subject_size") = 512, py::arg("crop_to_subject") = true,
        py::arg("refine_mask") = -1,
        "What a Reference window shows over one photo (a view dict as guides_from_reference takes, mask_strokes too): "
        "{mask: bytes (uint8, width x height, rows top-down), width, height, crop (x0, y0, w, h photo pixels the mask "
        "covers), lines: the orientation field as segments (x0, y0, x1, y1) in photo pixels}.");
}

}  // namespace groomlab::py_bind
