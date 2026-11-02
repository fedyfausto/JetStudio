#include "physics.hpp"
#include "script_host.hpp"
#include <algorithm>
#include <cmath>

namespace jetstudio{
    namespace physics{
        namespace{

            float clampf(float v, float lo, float hi){ return std::max(lo, std::min(v, hi)); }

            Vec3 capsuleA(const Shape& s){ return s.center + s.axes.column(1) * s.halfSegment; }
            Vec3 capsuleB(const Shape& s){ return s.center - s.axes.column(1) * s.halfSegment; }

            Vec3 closestOnSegment(Vec3 p, Vec3 a, Vec3 b){
                const Vec3 ab = b - a;
                const float len2 = ab.dot(ab);
                const float t = len2 > 1e-12f ? clampf((p - a).dot(ab) / len2, 0, 1) : 0;
                return a + ab * t;
            }

            // Closest points of two segments (Ericson, Real-Time Collision Detection 5.1.9).
            void closestSegments(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2, Vec3& c1, Vec3& c2){
                const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
                const float a = d1.dot(d1), e = d2.dot(d2), f = d2.dot(r);
                float s = 0, t = 0;

                if(a <= 1e-12f && e <= 1e-12f){ c1 = p1; c2 = p2; return; }

                if(a <= 1e-12f){
                    t = clampf(f / e, 0, 1);
                } else {
                    const float c = d1.dot(r);

                    if(e <= 1e-12f){
                        s = clampf(-c / a, 0, 1);
                    } else {
                        const float b = d1.dot(d2), denom = a * e - b * b;
                        s = denom > 1e-12f ? clampf((b * f - c * e) / denom, 0, 1) : 0;
                        t = (b * s + f) / e;

                        if(t < 0){ t = 0; s = clampf(-c / a, 0, 1); }
                        else if(t > 1){ t = 1; s = clampf((b - c) / a, 0, 1); }
                    }
                }

                c1 = p1 + d1 * s;
                c2 = p2 + d2 * t;
            }

            Vec3 closestOnBox(Vec3 p, const Shape& box){
                const Vec3 d = p - box.center;
                Vec3 q = box.center;
                const float half[3] = {box.half.x, box.half.y, box.half.z};

                for(int i = 0; i < 3; ++i){
                    const Vec3 axis = box.axes.column(i);
                    q = q + axis * clampf(d.dot(axis), -half[i], half[i]);
                }

                return q;
            }

            bool sphereBox(Vec3 c, float r, const Shape& box, Contact& out){
                const Vec3 q = closestOnBox(c, box);
                const Vec3 d = c - q;
                const float dist2 = d.dot(d);

                if(dist2 > r * r) return false;

                if(dist2 > 1e-10f){
                    const float dist = std::sqrt(dist2);
                    out.normal = d * (1.0f / dist);
                    out.depth = r - dist;
                    return true;
                }

                //centre inside the box: leave through the nearest face.
                const Vec3 local = c - box.center;
                const float half[3] = {box.half.x, box.half.y, box.half.z};
                int best = 0;
                float bestGap = 1e30f;

                for(int i = 0; i < 3; ++i){
                    const float gap = half[i] - std::abs(local.dot(box.axes.column(i)));

                    if(gap < bestGap){ bestGap = gap; best = i; }
                }

                const Vec3 axis = box.axes.column(best);
                out.normal = local.dot(axis) >= 0 ? axis : -axis;
                out.depth = r + bestGap;
                return true;
            }

            bool sphereSphere(Vec3 a, float ra, Vec3 b, float rb, Contact& out){
                const Vec3 d = a - b;
                const float dist2 = d.dot(d), r = ra + rb;

                if(dist2 > r * r) return false;

                const float dist = std::sqrt(dist2);
                out.normal = dist > 1e-6f ? d * (1.0f / dist) : Vec3{0, 1, 0};
                out.depth = r - dist;
                return true;
            }

