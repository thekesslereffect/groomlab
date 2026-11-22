# SPDX-License-Identifier: Apache-2.0
"""Renders a groom in V-Ray Standalone from GroomLab's .vrscene, the scene of arnold_compare.py, for a
side-by-side with Arnold (and the calibration of vrscene_writer.h's vray:: mapping).

    python tests/data/app/vray_compare.py --groom head.groom --scalp head_scalp.obj \
        --camera "eye x y z target x y z fov 40" --out vray.png --cli groomlab-cli.exe [--vray vray.exe]

groomlab-cli writes the groom's .vrscene (the hair, its BRDFHair4 shader: `groomlab-cli eval
head.groom -o groom.vrscene`) beside the PNG, and a scene that #includes it: the same camera,
lights (the app's "studio" preset: the key, fill and rim as directional lights, the studio
environment as a dome from a lat-long .hdr written here; or the key alone, the environment alone,
one directional light) and scalp (the OBJ as a GeomStaticMesh, the viewport's scalp color; any
OBJ: engine/assets/meshes/nika_head.obj too) as arnold_compare.py, in V-Ray's linear sRGB (its
default rendering space: arnold_compare.py's --linear-srgb). The PNG is sRGB without a tone map.
Measured with vray: LightDirect's intensity times pi is the irradiance it gives (Arnold's distant
light's intensity is the irradiance, so each is divided by pi), it shines down its -Z as Arnold's;
LightDome's lat-long "spherical" mapping is Z-up (uvw_matrix turns it to Arnold's Y-up skydome);
GeomMayaHair's widths are radii. Standard library only.
"""

import argparse
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from arnold_compare import light_direction, normalized, cross, read_obj, studio_radiance, write_hdr  # noqa: E402

VRAY = r"C:\Program Files\Chaos\V-Ray\Standalone for x64\bin\vray.exe"


def scene_path(path):
    return os.path.abspath(path).replace("\\", "/")


def vector(v):
    return "Vector(%.6f, %.6f, %.6f)" % tuple(v)


def transform(x, y, z, origin):
    """A V-Ray Transform: Matrix of the images of the axes (columns), then the translation."""
    return "Transform(Matrix(%s, %s, %s), %s)" % (vector(x), vector(y), vector(z), vector(origin))


def mesh_plugin(name, path, color):
    """An OBJ as GeomStaticMesh (polygons fanned into triangles, smooth normals), a diffuse
    material and a node."""
    verts, faces = read_obj(path)
    tris = []
    for f in faces:
        for k in range(1, len(f) - 1):
            tris += [f[0], f[k], f[k + 1]]
    return ("GeomStaticMesh %s@mesh {\n  vertices=ListVector(%s);\n  faces=ListInt(%s);\n  compute_normals=1;\n"
            "  smooth_angle=180;\n}\n\n" % (name, ", ".join(vector(v) for v in verts), ", ".join(str(i) for i in tris)) +
            "BRDFDiffuse %s@brdf {\n  color=Color(%f, %f, %f);\n}\n\nMtlSingleBRDF %s@mtl {\n  brdf=%s@brdf;\n}\n\n"
            % ((name,) + tuple(color) + (name, name)) +
            "Node %s@node {\n  transform=%s;\n  geometry=%s@mesh;\n  material=%s@mtl;\n}\n\n"
            % (name, transform([1, 0, 0], [0, 1, 0], [0, 0, 1], [0, 0, 0]), name, name))


