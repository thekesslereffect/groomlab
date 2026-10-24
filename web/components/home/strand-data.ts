// SPDX-License-Identifier: Apache-2.0
/*
 * What the 3D viewer draws: the GroomLab engine built to WebAssembly (home-engine.ts, the /view
 * page), and the bare head (head-mesh.ts). Plain typed arrays, no three.js, so a source never has to
 * know about the renderer.
 */

/** Strands as polylines, one after the other in one buffer. */
export interface StrandData {
  /** Number of strands. */
  count: number;
  /** Strand i's points are offsets[i] up to offsets[i + 1] - 1; at least count + 1 long. */
  offsets: Uint32Array;
  /** xyz per point (at least offsets[count] * 3 long). Y is up; the scalp sits around the origin. */
  positions: Float32Array;
  /** Optional sRGB color per point, rgb 0 to 1 (the hair's base color: the shading darkens it); grey when left out. */
  colors?: Float32Array;
  /**
   * Optional width per point (the strand's diameter there, in these positions' units; negative: no
   * widths). The viewer draws a strand thinner than a pixel as that share of one, as Maya's viewport
   * does (width as coverage); without widths every strand is a solid line.
   */
  widths?: Float32Array;
  /** The share of the groom's strands these are (a preview): each drawn strand covers for the rest. */
  density?: number;
}

/**
 * The hair's highlight as Maya's viewport shades it (the engine's: core's hairLobes of the groom's
 * HairColor, the material engine/gpu/shaders/hair_bsdf.glsl takes).
 */
export interface HairLook {
  /** The R lobe's weight (HairColor's specular as aiStandardHair's tint). */
  specular: number;
  /** The longitudinal width B (from the roughness). */
  width: number;
  /** The azimuthal spread, radians. */
  spread: number;
  ior: number;
  /** The cuticle tilt, radians. */
  tilt: number;
  /** The azimuthal roughness the strands' color is absorbed by. */
  azimuthalRoughness: number;
  /** Each strand's variation: specular, roughness, tilt (radians), pigment. */
  variation: [number, number, number, number];
}

/** The mesh the strands grow from, drawn as a smooth skin-neutral surface. */
export interface RootMesh {
  /** xyz per vertex. */
  positions: Float32Array;
  /** xyz per vertex; computed from the triangles when left out. */
  normals?: Float32Array;
  /** Three vertex indices per triangle. */
  indices: Uint32Array | Uint16Array;
  /** Optional sRGB color per vertex, rgb 0 to 1 (a scalp painted with the hair's root color). */
  colors?: Float32Array;
}
