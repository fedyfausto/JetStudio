#include "runtime.hpp"
#include "ParticleSystem.hpp"
#include <algorithm>
#include <cmath>

namespace jetstudio{
    namespace{
        constexpr float JetUnits = 100;
        // Jet's emitters and LOD assume 60 units per metre (its gravity is 9.8 * 60 * scale).
        constexpr float JetWorldScale = JetUnits / 60;

        Renderer::Vec3f toJetF(Vec3 v){ return {v.x * JetUnits, v.y * JetUnits, -v.z * JetUnits}; }

        uint32_t mixRgb(uint32_t a, uint32_t b, float t){
            uint32_t out = 0;

            for(int shift = 0; shift <= 16; shift += 8){
                const float x = float((a >> shift) & 255), y = float((b >> shift) & 255);
                out |= uint32_t(std::lround(x + (y - x) * t)) << shift;
            }

            return out;
        }

        Vec3 perpendicular(Vec3 v){
            const Vec3 axis = std::abs(v.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
            return v.cross(axis).normalized();
        }
    }

    void Runtime::FreeDrawer::operator()(Renderer::ParticleSystem* p) const{
        p->~ParticleSystem();
        coldFree(p);
    }

    float Runtime::random01(){
        random_ ^= random_ << 13;
        random_ ^= random_ >> 17;
        random_ ^= random_ << 5;
        return float(random_ >> 8) * (1.0f / 16777216.0f);
    }

    // Godot's CPUParticles3D emission, simulated here in Jet units. Jet's ParticleSystem only draws the streaks:
    // one is shared by all emitters, each keeps just its own particles.
    void Runtime::startParticles(){
        particles_.clear();

        for(auto& n : nodes_){
            n.particles = -1;

            if(n.d.type == NodeType::CPUParticles3D && n.inTree) startParticle(n);
        }
    }

    // Takes a free slot first: emitters of instanced scenes come and go (releaseNode() empties theirs).
    void Runtime::startParticle(Live& n){
        size_t slot = 0;

        while(slot < particles_.size() && particles_[slot].used) ++slot;

        if(slot == particles_.size()) particles_.emplace_back();

        particles_[slot].used = true;
        particles_[slot].particles.reserve(std::clamp(int(n.d.more().amount), 1, MaxParticles));
        n.particles = int32_t(slot);
        n.cycle = 0;
        n.emitted = 0;
    }

    int Runtime::liveParticles() const{
        int count = 0;

        for(const auto& pool : particles_)
            for(const auto& p : pool.particles) count += p.active;

        return count;
    }

    void Runtime::restartParticles(Live& n){
        n.d.extras().emitting = true;
        n.cycle = 0;
        n.emitted = 0;

        if(n.particles >= 0)
            for(auto& p : particles_[size_t(n.particles)].particles) p.active = false;
    }

