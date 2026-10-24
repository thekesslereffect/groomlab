// SPDX-License-Identifier: Apache-2.0
/*
 * The landing page's 3D viewport: a scalp mesh and its strands, drawn with three.js. Data-driven:
 * it takes StrandData and a RootMesh from whatever makes them (the GroomLab engine as WebAssembly)
 * and only draws. It is lit as Maya's viewport lights a groom (a key at the camera, a faint rim)
 * and shown through ACES as Maya's default view. Strands are GL lines shaded with
 * GroomLab's own hair shading, the app's and Maya's viewport's (engine/gpu/shaders/hair_bsdf.glsl:
 * Marschner's lobes, dual scattering, calibrated against Arnold), the groom's highlight from the
 * engine (setLook), the hair volume's depth standing in for Maya's density grid; the head is clay. A slow turntable, drag to
 * orbit (touch drags turn it sideways only, so the page still scrolls), and it only renders while
 * on screen.
 */
import {
  ACESFilmicToneMapping,
  BufferAttribute,
  BufferGeometry,
  Color,
  DirectionalLight,
  DynamicDrawUsage,
  Group,
  HemisphereLight,
  LineBasicMaterial,
  LineSegments,
  Mesh,
  MeshBasicMaterial,
  MeshLambertMaterial,
  PerspectiveCamera,
  Points,
  Scene,
  ShaderMaterial,
  SRGBColorSpace,
  Vector2,
  Vector3,
  Vector4,
  Data3DTexture,
  DataUtils,
  HalfFloatType,
  LinearFilter,
  Matrix4,
  RedFormat,
  WebGLRenderer,
} from 'three';
import { HAIR_BSDF, LINE_COVERAGE } from './hair-bsdf';
import type { HairLook, RootMesh, StrandData } from './strand-data';

export interface ViewerOptions {
  /** prefers-reduced-motion: no turntable, no sway, no grow; one frame per change. */
  still: boolean;
  /** Turntable speed, radians a second. */
  spin?: number;
  /** Tip sway, 0 to 1. */
  sway?: number;
  /** Camera elevation, radians. */
  pitch?: number;
  /** How much of the view the groom fills (1 = its bounding sphere touches the edges). */
  fill?: number;
  /** Where the turntable starts, radians (0 faces the camera). */
  yaw?: number;
  /** Called before each frame with seconds since start, for a source that changes over time. */
  onFrame?: (time: number) => void;
}

export interface StrandViewer {
  setScalp(mesh: RootMesh): void;
  setStrands(data: StrandData): void;
  /** The hair's highlight (the engine's: its HairColor's specular, roughness and tilt). */
  setLook(look: HairLook): void;
  /** New colors for the strands already set (same layout), without touching their shape. */
  setStrandColors(colors: Float32Array): void;
  setGuides(data: StrandData | null): void;
  /** Frames the camera on this sphere (else on the scalp's bounds). */
  setFraming(center: [number, number, number], radius: number): void;
  /** Grows the strands in from their roots over `seconds`. */
  grow(seconds: number): void;
  setActive(active: boolean): void;
  resize(width: number, height: number): void;
  invalidate(): void;
  dispose(): void;
}

const hairVertex = /* glsl */ `
attribute vec3 tang;
attribute vec3 hairColor;
attribute vec2 meta;
attribute vec2 strand;  // the point's width (negative: none), its strand's index
uniform float uTime;
uniform float uSway;
uniform vec3 uCenter;
varying vec3 vColor;
varying vec3 vT;
varying vec3 vN;
varying vec3 vP;
varying float vS;
varying float vR;
varying float vRand;
varying float vWidth;
varying float vW;
varying vec3 vRad;
varying vec3 vObj;
flat varying uint vStrand;
void main() {
  vec3 p = position;
  float s = meta.x;
  p += uSway * s * s * 0.035 * vec3(sin(uTime * 1.3 + meta.y), 0.35 * sin(uTime * 1.9 + meta.y * 2.0), cos(uTime * 1.0 + meta.y * 1.3));
  vec4 mv = modelViewMatrix * vec4(p, 1.0);
  vT = normalize(normalMatrix * tang);
  vRad = normalMatrix * (p - uCenter);
  vObj = p;
  vN = normalize(vRad);
  vP = mv.xyz;
  vColor = hairColor;
  vS = s;
  vR = length(p - uCenter);
  vRand = fract(sin(meta.y * 12.9898) * 43758.5453);
  vWidth = strand.x;
  vStrand = uint(strand.y);
  gl_Position = projectionMatrix * mv;
  vW = gl_Position.w;
}
`;

