# Rendering

Review renders, Arnold (in Maya and from groom files on a farm) and RenderMan.

User guide: [Rendering](https://www.getgroomlab.com/docs/rendering), [Other renderers](https://www.getgroomlab.com/docs/other-renderers).

## Review

GroomLab > Review, or by script:

```python
groomlab.density_heatmap(groom)          # strandColorMode density; False goes back to solid
groomlab.show_hud()                      # active groom: strands, strands at render, guides, eval ms
groomlab.groom_stats(groom)              # the same numbers as a dict
rig = groomlab.turntable(groom, frames=120)  # orbiting camera (renderable) + key/fill/rim lights
```

The turntable camera frames the groom's bounds and orbits once over `frames`; the lights stay
put, so highlights travel over the hair. It sets the playback and render range.
- **Render Turntable...** (GroomLab > Review): the groom going round once, rendered in Arnold for
  dailies. Pick the frames, every Nth frame, the size and Preview or Final quality. It uses the
  scene's turntable rig (making one, and a hair shader if the groom has none), saves the scene and
  renders the saved file in a background `Render` process, so Maya stays free; a progress window
  can stop it. Frames land in the scene's `groomlab/<scene>/renders/turntable_<date>` folder (opened when
  done), with a `turntable.mp4` when `ffmpeg` is on the PATH, and a `render.log`.
  `groomlab.render_turntable(groom, frames=120, step=1, width=960, height=540, quality="preview")`
  returns the job (`poll()`, `wait()`, `cancel()`, `rendered()`).

## Rendering with Arnold

The module adds its `extensions` folder to `MTOA_EXTENSIONS_PATH`, so MtoA loads
`groomLabTranslator` automatically. At render time the translator re-evaluates the
graph at full density and ignores `previewDensity`. It exports linear curves with
one radius per point (half the strand width).

- **Shader:** the shading group's shader if one is assigned (use
  `groomlab.assign_shading_group(groom, sg)`, because `sets -forceElement`
  rejects plugin shapes), otherwise a default `standard_hair`.
- **Hair color from the groom:** `groomlab.create_hair_shader(groom)` makes an `aiStandardHair`
  whose `melanin` and `melaninRedness` read the strand channels `HairColor` writes (through
  `aiUserDataFloat`), assigns it, and turns off `melaninRandomize`, since HairColor varies the
  color per strand and per clump. When the color is more than the pigment (a map, hue,
  saturation or value randomness, a tip color, a Bands or FeatherColor node) or a Blend mixes two
  looks' colors, the shader reads the RGB (`hair_color`) instead. Its highlight is the HairColor
  node's (specular tint `min(1, specular / 0.3)`, roughness `0.1 + 0.4 x roughness`, shift the
  tilt; 0.3, 0.5 and 3 degrees without one). The lit viewport shows each strand's own color, as without a shader.
- **One color, everywhere:** the `HairColor` node writes the color twice, as melanin for the
  Arnold hair shader and as linear RGB per strand (`colorR/G/B`) for engines and formats that
  want a color: Unreal `groom_color`, USD `displayColor`, Alembic `Cd`, Arnold `hair_color`,
  the cards' `_Color` bake. A tip color (`tipStrength`), Bands and FeatherColor write the RGB
  per point as well; USD's per-point primvars and Arnold's `hair_color` carry it per point (Alembic's `Cd`
  is per curve). The RGB is the
  melanin's color tinted by the dye, or the `map` (a painted map, a scalp color set or an image),
  so the same groom renders in Arnold and runs in Unreal alike.
- **Presets:** `cinematic_hair` (two clump levels, restrained noise, frizz, flyaways, length,
  width and color variation) is the starting point for hero hair; insert the `wet` add-on for
  wet hair.
