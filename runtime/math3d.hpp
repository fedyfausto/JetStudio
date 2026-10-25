#pragma once
// Godot conventions (right-handed, Y up, -Z forward). Jet is left-handed, so Z gets mirrored where the two meet.
#include <cmath>

namespace jetstudio {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
    float dot(Vec3 o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(Vec3 o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    float length() const { return std::sqrt(dot(*this)); }
    Vec3 normalized() const { const float l = length(); return l > 0 ? *this * (1.0f / l) : Vec3{}; }
    bool finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
    bool operator==(Vec3 o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(Vec3 o) const { return !(*this == o); }
};

constexpr float Pi = 3.14159265358979f;
inline float radians(float degrees) { return degrees * (Pi / 180.0f); }
inline float degrees(float radians) { return radians * (180.0f / Pi); }

// Row-major 3x3 matrix acting on column vectors.
struct Basis {
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    static Basis rotation_x(float a) { const float c = std::cos(a), s = std::sin(a); return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}}; }
    static Basis rotation_y(float a) { const float c = std::cos(a), s = std::sin(a); return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}}; }
    static Basis rotation_z(float a) { const float c = std::cos(a), s = std::sin(a); return {{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}}; }
    static Basis axis_angle(Vec3 axis, float a) {
        axis = axis.normalized();
        const float c = std::cos(a), s = std::sin(a), t = 1 - c;
        const float x = axis.x, y = axis.y, z = axis.z;
        return {{{t*x*x + c, t*x*y - s*z, t*x*z + s*y}, {t*x*y + s*z, t*y*y + c, t*y*z - s*x}, {t*x*z - s*y, t*y*z + s*x, t*z*z + c}}};
    }
    // Godot's default YXZ order: basis = Ry * Rx * Rz.
    static Basis from_euler(Vec3 radians) {
        return rotation_y(radians.y) * rotation_x(radians.x) * rotation_z(radians.z);
    }
    static Basis from_scale(Vec3 s) { return {{{s.x, 0, 0}, {0, s.y, 0}, {0, 0, s.z}}}; }

    Basis operator*(const Basis& o) const {
        Basis r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
        return r;
    }
    Vec3 xform(Vec3 v) const {
        return {m[0][0]*v.x + m[0][1]*v.y + m[0][2]*v.z, m[1][0]*v.x + m[1][1]*v.y + m[1][2]*v.z, m[2][0]*v.x + m[2][1]*v.y + m[2][2]*v.z};
    }
    Basis transposed() const {
        Basis r;
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i][j] = m[j][i];
        return r;
    }
    Vec3 column(int j) const { return {m[0][j], m[1][j], m[2][j]}; }
    void set_column(int j, Vec3 v) { m[0][j] = v.x; m[1][j] = v.y; m[2][j] = v.z; }
    float determinant() const {
        return m[0][0]*(m[1][1]*m[2][2] - m[1][2]*m[2][1]) - m[0][1]*(m[1][0]*m[2][2] - m[1][2]*m[2][0]) + m[0][2]*(m[1][0]*m[2][1] - m[1][1]*m[2][0]);
    }
    Basis inverse() const {
        const float det = determinant();
        if (std::abs(det) < 1e-12f) return {};
        const float d = 1.0f / det;
        Basis r;
        r.m[0][0] = (m[1][1]*m[2][2] - m[1][2]*m[2][1]) * d;
        r.m[0][1] = (m[0][2]*m[2][1] - m[0][1]*m[2][2]) * d;
        r.m[0][2] = (m[0][1]*m[1][2] - m[0][2]*m[1][1]) * d;
        r.m[1][0] = (m[1][2]*m[2][0] - m[1][0]*m[2][2]) * d;
        r.m[1][1] = (m[0][0]*m[2][2] - m[0][2]*m[2][0]) * d;
        r.m[1][2] = (m[0][2]*m[1][0] - m[0][0]*m[1][2]) * d;
        r.m[2][0] = (m[1][0]*m[2][1] - m[1][1]*m[2][0]) * d;
        r.m[2][1] = (m[0][1]*m[2][0] - m[0][0]*m[2][1]) * d;
        r.m[2][2] = (m[0][0]*m[1][1] - m[0][1]*m[1][0]) * d;
        return r;
    }
    // Rotation part (Gram-Schmidt) and per-axis scale; a mirrored basis gets a negative X scale.
    Basis orthonormalized(Vec3* scale_out = nullptr) const {
        Vec3 x = column(0), y = column(1), z = column(2);
        Vec3 s{x.length(), 0, 0};
        x = x.normalized();
        y = y - x * x.dot(y); s.y = y.length(); y = y.normalized();
        z = z - x * x.dot(z) - y * y.dot(z); s.z = z.length(); z = z.normalized();
        if (s.x == 0 || s.y == 0 || s.z == 0) { if (scale_out) *scale_out = {1, 1, 1}; return {}; }
        if (x.cross(y).dot(z) < 0) { x = -x; s.x = -s.x; }
        Basis r; r.set_column(0, x); r.set_column(1, y); r.set_column(2, z);
        if (scale_out) *scale_out = s;
        return r;
    }
    // YXZ Euler angles (radians), identical to Godot's Basis::get_euler().
    Vec3 euler() const {
        const float m12 = m[1][2];
        if (m12 < 1 - 1e-6f) {
            if (m12 > -(1 - 1e-6f)) {
                if (m[1][0] == 0 && m[0][1] == 0 && m[0][2] == 0 && m[2][0] == 0 && m[0][0] == 1)
                    return {std::atan2(-m12, m[1][1]), 0, 0};
                return {std::asin(-m12), std::atan2(m[0][2], m[2][2]), std::atan2(m[1][0], m[1][1])};
            }
            return {Pi * 0.5f, std::atan2(m[0][1], m[0][0]), 0};
        }
        return {-Pi * 0.5f, -std::atan2(m[0][1], m[0][0]), 0};
    }
    // Euler angles for Jet objects: basis = Rz * Ry * Rx.
    Vec3 euler_zyx() const {
        const float m20 = m[2][0];
        if (m20 < 1 - 1e-6f && m20 > -(1 - 1e-6f))
            return {std::atan2(m[2][1], m[2][2]), std::asin(-m20), std::atan2(m[1][0], m[0][0])};
        return {std::atan2(-m[1][2], m[1][1]), m20 < 0 ? Pi * 0.5f : -Pi * 0.5f, 0};
    }
    // Mirror Z on both sides (Godot right-handed <-> Jet left-handed).
    Basis mirrored_z() const {
        Basis r = *this;
        r.m[0][2] = -r.m[0][2]; r.m[1][2] = -r.m[1][2]; r.m[2][0] = -r.m[2][0]; r.m[2][1] = -r.m[2][1];
        return r;
    }
};

struct Transform {
    Basis basis;
    Vec3 origin;
    Transform operator*(const Transform& o) const { return {basis * o.basis, basis.xform(o.origin) + origin}; }
    Vec3 xform(Vec3 v) const { return basis.xform(v) + origin; }
    Transform inverse() const { const Basis i = basis.inverse(); return {i, i.xform(-origin)}; }
    bool operator==(const Transform& o) const {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                if (basis.m[r][c] != o.basis.m[r][c]) return false;
        return origin == o.origin;
    }
};

inline Transform local_transform(Vec3 position, Vec3 rotation_degrees, Vec3 scale) {
    const Vec3 r{radians(rotation_degrees.x), radians(rotation_degrees.y), radians(rotation_degrees.z)};
    return {Basis::from_euler(r) * Basis::from_scale(scale), position};
}

} // namespace jetstudio
