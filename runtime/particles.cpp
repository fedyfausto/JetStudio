// Godot's CPUParticles3D emission, simulated here in Jet units. Jet's ParticleSystem only draws the streaks.
#include "runtime.hpp"
#include "ParticleSystem.hpp"
#include <algorithm>
#include <cmath>

namespace jetstudio {
namespace {
constexpr float JetUnits = 100;
// Jet's emitters and LOD assume 60 units per metre (its gravity is 9.8 * 60 * scale).
constexpr float JetWorldScale = JetUnits / 60;

Renderer::Vec3f to_jet_f(Vec3 v) { return {v.x * JetUnits, v.y * JetUnits, -v.z * JetUnits}; }

uint32_t mix_rgb(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const float x = float((a >> shift) & 255), y = float((b >> shift) & 255);
        out |= uint32_t(std::lround(x + (y - x) * t)) << shift;
    }
    return out;
}

Vec3 perpendicular(Vec3 v) {
    const Vec3 axis = std::abs(v.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    return v.cross(axis).normalized();
}
} // namespace

float Runtime::random01() {
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return float(random_ >> 8) * (1.0f / 16777216.0f);
}

void Runtime::start_particles() {
    particles_.clear();
    particle_damping_.clear();
    for (auto& n : nodes_) {
        n.particles = -1;
        if (n.d.type != NodeType::CPUParticles3D) continue;
        particles_.push_back(std::make_unique<Renderer::ParticleSystem>(JetWorldScale));
        particle_damping_.emplace_back();
        n.particles = int32_t(particles_.size()) - 1;
        n.cycle = 0;
        n.emitted = 0;
    }
}

int Runtime::live_particles() const {
    int count = 0;
    for (const auto& system : particles_) count += int(system->activeCount());
    return count;
}

void Runtime::restart_particles(Live& n) {
    n.d.extras().emitting = true;
    n.cycle = 0;
    n.emitted = 0;
    if (n.particles >= 0)
        for (auto& p : particles_[size_t(n.particles)]->pool) p.active = false;
}

// age: how long before the end of this update the particle was emitted.
void Runtime::emit_particle(Live& n, Renderer::ParticleSystem& system, float age) {
    Renderer::Particle* slot = nullptr;
    for (auto& p : system.pool)
        if (!p.active) { slot = &p; break; }
    if (!slot) return; // pool full: Godot would not emit either (amount <= pool)
    const NodeDesc& d = n.d;
    const Basis& basis = n.world.basis;
    Vec3 offset{};
    if (d.more().emission_shape == EmissionShape::Sphere) {
        // Uniform in the ball: rejection sampling of the unit cube.
        Vec3 u;
        do u = {random01() * 2 - 1, random01() * 2 - 1, random01() * 2 - 1}; while (u.dot(u) > 1);
        offset = u * d.more().emission_radius;
    } else if (d.more().emission_shape == EmissionShape::Box) {
        offset = {(random01() * 2 - 1) * d.more().emission_box.x, (random01() * 2 - 1) * d.more().emission_box.y, (random01() * 2 - 1) * d.more().emission_box.z};
    }
    const Vec3 origin = n.world.origin + basis.xform(offset);
    // Spread cone around the direction, from two random angles like Godot.
    Vec3 axis = basis.orthonormalized().xform(d.more().direction).normalized();
    if (axis.dot(axis) == 0) axis = {0, 1, 0};
    const Vec3 u = perpendicular(axis), v = axis.cross(u);
    const float spread = radians(std::clamp(d.more().spread, 0.0f, 180.0f));
    const float a = (random01() * 2 - 1) * spread, b = (random01() * 2 - 1) * spread;
    const Vec3 dir = (axis * (std::cos(a) * std::cos(b)) + u * std::sin(a) + v * (std::sin(b) * std::cos(a))).normalized();
    const float speed = d.more().velocity_min + (d.more().velocity_max - d.more().velocity_min) * random01();
    const float life = std::max(0.01f, d.more().lifetime * (1 - std::clamp(d.more().lifetime_randomness, 0.0f, 1.0f) * random01()));
    if (age >= life) return;
    particle_damping_[size_t(n.particles)][size_t(slot - system.pool)] =
        (d.more().damping_min + (d.more().damping_max - d.more().damping_min) * random01()) * JetUnits;
    Vec3 velocity = dir * speed;
    const Vec3 position = origin + velocity * age + d.more().gravity * (0.5f * age * age);
    velocity = velocity + d.more().gravity * age;
    slot->pos = to_jet_f(position);
    slot->vel = to_jet_f(velocity);
    slot->maxLife = life;
    slot->life = life - age;
    slot->kind = d.more().particle_style == ParticleStyle::Water ? Renderer::ParticleKind::Splash : Renderer::ParticleKind::Spark;
    slot->active = true;
}

// Godot's cycle: amount particles per lifetime, all emitted in the first (1 - explosiveness) of it.
void Runtime::update_particles(float dt) {
    for (auto& n : nodes_) {
        if (n.particles < 0 || n.freed) continue;
        Renderer::ParticleSystem& system = *particles_[size_t(n.particles)];
        const Renderer::Vec3f gravity = to_jet_f(n.d.more().gravity);
        const auto& damping = particle_damping_[size_t(n.particles)];
        for (auto& p : system.pool) {
            if (!p.active) continue;
            p.vel = p.vel + gravity * dt;
            // Godot's damping: the speed drops by `damping` per second, down to zero.
            const float slow = damping[size_t(&p - system.pool)] * dt;
            if (slow > 0) {
                const float speed = p.vel.length();
                p.vel = speed > slow ? p.vel * ((speed - slow) / speed) : Renderer::Vec3f{};
            }
            p.pos = p.pos + p.vel * dt;
            p.life -= dt;
            if (p.life <= 0) p.active = false;
        }
        system.additiveSparks = n.d.more().additive;
        if (!n.d.more().emitting) continue;
        const int amount = std::clamp(int(n.d.more().amount), 1, MaxParticles);
        const float lifetime = std::max(n.d.more().lifetime, 0.01f);
        const float window = lifetime * (1 - std::clamp(n.d.more().explosiveness, 0.0f, 1.0f));
        n.cycle += dt;
        for (int guard = 0; guard < 8 * MaxParticles; ++guard) {
            if (n.emitted >= amount) {
                if (n.cycle < lifetime) break;
                if (n.d.more().one_shot) { n.d.extras().emitting = false; break; }
                n.cycle -= lifetime;
                n.emitted = 0;
                continue;
            }
            const float at = window * float(n.emitted) / float(amount);
            if (n.cycle < at) break;
            emit_particle(n, system, n.cycle - at);
            ++n.emitted;
        }
    }
}

// Projects like Jet (camera space x right, y up, z forward). The matrix is built as in Scene::renderObject().
struct Runtime::ParticleView {
    float m[3][3];
    float px, py, pz, fov, cx, cy, near, far;
    void to_camera(float x, float y, float z, float out[3]) const {
        const float dx = x - px, dy = y - py, dz = z - pz;
        for (int r = 0; r < 3; ++r) out[r] = m[r][0] * dx + m[r][1] * dy + m[r][2] * dz;
    }
};

void Runtime::render_particles() {
    particle_triangles_ = 0;
    if (particles_.empty()) return;
    ParticleView view;
    {
        int32_t icx, isx, icy, isy, icz, isz;
        camera_.getRotationMatrix(icx, isx, icy, isy, icz, isz);
        const float f = 1.0f / float(FIXED_POINT_SCALE);
        const float cx = icx * f, sx = isx * f, cy = icy * f, sy = isy * f, cz = icz * f, sz = isz * f;
        const float k[3][3] = {{cy, 0, sy}, {sx * sy, cx, -sx * cy}, {-cx * sy, sx, cx * cy}};
        for (int j = 0; j < 3; ++j) {
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
    // Screen area and farthest depth of the live particles: only meshes in there and nearer can hide them.
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, far_z = 0;
    for (const auto& n : nodes_) {
        if (n.particles < 0 || n.freed || !n.world_visible || n.d.more().no_depth_test) continue;
        const float margin = n.d.more().particle_style == ParticleStyle::Puff ? std::max(n.d.more().puff_size, n.d.more().puff_size_end) * JetUnits * 0.5f : 0;
        for (const auto& p : particles_[size_t(n.particles)]->pool) {
            if (!p.active) continue;
            float c[3];
            view.to_camera(p.pos.x, p.pos.y, p.pos.z, c);
            if (c[2] <= view.near) continue;
            const float k = view.fov / c[2], r = margin * k + 4;
            x0 = std::min(x0, view.cx + c[0] * k - r); x1 = std::max(x1, view.cx + c[0] * k + r);
            y0 = std::min(y0, view.cy - c[1] * k - r); y1 = std::max(y1, view.cy - c[1] * k + r);
            far_z = std::max(far_z, c[2]);
        }
    }
    // Rebuilt once per picture (two fields): on the ESP32 it costs about as much as Jet transforming the same meshes.
    if (far_z <= 0) {
        occlusion_.clear();
    } else if (occlusion_.empty() || (scene_.frameCounter & 1) == 0) {
        build_occlusion(view, int(std::max(0.0f, x0)), int(std::max(0.0f, y0)),
                        int(std::min(float(width_ - 1), x1)), int(std::min(float(height_ - 1), y1)), far_z);
    }
#endif
    // No Depth Test emitters go last, after clearing the z-buffer (Jet's streaks always test it, nothing reads it later).
    bool over = false;
    for (const auto& n : nodes_) {
        if (n.particles < 0 || n.freed || !n.world_visible) continue;
        if (n.d.more().no_depth_test) over = true;
        else particle_triangles_ += draw_particles(n, view);
    }
    if (!over) return;
#if Z_BUFFERING
    std::fill(depth_.begin(), depth_.end(), uint16_t(0xffff));
#endif
    for (const auto& n : nodes_)
        if (n.particles >= 0 && !n.freed && n.world_visible && n.d.more().no_depth_test) particle_triangles_ += draw_particles(n, view);
}

unsigned Runtime::draw_particles(const Live& n, const ParticleView& view) {
    Renderer::ParticleSystem& system = *particles_[size_t(n.particles)];
    if (n.d.more().particle_style == ParticleStyle::Puff) return draw_puffs(n, system, view);
#if !Z_BUFFERING
    // Jet draws its streaks over the frame: leave out those behind a mesh.
    bool hidden[MaxParticles] = {};
    if (!occlusion_.empty() && !n.d.more().no_depth_test)
        for (int i = 0; i < MaxParticles; ++i) {
            auto& p = system.pool[i];
            if (!p.active) continue;
            float c[3];
            view.to_camera(p.pos.x, p.pos.y, p.pos.z, c);
            if (c[2] <= view.near) continue;
            const float k = view.fov / c[2];
            if (occluded(view, view.cx + c[0] * k, view.cy - c[1] * k, c[2])) hidden[i] = true, p.active = false;
        }
#endif
    system.render(&scene_, &camera_, width_, height_);
#if !Z_BUFFERING
    for (int i = 0; i < MaxParticles; ++i)
        if (hidden[i]) system.pool[i].active = true;
#endif
    return system.lastRenderedTriangles;
}

// Device only: nearest opaque depth per 4x4 pixel cell. Coarse, but enough to hide particles behind a car.
constexpr int OcclusionCell = 4;
void Runtime::build_occlusion(const ParticleView& view, int x0, int y0, int x1, int y1, float far_z) {
    if (x1 < x0 || y1 < y0) return;
    const int gw = (width_ + OcclusionCell - 1) / OcclusionCell, gh = (height_ + OcclusionCell - 1) / OcclusionCell;
    occlusion_.assign(size_t(gw) * size_t(gh), 0xffff);
    const int gx0 = x0 / OcclusionCell, gx1 = std::min(gw - 1, x1 / OcclusionCell);
    const int gy0 = y0 / OcclusionCell, gy1 = std::min(gh - 1, y1 / OcclusionCell);
    for (const auto& n : nodes_) {
        if (!n.object || n.freed || !n.world_visible || n.d.type != NodeType::MeshInstance3D) continue;
        if (n.d.draw_order == DrawOrder::Background || n.d.alpha < 0.99f || n.d.shading == Shading::Additive) continue;
        const float ox = n.world.origin.x * JetUnits, oy = n.world.origin.y * JetUnits, oz = -n.world.origin.z * JetUnits;
        float c[3];
        view.to_camera(ox, oy, oz, c);
        const float r = n.jet_radius;
        if (c[2] - r > far_z || c[2] + r <= view.near) continue;
        if (c[2] - r > view.near) {
            // Screen box of the bounding sphere: skip meshes away from the particles.
            const float kn = view.fov / (c[2] - r), kf = view.fov / (c[2] + r);
            const float sx0 = view.cx + std::min((c[0] - r) * kn, (c[0] - r) * kf), sx1 = view.cx + std::max((c[0] + r) * kn, (c[0] + r) * kf);
            const float sy0 = view.cy - std::max((c[1] + r) * kn, (c[1] + r) * kf), sy1 = view.cy - std::min((c[1] - r) * kn, (c[1] - r) * kf);
            if (sx1 < float(x0) || sx0 > float(x1) || sy1 < float(y0) || sy0 > float(y1)) continue;
        }
        const Basis rotation = n.world.basis.orthonormalized().mirrored_z(); // as sync_object() gives it to Jet
        const bool two_sided = n.d.cull == CullMode::Disabled;
        // What the node draws: its shared copy, or geometry of its own.
        const Renderer::Object& geometry = n.object->instances.empty() ? *n.object : *n.object->instances[0].mesh;
        const auto& vertices = geometry.vertices;
        occlusion_points_.resize(vertices.size() * 3);
        for (size_t i = 0; i < vertices.size(); ++i) {
            const float vx = float(vertices[i].position.x), vy = float(vertices[i].position.y), vz = float(vertices[i].position.z);
            float q[3];
            view.to_camera(ox + rotation.m[0][0] * vx + rotation.m[0][1] * vy + rotation.m[0][2] * vz,
                           oy + rotation.m[1][0] * vx + rotation.m[1][1] * vy + rotation.m[1][2] * vz,
                           oz + rotation.m[2][0] * vx + rotation.m[2][1] * vy + rotation.m[2][2] * vz, q);
            float* out = &occlusion_points_[i * 3];
            if (q[2] <= view.near) { out[2] = -1; continue; }
            const float k = view.fov / q[2];
            out[0] = view.cx + q[0] * k;
            out[1] = view.cy - q[1] * k;
            out[2] = q[2];
        }
        for (const auto& t : geometry.triangles) {
            const float* a = &occlusion_points_[size_t(t.v1) * 3];
            const float* b = &occlusion_points_[size_t(t.v2) * 3];
            const float* d = &occlusion_points_[size_t(t.v3) * 3];
            if (a[2] < 0 || b[2] < 0 || d[2] < 0) continue;
            // Same sign as Jet's shoelace test: positive is a front face.
            const float area = (b[0] - a[0]) * (d[1] - a[1]) - (b[1] - a[1]) * (d[0] - a[0]);
            if (std::abs(area) < 0.25f || (!two_sided && area < 0)) continue;
            const int cx0 = std::max(gx0, int(std::floor(std::min({a[0], b[0], d[0]}) / OcclusionCell)));
            const int cx1 = std::min(gx1, int(std::floor(std::max({a[0], b[0], d[0]}) / OcclusionCell)));
            const int cy0 = std::max(gy0, int(std::floor(std::min({a[1], b[1], d[1]}) / OcclusionCell)));
            const int cy1 = std::min(gy1, int(std::floor(std::max({a[1], b[1], d[1]}) / OcclusionCell)));
            const float inv = 1 / area;
            for (int gy = cy0; gy <= cy1; ++gy) {
                const float py = float(gy * OcclusionCell) + OcclusionCell * 0.5f;
                for (int gx = cx0; gx <= cx1; ++gx) {
                    const float px = float(gx * OcclusionCell) + OcclusionCell * 0.5f;
                    const float w0 = ((d[0] - b[0]) * (py - b[1]) - (d[1] - b[1]) * (px - b[0])) * inv;
                    const float w1 = ((a[0] - d[0]) * (py - d[1]) - (a[1] - d[1]) * (px - d[0])) * inv;
                    const float w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    const float z = w0 * a[2] + w1 * b[2] + w2 * d[2];
                    uint16_t& cell = occlusion_[size_t(gy) * size_t(gw) + size_t(gx)];
                    cell = uint16_t(std::min(float(cell), std::min(z, 65534.0f)));
                }
            }
        }
    }
}

bool Runtime::occluded(const ParticleView&, float x, float y, float z) const {
    if (occlusion_.empty() || x < 0 || y < 0 || x >= float(width_) || y >= float(height_)) return false;
    const int gw = (width_ + OcclusionCell - 1) / OcclusionCell;
    return z > float(occlusion_[size_t(int(y) / OcclusionCell) * size_t(gw) + size_t(int(x) / OcclusionCell)]);
}

// Soft discs painted far to near straight into the frame. Triangle fans would blend their shared edges twice.
unsigned Runtime::draw_puffs(const Live& n, Renderer::ParticleSystem& system, const ParticleView& view) {
    struct Puff { float x, y, z, age; };
    Puff puffs[MaxParticles];
    int count = 0;
    const float far_sq = (1500.0f * JetWorldScale) * (1500.0f * JetWorldScale);
    for (const auto& p : system.pool) {
        if (!p.active) continue;
        const float dx = p.pos.x - view.px, dy = p.pos.y - view.py, dz = p.pos.z - view.pz;
        if (dx * dx + dy * dy + dz * dz > far_sq) continue;
        float c[3];
        view.to_camera(p.pos.x, p.pos.y, p.pos.z, c);
        if (c[2] <= view.near || c[2] >= view.far) continue;
        puffs[count++] = {c[0], c[1], c[2], 1 - p.life / p.maxLife};
    }
    std::sort(puffs, puffs + count, [](const Puff& a, const Puff& b) { return a.z > b.z; });
    // scene_.render() has already advanced the frame counter.
    const int parity = FIELD_BUFFERS ? ((scene_.frameCounter - 1) % 2 == 0 ? 1 : 0) : 0;
    const int columns = buffer_width(), rows = buffer_rows();
#if Z_BUFFERING
    const uint16_t* depth = n.d.more().no_depth_test ? nullptr : depth_.data();
#else
    const int gw = (width_ + OcclusionCell - 1) / OcclusionCell;
    const uint16_t* cells = occlusion_.empty() || n.d.more().no_depth_test ? nullptr : occlusion_.data();
#endif
    const NodeDesc& d = n.d;
    unsigned drawn = 0;
    for (int i = 0; i < count; ++i) {
        const Puff& p = puffs[i];
        // Opacity: quick fade in (first tenth of the life), then out to zero.
        const float opacity = d.alpha * std::min(1.0f, p.age * 10) * (1 - p.age);
        if (opacity < 0.02f) continue;
        const float diameter = (d.more().puff_size + (d.more().puff_size_end - d.more().puff_size) * p.age) * JetUnits;
        const float scale = view.fov / p.z;
        const float radius = std::min(diameter * 0.5f * scale, 96.0f);
        if (radius < 0.75f) continue;
        const float px = p.x * scale + view.cx, py = view.cy - p.y * scale;
        const uint32_t rgb = mix_rgb(d.color, d.more().color_end, p.age);
        const int r = int((rgb >> 16) & 255) >> 3, g = int((rgb >> 8) & 255) >> 2, b = int(rgb & 255) >> 3;
        const int y0 = std::max(0, int(std::ceil(py - radius))), y1 = std::min(height_ - 1, int(std::floor(py + radius)));
        const uint16_t z = uint16_t(std::min(p.z, 65534.0f));
        bool any = false;
        for (int y = y0; y <= y1; ++y) {
            if (FIELD_BUFFERS && (y & 1) != parity) continue;
            if (y / BufferRowStep >= rows) continue;
            const float fy = (float(y) - py) / radius;
            const float half = radius * std::sqrt(std::max(0.0f, 1 - fy * fy));
            const int x0 = std::max(0, int(std::ceil(px - half))) / BufferColumnStep;
            const int x1 = std::min(width_ - 1, int(std::floor(px + half))) / BufferColumnStep;
            uint16_t* row = target_ + size_t(y / BufferRowStep) * size_t(columns);
            for (int x = x0; x <= x1; ++x) {
                const int screen_x = x * BufferColumnStep + BufferColumnStep / 2;
#if Z_BUFFERING
                if (depth && z > depth[size_t(y) * size_t(ZBUFFER_STRIDE(width_)) + size_t(screen_x)]) continue;
#else
                if (cells && z > cells[size_t(y / OcclusionCell) * size_t(gw) + size_t(screen_x / OcclusionCell)]) continue;
#endif
                // Soft edge: full opacity in the middle, zero at the rim.
                const float fx = (float(screen_x) - px) / radius;
                const float rim = 1 - (fx * fx + fy * fy);
                if (rim <= 0) continue;
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

} // namespace jetstudio