const hairFragment = /* glsl */ `
uniform vec3 uKey;
uniform vec3 uRim;
uniform vec3 uFill;
uniform vec3 uKeyColor;
uniform vec3 uRimColor;
uniform vec3 uFillColor;
uniform vec3 uAmbient;
uniform vec2 uDepth;
uniform float uGrow;
uniform vec4 uLobes;      // the R lobe's weight, the longitudinal width B, the azimuthal spread, the ior
uniform vec4 uLobes2;     // the cuticle tilt (radians), the azimuthal roughness, the hairs in the volume's depth, the exposure
uniform vec4 uVariation;  // each strand's: specular, roughness, tilt, pigment
uniform vec2 uCoverage;   // the pixels a unit is at clip w 1 (linePixelScale), the share of the groom's strands drawn
uniform float uCoverageGain;  // the strands' coverage times this (their body solid, their tips thinner)
uniform vec2 uTune;       // the highlight's width and the R lobe's weight, times these (the page's look)
uniform vec2 uTune2;      // the TT lobe's weight (light through the strands) and the multiple scattering's
uniform vec2 uShell;      // the hair volume about the center: the roots' mean radius, the tips' mean radius
varying vec3 vColor;
varying vec3 vT;
varying vec3 vN;
varying vec3 vP;
varying float vS;
varying float vR;
varying float vRand;
varying float vWidth;
varying float vW;
varying vec3 vRad;
varying vec3 vObj;
flat varying uint vStrand;
uniform highp sampler3D uGrid;  // the hairs' density: the hairs a unit of path crosses, per cell
uniform vec4 uGridOrigin;       // its first cell's corner (the strands' space); w: a cell's size
uniform vec4 uGridDims;         // its cells along x, y, z; w: 1 with a grid
uniform vec3 uKeyO;             // the lights' directions in the strands' space (the grid's)
uniform vec3 uRimO;
uniform vec3 uFillO;
uniform vec3 uCenterO;
` + LINE_COVERAGE + HAIR_BSDF + /* glsl */ `
vec4 hairCalibration() { return vec4(0.0); }
vec4 hairCalibration2() { return vec4(0.0); }
vec4 hairCalibration3() { return vec4(0.0); }
vec3 toLinear(vec3 c) {
  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}
// The hairs between this point and a light (Maya's viewport marches a density grid for it; here the
// hair volume stands in for one: a shell about the groom's center from the roots' radius out to the
// tips', uLobes2.z hairs across it): the way to the light out of the shell, and many more where it
// runs into the scalp inside.
float hairsToward(vec3 L) {
  float rIn = uShell.x, rOut = max(uShell.y, rIn + 1e-3);
  float b = dot(vRad, L), c = dot(vRad, vRad);
  float out_ = max(-b + sqrt(max(b * b - (c - rOut * rOut), 0.0)), 0.0);
  float hit = b < 0.0 && b * b - (c - rIn * rIn) > 0.0 ? 1.0 : 0.0;
  return uLobes2.z * (out_ / (rOut - rIn) + 3.0 * hit);
}
// The hairs between this point and a light, as Maya's viewport counts them (hair_shading.cpp
// hairCount): the density grid sampled every step out of the grid, the first a cell, each next a
// third longer, starting half a step plus the strand's own jitter out, times the self-shadowing
// (uLobes2.z). The strands' own clumps shade them. Without a grid, the shell's (hairsToward).
float gridHairs(vec3 L, float jitter) {
  float step = uGridOrigin.w;
  vec3 extent = uGridDims.xyz * uGridOrigin.w;
  float t = (0.5 + jitter) * step;
  float tau = 0.0;
  for (int i = 0; i < 24; ++i) {
    vec3 uvw = (vObj + L * t - uGridOrigin.xyz) / extent;
    if (any(lessThan(uvw, vec3(0.0))) || any(greaterThan(uvw, vec3(1.0)))) break;
    tau += texture(uGrid, uvw).r * step;
    if (tau * uLobes2.z > 8.0) break;
    t += step;
    step *= 1.3;
  }
  return tau * uLobes2.z;
}
// How much of a light the scalp lets through to this point (1 lit, 0 behind the scalp): the scalp as
// a sphere about the center at the roots' mean radius, the way toward the light blocked where it
// passes through it, softly over its last fifth.
float scalpLets(vec3 L) {
  vec3 r = vObj - uCenterO;
  float b = dot(r, L);
  if (b >= 0.0) return 1.0;  // the light is on this side of the center
  float closest = sqrt(max(dot(r, r) - b * b, 0.0));
  return smoothstep(0.8 * uShell.x, uShell.x, closest);
}
void main() {
  if (vS > uGrow) discard;
  // Width as coverage (line_coverage.glsl, Maya's viewport's), to the multisampled target's samples
  // (alpha to coverage, so MSAA smooths every strand): each drawn strand covering for those a preview
  // leaves out, times uCoverageGain so a strand's body is solid (the hair hides what is under it)
  // and only its thinnest stretch, the tip, thins out.
  vec2 cover = lineCoverageOf(vWidth, vW, 1.0, uCoverage.x, 0.0, uCoverage.y, 0.0);
  float alpha = clamp(cover.x * uCoverageGain, 0.2, 1.0);
  vec3 T = normalize(vT);
  vec3 N = normalize(vN);
  vec3 V = normalize(-vP);
  // 0 on the volume's outside, 1 at the scalp.
  float inner = 1.0 - smoothstep(uDepth.x, uDepth.y, vR);
  // Each strand a little apart, as Maya draws them (hair_shading.cpp hairShade's variation: random
  // numbers per strand, from its index here, so neighbors differ).
  vec3 rnd = vec3(linePcg3d(uvec3(vStrand, 0x51EDu, 0x7A3u))) * (1.0 / 4294967296.0);
  float r1 = rnd.x, r2 = rnd.y, r3 = rnd.z, r4 = fract(rnd.x * 7.13 + rnd.y);
  HairLobes m;
  m.shape = vec4(max(uLobes.y * uTune.x * (1.0 + uVariation.y * (2.0 * r1 - 1.0)), 0.02), uLobes.z, uLobes2.x + uVariation.z * (2.0 * r2 - 1.0), uLobes.w);
  float specular = max(uLobes.x * uTune.y * (1.0 + uVariation.x * (2.0 * r3 - 1.0)), 0.0);
  m.weightR = vec3(specular, specular, specular);
  m.weightTT = vec3(uTune2.x, uTune2.x, uTune2.x);
  m.weightTRT = vec3(1.0, 1.0, 1.0);
  m.multiple = uTune2.y;
  // The strand's hair color (its base color, sRGB, as Maya's viewport takes it), absorbed as
  // aiStandardHair absorbs a base color, the strand's own pigment a share more or less.
  float pigment = 1.0 + uVariation.w * (2.0 * r4 - 1.0);
  vec3 color = hairFiberColor(hairColorSigma(toLinear(vColor), uLobes2.y) * pigment, vec3(1.0, 1.0, 1.0));
  vec3 af, ab;
  hairAttenuations(color, af, ab);
  bool grid = uGridDims.w > 0.5;
  float jitter = rnd.y;
  float nKey = grid ? gridHairs(uKeyO, jitter) : hairsToward(uKey);
  float nRim = grid ? gridHairs(uRimO, jitter) : hairsToward(uRim);
  float nFill = grid ? gridHairs(uFillO, jitter) : hairsToward(uFill);
  vec3 col = hairShadeLight(T, uKey, V, color, m, hairShadowCount(nKey), af, ab) * uKeyColor * scalpLets(uKeyO)
    + hairShadeLight(T, uRim, V, color, m, hairShadowCount(nRim), af, ab) * uRimColor * scalpLets(uRimO)
    + hairShadeLight(T, uFill, V, color, m, hairShadowCount(nFill), af, ab) * uFillColor * scalpLets(uFillO);
  // Light from everywhere, by the fiber's albedo, under the hair above it (the grid's way out from
  // the center: Maya's occlusion).
  float above = grid ? gridHairs(normalize(vObj - uCenterO), jitter) : uLobes2.z * inner;
  col += uAmbient * hairAmbientAlbedo(color, m.multiple) * exp(-0.5 * above);
  col *= uLobes2.w;
  // Maya's viewport shows it through ACES (its default view, sRGB): the renderer's ACES filmic curve.
  gl_FragColor = vec4(col, alpha);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}
`;

