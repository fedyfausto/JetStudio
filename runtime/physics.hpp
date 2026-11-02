#pragma once
#include "runtime.hpp"
#include <vector>

// Arcade physics behind Godot's body nodes. Rigid boxes get pushed around but never tumble, spheres roll.
// 2D bodies use the same code on the z = 0 plane, in canvas pixels with y down: a rectangle is a box as deep
// as needed never to separate along z, a circle a sphere, and a 2D shape only meets other 2D shapes.
namespace jetstudio::physics{

    constexpr float Step = 1.0f / 60.0f;
    // m/s², down (Godot's default)
    constexpr float Gravity = 9.8f;
    // Godot's project defaults, added to each body's own damping ("Combine" mode).
    constexpr float DefaultLinearDamp = 0.1f, DefaultAngularDamp = 0.1f;

    // Units of a world: 3D in metres with y up, 2D in pixels with y down (Godot's 980 px/s² gravity, so 100 px a metre).
    struct Space{
        Vec3 up, gravity;
        // lengths per metre, for the tolerances
        float unit;
        // how far move_and_slide() follows the floor down
        float snap;
    };
    constexpr Space Space3D{{0, 1, 0}, {0, -Gravity, 0}, 1, 0.2f};
    constexpr Space Space2D{{0, -1, 0}, {0, Gravity * 100, 0}, 100, 8};

    struct Shape{
        CollisionKind kind = CollisionKind::Box;
        // box/sphere centre, capsule segment midpoint
        Vec3 center;
        // orthonormal; capsule axis = column 1
        Basis axes;
        // box half extents
        Vec3 half;
        // sphere and capsule
        float radius = 0;
        // capsule: half length of the segment between the caps
        float halfSegment = 0;
        // bounding box
        Vec3 lo, hi;
        // owner body/area and CollisionShape3D (node indices)
        int body = -1, node = -1;
        // Copied from the owner, so pair tests do not touch the nodes.
        NodeType owner = NodeType::StaticBody3D;
        uint32_t layer = 1, mask = 1;
        // 2D
        bool flat = false;
        // 2D: blocks only what comes from its up side (local -y)
        bool oneWay = false;
        // relative to the body
        Transform local;
        // world transform of the last place()
        Transform placed;
        // Authored size, for placing the shape again after the body moves.
        Vec3 size{1, 1, 1};
        float authoredRadius = 0.5f, authoredHeight = 2;
    };

    struct Contact{
        // pushes the first shape out of the second
        Vec3 normal;
        float depth = 0;
    };

    void place(Shape& shape, const Transform& world);
    bool overlapBounds(const Shape& a, const Shape& b);
    bool collide(const Shape& a, const Shape& b, Contact& out);

    // A pair collides when either mask sees the other's layer (and both are 2D or both 3D).
    inline bool masksMatch(const Shape& a, const Shape& b){ return a.flat == b.flat && ((a.mask & b.layer) || (b.mask & a.layer)); }

    // Static, Character, Rigid, 3D and 2D
    bool isBody(NodeType type);
    // bodies and areas
    bool isCollisionObject(NodeType type);
    bool isArea(NodeType type);
    bool isRigid(NodeType type);
    bool isCharacter(NodeType type);
    bool isCollisionShape(NodeType type);
    // the 2D bodies, areas and shapes
    bool isFlat(NodeType type);
    inline const Space& spaceOf(NodeType type){ return isFlat(type) ? Space2D : Space3D; }

    struct World{
        std::vector<Shape> shapes;
        // the list matches the nodes (freed, disabled, layers)
        bool built = false;
        // shapes are where their nodes are
        bool valid = false;
    };

}
