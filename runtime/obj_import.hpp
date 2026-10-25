#pragma once
// Editor and exporter only: devices get the meshes already converted in the .jetpack.
#include "runtime.hpp"
#include <string_view>

namespace jetstudio {

// Options of a model's .import file, as in Godot's import dock.
struct ObjImportOptions {
    float scale = 1;        // OBJ units to metres
    bool mirror_x = false;  // for exports with a mirrored coordinate system
    bool flip_faces = false; // for inward-facing normals/winding
    bool flip_v = false;    // texture V already top-down
};

struct ObjMaterial {
    std::string name;
    uint32_t color = 0xffffff; // Kd
    std::string texture;       // map_Kd, relative to the MTL file
    float alpha = 1;           // d / 1 - Tr
    float shininess = 0;       // Ns
};

// One part per OBJ object and material: becomes one MeshInstance3D.
struct ObjPart {
    std::string name, object, material;
    MeshAsset mesh;
};

bool import_obj(std::string_view obj, std::string_view mtl, const ObjImportOptions& options,
                std::vector<ObjPart>& parts, std::vector<ObjMaterial>& materials, std::string& error);

} // namespace jetstudio