    // age: how long before the end of this update the particle was emitted.
    // The first free slot, as many as the amount (one more for a moment when one dies as the next is emitted).
    void Runtime::emitParticle(Live& n, ParticlePool& pool, float age){
        Particle* slot = nullptr;

        for(auto& p : pool.particles)
            if(!p.active){ slot = &p; break; }

        if(!slot){
            if(pool.particles.size() >= size_t(MaxParticles)) return;

            slot = &pool.particles.emplace_back();
        }

        const NodeDesc& d = n.d;
        const Basis& basis = n.world.basis;
        Vec3 offset{};

        if(d.more().emission_shape == EmissionShape::Sphere){
            //uniform in the ball: rejection sampling of the unit cube.
            Vec3 u;

            do u = {random01() * 2 - 1, random01() * 2 - 1, random01() * 2 - 1}; while(u.dot(u) > 1);

            offset = u * d.more().emissionRadius;
        } else if(d.more().emission_shape == EmissionShape::Box){
            offset = {(random01() * 2 - 1) * d.more().emissionBox.x, (random01() * 2 - 1) * d.more().emissionBox.y, (random01() * 2 - 1) * d.more().emissionBox.z};
        }

        const Vec3 origin = n.world.origin + basis.xform(offset);
        //spread cone around the direction, from two random angles like Godot.
        Vec3 axis = basis.orthonormalized().xform(d.more().direction).normalized();

        if(axis.dot(axis) == 0) axis = {0, 1, 0};

        const Vec3 u = perpendicular(axis), v = axis.cross(u);
        const float spread = radians(std::clamp(d.more().spread, 0.0f, 180.0f));
        const float a = (random01() * 2 - 1) * spread, b = (random01() * 2 - 1) * spread;
        const Vec3 dir = (axis * (std::cos(a) * std::cos(b)) + u * std::sin(a) + v * (std::sin(b) * std::cos(a))).normalized();
        const float speed = d.more().velocityMin + (d.more().velocityMax - d.more().velocityMin) * random01();
        const float life = std::max(0.01f, d.more().lifetime * (1 - std::clamp(d.more().lifetime_randomness, 0.0f, 1.0f) * random01()));

        if(age >= life) return;

        slot->damping = (d.more().damping_min + (d.more().damping_max - d.more().damping_min) * random01()) * JetUnits;
        Vec3 velocity = dir * speed;
        const Vec3 position = origin + velocity * age + d.more().gravity * (0.5f * age * age);
        velocity = velocity + d.more().gravity * age;
        slot->pos = toJetF(position);
        slot->vel = toJetF(velocity);
        slot->maxLife = life;
        slot->life = life - age;
        slot->active = true;
    }

    // Godot's cycle: amount particles per lifetime, all emitted in the first (1 - explosiveness) of it.
    void Runtime::updateParticles(float dt){
        for(auto& n : nodes_){
            if(n.particles < 0 || n.freed || !n.inTree) continue;

            ParticlePool& pool = particles_[size_t(n.particles)];
            const Renderer::Vec3f gravity = toJetF(n.d.more().gravity);

            for(auto& p : pool.particles){
                if(!p.active) continue;

                p.vel = p.vel + gravity * dt;
                //Godot's damping: the speed drops by `damping` per second, down to zero.
                const float slow = p.damping * dt;

                if(slow > 0){
                    const float speed = p.vel.length();
                    p.vel = speed > slow ? p.vel * ((speed - slow) / speed) : Renderer::Vec3f{};
                }

                p.pos = p.pos + p.vel * dt;
                p.life -= dt;

                if(p.life <= 0) p.active = false;
            }

            if(!n.d.more().emitting) continue;

            const int amount = std::clamp(int(n.d.more().amount), 1, MaxParticles);
            const float lifetime = std::max(n.d.more().lifetime, 0.01f);
            const float window = lifetime * (1 - std::clamp(n.d.more().explosiveness, 0.0f, 1.0f));
            n.cycle += dt;

            for(int guard = 0; guard < 8 * MaxParticles; ++guard){
                if(n.emitted >= amount){
                    if(n.cycle < lifetime) break;

                    if(n.d.more().one_shot){
                        n.d.extras().emitting = false;
                        deferSignal(n.d.id, "finished");
                        break;
                    }

                    n.cycle -= lifetime;
                    n.emitted = 0;
                    continue;
                }

                const float at = window * float(n.emitted) / float(amount);

                if(n.cycle < at) break;

                emitParticle(n, pool, n.cycle - at);
                ++n.emitted;
            }
        }

        runDeferred();
    }

    // Projects like Jet (camera space x right, y up, z forward). The matrix is built as in Scene::renderObject().
    struct Runtime::ParticleView{
        float m[3][3];
        float px, py, pz, fov, cx, cy, near, far;
        void toCamera(float x, float y, float z, float out[3]) const{
            const float dx = x - px, dy = y - py, dz = z - pz;

            for(int r = 0; r < 3; ++r) out[r] = m[r][0] * dx + m[r][1] * dy + m[r][2] * dz;
        }
    };

