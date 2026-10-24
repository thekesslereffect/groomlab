# An Unreal hair material for GroomLab's colors and iridescence

A recipe for one material in Unreal Engine 5 that shows a GroomLab groom's colors as the viewport,
the app and Arnold show them: Bands' bands, FeatherColor's feather patterns and HairColor's tip
colors (all of them the groom's per-point color), and FeatherColor's iridescence (structural
color), for strands and for cards. The
same graph, node by node, is in [`tools/unreal/groomlab_hair_material.json`](../tools/unreal/groomlab_hair_material.json)
(names, values, links), for a script or a person to rebuild it.

## What the groom carries

| Export | Attribute or map | What it holds |
| --- | --- | --- |
| Strands (`groomlab.export_unreal_groom`, the app's Unreal groom) | `groom_color` | The color per vertex: bands, patterns, tips as the color nodes wrote them (else each strand's color repeated). Unreal's **Hair Attributes > Color**. |
| | `groom_iridescence` | How much structural color, per vertex (0-1). GroomLab's own attribute: Unreal's importer keeps only the attributes it knows. |
| | `groom_ao` (with `iridescence_as_ao=True`, Maya's `-iridescenceAo`, the app's **Iridescence as AO**) | The same iridescence, where Unreal's **Hair Attributes > AO** hands it to a material. Use it only for this: GroomLab writes no ambient occlusion of its own. |
| | `groom_roughness` (with a HairColor's roughness set) | HairColor's `roughness` per strand (0-1), which Unreal reads per curve. |
| Cards (`export_cards_unreal`, a bake) | `_Color` RGB | The strands' color, bands and patterns baked in (sRGB). |
| | `_Color` alpha | The iridescence, baked (255 full); the map is RGBA only when the groom has any. |
| | vertex color | Each card's tint, half its value (0.5: none): multiply the Color map by 2 x it. |

The iridescence's own color and its hue shift are the FeatherColor node's `iridescenceColor` and
`iridescenceShift`: material parameters here (the groom file holds them; type them in once). The
roughness is the HairColor node's `roughness` (0.5 without one).

## The strand material

1. New Material, **Shading Model** Hair, **Used with Hair Strands** on.
2. Parameters: `IridescenceColor` (Vector, FeatherColor's `iridescenceColor` in linear RGB),
   `IridescenceShift` (Scalar, FeatherColor's `iridescenceShift`, in turns), `Roughness` (Scalar:
   `0.1 + 0.4 x` HairColor's `roughness`, as `create_hair_shader` maps it; 0.3 without one).
3. **Hair Attributes** node: `Color` is the groom's color, `AO` the iridescence (exported as AO).
4. The angle: GroomLab turns the hue as the view goes from facing a strand's normal (a feather's
   vane) to seeing it edge-on. Unreal gives a hair material the strand's tangent, not that
   normal, so the recipe takes the view's angle to the fiber instead: `Grazing = abs(dot(CameraVector,
   HairAttributes.Tangent))` (0 across the fiber, 1 along it). On a feather lying flat the two
   agree face-on and part as it turns; the shift reads the same way, its stripes fall elsewhere.
5. The structural color: Engine's **HueShift** material function with `Texture` =
   `IridescenceColor` and `Hue Shift Percentage` = `IridescenceShift x Grazing` (HueShift turns by
   a share of the circle: GroomLab's turns).
6. **Base Color** = `Lerp(A: HairAttributes.Color, B: HueShift, Alpha: HairAttributes.AO)`.
   Unreal's hair highlight takes no tint, so it stays white where the viewport, the app and
   Arnold tint it toward the structural color too: the one difference.
7. **Roughness** = `Roughness`. Assign the material to the groom (its group: a feather design's
   group gets one of its own).

Without iridescence, steps 4 to 6 drop out: Base Color is Hair Attributes' Color. (The pins'
names are those of Unreal 5's Hair Attributes node: Color is its base color output, Tangent the
strand's world tangent.)

## The card material

1. New Material, **Shading Model** Hair, **Blend Mode** Masked, **Two Sided** on, **Used with
   Static Lighting**/**Skeletal Mesh** as the cards are meshed.
2. `ColorMap` = the `_Color` texture (sRGB), `CoverageMap` = `_Coverage` (linear, **Opacity
   Mask**), `TangentMap` = `_Tangent` (linear, **Tangent**: `x 2 - 1`). (The Hair shading model's
   Normal pin is this tangent; `_Normal`, the strands' normal with DirectX's green, is for a card
   material with another shading model, Default Lit for a mobile or stylized look, into
   **Normal** with Normal map compression.)
3. `CardColor = ColorMap.RGB x 2 x VertexColor.RGB` (each card's own color over its shared texture's).
4. The iridescence is `ColorMap.A`; the angle as for strands, with the card's tangent from
   `TangentMap` (transformed tangent to world) or, simpler, the card's own normal:
   `Grazing = 1 - abs(dot(CameraVector, VertexNormalWS))`, which is GroomLab's own measure (a
   feather card is its vane laid flat).
5. **Base Color** = `Lerp(CardColor, HueShift(IridescenceColor, IridescenceShift x Grazing), ColorMap.A)`.

## Checking it

Render the same feather in Arnold (`create_hair_shader`, which builds the same mix as a ramp) and
in Unreal from two angles: face-on the vane shows `iridescenceColor`, tilted its hue turns by up
to `iridescenceShift`. `tests/hosts/maya/test_arnold.py`'s `test_bands_and_iridescence_render`
checks what the groom carries; the attributes are read back in `tests/engine/io/test_alembic.cpp`
(`UnrealCarriesBandsPerPointAndTheIridescence`).
