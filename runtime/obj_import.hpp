#pragma once
#include "runtime.hpp"
#include <string_view>

// Editor and exporter only: devices get the meshes already converted in the .jetpack.
namespace jetstudio{

    // Options of a model's .import file, as in Godot's import dock.
    struct ObjImportOptions{
        // OBJ units to metres
        float scale = 1;
        // for exports with a mirrored coordinate system
        bool mirror_x = false;
        // for inward-facing normals/winding
        bool flip_faces = false;
        // texture V already top-down
        bool flip_v = false;
    };

    struct ObjMaterial{
        std::string name;
        // Kd
        uint32_t color = 0xffffff;
        // map_Kd, relative to the MTL file
        std::string texture;
        // d / 1 - Tr
        float alpha = 1;
        // Ns
        float shininess = 0;
    };

    // One part per OBJ object and material: becomes one MeshInstance3D.
    struct ObjPart{
        std::string name, object, material;
        MeshAsset mesh;
    };

    bool importObj(std::string_view obj, std::string_view mtl, const ObjImportOptions& options,
                    std::vector<ObjPart>& parts, std::vector<ObjMaterial>& materials, std::string& error);

}