            // The segment-to-box distance is convex along the segment, so a ternary search finds the closest point.
            bool capsuleBox(const Shape& cap, const Shape& box, Contact& out){
                const Vec3 a = capsuleA(cap), b = capsuleB(cap);
                auto distance2 = [&](float t){
                    const Vec3 p = a + (b - a) * t;
                    const Vec3 d = p - closestOnBox(p, box);
                    return d.dot(d);
                };

                float lo = 0, hi = 1;

                for(int i = 0; i < 20; ++i){
                    const float m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;

                    if(distance2(m1) < distance2(m2)) hi = m2; else lo = m1;
                }

                float t = (lo + hi) * 0.5f;
                //lying flat on a face, both ends are equally close: use the middle.
                if(std::abs(distance2(0) - distance2(1)) < 1e-6f && distance2(0) < cap.radius * cap.radius) t = 0.5f;

                return sphereBox(a + (b - a) * t, cap.radius, box, out);
            }

            // Separating axis test over the 15 axes, least penetration wins.
            bool boxBox(const Shape& a, const Shape& b, Contact& out){
                const Vec3 delta = a.center - b.center;
                Vec3 axes[15];
                int count = 0;

                for(int i = 0; i < 3; ++i) axes[count++] = a.axes.column(i);

                for(int i = 0; i < 3; ++i) axes[count++] = b.axes.column(i);

                for(int i = 0; i < 3; ++i)
                    for(int j = 0; j < 3; ++j){
                        const Vec3 c = a.axes.column(i).cross(b.axes.column(j));

                        if(c.dot(c) > 1e-6f) axes[count++] = c.normalized();
                    }

                const float ha[3] = {a.half.x, a.half.y, a.half.z}, hb[3] = {b.half.x, b.half.y, b.half.z};
                float best = 1e30f;
                Vec3 bestAxis;

                for(int k = 0; k < count; ++k){
                    const Vec3 axis = axes[k];
                    float ra = 0, rb = 0;

                    for(int i = 0; i < 3; ++i){
                        ra += ha[i] * std::abs(a.axes.column(i).dot(axis));
                        rb += hb[i] * std::abs(b.axes.column(i).dot(axis));
                    }

                    const float distance = delta.dot(axis);
                    const float overlap = ra + rb - std::abs(distance);

                    if(overlap <= 0) return false;

                    //prefer face axes on ties: edge axes give poor normals for resting boxes.
                    if(overlap < best - (k >= 6 ? 1e-3f : 0)){
                        best = overlap;
                        bestAxis = distance >= 0 ? axis : -axis;
                    }
                }

                out.normal = bestAxis;
                out.depth = best;
                return true;
            }

        }

        bool isBody(NodeType type){
            return type == NodeType::StaticBody3D || type == NodeType::CharacterBody3D || type == NodeType::RigidBody3D
                || type == NodeType::StaticBody2D || type == NodeType::CharacterBody2D || type == NodeType::RigidBody2D;
        }
        bool isCollisionObject(NodeType type){ return isBody(type) || isArea(type); }
        bool isArea(NodeType type){ return type == NodeType::Area3D || type == NodeType::Area2D; }
        bool isRigid(NodeType type){ return type == NodeType::RigidBody3D || type == NodeType::RigidBody2D; }
        bool isCharacter(NodeType type){ return type == NodeType::CharacterBody3D || type == NodeType::CharacterBody2D; }
        bool isCollisionShape(NodeType type){ return type == NodeType::CollisionShape3D || type == NodeType::CollisionShape2D; }
        bool isFlat(NodeType type){
            return type == NodeType::StaticBody2D || type == NodeType::CharacterBody2D || type == NodeType::RigidBody2D
                || type == NodeType::Area2D || type == NodeType::CollisionShape2D;
        }