- **Level of detail:** for crowds and wide shots, the groom's Level of Detail attributes (in
  the Attribute Editor under Rendering) thin and cull strands at render time, in both render
  modes, against the render camera:
  - `lodDistance`: strands are kept with a probability falling from 1 at `lodNear` to
    `lodMinDensity` at `lodFar` (distance from the camera to the root, in the groom's space),
    by stable ID, so the same strands stay from frame to frame. Survivors are widened by
    density^-`lodWidthCompensation` (1 keeps the coverage; at most 10x).
  - `cameraCull`: strands wholly outside the camera's view, widened by `cullMargin` on each
    side, are dropped. Keep a margin for shadows, reflections and motion blur near the frame.
  - `backfaceCull` (off by default): strands whose root faces away from the camera (the scalp's
    normal under it, else the strand's first segment) by more than `backfaceAngle` degrees past
    side-on are dropped: the back of a head in a close-up. Keep a padding for long hair that
    hangs into view, silhouettes, shadows and reflections; decided by root, so by strand.
  - Every motion key keeps the same strands. The render log reports what LOD kept.
- **Render overrides** (Attribute Editor > Rendering > Render Overrides; renders alone, the
  viewport and the groom as groomed, both render modes): `renderDensity` the share of the strands
  rendered (0-1, by the preview's thinning: the groom's own strands, the same look),
  `renderWidthScale` times every width, `renderLengthScale` each strand scaled about its root as a
  Transform node's scale does (shape and all; 0.5 renders it at half size, not trimmed). The
  procedural takes them as `density`, `width_scale` and `length_scale`; exports with
  `-renderOverrides`. The defaults (1) change nothing.
- **Arnold attributes:** `aiMode` (ribbon or thick; Arnold's `oriented` mode needs orientations,
  which it does not support on linear curves, so that choice renders as ribbon with a warning),
  `aiMinPixelWidth` and the usual visibility flags appear on the groom node.
- **Shading data:** curves carry the root UV in `uvs` (when the scalp has UVs), a `strand_id`
  user attribute (uniform UINT), every strand channel, such as `clumpId`, as uniform FLOAT
  user data, and `hair_color` (linear RGB) when the groom has a color node (its RGB):
  uniform, one color per strand, or varying, one per point, when a HairColor tip color, Bands or
  FeatherColor write the RGB per point. With FeatherColor's iridescence: `iridescence` (varying
  FLOAT) and `root_normal` (uniform RGB holding a direction, for `aiUserDataColor`: a feather's
  vane normal, else the scalp's at each root), which `create_hair_shader`'s iridescence network
  reads ([Color patterns](looks-and-maps.md#color-patterns)). `aiUserDataColor` reads either; a varying color is interpolated along the
  curve. A RenderData node adds `opacity` (varying FLOAT; the curves not opaque when any point is
  below 1), a HairColor with its highlight set off the defaults `specular`, `roughness` and `tilt`
  (uniform FLOAT, as every strand channel). Read
  them with `aiUserDataInt`, `aiUserDataFloat` and `aiUserDataRGB`. Every exporter (Alembic,
  USD, Unreal, Arnold) describes the strands the same way (`engine/core/include/groomlab/export_view.h`).
- **Render LOD camera:** both render modes read the camera from Arnold (`fov`, the screen window
  from Maya's film fit and offsets, and the options' `xres`, `yres` and `pixel_aspect_ratio`), so
  they see the same view. An orthographic camera culls to its width and never thins by distance.
- **Motion blur:** with deformation blur on, the groom is evaluated at every motion step and
  the points get one key per step. Keys need the same strands, which a rest mesh guarantees on a
  deforming scalp. If the strands still differ, the groom renders without deformation blur and
  logs a warning. Instances blur the same way: the instancer's `instance_matrix` gets a key per
  step from that step's strands (and its `matrix` the groom's transform keys), in both render
  modes; if the instances differ between steps (other strands), they render without blur on
  those steps and a warning is logged. The prototype meshes themselves are exported once, in
  their own space.

### Rendering from a groom file (farms)

```python
groomlab.render_from_groom_file(groom, "C:/shots/010/hair.groom", start=1, end=48)
```

This exports the groom file and scalp cache and sets the groom's `renderMode` to `groomFile`.
Exported `.ass` files then hold a small `groomlab_procedural` node instead of every strand, and
Arnold evaluates the groom at render time, motion keys included. Re-run it after editing the
groom. `groomlab.render_curves(groom)` switches back. Both are also under GroomLab > Export.

The procedural needs only Arnold. It works in kick and on farms without Maya:

```bash
kick -i shot.0010.ass -l C:/path/to/modules/GroomLab/procedurals
```

- **Install:** the module ships `GroomLab/procedurals/groomlab_procedural.dll`, with TBB beside
  it, and puts that folder on `ARNOLD_PLUGIN_PATH`. `GroomLab/bin` holds `groomlab-cli`.
- **Hand-written scenes:** `.ass` files can use the procedural directly:

```
groomlab_procedural
{
 name hair
 filename "C:/shots/010/hair.groom"
 frame 10
 motion_start -0.25
 motion_end 0.25
 motion_keys 3
 shader "hairShader"
}
```

- **Parameters:** `fps` (0 uses the file's), `density`, `mode`, `min_pixel_width`,
  `width_scale` and `length_scale`, plus the LOD parameters (`lod_distance`, `lod_near`, `lod_far`,
  `lod_min_density`, `lod_width_compensation`, `camera_cull`, `cull_margin`, `backface_cull`,
  `backface_angle`).
- **Shape parameters:** `shader` is the shape's usual `NODE[]` (one shader, as above; `kick
  -info groomlab_procedural` lists it). It and the other shape parameters the procedural
  carries (`visibility`, `sidedness`, `matte`, `opaque`, `receive_shadows`, `self_shadows`,
  `light_group`/`use_light_group`, `shadow_group`/`use_shadow_group`, `trace_sets`, `id`,
  `transform_type`) pass to the curves it makes.

## Rendering with RenderMan

The module ships `GroomLab/procedurals/renderman/groomlab_renderman.dll`, a RenderMan 27
procedural that grows a groom file at render time in prman, without Maya, with primvars for a
PxrHairColor + PxrMarschnerHair network (`groom.rib` beside it is a complete example scene). RIB
files call it in either of RenderMan's two procedural forms:

```
Procedural2 "DynamicLoad" "DSOBound" "string dsoname" ["groomlab_renderman"]
    "string filename" ["C:/shots/010/hair.groom"] "float frame" [12]

Procedural "DynamicLoad" ["groomlab_renderman" "-file 'C:/shots/010/hair.groom' -frame 12"]
    [-100 100 -100 100 -100 100]
```

The second, older form takes its parameters as one data string of `-key value` pairs: `-file`,
`-frame`, `-fps`, `-density`, `-widthScale`, `-lengthScale`, `-motionStart`, `-motionEnd`,
`-motionKeys`, `-searchPaths`, `-name`, `-debug`. A value with spaces goes in quotes. An unknown key
is skipped with a warning. docs/DEVELOPING.md, "RenderMan procedural", has the details.

### RenderMan for Maya

This needs RenderMan for Maya 27 on Maya 2024 to 2026 (rendered in all three with RenderMan 27.3).
RenderMan for Maya has no translator API, so it renders the groom through its own generic
**RenderManProcedural** node, which GroomLab keeps in place:

1. Load RenderMan for Maya. While it is loaded, every groom gets `<groom>_rman`, a
   RenderManProcedural node linked to the groom (`groomlab.sync_renderman`, run when a scene opens
   or is imported, when RenderMan for Maya loads, and when a groom is made or deleted). The node:
   - points `DSO Name` at the module's `groomlab_renderman.dll`;
   - sets `Data` to the groom's settings;
   - takes its material from the groom: a RenderMan material assigned to the groom in the
     Hypershade, else a PxrMarschnerHair network reading the procedural's primvars;
   - is visible to reflections and refractions (RenderMan's indirect rays: light between strands).
   A node whose groom is gone is deleted. **GroomLab > Render > Select RenderMan Procedural**
   selects the selected groom's node.
2. Render, interactively or in a batch render or on a farm: the scene needs nothing saved for it.
   Before RenderMan for Maya translates the node at each frame, its **Pre Shape Python Script**
   writes the groom to a groom file at that frame:
   `<scene folder>/groomlab/<scene>/renders/renderman/<groom>.<frame>.groom`. If the groom renders
   from its own groom file (**Render > Render from Groom File**, which caches the scalp's
   animation), it uses that file instead. Then it puts `-file` and `-frame` into `Data`, plus the
   shutter as motion keys while 3D motion blur is on in the render settings.
3. Edit `Data` for the rest, for example `-density 0.5 -widthScale 1.2 -debug`. The script only
   replaces `-file`, `-frame` and the motion keys, and keeps your keys. Set the node's
   `groomLabShutterFromGlobals` off to set the motion keys yourself.

The groom's own shape renders nothing in RenderMan for Maya. Without a scalp cache, a moving scalp
doesn't blur: each frame's file holds the scalp at that frame. Use **Render from Groom File** for
deformation blur. On Maya 2022, or with RenderMan for Maya not loaded, nothing is made and the menu
item doesn't show.