const dotVertex = /* glsl */ `
uniform float uSize;
void main() {
  gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
  gl_PointSize = uSize;
}
`;

const dotFragment = /* glsl */ `
uniform vec3 uRing;
void main() {
  vec2 c = gl_PointCoord * 2.0 - 1.0;
  float r = length(c);
  if (r > 1.0) discard;
  gl_FragColor = vec4(r > 0.5 ? uRing : vec3(1.0), 1.0);
  #include <colorspace_fragment>
}
`;

/** The shading's knobs the tuning panel (?tune, tune-panel.tsx) drives; TUNE_DEFAULTS are the page's look. */
export interface TuneValues {
  highlightWidth: number;
  specular: number;
  key: number;
  fill: number;
  rim: number;
  ambient: number;
  shadow: number;
  coverage: number;
  exposure: number;
  scalp: number;
  transmission: number;
  scatter: number;
}
export const TUNE_DEFAULTS: TuneValues = {
  highlightWidth: 1.01,
  specular: 0.44,
  key: 0.83,
  fill: 0.52,
  rim: 2.44,
  ambient: 0.14,
  shadow: 1.08,
  coverage: 2,
  exposure: 1.3,
  scalp: 1,
  transmission: 0.59,
  scatter: 0.26,
};
const tuneTargets = new Set<(v: TuneValues) => void>();
/** Sets the knobs on every viewer. */
export function setTune(v: TuneValues) {
  for (const t of tuneTargets) t(v);
}