    void Runtime::renderParticles(){
        particleTriangles_ = 0;

        if(particles_.empty()) return;

        ParticleView view;
        {
            int32_t icx, isx, icy, isy, icz, isz;
            camera_.getRotationMatrix(icx, isx, icy, isy, icz, isz);
            const float f = 1.0f / float(FIXED_POINT_SCALE);
            const float cx = icx * f, sx = isx * f, cy = icy * f, sy = isy * f, cz = icz * f, sz = isz * f;
            const float k[3][3] = {{cy, 0, sy}, {sx * sy, cx, -sx * cy}, {-cx * sy, sx, cx * cy}};

            for(int j = 0; j < 3; ++j){
                view.m[0][j] = cz * k[0][j] - sz * k[1][j];
                view.m[1][j] = sz * k[0][j] + cz * k[1][j];
                view.m[2][j] = k[2][j];
            }

            view.px = float(camera_.position.x); view.py = float(camera_.position.y); view.pz = float(camera_.position.z);
            view.fov = camera_.fovFactor;
            view.cx = float(width_ / 2); view.cy = float(height_ / 2);
            view.near = float(camera_.nearPlane); view.far = float(camera_.farPlane);
        }
#if !Z_BUFFERING

        //screen area and farthest depth of the live particles: only meshes in there and nearer can hide them.
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, farZ = 0;

        for(const auto& n : nodes_){
            if(n.particles < 0 || n.freed || !n.worldVisible || n.d.more().no_depth_test) continue;

            const float margin = n.d.more().particle_style == ParticleStyle::Puff ? std::max(n.d.more().puff_size, n.d.more().puff_size_end) * JetUnits * 0.5f : 0;

            for(const auto& p : particles_[size_t(n.particles)].particles){
                if(!p.active) continue;

                float c[3];
                view.toCamera(p.pos.x, p.pos.y, p.pos.z, c);

                if(c[2] <= view.near) continue;

                const float k = view.fov / c[2], r = margin * k + 4;
                x0 = std::min(x0, view.cx + c[0] * k - r); x1 = std::max(x1, view.cx + c[0] * k + r);
                y0 = std::min(y0, view.cy - c[1] * k - r); y1 = std::max(y1, view.cy - c[1] * k + r);
                farZ = std::max(farZ, c[2]);
            }
        }

        //rebuilt once per picture (two fields): on the ESP32 it costs about as much as Jet transforming the same meshes.
        if(farZ <= 0){
            occlusion_.clear();
        } else if(occlusion_.empty() || (scene_.frameCounter & 1) == 0){
            buildOcclusion(view, int(std::max(0.0f, x0)), int(std::max(0.0f, y0)),
                            int(std::min(float(width_ - 1), x1)), int(std::min(float(height_ - 1), y1)), farZ);
        }
#endif

        //no Depth Test emitters go last, after clearing the z-buffer (Jet's streaks always test it, nothing reads it later).
        bool over = false;

        for(const auto& n : nodes_){
            if(n.particles < 0 || n.freed || !n.worldVisible) continue;

            if(n.d.more().no_depth_test) over = true;
            else particleTriangles_ += drawParticles(n, view);
        }

        if(!over) return;
#if Z_BUFFERING

        std::fill(depth_.begin(), depth_.end(), uint16_t(0xffff));
#endif

        for(const auto& n : nodes_)
            if(n.particles >= 0 && !n.freed && n.worldVisible && n.d.more().no_depth_test) particleTriangles_ += drawParticles(n, view);
    }

