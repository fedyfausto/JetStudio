#pragma once
// Device Jet config, the same profile as CubeCoders' JetExamples demos.
// Half-width interlaced fields, no z-buffer (depth-sorted triangles and draw bands), FAST_Z, nearest sampling, no picking.
// Derived from runtime/config/JetConfig.hpp (editor and desktop player), keep the two in sync.
#define JET32_WORLD_SCALE 4
#define JET_MESH_INSTANCING 1
#define RENDER_TILE_BUFFER 0
#define TILE_WIDTH 16
#define TILE_HEIGHT 16
#define FAST_Z 1
#define LAZY_Z 0
#define SCREEN_DOOR_ALPHA 0
#define SKIP_ZERO_AREA_TRIANGLES 1
#define NOISE_ALPHA 0
#define Z_BUFFERING 0
#define SORT_TRIANGLES 1
#define SORT_SCENE_OBJECTS 0
#define SORT_SCENE_REVERSE 0
#define DEPTH_ALPHA_BLEND 0
#define TEXTURE_MAPPING 1
#define PERSPECTIVE_CORRECT_TEXTURES 1
#define BILINEAR_FILTER 0
#define LIGHTING 1
#define Z_BRIGHTNESS 0
#define FLOAT_CAMERA_ANGLES 1
#define FLOAT_SIN_CACHE_SCALE 10
#define FLOAT_TAN_CACHE_SCALE 1
#define HALF_WIDTH_BUFFERS 1
#define FIELD_BUFFERS 1
#define SSR_FIELD_REFLECT 1
#define POSTFX_CRT 0
#define POSTFX_CELLSHADING 0
#define POSTFX_ANTIALIASING 0
#define POSTFX_BLOOM 0
#define POSTFX_MOTION_BLUR 0
#define POSTFX_CHROMATIC 0
#define POSTFX_PIXELATE 0
#define CRT_SCANLINE_INTENSITY 48
#define MOTION_BLUR_STRENGTH 50
#define CHROMATIC_OFFSET 2
#define PIXELATE_SIZE 4
#define CELLSHADING_CELL_BITS 4
#define DEBUG_OVERDRAW 0
#define zBrightNear 200
#define zBrightFar 1600
#define zBrightScale 48
#define depthFogNear 6144
#define depthFogFar 8192
#define CHECKERBOARD_MODE 0
#define CHECKERBOARD_RECONSTRUCTION 0
#define MAX_PICK_QUERIES 0
// Keep Jet's transform scratch in internal RAM, like the neon-car demo.
#define JET_S3_TRANSFORM_INTERNAL_BYTES 32768

#if defined(ESP_PLATFORM)
#define ESP32
#endif