export function createStrandViewer(canvas: HTMLCanvasElement, opts: ViewerOptions): StrandViewer | null {
  let renderer: WebGLRenderer;
  try {
    // MSAA: smooth edges on the lines and the scalp.
    renderer = new WebGLRenderer({ canvas, antialias: true, alpha: true, powerPreference: 'low-power' });
  } catch {
    return null;
  }
  renderer.setClearColor(0x000000, 0);
  // As Maya's viewport shows the hair (ACES 1.0 SDR-video, its default view transform).
  renderer.toneMapping = ACESFilmicToneMapping;
  renderer.toneMappingExposure = 1.3;
  renderer.autoClear = false;

  const still = opts.still;
  const spin = still ? 0 : (opts.spin ?? 0.22);
  const sway = still ? 0 : (opts.sway ?? 0.6);
  const fov = 28;
  const camera = new PerspectiveCamera(fov, 1, 0.05, 100);
  const scene = new Scene();
  const overlay = new Scene();
  const group = new Group();
  const overlayGroup = new Group();
  scene.add(group);
  overlay.add(overlayGroup);

  // Lit as Maya's viewport lights a groom by default: the key is the camera's own light; a softer
  // one up and to the left of it shapes the volume, a faint rim comes from behind, a dim sky from
  // everywhere; shown through ACES as Maya's default view. The hair's shader reads the same lights.
  const keyColor = new Color(1, 0.96, 0.9).multiplyScalar(0.83);
  const rimColor = new Color(0.85, 0.9, 1).multiplyScalar(2.44);
  const fillColor = new Color(1, 0.97, 0.94).multiplyScalar(0.52);
  const ambient = new Color(0.16, 0.15, 0.14).multiplyScalar(0.14);
  // The key in the camera's space (Maya's default light is the camera's own).
  const headlight = new Vector3(0, 0, 1);
  // A softer light up and to the left of the camera, for the volume's shape.
  const shapeLight = new Vector3(-0.55, 0.7, 0.9).normalize();
  scene.add(new HemisphereLight(0x9aa8c0, 0x1c1714, 0.9));
  const keyDir = new Vector3(-2.4, 2.8, 2.6).normalize();
  const key = new DirectionalLight(0xffdcbc, 2.4);
  key.position.copy(keyDir);
  scene.add(key);
  const rimDir = new Vector3(2.4, 1.8, -2.8).normalize();
  const rimLight = new DirectionalLight(0x9cc0ff, 2.6);
  rimLight.position.copy(rimDir);
  scene.add(rimLight);
  const fillDir = new Vector3(3, 0.3, 1.8).normalize();
  const fillLight = new DirectionalLight(0xc8d4ff, 0.35);
  fillLight.position.copy(fillDir);
  scene.add(fillLight);

  const scalpMaterial = new MeshLambertMaterial({ color: new Color('#9a9692') });
  const scalpTint = 1;  // the painted scalp's brightness (vertex colors times it)
  let scalpColorKey = '';
  const depthMaterial = new MeshBasicMaterial({ colorWrite: false });
  let scalpGeometry: BufferGeometry | null = null;
  const scalpMesh = new Mesh(new BufferGeometry(), scalpMaterial);
  const scalpDepth = new Mesh(new BufferGeometry(), depthMaterial);
  group.add(scalpMesh);
  overlayGroup.add(scalpDepth);

  const hairMaterial = new ShaderMaterial({
    vertexShader: hairVertex,
    alphaToCoverage: true,
    fragmentShader: hairFragment,
    uniforms: {
      uTime: { value: 0 },
      uSway: { value: 0 },
      uGrow: { value: 1 },
      uCenter: { value: new Vector3() },
      uKey: { value: new Vector3() },
      uRim: { value: new Vector3() },
      uFill: { value: new Vector3() },
      uKeyColor: { value: keyColor },
      uRimColor: { value: rimColor },
      uFillColor: { value: fillColor },
      uAmbient: { value: ambient },
      uDepth: { value: new Vector2(0.9, 1.6) },
      // HairColor's defaults (core's hairLobes) until the engine says the groom's.
      uLobes: { value: new Vector4(1, 0.083, 0.42, 1.55) },
      uLobes2: { value: new Vector4((3 * Math.PI) / 180, 0.3, 1.08, Math.PI) },
      uVariation: { value: new Vector4(0.3, 0.3, (4 * Math.PI) / 180, 0.3) },
      uCoverage: { value: new Vector2(1000, 1) },
      uCoverageGain: { value: 2 },
      uTune: { value: new Vector2(1.01, 0.44) },
      uTune2: { value: new Vector2(0.59, 0.26) },
      uGrid: { value: null as Data3DTexture | null },
      uGridOrigin: { value: new Vector4(0, 0, 0, 1) },
      uGridDims: { value: new Vector4(1, 1, 1, 0) },
      uKeyO: { value: new Vector3(0, 0, 1) },
      uRimO: { value: new Vector3(0, 0, 1) },
      uFillO: { value: new Vector3(0, 0, 1) },
      uCenterO: { value: new Vector3() },
      uShell: { value: new Vector2(0.9, 1.4) },
    },
  });
  let hairGeometry = new BufferGeometry();
  const hair = new LineSegments(hairGeometry, hairMaterial);
  hair.frustumCulled = false;
  group.add(hair);
  let capacity = 0;
  let indexCapacity = 0;
  let layout = new Uint32Array(0);
  let layoutCount = -1;

  const guideLineMaterial = new LineBasicMaterial({ color: new Color('#6aa3ff') });
  const dotMaterial = new ShaderMaterial({
    vertexShader: dotVertex,
    fragmentShader: dotFragment,
    uniforms: { uSize: { value: 6 }, uRing: { value: new Color('#2f7bf6') } },
  });
  let guideLines: LineSegments | null = null;
  let guideDots: Points | null = null;

  const center = new Vector3();
  let radius = 1.6;
  let framed = false;
  let width = 1;
  let height = 1;
  let yaw = opts.yaw ?? -0.5;
  let pitch = opts.pitch ?? 0.32;
  let yawVelocity = 0;
  let spinFactor = 1;
  let dragging = false;
  let active = false;
  let raf = 0;
  let inFrame = false;
  /** Whether frames are coming one after another (else the next one starts from rest). */
  let running = false;
  let growStart = -1;
  let growLength = 0;
  const start = performance.now();
  let last = start;

  const ensureCapacity = (points: number, segments: number) => {
    if (points <= capacity && segments * 2 <= indexCapacity) return;
    capacity = Math.max(points, Math.ceil(capacity * 1.25));
    indexCapacity = Math.max(segments * 2, Math.ceil(indexCapacity * 1.25));
    hairGeometry.dispose();
    hairGeometry = new BufferGeometry();
    const attr = (size: number) => new BufferAttribute(new Float32Array(capacity * size), size).setUsage(DynamicDrawUsage);
    hairGeometry.setAttribute('position', attr(3));
    hairGeometry.setAttribute('tang', attr(3));
    hairGeometry.setAttribute('hairColor', attr(3));
    hairGeometry.setAttribute('meta', attr(2));
    hairGeometry.setAttribute('strand', attr(2));
    hairGeometry.setIndex(new BufferAttribute(new Uint32Array(indexCapacity), 1).setUsage(DynamicDrawUsage));
    hair.geometry = hairGeometry;
    layoutCount = -1;
  };

  const touch = (name: string, count: number) => {
    const a = hairGeometry.getAttribute(name) as BufferAttribute;
    a.clearUpdateRanges();
    a.addUpdateRange(0, count * a.itemSize);
    a.needsUpdate = true;
  };

  // The tuning panel (?tune, tune-panel.tsx) sets these live on every viewer.
  const keyBase = keyColor.clone(), rimBase = rimColor.clone(), fillBase = fillColor.clone(), ambientBase = ambient.clone();
  const tuneTarget = (v: TuneValues) => {
    const u = hairMaterial.uniforms;
    (u.uTune.value as Vector2).set(v.highlightWidth, v.specular);
    (u.uTune2.value as Vector2).set(v.transmission, v.scatter);
    keyColor.copy(keyBase).multiplyScalar(v.key / 0.83);
    rimColor.copy(rimBase).multiplyScalar(v.rim / 2.44);
    fillColor.copy(fillBase).multiplyScalar(v.fill / 0.52);
    ambient.copy(ambientBase).multiplyScalar(v.ambient / 0.14);
    (u.uLobes2.value as Vector4).z = v.shadow;
    u.uCoverageGain.value = v.coverage;
    renderer.toneMappingExposure = v.exposure;
    scalpMaterial.color.setScalar(v.scalp);
    viewer.invalidate();
  };
  tuneTargets.add(tuneTarget);

  const viewer: StrandViewer = {
    setScalp(mesh) {
      scalpGeometry?.dispose();
      const g = new BufferGeometry();
      g.setAttribute('position', new BufferAttribute(mesh.positions, 3));
      if (mesh.normals) g.setAttribute('normal', new BufferAttribute(mesh.normals, 3));
      if (mesh.colors) {
        // Vertex colors are sRGB; the material works in linear.
        const c = new Color();
        const lin = new Float32Array(mesh.colors.length);
        for (let i = 0; i < lin.length; i += 3) {
          c.setRGB(mesh.colors[i], mesh.colors[i + 1], mesh.colors[i + 2], SRGBColorSpace);
          lin[i] = c.r;
          lin[i + 1] = c.g;
          lin[i + 2] = c.b;
        }
        g.setAttribute('color', new BufferAttribute(lin, 3));
      }
      const key = mesh.colors ? 'vertex' : 'flat';
      if (key !== scalpColorKey) {
        scalpColorKey = key;
        scalpMaterial.vertexColors = !!mesh.colors;
        if (mesh.colors) scalpMaterial.color.setScalar(scalpTint);
        else scalpMaterial.color.set('#9a9692');
        scalpMaterial.needsUpdate = true;
      }
      g.setIndex(new BufferAttribute(mesh.indices, 1));
      if (!mesh.normals) g.computeVertexNormals();
      g.computeBoundingSphere();
      scalpGeometry = g;
      scalpMesh.geometry = g;
      scalpDepth.geometry = g;
      if (!framed && g.boundingSphere) {
        center.copy(g.boundingSphere.center);
        radius = g.boundingSphere.radius * 1.45;
        hairMaterial.uniforms.uCenter.value.copy(center);
        frame();
      }
      viewer.invalidate();
    },

    setStrands(data) {
      const n = data.count;
      const points = data.offsets[n];
      const segments = points - n;
      ensureCapacity(points, segments);
      const pos = hairGeometry.getAttribute('position').array as Float32Array;
      const tang = hairGeometry.getAttribute('tang').array as Float32Array;
      const meta = hairGeometry.getAttribute('meta').array as Float32Array;
      const strand = hairGeometry.getAttribute('strand').array as Float32Array;
      pos.set(data.positions.subarray(0, points * 3));
      for (let i = 0; i < n; i++) {
        const a = data.offsets[i];
        const b = data.offsets[i + 1];
        const last = b - a - 1;
        // A phase for the sway from where the root is, so neighbors sway together.
        const phase = pos[a * 3] * 3.1 + pos[a * 3 + 1] * 1.7 + pos[a * 3 + 2] * 2.3;
        for (let p = a; p < b; p++) {
          const p0 = Math.max(a, p - 1) * 3;
          const p1 = Math.min(b - 1, p + 1) * 3;
          tang[p * 3] = pos[p1] - pos[p0];
          tang[p * 3 + 1] = pos[p1 + 1] - pos[p0 + 1];
          tang[p * 3 + 2] = pos[p1 + 2] - pos[p0 + 2];
          meta[p * 2] = last > 0 ? (p - a) / last : 0;
          meta[p * 2 + 1] = phase;
          strand[p * 2] = data.widths ? data.widths[p] : -1;
          strand[p * 2 + 1] = i;
        }
      }
      touch('position', points);
      touch('tang', points);
      touch('meta', points);
      touch('strand', points);
      hairMaterial.uniforms.uCoverage.value.y = data.density ?? 1;
      depthRange(data);
      densityGrid(data);

      // The segment index, only when the strands' layout changed.
      let same = layoutCount === n;
      for (let i = 0; same && i <= n; i++) same = layout[i] === data.offsets[i];
      if (!same) {
        if (layout.length < n + 1) layout = new Uint32Array(n + 1);
        layout.set(data.offsets.subarray(0, n + 1));
        layoutCount = n;
        const index = hairGeometry.getIndex()!;
        const idx = index.array as Uint32Array;
        let k = 0;
        for (let i = 0; i < n; i++)
          for (let p = data.offsets[i]; p < data.offsets[i + 1] - 1; p++) {
            idx[k++] = p;
            idx[k++] = p + 1;
          }
        index.clearUpdateRanges();
        index.addUpdateRange(0, k);
        index.needsUpdate = true;
        hairGeometry.setDrawRange(0, k);
      }
      if (data.colors) viewer.setStrandColors(data.colors);
      else if (!same) viewer.setStrandColors(new Float32Array(points * 3).fill(0.4));
      viewer.invalidate();
    },

    setLook(look) {
      const u = hairMaterial.uniforms;
      (u.uLobes.value as Vector4).set(look.specular, look.width, look.spread, look.ior);
      const l2 = u.uLobes2.value as Vector4;
      l2.x = look.tilt;
      l2.y = look.azimuthalRoughness;
      (u.uVariation.value as Vector4).fromArray(look.variation);
      viewer.invalidate();
    },

    setStrandColors(colors) {
      const points = layoutCount < 0 ? 0 : layout[layoutCount];
      const col = hairGeometry.getAttribute('hairColor').array as Float32Array;
      col.set(colors.subarray(0, points * 3));
      touch('hairColor', points);
      viewer.invalidate();
    },

    setGuides(data) {
      for (const o of [guideLines, guideDots]) {
        if (!o) continue;
        overlayGroup.remove(o);
        o.geometry.dispose();
      }
      guideLines = guideDots = null;
      if (data && data.count > 0) {
        const n = data.count;
        const idx: number[] = [];
        const dots: number[] = [];
        for (let i = 0; i < n; i++) {
          const a = data.offsets[i];
          const b = data.offsets[i + 1];
          for (let p = a; p < b - 1; p++) idx.push(p, p + 1);
          const step = Math.max(1, Math.round((b - a) / 6));
          for (let p = a; p < b; p += step) dots.push(data.positions[p * 3], data.positions[p * 3 + 1], data.positions[p * 3 + 2]);
        }
        const lines = new BufferGeometry();
        lines.setAttribute('position', new BufferAttribute(data.positions.slice(0, data.offsets[n] * 3), 3));
        lines.setIndex(idx);
        guideLines = new LineSegments(lines, guideLineMaterial);
        const points = new BufferGeometry();
        points.setAttribute('position', new BufferAttribute(new Float32Array(dots), 3));
        guideDots = new Points(points, dotMaterial);
        guideLines.frustumCulled = guideDots.frustumCulled = false;
        overlayGroup.add(guideLines, guideDots);
      }
      viewer.invalidate();
    },

    setFraming(c, r) {
      center.set(c[0], c[1], c[2]);
      radius = r;
      framed = true;
      hairMaterial.uniforms.uCenter.value.copy(center);
      frame();
      viewer.invalidate();
    },

    grow(seconds) {
      if (still) return;
      growStart = performance.now();
      growLength = seconds * 1000;
      hairMaterial.uniforms.uGrow.value = 0;
      viewer.invalidate();
    },

    setActive(on) {
      active = on;
      if (on) {
        last = performance.now();
        viewer.invalidate();
      } else {
        cancelAnimationFrame(raf);
        raf = 0;
        running = false;
      }
    },

    resize(w, h) {
      width = Math.max(1, w);
      height = Math.max(1, h);
      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      renderer.setPixelRatio(dpr);
      renderer.setSize(width, height, false);
      dotMaterial.uniforms.uSize.value = 6 * dpr;
      // The device pixels a unit spans at clip w 1 (the strands' width as coverage).
      hairMaterial.uniforms.uCoverage.value.x = (height * dpr) / (2 * Math.tan(((fov / 2) * Math.PI) / 180));
      frame();
      viewer.invalidate();
    },

    invalidate() {
      if (raf || !active || inFrame) return;
      // The first frame after standing still steps nothing (no jump from the time it stood).
      if (!running) last = -1;
      raf = requestAnimationFrame(tick);
    },

    dispose() {
      tuneTargets.delete(tuneTarget);
      active = false;
      cancelAnimationFrame(raf);
      canvas.removeEventListener('pointerdown', onDown);
      canvas.removeEventListener('pointermove', onMove);
      canvas.removeEventListener('pointerup', onUp);
      canvas.removeEventListener('pointercancel', onUp);
      viewer.setGuides(null);
      hairGeometry.dispose();
      scalpGeometry?.dispose();
      gridTexture?.dispose();
      for (const m of [hairMaterial, scalpMaterial, depthMaterial, guideLineMaterial, dotMaterial]) m.dispose();
      renderer.dispose();
      renderer.forceContextLoss();
    },
  };

  /**
   * The hair volume's depth for its occlusion: from about where the roots sit to the outermost
   * points, measured from the center the camera turns around.
   */
  /**
   * The hairs' density grid (Maya's viewport's: hair_shading.h HairGroomState), the hairs a unit of
   * path crosses in each cell: every segment's length times its width over a cell's volume, spread
   * over the cells around its middle, each drawn strand standing for those a preview leaves out. Up
   * to 64 cells along the strands' longest side, half floats, filtered.
   */
  let gridTexture: Data3DTexture | null = null;
  const kGridMedianHairs = 0.155;  // the hairs a typical occupied cell holds (the hero's, as its look was tuned)
  function densityGrid(data: StrandData) {
    const n = data.count;
    const pts = data.offsets[n];
    const p = data.positions;
    if (!pts) return;
    const lo = [Infinity, Infinity, Infinity];
    const hi = [-Infinity, -Infinity, -Infinity];
    for (let i = 0; i < pts * 3; i += 3)
      for (let k = 0; k < 3; k++) {
        lo[k] = Math.min(lo[k], p[i + k]);
        hi[k] = Math.max(hi[k], p[i + k]);
      }
    const most = 64;
    const cell = Math.max(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]) / (most - 2) || 1;
    const dims = [0, 1, 2].map((k) => Math.min(most, Math.ceil((hi[k] - lo[k]) / cell) + 2));
    const origin = [0, 1, 2].map((k) => lo[k] - cell);
    const [nx, ny, nz] = dims;
    const grid = new Float32Array(nx * ny * nz);
    const share = 1 / Math.max(1e-3, Math.min(1, data.density ?? 1));
    const fallback = cell * 0.01;
    const perVolume = share / (cell * cell * cell);
    for (let s = 0; s < n; s++) {
      for (let a = data.offsets[s]; a < data.offsets[s + 1] - 1; a++) {
        const i = a * 3;
        const dx = p[i + 3] - p[i], dy = p[i + 4] - p[i + 1], dz = p[i + 5] - p[i + 2];
        const width = data.widths && data.widths[a] >= 0 ? 0.5 * (data.widths[a] + data.widths[a + 1]) : fallback;
        const amount = Math.hypot(dx, dy, dz) * width * perVolume;
        // Trilinear: the segment's middle shared among the eight cells around it.
        const gx = (p[i] + 0.5 * dx - origin[0]) / cell - 0.5;
        const gy = (p[i + 1] + 0.5 * dy - origin[1]) / cell - 0.5;
        const gz = (p[i + 2] + 0.5 * dz - origin[2]) / cell - 0.5;
        const x0 = Math.floor(gx), y0 = Math.floor(gy), z0 = Math.floor(gz);
        const fx = gx - x0, fy = gy - y0, fz = gz - z0;
        for (let c = 0; c < 8; c++) {
          const x = x0 + (c & 1), y = y0 + ((c >> 1) & 1), z = z0 + ((c >> 2) & 1);
          if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz) continue;
          const w = (c & 1 ? fx : 1 - fx) * ((c >> 1) & 1 ? fy : 1 - fy) * ((c >> 2) & 1 ? fz : 1 - fz);
          grid[(z * ny + y) * nx + x] += amount * w;
        }
      }
    }
    // Each groom's grid normalized to the same density in its typical occupied cell (the median's
    // hairs a cell, the hero's as tuned), so the self-shadowing reads alike on any groom whatever its
    // widths (a groom with no Width node takes the engine's default) or its preview's share.
    const occupied: number[] = [];
    for (let i = 0; i < grid.length; i++) if (grid[i] > 0) occupied.push(grid[i]);
    occupied.sort((a, b) => a - b);
    const median = occupied.length ? occupied[occupied.length >> 1] * cell : 0;
    const norm = median > 0 ? kGridMedianHairs / median : 1;
    const half = new Uint16Array(grid.length);
    for (let i = 0; i < grid.length; i++) half[i] = DataUtils.toHalfFloat(Math.min(grid[i] * norm, 60000));
    gridTexture?.dispose();
    gridTexture = new Data3DTexture(half, nx, ny, nz);
    gridTexture.format = RedFormat;
    gridTexture.type = HalfFloatType;
    gridTexture.minFilter = gridTexture.magFilter = LinearFilter;
    gridTexture.unpackAlignment = 1;
    gridTexture.needsUpdate = true;
    const u = hairMaterial.uniforms;
    u.uGrid.value = gridTexture;
    (u.uGridOrigin.value as Vector4).set(origin[0], origin[1], origin[2], cell);
    (u.uGridDims.value as Vector4).set(nx, ny, nz, 1);
    (u.uCenterO.value as Vector3).copy(center);
  }

  function depthRange(data: StrandData) {
    const n = data.count;
    const p = data.positions;
    let roots = 0;
    let far = 0;
    let tips = 0;
    for (let i = 0; i < n; i++) {
      const a = data.offsets[i] * 3;
      roots += Math.hypot(p[a] - center.x, p[a + 1] - center.y, p[a + 2] - center.z);
      const b = (data.offsets[i + 1] - 1) * 3;
      const tip = Math.hypot(p[b] - center.x, p[b + 1] - center.y, p[b + 2] - center.z);
      far = Math.max(far, tip);
      tips += tip;
    }
    const inner = n ? roots / n : 0.9;
    hairMaterial.uniforms.uDepth.value.set(inner, Math.max(inner + 0.05, inner + (far - inner) * 0.8));
    hairMaterial.uniforms.uShell.value.set(inner, Math.max(inner + 0.05, n ? tips / n : 1.4));
  }

  /** Puts the camera where the groom's sphere fills the view, at the current elevation. */
  function frame() {
    camera.aspect = width / height;
    const v = (fov / 2) * (Math.PI / 180);
    const h = Math.atan(Math.tan(v) * camera.aspect);
    const dist = radius / Math.sin(Math.min(v, h)) / (opts.fill ?? 1);
    camera.position.set(center.x, center.y + Math.sin(pitch) * dist, center.z + Math.cos(pitch) * dist);
    camera.lookAt(center);
    camera.near = Math.max(0.01, dist - radius * 3);
    camera.far = dist + radius * 3;
    camera.updateProjectionMatrix();
  }

  const view = new Vector3();
  const toObject = new Matrix4();
  function tick(now: number) {
    raf = 0;
    if (!active) {
      running = false;
      return;
    }
    inFrame = true;
    const dt = last < 0 ? 0 : Math.min(0.05, (now - last) / 1000);
    last = now;
    const t = (now - start) / 1000;
    opts.onFrame?.(t);
    inFrame = false;

    if (!dragging) {
      spinFactor = Math.min(1, spinFactor + dt * 0.6);
      yaw += spin * spinFactor * dt + yawVelocity * dt;
      yawVelocity *= Math.exp(-dt * 4);
      if (Math.abs(yawVelocity) < 0.01) yawVelocity = 0;
    }
    group.rotation.y = overlayGroup.rotation.y = yaw;
    frame();
    camera.updateMatrixWorld();
    const u = hairMaterial.uniforms;
    u.uTime.value = t;
    u.uSway.value = sway;
    // The key is the camera's own light, as Maya's default light is; the fill follows it, up and to the left.
    u.uKey.value.copy(shapeLight);
    u.uFill.value.copy(headlight);
    u.uRim.value.copy(view.copy(rimDir).transformDirection(camera.matrixWorldInverse));
    // The lights in the strands' own space, for the density grid's march.
    group.updateMatrixWorld();
    toObject.copy(hair.matrixWorld).invert();
    u.uKeyO.value.copy(view.copy(shapeLight).transformDirection(camera.matrixWorld).transformDirection(toObject));
    u.uFillO.value.copy(view.copy(headlight).transformDirection(camera.matrixWorld).transformDirection(toObject));
    u.uRimO.value.copy(view.copy(rimDir).transformDirection(toObject));
    let growing = false;
    if (growStart >= 0) {
      const k = Math.min(1, (now - growStart) / growLength);
      u.uGrow.value = 1 - Math.pow(1 - k, 3);
      growing = k < 1;
      if (!growing) growStart = -1;
    }

    renderer.clear();
    renderer.render(scene, camera);
    if (guideLines) {
      renderer.clearDepth();
      renderer.render(overlay, camera);
    }
    // Only what moves keeps the frames coming: a turntable, sway, a drag and its glide, the grow.
    const moving = spin > 0 || sway > 0 || dragging || yawVelocity !== 0 || growing;
    running = moving && active && !document.hidden;
    if (running) raf = requestAnimationFrame(tick);
  }

  // Drag to orbit. Touch turns it sideways only (touch-action: pan-y leaves vertical swipes to
  // the page); a mouse can also tip it up and down. Wheel and pinch are left alone.
  let lastX = 0;
  let lastY = 0;
  let lastT = 0;
  let pointer = -1;
  function onDown(e: PointerEvent) {
    if (pointer >= 0 || (e.pointerType === 'mouse' && e.button !== 0)) return;
    pointer = e.pointerId;
    dragging = true;
    yawVelocity = 0;
    lastX = e.clientX;
    lastY = e.clientY;
    lastT = e.timeStamp;
    canvas.setPointerCapture(e.pointerId);
    canvas.style.cursor = 'grabbing';
    viewer.invalidate();
  }
  function onMove(e: PointerEvent) {
    if (e.pointerId !== pointer) return;
    const dx = e.clientX - lastX;
    const dy = e.clientY - lastY;
    const dts = Math.max(1, e.timeStamp - lastT) / 1000;
    lastX = e.clientX;
    lastY = e.clientY;
    lastT = e.timeStamp;
    const k = 5.5 / Math.max(240, width);
    yaw += dx * k;
    yawVelocity = yawVelocity * 0.6 + ((dx * k) / dts) * 0.4;
    if (e.pointerType === 'mouse') pitch = Math.max(-0.15, Math.min(1.1, pitch + dy * k * 0.6));
    viewer.invalidate();
  }
  function onUp(e: PointerEvent) {
    if (e.pointerId !== pointer) return;
    pointer = -1;
    dragging = false;
    spinFactor = 0;
    if (e.type === 'pointercancel') yawVelocity = 0;
    yawVelocity = Math.max(-4, Math.min(4, yawVelocity));
    canvas.style.cursor = '';
    viewer.invalidate();
  }
  canvas.addEventListener('pointerdown', onDown);
  canvas.addEventListener('pointermove', onMove);
  canvas.addEventListener('pointerup', onUp);
  canvas.addEventListener('pointercancel', onUp);

  return viewer;
}