    unsigned Runtime::drawParticles(const Live& n, const ParticleView& view){
        if(n.d.more().particle_style == ParticleStyle::Puff) return drawPuffs(n, view);

        if(!particleDrawer_) particleDrawer_.reset(new (coldAlloc(sizeof(Renderer::ParticleSystem))) Renderer::ParticleSystem(JetWorldScale));

        Renderer::ParticleSystem& drawer = *particleDrawer_;
        drawer.additiveSparks = n.d.more().additive;
        const auto kind = n.d.more().particle_style == ParticleStyle::Water ? Renderer::ParticleKind::Splash : Renderer::ParticleKind::Spark;

        //into Jet's pool in the same order; on the device, without those behind a mesh (Jet draws over the frame).
        const bool occlude = !Z_BUFFERING && !occlusion_.empty() && !n.d.more().no_depth_test;
        int count = 0;

        for(const auto& p : particles_[size_t(n.particles)].particles){
            if(!p.active || count == MaxParticles) continue;

            if(occlude){
                float c[3];
                view.toCamera(p.pos.x, p.pos.y, p.pos.z, c);

                if(c[2] > view.near && occluded(view, view.cx + c[0] * view.fov / c[2], view.cy - c[1] * view.fov / c[2], c[2])) continue;
            }

            drawer.pool[count++] = {p.pos, p.vel, p.life, p.maxLife, kind, true};
        }

        drawer.render(&scene_, &camera_, width_, height_);

        for(int i = 0; i < count; ++i) drawer.pool[i].active = false;

        return drawer.lastRenderedTriangles;
    }

    // Device only: nearest opaque depth per 4x4 pixel cell. Coarse, but enough to hide particles behind a car.
    constexpr int OcclusionCell = 4;
    void Runtime::buildOcclusion(const ParticleView& view, int x0, int y0, int x1, int y1, float farZ){
        if(x1 < x0 || y1 < y0) return;

        const int gw = (width_ + OcclusionCell - 1) / OcclusionCell, gh = (height_ + OcclusionCell - 1) / OcclusionCell;
        occlusion_.assign(size_t(gw) * size_t(gh), 0xffff);
        const int gx0 = x0 / OcclusionCell, gx1 = std::min(gw - 1, x1 / OcclusionCell);
        const int gy0 = y0 / OcclusionCell, gy1 = std::min(gh - 1, y1 / OcclusionCell);

        for(const auto& n : nodes_){
            if(!n.object || n.freed || !n.worldVisible || n.d.type != NodeType::MeshInstance3D) continue;

            if(n.d.draw_order == DrawOrder::Background || n.d.alpha < 0.99f || n.d.shading == Shading::Additive) continue;

            const float ox = n.world.origin.x * JetUnits, oy = n.world.origin.y * JetUnits, oz = -n.world.origin.z * JetUnits;
            float c[3];
            view.toCamera(ox, oy, oz, c);
            const float r = n.jetRadius;

            if(c[2] - r > farZ || c[2] + r <= view.near) continue;

            if(c[2] - r > view.near){
                //screen box of the bounding sphere: skip meshes away from the particles.
                const float kn = view.fov / (c[2] - r), kf = view.fov / (c[2] + r);
                const float sx0 = view.cx + std::min((c[0] - r) * kn, (c[0] - r) * kf), sx1 = view.cx + std::max((c[0] + r) * kn, (c[0] + r) * kf);
                const float sy0 = view.cy - std::max((c[1] + r) * kn, (c[1] + r) * kf), sy1 = view.cy - std::min((c[1] - r) * kn, (c[1] - r) * kf);

                if(sx1 < float(x0) || sx0 > float(x1) || sy1 < float(y0) || sy0 > float(y1)) continue;
            }

            //as syncObject() gives it to Jet
            const Basis rotation = n.world.basis.orthonormalized().mirroredZ();
            const bool twoSided = n.d.cull == CullMode::Disabled;
            //what the node draws: its shared copy, or geometry of its own.
            const Renderer::Object& geometry = n.object->instances.empty() ? *n.object : *n.object->instances[0].mesh;
            const auto& vertices = geometry.vertices;
            occlusionPoints_.resize(vertices.size() * 3);

            for(size_t i = 0; i < vertices.size(); ++i){
                const float vx = float(vertices[i].position.x), vy = float(vertices[i].position.y), vz = float(vertices[i].position.z);
                float q[3];
                view.toCamera(ox + rotation.m[0][0] * vx + rotation.m[0][1] * vy + rotation.m[0][2] * vz,
                               oy + rotation.m[1][0] * vx + rotation.m[1][1] * vy + rotation.m[1][2] * vz,
                               oz + rotation.m[2][0] * vx + rotation.m[2][1] * vy + rotation.m[2][2] * vz, q);
                float* out = &occlusionPoints_[i * 3];

                if(q[2] <= view.near){ out[2] = -1; continue; }
                const float k = view.fov / q[2];
                out[0] = view.cx + q[0] * k;
                out[1] = view.cy - q[1] * k;
                out[2] = q[2];
            }

            for(const auto& t : geometry.triangles){
                const float* a = &occlusionPoints_[size_t(t.v1) * 3];
                const float* b = &occlusionPoints_[size_t(t.v2) * 3];
                const float* d = &occlusionPoints_[size_t(t.v3) * 3];

                if(a[2] < 0 || b[2] < 0 || d[2] < 0) continue;

                //same sign as Jet's shoelace test: positive is a front face.
                const float area = (b[0] - a[0]) * (d[1] - a[1]) - (b[1] - a[1]) * (d[0] - a[0]);

                //NaN fails the first test too
                if(!(std::abs(area) >= 0.25f) || (!twoSided && area < 0)) continue;

                //points close to the camera project far off screen: clamped before the int conversion
                auto cell = [](float v){ return std::isnan(v) ? 0 : int(std::clamp(std::floor(v / OcclusionCell), -1e6f, 1e6f)); };
                const int cx0 = std::max(gx0, cell(std::min({a[0], b[0], d[0]})));
                const int cx1 = std::min(gx1, cell(std::max({a[0], b[0], d[0]})));
                const int cy0 = std::max(gy0, cell(std::min({a[1], b[1], d[1]})));
                const int cy1 = std::min(gy1, cell(std::max({a[1], b[1], d[1]})));
                const float inv = 1 / area;

                for(int gy = cy0; gy <= cy1; ++gy){
                    const float py = float(gy * OcclusionCell) + OcclusionCell * 0.5f;

                    for(int gx = cx0; gx <= cx1; ++gx){
                        const float px = float(gx * OcclusionCell) + OcclusionCell * 0.5f;
                        const float w0 = ((d[0] - b[0]) * (py - b[1]) - (d[1] - b[1]) * (px - b[0])) * inv;
                        const float w1 = ((a[0] - d[0]) * (py - d[1]) - (a[1] - d[1]) * (px - d[0])) * inv;
                        const float w2 = 1 - w0 - w1;

                        if(w0 < 0 || w1 < 0 || w2 < 0) continue;

                        const float z = w0 * a[2] + w1 * b[2] + w2 * d[2];
                        uint16_t& cell = occlusion_[size_t(gy) * size_t(gw) + size_t(gx)];
                        cell = uint16_t(std::min(float(cell), std::min(z, 65534.0f)));
                    }
                }
            }
        }
    }

