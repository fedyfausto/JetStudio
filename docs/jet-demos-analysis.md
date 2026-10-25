# Analysis: JetExamples demos in Jet Studio

Source analyzed: `third_party/JetExamples` (commit 57b05a2, MIT license, CubeCoders), in
particular `esp32-textured-boxes` and `esp32-neon-film` ("ESP 88"). The Jet revision used
by the examples (19018b5) is an ancestor of ours (c56dfc0): **all the renderer features
used by the demos already exist in our Jet**. What is missing is exposing them in the editor,
the project format, the runtime and JetScript.

## What the two demos do

**Textured boxes**: a cube with a 128×128 texture (crate, CC0). Every 3 seconds it switches
between affine/perspective UVs and nearest/bilinear sampling. Needs: RGB565 textures, primitive
UVs, `material.perspectiveCorrect`, `texture.bilinear`, gradient background, 2D labels.

**Neon film**: a 116 s short film in 12 shots, entirely procedural C++ code
(`Film.hpp`, `World.hpp`, `City.hpp`, `Vehicle.hpp`, `RoadTrack.hpp`, ~62 KB) plus generated
assets (`Assets.hpp`, palette textures produced by `tools/prepare_assets.py`).

| Demo element | Jet feature used | Status in Jet Studio |
| --- | --- | --- |
| Night sky | `Scene::backgroundGradientColors` (one color per row) | missing (fixed color only) |
| Reflective river | `ShadingMode::WATER_REFLECT`, `waterYBias`, `waterReflectionMaxY`, previous field (`SSR_FIELD_REFLECT`) | missing |
| Facades, signs, shops, dashboard | 8-bit palette textures (`Texture::palette`), UVs, `perspectiveCorrect` | missing (TEXTURE_MAPPING=0) |
| Car windows | environment mapping (`environmentReflectionUV`, Phong + specular) | missing |
| Spotlight beams, halos | `ShadingMode::ADDITIVE`, material alpha | missing |
| Holograms | billboards (`createBillboard`, `isBillboard`), textures with transparency | missing |
| Streetlight/headlight glows | additive `Sprite2D` projected from 3D points | missing |
| Fades, letterbox bars, end credits | full-screen `Sprite2D` with alpha, bitmap mask | missing (no 2D) |
| Rain and splashes | `ParticleSystem` (Splash) + triangles drawn by hand every frame | missing |
| Flashing signs, red scanner | animation of `material->alpha`/`color` from code | partial (only the mesh `color` from scripts) |
| Speedometer 70→88 | dynamic texture rewritten from code | missing |
| Beveled buildings, car, wheels, roads | procedural geometry (quads, lofts, discs) | missing: only 6 primitives |
| Endless road | recycling the objects behind the camera | possible with JetScript |
| LOD of distant shops, streetlight fade | `lodMeshes`, `lodScale`, `fadeNear/fadeFar` | missing |
| Correct car/wheel sorting | `preciseDepthSort`, 128 buckets, no z-buffer | missing (we use a z-buffer) |
| Neon always ≥2 pixels | vertices recomputed every frame from the projection | missing (needs a mesh API) |
| Editing, lenses, camera moves | C++ timeline, animated FOV | partial (scripts, no timeline) |
| 480×320 at 60 fields/s | half-width, field buffers, overlapped DMA, 2 raster cores (`components/esp32_jet`) | missing: full 320×240, simple loop |

**Hardware**: the demo uses an ST7796 480×320 at 80 MHz; our board has an ILI9341 320×240
(4:3). At 320×240 the shots have to be recomposed (its 3:2 becomes 4:3).

## Neon car ("Neon Motorworks")

A 20 s orbit around a race car in a neon workshop (`esp32-neon-car/main/Workshop.hpp`,
~8 KB of code). It is the right demo to port first: small, but it exercises model loading
and materials.