def write_scene(path, groom_vrscene, args, eye, target, fov_vertical, width, height):
    out_dir = os.path.dirname(path)
    s = ['// GroomLab vray_compare.py: arnold_compare.py\'s scene around a GroomLab .vrscene\n',
         '#include "%s"\n\n' % scene_path(groom_vrscene)]
    s.append("SettingsOutput output {\n  img_width=%d;\n  img_height=%d;\n}\n\n" % (width, height))
    # Brute-force GI (hair is lit through many hairs: BRDFHair4's own bounces, trace_depth, do most of it).
    s.append("SettingsGI gi {\n  on=1;\n  primary_engine=2;\n  secondary_engine=2;\n}\n\n")
    s.append("SettingsDMCGI dmcgi {\n  depth=%d;\n}\n\n" % args.depth)
    s.append("SettingsImageSampler sampler {\n  type=1;\n  dmc_minSubdivs=1;\n  dmc_maxSubdivs=%d;\n"
             "  dmc_threshold=%g;\n}\n\n" % (args.samples, args.threshold))
    s.append("SettingsOptions options {\n  light_doDefaultLights=0;\n}\n\n")
    s.append("FilterGaussian filter {\n  size=1.5;\n}\n\n")
    if args.motion_blur:  # the frame to the next: the groom's keys between them
        s.append("SettingsMotionBlur motion_blur {\n  on=1;\n  duration=1;\n  interval_center=0.5;\n  geom_samples=4;\n}\n\n")
    # The camera: V-Ray's looks down its -Z, Y up; fov is the horizontal one, in radians.
    f = normalized([t - e for t, e in zip(target, eye)])
    r = normalized(cross(f, [0.0, 1.0, 0.0]))
    u = cross(r, f)
    fov_h = 2.0 * math.atan(math.tan(math.radians(fov_vertical) * 0.5) * width / height)
    s.append("RenderView camera {\n  transform=%s;\n  fov=%f;\n  clipping=0;\n}\n\n"
             % (transform(r, u, [-c for c in f], eye), fov_h))

    lights = [] if args.lights == "env" else [("key", -35.0, 35.0, (1.0, 0.97, 0.92), 2.0)]
    if args.lights == "distant":
        d = [float(v) for v in args.distant.split()]
        lights = [("distant", [-d[0], -d[1], -d[2]], (1.0, 1.0, 1.0), d[3])]
    if args.lights == "studio":
        lights += [("fill", 55.0, 10.0, (0.85, 0.9, 1.0), 0.5), ("rim", 160.0, 30.0, (1.0, 1.0, 1.0), 1.5)]
    for light in lights:
        name, color, intensity = light[0], light[-2], light[-1]
        toward = light[1] if len(light) == 4 else light_direction(light[1], light[2])
        z = normalized(toward)  # LightDirect shines down its -Z: Z toward the light
        up = [1.0, 0.0, 0.0] if abs(z[1]) > 0.9 else [0.0, 1.0, 0.0]
        x = normalized(cross(up, z))
        y = cross(z, x)
        s.append("LightDirect %s {\n  transform=%s;\n  color=AColor(%f, %f, %f, 1);\n  intensity=%f;\n}\n\n"
                 % (name, transform(x, y, z, [0, 0, 0]), color[0], color[1], color[2], intensity / math.pi))
    if args.lights in ("studio", "env"):
        hdr = os.path.join(out_dir, "studio_env.hdr")
        write_hdr(hdr, 256, 128, studio_radiance)
        s.append('BitmapBuffer env_bitmap {\n  file="%s";\n  transfer_function=0;\n  filter_type=0;\n}\n\n' % scene_path(hdr))
        s.append("UVWGenEnvironment env_uvw {\n  mapping_type=\"spherical\";\n"
                 "  uvw_matrix=Matrix(Vector(0, 1, 0), Vector(0, 0, 1), Vector(1, 0, 0));\n}\n\n")
        s.append("TexBitmap env_tex {\n  bitmap=env_bitmap;\n  uvwgen=env_uvw;\n}\n\n")
        s.append("LightDome sky {\n  use_dome_tex=1;\n  dome_tex=env_tex;\n  intensity=0.4;\n  dome_spherical=1;\n"
                 "  invisible=1;\n}\n\n")
    if args.scalp and not args.hide_scalp:
        s.append(mesh_plugin("scalp", args.scalp, (0.36, 0.29, 0.25)))
    with open(path, "w") as fh:
        fh.write("".join(s))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--groom", required=True, help="a .groom file (written as .vrscene by --cli), or a .vrscene")
    p.add_argument("--scalp", default="")
    p.add_argument("--camera", required=True, help='"eye x y z target x y z fov f" as the app prints it')
    p.add_argument("--out", required=True, help="the PNG")
    p.add_argument("--size", default="1280x720")
    p.add_argument("--vray", default=VRAY)
    p.add_argument("--cli", default="", help="groomlab-cli, to write the groom's .vrscene")
    p.add_argument("--frame", type=float, default=1.0)
    p.add_argument("--motion-blur", action="store_true",
                   help="blur the frame to the next (--cli writes both frames' keys)")
    p.add_argument("--density", type=float, default=1.0)
    p.add_argument("--samples", type=int, default=8, help="the adaptive sampler's max subdivs (samples: their square)")
    p.add_argument("--threshold", type=float, default=0.01)
    p.add_argument("--depth", type=int, default=8, help="brute-force GI's depth")
    p.add_argument("--hide-scalp", action="store_true")
    p.add_argument("--lights", default="studio", choices=["studio", "key", "env", "distant"])
    p.add_argument("--distant", default="0 0 -1 3.14159265", help='"dx dy dz intensity" (arnold_compare.py\'s)')
    args = p.parse_args()

    words = args.camera.split()
    eye = [float(w) for w in words[2:5]]
    target = [float(w) for w in words[6:9]]
    fov_vertical = float(words[10])
    width, height = (int(v) for v in args.size.split("x"))
    out = os.path.abspath(args.out)
    out_dir = os.path.dirname(out) or "."
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.splitext(out)[0]

    groom = args.groom
    if not groom.endswith(".vrscene"):
        if not args.cli:
            sys.exit("--cli is needed to write a .groom file's .vrscene")
        groom = stem + "_groom.vrscene"
        frames = "%g-%g" % (args.frame, args.frame + 1) if args.motion_blur else "%g" % args.frame
        code = subprocess.call([args.cli, "eval", args.groom, "-o", groom, "-f", frames, "--density", "%g" % args.density])
        if code:
            sys.exit(code)
    scene = stem + "_scene.vrscene"
    write_scene(scene, groom, args, eye, target, fov_vertical, width, height)
    cmd = [args.vray, "-sceneFile=" + scene, "-imgFile=" + out, "-display=0", "-autoClose=1",
           "-frames=%g" % args.frame, "-verboseLevel=2"]
    print(" ".join(cmd))
    result = subprocess.run(cmd, capture_output=True, text=True)
    issues = [line for line in result.stdout.splitlines() if "error" in line.lower() or "warning" in line.lower()]
    for line in issues[:20]:
        print(line)
    if result.returncode == 0 and not os.path.isfile(out):
        print("vray wrote no image")
        sys.exit(1)
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