    bool Runtime::occluded(const ParticleView&, float x, float y, float z) const{
        if(occlusion_.empty() || x < 0 || y < 0 || x >= float(width_) || y >= float(height_)) return false;

        const int gw = (width_ + OcclusionCell - 1) / OcclusionCell;
        return z > float(occlusion_[size_t(int(y) / OcclusionCell) * size_t(gw) + size_t(int(x) / OcclusionCell)]);
    }

    // Soft discs painted far to near straight into the frame. Triangle fans would blend their shared edges twice.
    unsigned Runtime::drawPuffs(const Live& n, const ParticleView& view){
        struct Puff{ float x, y, z, age; };
        Puff puffs[MaxParticles];
        int count = 0;
        const float farSq = (1500.0f * JetWorldScale) * (1500.0f * JetWorldScale);

        for(const auto& p : particles_[size_t(n.particles)].particles){
            if(!p.active || count == MaxParticles) continue;

            const float dx = p.pos.x - view.px, dy = p.pos.y - view.py, dz = p.pos.z - view.pz;

            if(dx * dx + dy * dy + dz * dz > farSq) continue;

            float c[3];
            view.toCamera(p.pos.x, p.pos.y, p.pos.z, c);

            if(c[2] <= view.near || c[2] >= view.far) continue;

            puffs[count++] = {c[0], c[1], c[2], 1 - p.life / p.maxLife};
        }

        std::sort(puffs, puffs + count, [](const Puff& a, const Puff& b){ return a.z > b.z; });
        //scene_.render() has already advanced the frame counter.
        const int parity = FIELD_BUFFERS ? ((scene_.frameCounter - 1) % 2 == 0 ? 1 : 0) : 0;
        const int columns = bufferWidth(), rows = bufferRows();
#if Z_BUFFERING
        const uint16_t* depth = n.d.more().no_depth_test ? nullptr : depth_.data();
#else
        const int gw = (width_ + OcclusionCell - 1) / OcclusionCell;
        const uint16_t* cells = occlusion_.empty() || n.d.more().no_depth_test ? nullptr : occlusion_.data();
#endif
        const NodeDesc& d = n.d;
        unsigned drawn = 0;

        for(int i = 0; i < count; ++i){
            const Puff& p = puffs[i];
            //opacity: quick fade in (first tenth of the life), then out to zero.
            const float opacity = d.alpha * std::min(1.0f, p.age * 10) * (1 - p.age);

            if(opacity < 0.02f) continue;

            const float diameter = (d.more().puff_size + (d.more().puff_size_end - d.more().puff_size) * p.age) * JetUnits;
            const float scale = view.fov / p.z;
            const float radius = std::min(diameter * 0.5f * scale, 96.0f);

            if(radius < 0.75f) continue;

            const float px = p.x * scale + view.cx, py = view.cy - p.y * scale;

            //far off screen (or not a number): skipped before any int conversion
            if(!(std::abs(px) < 1e6f) || !(std::abs(py) < 1e6f)) continue;

            const uint32_t rgb = mixRgb(d.color, d.more().color_end, p.age);
            const int r = int((rgb >> 16) & 255) >> 3, g = int((rgb >> 8) & 255) >> 2, b = int(rgb & 255) >> 3;
            const int y0 = std::max(0, int(std::ceil(py - radius))), y1 = std::min(height_ - 1, int(std::floor(py + radius)));
            const uint16_t z = uint16_t(std::min(p.z, 65534.0f));
            bool any = false;

            for(int y = y0; y <= y1; ++y){
                if(FIELD_BUFFERS && (y & 1) != parity) continue;

                if(y / BufferRowStep >= rows) continue;

                const float fy = (float(y) - py) / radius;
                const float half = radius * std::sqrt(std::max(0.0f, 1 - fy * fy));
                const int x0 = std::max(0, int(std::ceil(px - half))) / BufferColumnStep;
                const int x1 = std::min(width_ - 1, int(std::floor(px + half))) / BufferColumnStep;
                uint16_t* row = target_ + size_t(y / BufferRowStep) * size_t(columns);

                for(int x = x0; x <= x1; ++x){
                    const int screenX = x * BufferColumnStep + BufferColumnStep / 2;
#if Z_BUFFERING

                    if(depth && z > depth[size_t(y) * size_t(ZBUFFER_STRIDE(width_)) + size_t(screenX)]) continue;
#else

                    if(cells && z > cells[size_t(y / OcclusionCell) * size_t(gw) + size_t(screenX / OcclusionCell)]) continue;
#endif

                    //soft edge: full opacity in the middle, zero at the rim.
                    const float fx = (float(screenX) - px) / radius;
                    const float rim = 1 - (fx * fx + fy * fy);

                    if(rim <= 0) continue;

                    const int a = int(opacity * std::min(1.0f, rim * 1.6f) * 256);
                    const uint16_t c = row[x];
                    const int cr = c >> 11, cg = (c >> 5) & 63, cb = c & 31;
                    row[x] = uint16_t((cr + (((r - cr) * a) >> 8)) << 11 | (cg + (((g - cg) * a) >> 8)) << 5 | (cb + (((b - cb) * a) >> 8)));
                    any = true;
                }
            }

            drawn += any;
        }

        return drawn;
    }

}
