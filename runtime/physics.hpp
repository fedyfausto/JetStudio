#pragma once
// Arcade physics behind Godot's body nodes. Rigid boxes get pushed around but never tumble, spheres roll.
#include "runtime.hpp"
#include <vector>

namespace jetstudio::physics {

constexpr float Step = 1.0f / 60.0f;
constexpr float Gravity = 9.8f; // m/s², down (Godot's default)
// Godot's project defaults, added to each body's own damping ("Combine" mode).
constexpr float DefaultLinearDamp = 0.1f, DefaultAngularDamp = 0.1f;

struct Shape {
    CollisionKind kind = CollisionKind::Box;
    Vec3 center;            // box/sphere centre, capsule segment midpoint
    Basis axes;             // orthonormal; capsule axis = column 1
    Vec3 half;              // box half extents
    float radius = 0;       // sphere and capsule
    float half_segment = 0; // capsule: half length of the segment between the caps
    Vec3 lo, hi;            // bounding box
    int body = -1, node = -1; // owner body/area and CollisionShape3D (node indices)
    // Copied from the owner, so pair tests do not touch the nodes.
    NodeType owner = NodeType::StaticBody3D;
    uint32_t layer = 1, mask = 1;
    Transform local;        // relative to the body
    Transform placed;       // world transform of the last place()
    // Authored size, for placing the shape again after the body moves.
    Vec3 size{1, 1, 1};
    float authored_radius = 0.5f, authored_height = 2;
};

struct Contact {
    Vec3 normal;    // pushes the first shape out of the second
    float depth = 0;
};

void place(Shape& shape, const Transform& world);
bool overlap_bounds(const Shape& a, const Shape& b);
bool collide(const Shape& a, const Shape& b, Contact& out);

// A pair collides when either mask sees the other's layer.
inline bool masks_match(const Shape& a, const Shape& b) { return (a.mask & b.layer) || (b.mask & a.layer); }

bool is_body(NodeType type);         // Static, Character, Rigid
bool is_collision_object(NodeType type); // bodies and Area3D

struct World {
    std::vector<Shape> shapes;
    bool built = false; // the list matches the nodes (freed, disabled, layers)
    bool valid = false; // shapes are where their nodes are
};

} // namespace jetstudio::physics
