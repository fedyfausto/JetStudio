#pragma once
// Arcade physics behind Godot's body nodes. Rigid boxes get pushed around but never tumble, spheres roll.
// 2D bodies use the same code on the z = 0 plane, in canvas pixels with y down: a rectangle is a box as deep
// as needed never to separate along z, a circle a sphere, and a 2D shape only meets other 2D shapes.
#include "runtime.hpp"
#include <vector>

namespace jetstudio::physics {

constexpr float Step = 1.0f / 60.0f;
constexpr float Gravity = 9.8f; // m/s², down (Godot's default)
// Godot's project defaults, added to each body's own damping ("Combine" mode).
constexpr float DefaultLinearDamp = 0.1f, DefaultAngularDamp = 0.1f;

// Units of a world: 3D in metres with y up, 2D in pixels with y down (Godot's 980 px/s² gravity, so 100 px a metre).
struct Space {
    Vec3 up, gravity;
    float unit; // lengths per metre, for the tolerances
    float snap; // how far move_and_slide() follows the floor down
};
constexpr Space Space3D{{0, 1, 0}, {0, -Gravity, 0}, 1, 0.2f};
constexpr Space Space2D{{0, -1, 0}, {0, Gravity * 100, 0}, 100, 8};

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
    bool flat = false;    // 2D
    bool one_way = false; // 2D: blocks only what comes from its up side (local -y)
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

// A pair collides when either mask sees the other's layer (and both are 2D or both 3D).
inline bool masks_match(const Shape& a, const Shape& b) { return a.flat == b.flat && ((a.mask & b.layer) || (b.mask & a.layer)); }

bool is_body(NodeType type);             // Static, Character, Rigid, 3D and 2D
bool is_collision_object(NodeType type); // bodies and areas
bool is_area(NodeType type);
bool is_rigid(NodeType type);
bool is_character(NodeType type);
bool is_collision_shape(NodeType type);
bool is_flat(NodeType type); // the 2D bodies, areas and shapes
inline const Space& space_of(NodeType type) { return is_flat(type) ? Space2D : Space3D; }

struct World {
    std::vector<Shape> shapes;
    bool built = false; // the list matches the nodes (freed, disabled, layers)
    bool valid = false; // shapes are where their nodes are
};

} // namespace jetstudio::physics
