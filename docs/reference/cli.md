# Groom files and the CLI

Groom files, groomlab-cli, and importing and reloading groom files in Maya.

User guide: [The .groom format](https://www.getgroomlab.com/docs/groom-format), [Python API](https://www.getgroomlab.com/docs/python-api), [Use GroomLab from AI agents](https://www.getgroomlab.com/docs/ai-agents).

## Groom files and the CLI

A groom file (`.groom`, JSON) holds everything a groom evaluates from: the graph, guide
hierarchy, correctives, parts, flow, painted maps, connected guides and the scalp, with its UVs,
color sets and rest pose, plus the last card bake. An animated scalp goes in an Alembic cache
next to it. It is the interchange between Maya and the standalone app (both open and save it),
and `groomlab-cli` evaluates groom files without Maya, for farms and other hosts:

```python
groomlab.export_groom(groom, "C:/shots/010/hair.groom", scalp_cache=True, start=1, end=48)
# -> hair.groom and hair.scalp.abc (AbcExport of the scalp, world space)
```

```bash
groomlab-cli info hair.groom
groomlab-cli eval hair.groom -o hair.abc -f 1-48 --density 1
groomlab-cli eval hair.groom -o hair.usda -f 1-48 --step 0.5
```

- **Matching:** the CLI matches Maya's own export frame for frame, subframes included, bit for
  bit on one machine: `groomlab-cli hash file.abc` prints an exact hash per frame of the strand
  IDs, points and widths (and one of the file), and the pipeline test compares the two exports
  by it. Across platforms or compilers the strands agree within a small tolerance (`pow`, `sin`
  and the like may differ in the last bits).
- **Units and up axis:** the file records its scene unit (`"units": "cm"` by default) and the
  world's up axis (`"upAxis": "Y"`, or `"Z"` from a Z-up scene) so another host knows what the
  scalp and guides are in. Maya and the app are Y-up; a Z-up file comes in turned a quarter
  about X (the scalp, its rest pose and the guides; world directions in the graph's params, a
  Gravity's or a Wind's, are not turned). Only centimeter files import into Maya.
- **Subframes:** between cache samples the scalp is blended linearly. Cache subframes (`step`)
  when the motion between frames matters.
- **Texture maps:** a painted texture map's image is named relative to the groom file (where
  every reader and the app look first); other files the graph names (texture params,
  `ImportCurves`) keep their paths, resolved through the file's search paths (the project and
  scene folders), so they must be reachable from where the file is evaluated.
- **Card bake:** a groom with a bake writes it beside the file as `groomlab/<name>/cards/<name>_*` (the
  TGA maps, the atlas and a preview: the app's layout) and names it with its stamp
  (`"cardBake"`), so the app and an import keep the cards' atlas cells and the stale check.
- **Flat guides:** connected guide curves are stored at the exported frame. They don't animate
  in the file.
- **Cache paths:** paths are stored relative to the groom file when the caches sit beside it.
  The scalp cache is written with its UVs and color sets, so an import takes its mesh as the
  scalp.
- **Command:** `groomLabExportGroom -file f [-scalpCache abc -scalpObject name]` writes the file
  alone.
- **Older files:** a `"license"` field written by versions before GroomLab was free is ignored
  on load.

### Importing and reloading groom files

GroomLab > Groom Files > **Import Groom File...** (`groomlab.import_groom`) makes a groom from a groom file,
one Maya exported or the standalone app saved; **Reload from Groom File**
(`groomlab.reload_groom`, also the Attribute Editor's **Reload** beside the linked file) brings a
file's edits back into the same node; **Save to Groom File** (`groomlab.save_groom`, the
Attribute Editor's **Save**) writes the groom back to the file it is linked to. Together they
make the round trip Maya -> app -> Maya:

```python
groom = groomlab.import_groom("C:/shots/010/hair.groom")   # a new groom (and its scalp)
groomlab.save_groom(groom)                                 # back to hair.groom, for the app
groomlab.reload_groom(groom)                               # the app's edits, one undo step
```

- **What comes in:** the graph (nodes, params, ramps, maps on params, bypass, param inputs,
  controls with their values), the guide hierarchy with its groups and layers, correctives
  (each layer's weight on the groom's transform, keyed as the file's weight curve), parts, flow,
  followScalp and the painted maps: vertex maps into the groom's `scalpMaps`, a texture map's
  image copied into the scene's masks folder for the groom (linked in place when it is that
  folder's already, as after a save and a reload). The card bake the file names is copied into
  the groom's cards folder, its stamp kept: the cards keep their atlas and are not stale.
- **The scalp:** a mesh passed (or selected) must be the file's scalp: as many vertices and the
  same faces, since the guides are rooted on its triangles. Another mesh is refused with the
  reason; import without one (the file's scalp is made, with its UVs and color sets, and a
  hidden rest mesh when it has a rest pose) and move the groom with Utilities > **Transfer
  Groom to Scalp**. Reload checks the groom's own scalp the same way and never touches it.
- **Animation:** the file's guide cache poses the groom (`guideCache`) and its driver cache moves the
  driver curves (`driverCache`; Maya's rig needs its own curves, the cache is not imported); its scalp cache is read
  with AbcImport (under `<name>ScalpCache`) and its mesh is the scalp when it is the file's,
  else it drives the scalp through a world-space blendShape. AbcImport does not undo: undoing
  an import leaves what it read, as File > Import does. Reload leaves caches alone.
- **The link:** the groom's `groomFile` names the file (as for Rendering from a groom file);
  `groomlab.groom_file(groom)` resolves it. `save_groom` keeps naming the scalp cache the file
  named before.
- **Exactness:** an exported groom imported into a new scene evaluates bit for bit as before
  (the same CLI hash), the cache's frames within float rounding; a groom the app saved
  evaluates in Maya exactly as the app evaluates it, and the other way round.
- **Command:** `groomLabImportGroom -file f` with `-info` (what the file holds, as JSON),
  `-checkScalp mesh`, `-makeScalp [-rest]`, or a groom to apply the file to (one undo step).