        void place(Shape& s, const Transform& world){
            s.placed = world;
            const Vec3 sx = world.basis.column(0), sy = world.basis.column(1), sz = world.basis.column(2);
            const Vec3 scale{sx.length(), sy.length(), sz.length()};
            s.axes = world.basis.orthonormalized();
            s.center = world.origin;

            //2D: only x and y scale; a rectangle is deep enough that z never gives the least overlap.
            constexpr float FlatDepth = 1e6f;

            switch(s.kind){
                case CollisionKind::Box:
                    s.half = {s.size.x * 0.5f * scale.x, s.size.y * 0.5f * scale.y, s.flat ? FlatDepth : s.size.z * 0.5f * scale.z};
                    break;
                case CollisionKind::Sphere:
                    s.radius = s.authoredRadius * (s.flat ? std::max(scale.x, scale.y) : std::max({scale.x, scale.y, scale.z}));
                    break;
                case CollisionKind::Capsule:
                    s.radius = s.authoredRadius * (s.flat ? scale.x : std::max(scale.x, scale.z));
                    s.halfSegment = std::max(0.0f, s.authoredHeight * 0.5f * scale.y - s.radius);
                    break;
            }

            Vec3 extent;

            if(s.kind == CollisionKind::Box){
                for(int i = 0; i < 3; ++i){
                    const Vec3 axis = s.axes.column(i) * (i == 0 ? s.half.x : i == 1 ? s.half.y : s.half.z);
                    extent = extent + Vec3{std::abs(axis.x), std::abs(axis.y), std::abs(axis.z)};
                }
            } else {
                const Vec3 axis = s.axes.column(1) * s.halfSegment;
                extent = Vec3{std::abs(axis.x), std::abs(axis.y), std::abs(axis.z)} + Vec3{s.radius, s.radius, s.radius};
            }

            s.lo = s.center - extent;
            s.hi = s.center + extent;
        }

        bool overlapBounds(const Shape& a, const Shape& b){
            return a.lo.x <= b.hi.x && b.lo.x <= a.hi.x && a.lo.y <= b.hi.y && b.lo.y <= a.hi.y && a.lo.z <= b.hi.z && b.lo.z <= a.hi.z;
        }

        bool collide(const Shape& a, const Shape& b, Contact& out){
            using K = CollisionKind;
            auto swapped = [&](bool hit){
                out.normal = -out.normal;
                return hit;
            };

            if(a.kind == K::Box && b.kind == K::Box) return boxBox(a, b, out);

            if(a.kind == K::Sphere && b.kind == K::Box) return sphereBox(a.center, a.radius, b, out);

            if(a.kind == K::Box && b.kind == K::Sphere) return swapped(sphereBox(b.center, b.radius, a, out));

            if(a.kind == K::Capsule && b.kind == K::Box) return capsuleBox(a, b, out);

            if(a.kind == K::Box && b.kind == K::Capsule) return swapped(capsuleBox(b, a, out));

            if(a.kind == K::Sphere && b.kind == K::Sphere) return sphereSphere(a.center, a.radius, b.center, b.radius, out);

            if(a.kind == K::Sphere && b.kind == K::Capsule)
                return sphereSphere(a.center, a.radius, closestOnSegment(a.center, capsuleA(b), capsuleB(b)), b.radius, out);

            if(a.kind == K::Capsule && b.kind == K::Sphere)
                return sphereSphere(closestOnSegment(b.center, capsuleA(a), capsuleB(a)), a.radius, b.center, b.radius, out);
            Vec3 ca, cb;
            closestSegments(capsuleA(a), capsuleB(a), capsuleA(b), capsuleB(b), ca, cb);
            return sphereSphere(ca, a.radius, cb, b.radius, out);
        }

    }