| Element | How the demo does it | Status in Jet Studio |
| --- | --- | --- |
| Car | Blender OBJ + MTL (404 triangles, 7 objects) loaded with `ObjLoader.h`; positions mirrored in X and normals flipped because the export was inside out | model import **missing** |
| Per-part materials | Phong body (diffuse 165, specular 205, exponent 32), Unlit wheels, separate windows | **missing**: a single primitive mesh per node; Phong exists, the diffuse coefficient is missing |
| Livery | 1.7 MB PNG reduced to 256×256, flipped vertically (OBJ UVs) and quantized to 256 colors (64 KiB + palette) | partial: automatic palette only if the image already has ≤256 colors; reduction, quantization and flip are missing |
| Chrome windows | 128×64 panorama of the workshop sampled with `environmentReflectionUV` (UVs recomputed per vertex every frame, seam handled) | **missing** (Jet's native `reflectionMap` flag is only a screen-space effect) |
| Workshop | quads, boxes, 32-segment disc with a cyan/pink neon rim, grid of thin lines, contact shadow; all Unlit | doable with primitives + Unlit, but Quad and Torus are missing and it gets awkward: better to import it as OBJ or with instances |
| Lights | directional (205,231,255) and purple ambient (130,123,150) | present |
| Camera | orbit with oscillating radius and height, `lookAt` on the center, 62° horizontal FOV | present with JetScript (`position`, `look_at`); equivalent vertical FOV ≈ 43.6° |
| "NEON MOTORWORKS" titles | two Sprite2D with pre-rasterized text | 2D over the scene (Label, TextureRect) **missing** |
| 60 fields/s on the S3 | 480×320 half-width fields, sorting without z-buffer, 2 cores, meshes in PSRAM | performance profile missing; already fine on the PC |

**License**: the car model and livery have no license ("No separate asset licence was supplied"):
they can be used locally from the JetExamples clone, but must not be included in the repository.
The rest (workshop, panorama, code) is MIT or has to be recreated by us.

### Status: ported (example `examples/neon_car`)

Points 1-6 below are done. `./jet-studio examples` generates the project (`tools/neon_car.py`)
and copies the car locally from JetExamples; the scene reproduces the workshop, lights, camera
orbit (`scripts/orbit.jet`) and titles. Differences from the demo:

- 320×240 (4:3) resolution instead of 480×320: 48.5° vertical FOV for the same 62° horizontal
  field of view, titles with the runtime's bitmap font instead of pre-rasterized bitmaps;
- z-buffer instead of the demo's sorting without z-buffer, and no fields;
- the platform is a 32-sided cylinder of a single color (the demo has a darker rim);
- the windows' panorama is generated by us with the same idea (neon ribs, panels);
- car conversion: the demo mirrors X after loading the model "raw" into Jet's left-handed
  space; here the import converts correctly, so Flip Faces (the file has faces and normals
  pointing inward) and a 180° rotation in Y are enough. Scale 0.02 m per OBJ unit.

Performance on the PC: ~1.5 ms per frame. On the Nano (320×240, z-buffer, display disconnected):

| | frame | raster | display transfer |
| --- | --- | --- | --- |
| first firmware (one core, blocking transfer) | 148.8 ms | 102.7 ms | 31.0 ms |
| raster on two cores + parallel transfer | 82 ms (12 FPS) | 66 ms | hidden |
| demo profile: half-width fields in internal RAM, no z-buffer, DMA queue | **16.2 ms (~62 fields/s)** | 5.6 ms | 16.3 ms, in parallel |

The cost was filling the large surfaces with color and z-buffer in PSRAM (the Phong lighting of
the car body makes no difference). With the demo profile (`firmware/config/JetConfig.hpp`) each
field has a quarter of the pixels of a full screen, fits in internal RAM and the DMA reads it
directly; the room is *Draw Order: Background*. Now the limit is the 40 MHz SPI.

### What was needed, in order

1. **Godot-style OBJ/MTL import**: dragging an `.obj` into the scene creates a Node3D with one
   MeshInstance3D per part (object × material), each with its own material taken from the
   MTL. Import options in the Inspector when the file is selected (scale, mirror X, flip faces,
   flip V), saved in `file.obj.import` as in Godot. The runtime and the `.jetpack` carry the
   meshes; imported meshes are mirrored in Z for Jet, with reversed winding.
2. **Texture import**: maximum size, quantization to 256 colors (palette with controlled loss,
   like the demo), vertical flip. Same `.import` settings.
3. **Material**: diffuse coefficient; environment reflection (environment map) with per-vertex
   UVs recomputed every frame from camera, position and normal.
4. **Quad and Torus primitives** (Godot's QuadMesh and TorusMesh) for neon strips and rings.
5. **2D over 3D**: Label with a bitmap font built into the runtime (text editable from scripts,
   so also HUDs), TextureRect and ColorRect with transparency.
6. **`neon_car` example project**: recreated workshop, panorama generated by us, script-driven
   orbit, titles; the car is imported from the local JetExamples clone.
7. Later, with the hardware: performance profile (sorting without z-buffer, half-width,
   possibly 480×320) to get closer to the demo's 60 fields/s.

## How to bring it into the editor (roadmap)

1. **Textures, materials and sky** (done, see README: `textured_boxes` and `first_world` examples)
   - Import of project PNGs as RGB565 or, if there are ≤256 colors, as a lossless 8-bit palette;
     transparent pixels as a key color.
   - Complete material on the MeshInstance3D: albedo texture, UV scale/offset, nearest/
     bilinear filter, perspective UVs, shading (Unshaded, Flat, Gouraud, Phong, Additive, Water),
     alpha, specular, culling, billboard.
   - WorldEnvironment sky: color, gradient in the style of Godot's ProceduralSky (sky/horizon/
     ground, the horizon follows the camera's tilt) and panorama from a texture.
   - Textures in the `.jetpack` and in the firmware.
2. **Meshes and scene structure**
   - OBJ import (done: our own importer with parts and materials, not Jet's loader).
   - Procedural mesh API in JetScript, modeled on SurfaceTool.
   - Scene instances (PackedScene).
   - Higher node limits and merging of static parts.
   - LOD and visibility distance.
3. **2D and effects**
   - ColorRect, TextureRect and bitmap Label (done); additive Sprites attached to 3D points are missing.
   - Particles (rain, sparks).
   - Lens flare, water reflection with the previous frame (environment mapping done).
4. **Animation and direction**
   - AnimationPlayer with keyframes on properties, camera and FOV.
   - Scene changes from scripts and an API for dynamic materials and textures.
5. **Device performance**
   - Per-project rendering profile: resolution and display (ILI9341 or ST7796), z-buffer or
     bucket sorting, half-width/fields.
   - S3 runtime with DMA and two raster cores (from `components/esp32_jet`).
   - Textures read directly from flash.
6. **Porting "ESP 88"**
   - Extract the textures from `Assets.hpp` into PNGs (MIT, with attribution).
   - Generate buildings, car and roads with a converter that runs the demo's C++ builders
     on the PC and exports meshes, or with the JetScript mesh API.
   - One scene per shot and a timeline with AnimationPlayer.
   - Scripts for road recycling, neon, rain, flashing lights and the speedometer.

A pixel-perfect reproduction also requires the same renderer configuration
(480×320, sorting without z-buffer, alternating fields for the water). Without phase 5 the
demo can be viewed in the editor and on the PC player, but not at the same performance on the Nano.
