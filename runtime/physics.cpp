#include "physics.hpp"
#include "script_host.hpp"
#include <algorithm>
#include <cmath>

namespace jetstudio {
namespace physics {
namespace {

float clampf(float v, float lo, float hi) { return std::max(lo, std::min(v, hi)); }

Vec3 capsule_a(const Shape& s) { return s.center + s.axes.column(1) * s.half_segment; }
Vec3 capsule_b(const Shape& s) { return s.center - s.axes.column(1) * s.half_segment; }

Vec3 closest_on_segment(Vec3 p, Vec3 a, Vec3 b) {
    const Vec3 ab = b - a;
    const float len2 = ab.dot(ab);
    const float t = len2 > 1e-12f ? clampf((p - a).dot(ab) / len2, 0, 1) : 0;
    return a + ab * t;
}

// Closest points of two segments (Ericson, Real-Time Collision Detection 5.1.9).
void closest_segments(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2, Vec3& c1, Vec3& c2) {
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = d1.dot(d1), e = d2.dot(d2), f = d2.dot(r);
    float s = 0, t = 0;
    if (a <= 1e-12f && e <= 1e-12f) { c1 = p1; c2 = p2; return; }
    if (a <= 1e-12f) {
        t = clampf(f / e, 0, 1);
    } else {
        const float c = d1.dot(r);
        if (e <= 1e-12f) {
            s = clampf(-c / a, 0, 1);
        } else {
            const float b = d1.dot(d2), denom = a * e - b * b;
            s = denom > 1e-12f ? clampf((b * f - c * e) / denom, 0, 1) : 0;
            t = (b * s + f) / e;
            if (t < 0) { t = 0; s = clampf(-c / a, 0, 1); }
            else if (t > 1) { t = 1; s = clampf((b - c) / a, 0, 1); }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
}

Vec3 closest_on_box(Vec3 p, const Shape& box) {
    const Vec3 d = p - box.center;
    Vec3 q = box.center;
    const float half[3] = {box.half.x, box.half.y, box.half.z};
    for (int i = 0; i < 3; ++i) {
        const Vec3 axis = box.axes.column(i);
        q = q + axis * clampf(d.dot(axis), -half[i], half[i]);
    }
    return q;
}

bool sphere_box(Vec3 c, float r, const Shape& box, Contact& out) {
    const Vec3 q = closest_on_box(c, box);
    const Vec3 d = c - q;
    const float dist2 = d.dot(d);
    if (dist2 > r * r) return false;
    if (dist2 > 1e-10f) {
        const float dist = std::sqrt(dist2);
        out.normal = d * (1.0f / dist);
        out.depth = r - dist;
        return true;
    }
    // Centre inside the box: leave through the nearest face.
    const Vec3 local = c - box.center;
    const float half[3] = {box.half.x, box.half.y, box.half.z};
    int best = 0;
    float best_gap = 1e30f;
    for (int i = 0; i < 3; ++i) {
        const float gap = half[i] - std::abs(local.dot(box.axes.column(i)));
        if (gap < best_gap) { best_gap = gap; best = i; }
    }
    const Vec3 axis = box.axes.column(best);
    out.normal = local.dot(axis) >= 0 ? axis : -axis;
    out.depth = r + best_gap;
    return true;
}

bool sphere_sphere(Vec3 a, float ra, Vec3 b, float rb, Contact& out) {
    const Vec3 d = a - b;
    const float dist2 = d.dot(d), r = ra + rb;
    if (dist2 > r * r) return false;
    const float dist = std::sqrt(dist2);
    out.normal = dist > 1e-6f ? d * (1.0f / dist) : Vec3{0, 1, 0};
    out.depth = r - dist;
    return true;
}

// The segment-to-box distance is convex along the segment, so a ternary search finds the closest point.
bool capsule_box(const Shape& cap, const Shape& box, Contact& out) {
    const Vec3 a = capsule_a(cap), b = capsule_b(cap);
    auto distance2 = [&](float t) {
        const Vec3 p = a + (b - a) * t;
        const Vec3 d = p - closest_on_box(p, box);
        return d.dot(d);
    };
    float lo = 0, hi = 1;
    for (int i = 0; i < 20; ++i) {
        const float m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
        if (distance2(m1) < distance2(m2)) hi = m2; else lo = m1;
    }
    float t = (lo + hi) * 0.5f;
    // Lying flat on a face, both ends are equally close: use the middle.
    if (std::abs(distance2(0) - distance2(1)) < 1e-6f && distance2(0) < cap.radius * cap.radius) t = 0.5f;
    return sphere_box(a + (b - a) * t, cap.radius, box, out);
}

// Separating axis test over the 15 axes, least penetration wins.
bool box_box(const Shape& a, const Shape& b, Contact& out) {
    const Vec3 delta = a.center - b.center;
    Vec3 axes[15];
    int count = 0;
    for (int i = 0; i < 3; ++i) axes[count++] = a.axes.column(i);
    for (int i = 0; i < 3; ++i) axes[count++] = b.axes.column(i);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const Vec3 c = a.axes.column(i).cross(b.axes.column(j));
            if (c.dot(c) > 1e-6f) axes[count++] = c.normalized();
        }
    const float ha[3] = {a.half.x, a.half.y, a.half.z}, hb[3] = {b.half.x, b.half.y, b.half.z};
    float best = 1e30f;
    Vec3 best_axis;
    for (int k = 0; k < count; ++k) {
        const Vec3 axis = axes[k];
        float ra = 0, rb = 0;
        for (int i = 0; i < 3; ++i) {
            ra += ha[i] * std::abs(a.axes.column(i).dot(axis));
            rb += hb[i] * std::abs(b.axes.column(i).dot(axis));
        }
        const float distance = delta.dot(axis);
        const float overlap = ra + rb - std::abs(distance);
        if (overlap <= 0) return false;
        // Prefer face axes on ties: edge axes give poor normals for resting boxes.
        if (overlap < best - (k >= 6 ? 1e-3f : 0)) {
            best = overlap;
            best_axis = distance >= 0 ? axis : -axis;
        }
    }
    out.normal = best_axis;
    out.depth = best;
    return true;
}

} // namespace

bool is_body(NodeType type) {
    return type == NodeType::StaticBody3D || type == NodeType::CharacterBody3D || type == NodeType::RigidBody3D;
}
bool is_collision_object(NodeType type) { return is_body(type) || type == NodeType::Area3D; }

void place(Shape& s, const Transform& world) {
    s.placed = world;
    const Vec3 sx = world.basis.column(0), sy = world.basis.column(1), sz = world.basis.column(2);
    const Vec3 scale{sx.length(), sy.length(), sz.length()};
    s.axes = world.basis.orthonormalized();
    s.center = world.origin;
    switch (s.kind) {
    case CollisionKind::Box:
        s.half = {s.size.x * 0.5f * scale.x, s.size.y * 0.5f * scale.y, s.size.z * 0.5f * scale.z};
        break;
    case CollisionKind::Sphere:
        s.radius = s.authored_radius * std::max({scale.x, scale.y, scale.z});
        break;
    case CollisionKind::Capsule:
        s.radius = s.authored_radius * std::max(scale.x, scale.z);
        s.half_segment = std::max(0.0f, s.authored_height * 0.5f * scale.y - s.radius);
        break;
    }
    Vec3 extent;
    if (s.kind == CollisionKind::Box) {
        for (int i = 0; i < 3; ++i) {
            const Vec3 axis = s.axes.column(i) * (i == 0 ? s.half.x : i == 1 ? s.half.y : s.half.z);
            extent = extent + Vec3{std::abs(axis.x), std::abs(axis.y), std::abs(axis.z)};
        }
    } else {
        const Vec3 axis = s.axes.column(1) * s.half_segment;
        extent = Vec3{std::abs(axis.x), std::abs(axis.y), std::abs(axis.z)} + Vec3{s.radius, s.radius, s.radius};
    }
    s.lo = s.center - extent;
    s.hi = s.center + extent;
}

bool overlap_bounds(const Shape& a, const Shape& b) {
    return a.lo.x <= b.hi.x && b.lo.x <= a.hi.x && a.lo.y <= b.hi.y && b.lo.y <= a.hi.y && a.lo.z <= b.hi.z && b.lo.z <= a.hi.z;
}

bool collide(const Shape& a, const Shape& b, Contact& out) {
    using K = CollisionKind;
    auto swapped = [&](bool hit) {
        out.normal = -out.normal;
        return hit;
    };
    if (a.kind == K::Box && b.kind == K::Box) return box_box(a, b, out);
    if (a.kind == K::Sphere && b.kind == K::Box) return sphere_box(a.center, a.radius, b, out);
    if (a.kind == K::Box && b.kind == K::Sphere) return swapped(sphere_box(b.center, b.radius, a, out));
    if (a.kind == K::Capsule && b.kind == K::Box) return capsule_box(a, b, out);
    if (a.kind == K::Box && b.kind == K::Capsule) return swapped(capsule_box(b, a, out));
    if (a.kind == K::Sphere && b.kind == K::Sphere) return sphere_sphere(a.center, a.radius, b.center, b.radius, out);
    if (a.kind == K::Sphere && b.kind == K::Capsule)
        return sphere_sphere(a.center, a.radius, closest_on_segment(a.center, capsule_a(b), capsule_b(b)), b.radius, out);
    if (a.kind == K::Capsule && b.kind == K::Sphere)
        return sphere_sphere(closest_on_segment(b.center, capsule_a(a), capsule_b(a)), a.radius, b.center, b.radius, out);
    Vec3 ca, cb;
    closest_segments(capsule_a(a), capsule_b(a), capsule_a(b), capsule_b(b), ca, cb);
    return sphere_sphere(ca, a.radius, cb, b.radius, out);
}

} // namespace physics

// Rebuilt only when nodes are freed or shapes and layers change; otherwise moved shapes are just placed again.
void Runtime::physics_begin() {
    auto& world = *physics_;
    if (world.built) {
        for (auto& shape : world.shapes) {
            const Transform& at = nodes_[size_t(shape.node)].world;
            if (at == shape.placed) continue;
            shape.local = nodes_[size_t(shape.body)].world.inverse() * at;
            physics::place(shape, at);
        }
        world.valid = true;
        return;
    }
    // Something was freed or disabled: bodies at rest on it must fall.
    if (!world.shapes.empty())
        for (int32_t i : collision_objects_) wake(nodes_[size_t(i)]);
    world.shapes.clear();
    for (const auto& [i, body] : shape_owners_) {
        const Live& n = nodes_[size_t(i)];
        if (n.freed || n.d.more().disabled || nodes_[size_t(body)].freed) continue;
        physics::Shape shape;
        shape.kind = n.d.more().collision;
        shape.size = n.d.more().box_size;
        shape.authored_radius = n.d.more().radius;
        shape.authored_height = n.d.more().height;
        shape.body = body;
        shape.node = int(i);
        const NodeDesc& owner = nodes_[size_t(body)].d;
        shape.owner = owner.type;
        shape.layer = owner.more().collision_layer;
        shape.mask = owner.more().collision_mask;
        shape.local = nodes_[size_t(body)].world.inverse() * n.world;
        physics::place(shape, n.world);
        world.shapes.push_back(shape);
    }
    world.built = world.valid = true;
}

// Writes the new pose back to the authored transform. rotated = false leaves the Euler angles alone.
void Runtime::move_body(int index, const Transform& world, bool rotated) {
    Live& n = nodes_[size_t(index)];
    n.world = world;
    for (auto& s : physics_->shapes)
        if (s.body == index) physics::place(s, world * s.local);
    const bool nested = n.parent >= 0 && is_node3d(nodes_[size_t(n.parent)].d.type);
    const Transform local = nested ? nodes_[size_t(n.parent)].world.inverse() * world : world;
    n.d.position = local.origin;
    if (rotated) {
        Vec3 scale;
        const Vec3 e = local.basis.orthonormalized(&scale).euler();
        n.d.rotation = {degrees(e.x), degrees(e.y), degrees(e.z)};
    }
    // Exact transform for update_transforms(), no Euler round trip.
    n.local = local;
    n.local_position = n.d.position;
    n.local_rotation = n.d.rotation;
    n.local_scale = n.d.scale;
    n.local_valid = true;
}

void Runtime::wake(Live& n) {
    n.edit_motion().sleeping = false;
    n.edit_motion().still_time = 0;
}

bool Runtime::deepest_contact(int index, physics::Contact& best, int& other) const {
    best.depth = 0;
    other = -1;
    const auto& shapes = physics_->shapes;
    for (const auto& a : shapes) {
        if (a.body != index) continue;
        for (const auto& b : shapes) {
            if (b.body == index || b.owner == NodeType::Area3D || !physics::masks_match(a, b) || !physics::overlap_bounds(a, b)
                || nodes_[size_t(b.body)].freed) continue;
            physics::Contact c;
            if (physics::collide(a, b, c) && c.depth > best.depth) {
                best = c;
                other = b.body;
            }
        }
    }
    return other >= 0;
}

// Like Godot's move_and_slide(), plus floor snapping on slopes and steps. Rigid bodies in the way get pushed.
bool Runtime::move_and_slide(int index) {
    if (!physics_->valid) physics_begin();
    Live& c = nodes_[size_t(index)];
    const Vec3 up{0, 1, 0};
    const float floor_cos = std::cos(radians(std::clamp(c.d.more().floor_max_angle, 0.0f, 89.0f)));
    const bool was_on_floor = c.motion().on_floor;
    c.edit_motion().on_floor = c.edit_motion().on_wall = c.edit_motion().on_ceiling = false;
    float smallest = 1e30f;
    for (const auto& s : physics_->shapes) {
        if (s.body != index) continue;
        smallest = std::min(smallest, s.kind == CollisionKind::Box ? std::min({s.half.x, s.half.y, s.half.z}) : s.radius);
    }
    if (smallest == 1e30f) {
        // No shape: plain motion, like Godot.
        Transform world = c.world;
        world.origin = world.origin + c.motion().velocity * physics_delta_;
        move_body(index, world, false);
        return false;
    }
    Transform world = c.world;
    // Standing on a moving body (platform): move with it first.
    if (was_on_floor && c.motion().floor_body >= 0 && !nodes_[size_t(c.motion().floor_body)].freed) {
        const Live& floor = nodes_[size_t(c.motion().floor_body)];
        world.origin = world.origin + floor.motion().last_motion; // as in Godot, the platform's last step
    }
    c.edit_motion().floor_body = -1;
    bool collided = false;
    auto resolve = [&]() {
        for (int iteration = 0; iteration < 4; ++iteration) {
            physics::Contact contact;
            int other;
            if (!deepest_contact(index, contact, other)) return;
            collided = true;
            const Vec3 moving = c.motion().velocity; // before the contact removes its push into the surface
            const float facing = contact.normal.dot(up);
            if (facing > floor_cos) {
                // Floor: lift straight up, so standing on a slope does not slide.
                c.edit_motion().on_floor = true;
                c.edit_motion().floor_normal = contact.normal;
                c.edit_motion().floor_body = other;
                world.origin.y += contact.depth / std::max(facing, 0.3f) + 0.0005f;
                if (c.edit_motion().velocity.y < 0) c.edit_motion().velocity.y = 0;
            } else {
                if (facing < -floor_cos) c.edit_motion().on_ceiling = true; else c.edit_motion().on_wall = true;
                world.origin = world.origin + contact.normal * (contact.depth + 0.0005f);
                const float into = c.motion().velocity.dot(contact.normal);
                if (into < 0) c.edit_motion().velocity = c.edit_motion().velocity - contact.normal * into;
            }
            Live& o = nodes_[size_t(other)];
            if (o.d.type == NodeType::RigidBody3D && !o.d.more().freeze) {
                const Vec3 push = -contact.normal;
                const float wanted = std::max(0.0f, moving.dot(push));
                const float current = o.motion().velocity.dot(push);
                if (wanted > current) o.edit_motion().velocity = o.edit_motion().velocity + push * ((wanted - current) * 0.8f + 0.2f);
                wake(o);
            }
            move_body(index, world, false);
        }
    };
    const Vec3 motion = c.motion().velocity * physics_delta_;
    const int steps = std::clamp(int(std::ceil(motion.length() / std::max(0.02f, smallest * 0.5f))), 1, 8);
    for (int s = 0; s < steps; ++s) {
        world.origin = world.origin + motion * (1.0f / float(steps));
        move_body(index, world, false);
        resolve();
    }
    // Snap to the floor when walking down slopes and steps (not while jumping).
    if (was_on_floor && !c.motion().on_floor && c.motion().velocity.dot(up) <= 0.01f) {
        const Transform before = world;
        const bool wall = c.motion().on_wall, ceiling = c.motion().on_ceiling;
        world.origin.y -= 0.2f;
        move_body(index, world, false);
        resolve();
        if (!c.motion().on_floor) {
            world = before;
            move_body(index, world, false);
            c.edit_motion().on_wall = wall;
            c.edit_motion().on_ceiling = ceiling;
        }
    }
    return collided;
}

// Rigid bodies: integrate, then resolve contacts (positions and velocities).
void Runtime::physics_step(float dt) {
    if (!physics_->valid) physics_begin();
    const Vec3 gravity{0, -physics::Gravity, 0};
    for (size_t i = 0; i < nodes_.size(); ++i) {
        Live& r = nodes_[i];
        if (r.freed || r.d.type != NodeType::RigidBody3D || r.d.more().freeze) continue;
        if (r.motion().sleeping && r.motion().force.dot(r.motion().force) == 0) continue;
        const float inv_mass = 1.0f / std::max(r.d.more().mass, 0.001f);
        r.edit_motion().velocity = r.edit_motion().velocity + (gravity * r.d.more().gravity_scale + r.edit_motion().force * inv_mass) * dt;
        r.edit_motion().force = {};
        r.edit_motion().velocity = r.edit_motion().velocity * (1.0f / (1.0f + dt * (physics::DefaultLinearDamp + r.d.more().linear_damp)));
        r.edit_motion().angular = r.edit_motion().angular * (1.0f / (1.0f + dt * (physics::DefaultAngularDamp + r.d.more().angular_damp)));
        Transform world = r.world;
        world.origin = world.origin + r.motion().velocity * dt;
        const float spin = r.motion().angular.length();
        if (spin > 1e-5f) world.basis = Basis::axis_angle(r.motion().angular * (1.0f / spin), spin * dt) * world.basis;
        move_body(int(i), world, spin > 1e-5f);
    }
    auto& shapes = physics_->shapes;
    for (int iteration = 0; iteration < 4; ++iteration) {
        for (size_t k = 0; k < shapes.size(); ++k) {
            const physics::Shape& a = shapes[k];
            if (a.owner != NodeType::RigidBody3D) continue;
            const size_t i = size_t(a.body);
            Live& r = nodes_[i];
            if (r.freed || r.d.more().freeze || r.motion().sleeping) continue;
            for (size_t m = 0; m < shapes.size(); ++m) {
                const physics::Shape& b = shapes[m];
                if (b.body == int(i) || b.owner == NodeType::Area3D || !physics::masks_match(a, b) || !physics::overlap_bounds(a, b)) continue;
                Live& o = nodes_[size_t(b.body)];
                if (o.freed) continue;
                physics::Contact c;
                if (!physics::collide(a, b, c)) continue;
                const bool dynamic = o.d.type == NodeType::RigidBody3D && !o.d.more().freeze;
                const float inv_a = 1.0f / std::max(r.d.more().mass, 0.001f), inv_b = dynamic ? 1.0f / std::max(o.d.more().mass, 0.001f) : 0;
                const float correction = std::max(c.depth - 0.002f, 0.0f) * 0.8f / (inv_a + inv_b);
                Transform wa = r.world;
                wa.origin = wa.origin + c.normal * (correction * inv_a);
                move_body(int(i), wa, false);
                if (dynamic) {
                    Transform wb = o.world;
                    wb.origin = wb.origin - c.normal * (correction * inv_b);
                    move_body(b.body, wb, false);
                    // Only a moving body or a real overlap wakes the other one, so resting stacks can sleep.
                    if (o.motion().sleeping && (r.motion().velocity.dot(r.motion().velocity) > 0.0025f || c.depth > 0.01f)) wake(o);
                }
                // Kinematic characters push with their velocity; static bodies are still.
                const Vec3 other_velocity = dynamic || o.d.type == NodeType::CharacterBody3D ? o.motion().velocity : Vec3{};
                const Vec3 relative = r.motion().velocity - other_velocity;
                const float vn = relative.dot(c.normal);
                if (vn < 0) {
                    // Godot's PhysicsMaterial combine: bounces add up to at most 1, friction takes the smaller one.
                    float e = std::min(1.0f, r.d.more().bounce + o.d.more().bounce);
                    if (-vn < 0.6f) e = 0; // resting contact: no jitter
                    const float j = -(1 + e) * vn / (inv_a + inv_b);
                    r.edit_motion().velocity = r.edit_motion().velocity + c.normal * (j * inv_a);
                    if (dynamic) o.edit_motion().velocity = o.edit_motion().velocity - c.normal * (j * inv_b);
                    const Vec3 tangent = relative - c.normal * vn;
                    const float slip = tangent.length();
                    if (slip > 1e-4f) {
                        const float mu = std::max(0.0f, std::min(r.d.more().friction, o.d.more().friction));
                        const float jt = std::min(slip / (inv_a + inv_b), mu * j);
                        const Vec3 t = tangent * (1.0f / slip);
                        r.edit_motion().velocity = r.edit_motion().velocity - t * (jt * inv_a);
                        if (dynamic) o.edit_motion().velocity = o.edit_motion().velocity + t * (jt * inv_b);
                    }
                }
                // Spheres roll on what they stand on (no slipping).
                if (a.kind == CollisionKind::Sphere && c.normal.y > 0.5f && a.radius > 0)
                    r.edit_motion().angular = c.normal.cross(r.edit_motion().velocity - other_velocity) * (1.0f / a.radius);
            }
        }
    }
    // Sleep after half a second (almost) at rest.
    for (auto& r : nodes_) {
        if (r.freed || r.d.type != NodeType::RigidBody3D || r.d.more().freeze || r.motion().sleeping) continue;
        if (r.motion().velocity.dot(r.motion().velocity) < 0.0025f && r.motion().angular.dot(r.motion().angular) < 0.01f) {
            r.edit_motion().still_time += dt;
            if (r.motion().still_time > 0.5f) {
                r.edit_motion().sleeping = true;
                r.edit_motion().velocity = r.edit_motion().angular = {};
            }
        } else {
            r.edit_motion().still_time = 0;
        }
    }
    update_areas();
}

// Calls _on_body_entered/_on_body_exited on the area's script, Godot's default names for connected signals.
void Runtime::update_areas() {
    const auto& shapes = physics_->shapes;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        Live& area = nodes_[i];
        if (area.freed || area.d.type != NodeType::Area3D) continue;
        std::vector<uint32_t> inside;
        if (area.d.more().monitoring) {
            for (const auto& a : shapes) {
                if (a.body != int(i)) continue;
                for (const auto& b : shapes) {
                    if (b.body == int(i) || !physics::is_body(b.owner) || !(a.mask & b.layer) || !physics::overlap_bounds(a, b)) continue;
                    const Live& body = nodes_[size_t(b.body)];
                    if (body.freed || std::find(inside.begin(), inside.end(), body.d.id) != inside.end()) continue;
                    physics::Contact c;
                    if (physics::collide(a, b, c)) inside.push_back(body.d.id);
                }
            }
        }
        const std::vector<uint32_t> before = area.motion().overlaps;
        area.edit_motion().overlaps = inside;
        if (!playing_ || !vm_->has_instance(area.d.id)) continue;
        for (uint32_t id : inside)
            if (std::find(before.begin(), before.end(), id) == before.end()) {
                const jetscript::Value body = jetscript::Value::object(id);
                vm_->call(area.d.id, "_on_body_entered", &body, 1);
            }
        for (uint32_t id : before)
            if (std::find(inside.begin(), inside.end(), id) == inside.end()) {
                const jetscript::Value body = jetscript::Value::object(id);
                vm_->call(area.d.id, "_on_body_exited", &body, 1);
            }
    }
}

// Scripts' _physics_process (where move_and_slide runs) comes first, then rigid bodies and areas.
void Runtime::physics_tick(float dt) {
    physics_delta_ = dt;
    in_physics_ = true;
    if (has_physics_) {
        // After a step the children of moved bodies have stale world transforms.
        if (transforms_stale_) update_transforms(false);
        physics_begin();
    }
    const jetscript::Value delta(dt);
    for (auto& n : nodes_)
        if (!n.freed && n.d.script >= 0 && vm_->has_instance(n.d.id)) vm_->call(n.d.id, "_physics_process", &delta, 1);
    flush_script_errors();
    if (has_physics_) {
        // Scripts may have moved bodies through their properties.
        update_transforms(false);
        physics_begin();
        physics_step(dt);
        // Bodies are current (moved by the step or by scripts); their children are not.
        transforms_stale_ = true;
        for (int32_t i : collision_objects_) {
            Live& n = nodes_[size_t(i)];
            n.edit_motion().last_motion = n.world.origin - n.edit_motion().last_origin;
            n.edit_motion().last_origin = n.world.origin;
        }
    }
    physics_->valid = false;
    physics_previous_input_ = input_;
    in_physics_ = false;
}

} // namespace jetstudio