    // Rebuilt only when nodes are freed or shapes and layers change; otherwise moved shapes are just placed again.
    void Runtime::physicsBegin(){
        auto& world = *physics_;

        if(world.built){
            for(auto& shape : world.shapes){
                const Transform& at = nodes_[size_t(shape.node)].world;

                if(at == shape.placed) continue;

                shape.local = nodes_[size_t(shape.body)].world.inverse() * at;
                physics::place(shape, at);
            }

            world.valid = true;
            return;
        }

        //something was freed or disabled: bodies at rest on it must fall.
        if(!world.shapes.empty())
            for(int32_t i : collisionObjects_) wake(nodes_[size_t(i)]);

        world.shapes.clear();

        for(const auto& [i, body] : shapeOwners_){
            const Live& n = nodes_[size_t(i)];

            if(n.freed || n.d.more().disabled || nodes_[size_t(body)].freed) continue;

            physics::Shape shape;
            shape.kind = n.d.more().collision;
            shape.size = n.d.more().boxSize;
            shape.authoredRadius = n.d.more().radius;
            shape.authoredHeight = n.d.more().height;
            shape.flat = physics::isFlat(n.d.type);
            shape.oneWay = shape.flat && n.d.more().oneWay;
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
    void Runtime::moveBody(int index, const Transform& world, bool rotated){
        Live& n = nodes_[size_t(index)];
        n.world = world;

        for(auto& s : physics_->shapes)
            if(s.body == index) physics::place(s, world * s.local);

        const bool flat = isNode2d(n.d.type);
        const bool nested = n.parent >= 0 && (flat ? isNode2d(nodes_[size_t(n.parent)].d.type) : isNode3d(nodes_[size_t(n.parent)].d.type));
        const Transform local = nested ? nodes_[size_t(n.parent)].world.inverse() * world : world;
        n.d.position = local.origin;

        if(rotated){
            Vec3 scale;
            const Vec3 e = local.basis.orthonormalized(&scale).euler();
            n.d.rotation = flat ? Vec3{0, 0, degrees(e.z)} : Vec3{degrees(e.x), degrees(e.y), degrees(e.z)};
        }

        //exact transform for updateTransforms(), no Euler round trip.
        n.local = local;
        n.localPosition = n.d.position;
        n.localRotation = n.d.rotation;
        n.localScale = n.d.scale;
        n.localValid = true;
    }

    void Runtime::wake(Live& n){
        n.editMotion().sleeping = false;
        n.editMotion().stillTime = 0;
    }

    namespace{
        // A one-way shape stops only what comes down onto its up side (local -y), no deeper than it just moved:
        // a body jumping up through it, or already inside it, passes.
        bool oneWayBlocks(const physics::Shape& platform, const physics::Contact& c, Vec3 motion){
            const Vec3 up = -platform.axes.column(1);
            const float down = -motion.dot(up);
            return c.normal.dot(up) > 0.5f && down >= 0 && c.depth <= down + 1;
        }
    }

    bool Runtime::deepestContact(int index, Vec3 motion, physics::Contact& best, int& other) const{
        best.depth = 0;
        other = -1;
        const auto& shapes = physics_->shapes;

        for(const auto& a : shapes){
            if(a.body != index) continue;

            for(const auto& b : shapes){
                if(b.body == index || physics::isArea(b.owner) || !physics::masksMatch(a, b) || !physics::overlapBounds(a, b)
                    || nodes_[size_t(b.body)].freed) continue;
                physics::Contact c;

                if(!physics::collide(a, b, c) || c.depth <= best.depth) continue;

                if(b.oneWay && !oneWayBlocks(b, c, motion)) continue;

                best = c;
                other = b.body;
            }
        }

        return other >= 0;
    }

    // Like Godot's move_and_slide(), plus floor snapping on slopes and steps. Rigid bodies in the way get pushed.
    bool Runtime::move_and_slide(int index){
        if(!physics_->valid) physicsBegin();

        Live& c = nodes_[size_t(index)];
        const physics::Space& space = physics::spaceOf(c.d.type);
        const Vec3 up = space.up;
        const float skin = 0.0005f * space.unit;
        const float floorCos = std::cos(radians(std::clamp(c.d.more().floor_max_angle, 0.0f, 89.0f)));
        const bool wasOnFloor = c.motion().onFloor;
        c.editMotion().onFloor = c.editMotion().onWall = c.editMotion().onCeiling = false;
        float smallest = 1e30f;

        for(const auto& s : physics_->shapes){
            if(s.body != index) continue;

            smallest = std::min(smallest, s.kind == CollisionKind::Box ? std::min({s.half.x, s.half.y, s.half.z}) : s.radius);
        }

        if(smallest == 1e30f){
            //no shape: plain motion, like Godot.
            Transform world = c.world;
            world.origin = world.origin + c.motion().velocity * physicsDelta_;
            moveBody(index, world, false);
            return false;
        }

        Transform world = c.world;
        //standing on a moving body (platform): move with it first.
        if(wasOnFloor && c.motion().floorBody >= 0 && !nodes_[size_t(c.motion().floorBody)].freed){
            const Live& floor = nodes_[size_t(c.motion().floorBody)];
            //as in Godot, the platform's last step
            world.origin = world.origin + floor.motion().lastMotion;
        }

        c.editMotion().floorBody = -1;
        bool collided = false;
        auto resolve = [&](Vec3 moved){
            for(int iteration = 0; iteration < 4; ++iteration){
                physics::Contact contact;
                int other;

                if(!deepestContact(index, moved, contact, other)) return;

                collided = true;
                //before the contact removes its push into the surface
                const Vec3 moving = c.motion().velocity;
                const float facing = contact.normal.dot(up);

                if(facing > floorCos){
                    //floor: lift straight up, so standing on a slope does not slide.
                    c.editMotion().onFloor = true;
                    c.editMotion().floorNormal = contact.normal;
                    c.editMotion().floorBody = other;
                    world.origin = world.origin + up * (contact.depth / std::max(facing, 0.3f) + skin);
                    const float falling = c.motion().velocity.dot(up);

                    if(falling < 0) c.editMotion().velocity = c.editMotion().velocity - up * falling;
                } else {
                    if(facing < -floorCos) c.editMotion().onCeiling = true; else c.editMotion().onWall = true;

                    world.origin = world.origin + contact.normal * (contact.depth + skin);
                    const float into = c.motion().velocity.dot(contact.normal);

                    if(into < 0) c.editMotion().velocity = c.editMotion().velocity - contact.normal * into;
                }

                Live& o = nodes_[size_t(other)];

                if(physics::isRigid(o.d.type) && !o.d.more().freeze){
                    const Vec3 push = -contact.normal;
                    const float wanted = std::max(0.0f, moving.dot(push));
                    const float current = o.motion().velocity.dot(push);

                    if(wanted > current) o.editMotion().velocity = o.editMotion().velocity + push * ((wanted - current) * 0.8f + 0.2f * space.unit);

                    wake(o);
                }

                moveBody(index, world, false);
                //pushed out: what is left is no longer a fall onto one-way shapes
                moved = {};
            }
        };

        const Vec3 motion = c.motion().velocity * physicsDelta_;
        const int steps = std::clamp(int(std::ceil(motion.length() / std::max(0.02f * space.unit, smallest * 0.5f))), 1, 8);
        const Vec3 step = motion * (1.0f / float(steps));

        for(int s = 0; s < steps; ++s){
            world.origin = world.origin + step;
            moveBody(index, world, false);
            resolve(step);
        }

        //snap to the floor when walking down slopes and steps (not while jumping).
        if(wasOnFloor && !c.motion().onFloor && c.motion().velocity.dot(up) <= 0.01f * space.unit){
            const Transform before = world;
            const bool wall = c.motion().onWall, ceiling = c.motion().onCeiling;
            world.origin = world.origin - up * space.snap;
            moveBody(index, world, false);
            resolve(-up * space.snap);

            if(!c.motion().onFloor){
                world = before;
                moveBody(index, world, false);
                c.editMotion().onWall = wall;
                c.editMotion().onCeiling = ceiling;
            }
        }

        return collided;
    }

    void Runtime::physicsStep(float dt){
        if(!physics_->valid) physicsBegin();

        for(size_t i = 0; i < nodes_.size(); ++i){
            Live& r = nodes_[i];

            if(r.freed || !r.inTree || !physics::isRigid(r.d.type) || r.d.more().freeze) continue;

            if(r.motion().sleeping && r.motion().force.dot(r.motion().force) == 0) continue;

            const Vec3 gravity = physics::spaceOf(r.d.type).gravity;
            const float invMass = 1.0f / std::max(r.d.more().mass, 0.001f);
            r.editMotion().velocity = r.editMotion().velocity + (gravity * r.d.more().gravity_scale + r.editMotion().force * invMass) * dt;
            r.editMotion().force = {};
            r.editMotion().velocity = r.editMotion().velocity * (1.0f / (1.0f + dt * (physics::DefaultLinearDamp + r.d.more().linear_damp)));
            r.editMotion().angular = r.editMotion().angular * (1.0f / (1.0f + dt * (physics::DefaultAngularDamp + r.d.more().angular_damp)));
            Transform world = r.world;
            world.origin = world.origin + r.motion().velocity * dt;
            const float spin = r.motion().angular.length();

            if(spin > 1e-5f) world.basis = Basis::axisAngle(r.motion().angular * (1.0f / spin), spin * dt) * world.basis;

            moveBody(int(i), world, spin > 1e-5f);
        }

        auto& shapes = physics_->shapes;

        for(int iteration = 0; iteration < 4; ++iteration){
            for(size_t k = 0; k < shapes.size(); ++k){
                const physics::Shape& a = shapes[k];

                if(!physics::isRigid(a.owner)) continue;

                const size_t i = size_t(a.body);
                Live& r = nodes_[i];

                if(r.freed || !r.inTree || r.d.more().freeze || r.motion().sleeping) continue;

                const physics::Space& space = physics::spaceOf(a.owner);
                const float unit = space.unit;

                for(size_t m = 0; m < shapes.size(); ++m){
                    const physics::Shape& b = shapes[m];

                    if(b.body == int(i) || physics::isArea(b.owner) || !physics::masksMatch(a, b) || !physics::overlapBounds(a, b)) continue;

                    Live& o = nodes_[size_t(b.body)];

                    if(o.freed) continue;

                    physics::Contact c;

                    if(!physics::collide(a, b, c)) continue;

                    if(b.oneWay && !oneWayBlocks(b, c, r.motion().velocity * dt)) continue;

                    const bool dynamic = physics::isRigid(o.d.type) && !o.d.more().freeze;
                    const float invA = 1.0f / std::max(r.d.more().mass, 0.001f), invB = dynamic ? 1.0f / std::max(o.d.more().mass, 0.001f) : 0;
                    const float correction = std::max(c.depth - 0.002f * unit, 0.0f) * 0.8f / (invA + invB);
                    Transform wa = r.world;
                    wa.origin = wa.origin + c.normal * (correction * invA);
                    moveBody(int(i), wa, false);

                    if(dynamic){
                        Transform wb = o.world;
                        wb.origin = wb.origin - c.normal * (correction * invB);
                        moveBody(b.body, wb, false);
                        //only a moving body or a real overlap wakes the other one, so resting stacks can sleep.
                        if(o.motion().sleeping && (r.motion().velocity.dot(r.motion().velocity) > 0.0025f * unit * unit || c.depth > 0.01f * unit)) wake(o);
                    }

                    //kinematic characters push with their velocity; static bodies are still.
                    const Vec3 otherVelocity = dynamic || physics::isCharacter(o.d.type) ? o.motion().velocity : Vec3{};
                    const Vec3 relative = r.motion().velocity - otherVelocity;
                    const float vn = relative.dot(c.normal);

                    if(vn < 0){
                        //Godot's PhysicsMaterial combine: bounces add up to at most 1, friction takes the smaller one.
                        float e = std::min(1.0f, r.d.more().bounce + o.d.more().bounce);

                        //resting contact: no jitter
                        if(-vn < 0.6f * unit) e = 0;

                        const float j = -(1 + e) * vn / (invA + invB);
                        r.editMotion().velocity = r.editMotion().velocity + c.normal * (j * invA);

                        if(dynamic) o.editMotion().velocity = o.editMotion().velocity - c.normal * (j * invB);

                        const Vec3 tangent = relative - c.normal * vn;
                        const float slip = tangent.length();

                        if(slip > 1e-4f * unit){
                            const float mu = std::max(0.0f, std::min(r.d.more().friction, o.d.more().friction));
                            const float jt = std::min(slip / (invA + invB), mu * j);
                            const Vec3 t = tangent * (1.0f / slip);
                            r.editMotion().velocity = r.editMotion().velocity - t * (jt * invA);

                            if(dynamic) o.editMotion().velocity = o.editMotion().velocity + t * (jt * invB);
                        }
                    }

                    //spheres (circles) roll on what they stand on (no slipping).
                    if(a.kind == CollisionKind::Sphere && c.normal.dot(space.up) > 0.5f && a.radius > 0)
                        r.editMotion().angular = c.normal.cross(r.editMotion().velocity - otherVelocity) * (1.0f / a.radius);
                }
            }
        }

        //sleep after half a second (almost) at rest.
        for(auto& r : nodes_){
            if(r.freed || !r.inTree || !physics::isRigid(r.d.type) || r.d.more().freeze || r.motion().sleeping) continue;

            const float unit = physics::spaceOf(r.d.type).unit;

            if(r.motion().velocity.dot(r.motion().velocity) < 0.0025f * unit * unit && r.motion().angular.dot(r.motion().angular) < 0.01f){
                r.editMotion().stillTime += dt;

                if(r.motion().stillTime > 0.5f){
                    r.editMotion().sleeping = true;
                    r.editMotion().velocity = r.editMotion().angular = {};
                }
            } else {
                r.editMotion().stillTime = 0;
            }
        }

        updateAreas();
    }

    // Emits body_entered/body_exited and area_entered/area_exited, and calls the area script's _on_body_entered...
    // (Godot's default names for connected signals) unless that same method is connected.
    void Runtime::updateAreas(){
        const auto& shapes = physics_->shapes;

        for(size_t i = 0; i < nodes_.size(); ++i){
            Live& area = nodes_[i];

            if(area.freed || !area.inTree || !physics::isArea(area.d.type)) continue;

            std::vector<uint32_t> bodies, areas;

            if(area.d.more().monitoring){
                for(const auto& a : shapes){
                    if(a.body != int(i)) continue;

                    for(const auto& b : shapes){
                        if(b.body == int(i) || a.flat != b.flat || !(a.mask & b.layer) || !physics::overlapBounds(a, b)) continue;

                        const Live& other = nodes_[size_t(b.body)];
                        auto& inside = physics::isArea(b.owner) ? areas : bodies;

                        if(other.freed || std::find(inside.begin(), inside.end(), other.d.id) != inside.end()) continue;

                        physics::Contact c;

                        if(physics::collide(a, b, c)) inside.push_back(other.d.id);
                    }
                }
            }

            const std::vector<uint32_t> bodiesBefore = area.motion().overlaps, areasBefore = area.motion().areas;
            area.editMotion().overlaps = bodies;
            area.editMotion().areas = areas;
            //the callbacks may add nodes, which can move nodes_: from here on, only the area's id.
            const uint32_t areaId = area.d.id;

            if(!playing_) continue;

            const bool scripted = vm_->hasInstance(areaId), connected = vm_->has_connections(areaId);

            if(!scripted && !connected) continue;

            auto send = [&](uint32_t id, const char* signal){
                const jetscript::Value other = jetscript::Value::object(id);
                const std::string callback = std::string("_on_") + signal;

                if(scripted && !(connected && vm_->is_connected(areaId, signal, areaId, callback))) vm_->call(areaId, callback, &other, 1);

                if(connected) vm_->emit_signal(areaId, signal, &other, 1);
            };

            auto notify = [&](const std::vector<uint32_t>& now, const std::vector<uint32_t>& before, const char* entered, const char* exited){
                for(uint32_t id : now)
                    if(std::find(before.begin(), before.end(), id) == before.end()) send(id, entered);

                for(uint32_t id : before)
                    if(std::find(now.begin(), now.end(), id) == now.end()) send(id, exited);
            };

            notify(bodies, bodiesBefore, "body_entered", "body_exited");
            notify(areas, areasBefore, "area_entered", "area_exited");
        }
    }

    // Scripts' _physics_process (where move_and_slide runs) comes first, then rigid bodies and areas.
    void Runtime::physicsTick(float dt){
        physicsDelta_ = dt;
        inPhysics_ = true;

        if(hasPhysics_){
            //after a step the children of moved bodies have stale world transforms.
            if(transformsStale_) updateTransforms(false);

            physicsBegin();
        }

        const jetscript::Value delta(dt);

        for(size_t i = 0; i < nodes_.size(); ++i){
            const Live& n = nodes_[i];

            if(n.freed || !n.inTree || n.d.script < 0) continue;

            const uint32_t id = n.d.id;

            if(vm_->hasInstance(id)) vm_->call(id, "_physics_process", &delta, 1);
        }

        flushScriptErrors();

        if(hasPhysics_){
            //scripts may have moved bodies through their properties.
            updateTransforms(false);
            physicsBegin();
            physicsStep(dt);
            //bodies are current (moved by the step or by scripts); their children are not.
            transformsStale_ = true;

            for(int32_t i : collisionObjects_){
                Live& n = nodes_[size_t(i)];
                n.editMotion().lastMotion = n.world.origin - n.editMotion().lastOrigin;
                n.editMotion().lastOrigin = n.world.origin;
            }
        }

        physics_->valid = false;
        physicsPreviousInput_ = input_;
        inPhysics_ = false;
    }

}
