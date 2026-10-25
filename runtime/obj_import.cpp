#include "obj_import.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <tuple>

namespace jetstudio {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> fields(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        const size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

bool number(std::string_view text, float& out) {
    const std::string copy(text);
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || !std::isfinite(value)) return false;
    out = float(value);
    return true;
}

// OBJ indices are 1-based or negative from the end. Returns -1 when absent, -2 when invalid.
int resolve(std::string_view text, size_t count) {
    if (text.empty()) return -1;
    const std::string copy(text);
    char* end = nullptr;
    const long value = std::strtol(copy.c_str(), &end, 10);
    if (end == copy.c_str()) return -2;
    const long index = value > 0 ? value - 1 : long(count) + value;
    return index >= 0 && size_t(index) < count ? int(index) : -2;
}

uint32_t color_of(float r, float g, float b) {
    auto channel = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    return channel(r) << 16 | channel(g) << 8 | channel(b);
}

std::string sanitize(std::string name) {
    for (char& c : name)
        if (c == '.' || c == ':' || c == '@' || c == '/' || c == '"' || c == '%' || uint8_t(c) < 32) c = '_';
    return name.empty() ? std::string("Mesh") : name.substr(0, 80);
}

} // namespace

bool import_obj(std::string_view obj, std::string_view mtl, const ObjImportOptions& options,
                std::vector<ObjPart>& parts, std::vector<ObjMaterial>& materials, std::string& error) {
    parts.clear();
    materials.clear();
    error.clear();
    if (!std::isfinite(options.scale) || options.scale <= 0) { error = "Invalid import scale."; return false; }
    for (size_t pos = 0; pos <= mtl.size();) {
        size_t end = mtl.find('\n', pos);
        if (end == std::string_view::npos) end = mtl.size();
        const auto line = trim(mtl.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == '#') continue;
        const auto f = fields(line);
        if (f[0] == "newmtl" && f.size() > 1) {
            materials.push_back({});
            materials.back().name = std::string(trim(line.substr(6)));
        } else if (materials.empty()) {
            continue;
        } else if (f[0] == "Kd" && f.size() >= 4) {
            float r, g, b;
            if (number(f[1], r) && number(f[2], g) && number(f[3], b)) materials.back().color = color_of(r, g, b);
        } else if (f[0] == "map_Kd" && f.size() > 1) {
            materials.back().texture = std::string(f.back()); // options such as -s come first
            const auto rest = trim(line.substr(6));
            if (rest.find(" -") == std::string_view::npos && rest.front() != '-') materials.back().texture = std::string(rest);
        } else if ((f[0] == "d" || f[0] == "Tr") && f.size() > 1) {
            float a;
            if (number(f[1], a)) materials.back().alpha = std::clamp(f[0] == "d" ? a : 1 - a, 0.0f, 1.0f);
        } else if (f[0] == "Ns" && f.size() > 1) {
            number(f[1], materials.back().shininess);
        }
    }

    std::vector<Vec3> positions, normals;
    std::vector<std::pair<float, float>> uvs;
    std::string object = "Mesh", material;
    std::map<std::pair<std::string, std::string>, size_t> part_index;
    std::vector<std::map<std::tuple<int, int, int, int>, uint16_t>> lookups;
    const bool swap = options.mirror_x != options.flip_faces; // each one reverses the winding
    size_t face_number = 0, line_number = 0;
    for (size_t pos = 0; pos <= obj.size();) {
        size_t end = obj.find('\n', pos);
        if (end == std::string_view::npos) end = obj.size();
        const auto line = trim(obj.substr(pos, end - pos));
        pos = end + 1;
        ++line_number;
        if (line.empty() || line[0] == '#') continue;
        const auto f = fields(line);
        const auto bad = [&](const char* what) {
            error = "OBJ line " + std::to_string(line_number) + ": " + what;
            return false;
        };
        if (f[0] == "v") {
            Vec3 p;
            if (f.size() < 4 || !number(f[1], p.x) || !number(f[2], p.y) || !number(f[3], p.z)) return bad("invalid vertex.");
            p = p * options.scale;
            if (options.mirror_x) p.x = -p.x;
            positions.push_back(p);
        } else if (f[0] == "vt") {
            float u = 0, v = 0;
            if (f.size() < 2 || !number(f[1], u) || (f.size() > 2 && !number(f[2], v))) return bad("invalid UV coordinate.");
            uvs.push_back({u, options.flip_v ? v : 1 - v}); // OBJ V grows upwards, Jet rows downwards
        } else if (f[0] == "vn") {
            Vec3 n;
            if (f.size() < 4 || !number(f[1], n.x) || !number(f[2], n.y) || !number(f[3], n.z)) return bad("invalid normal.");
            if (options.mirror_x) n.x = -n.x;
            if (options.flip_faces) n = -n;
            normals.push_back(n);
        } else if (f[0] == "o" || f[0] == "g") {
            object = f.size() > 1 ? std::string(trim(line.substr(1))) : "Mesh";
        } else if (f[0] == "usemtl") {
            material = f.size() > 1 ? std::string(trim(line.substr(6))) : "";
        } else if (f[0] == "f") {
            if (f.size() < 4) return bad("face with fewer than 3 vertices.");
            struct Corner { int v, t, n; };
            std::vector<Corner> corners;
            for (size_t k = 1; k < f.size(); ++k) {
                const auto token = f[k];
                const size_t a = token.find('/');
                const size_t b = a == std::string_view::npos ? std::string_view::npos : token.find('/', a + 1);
                Corner c{resolve(token.substr(0, a), positions.size()), -1, -1};
                if (a != std::string_view::npos) c.t = resolve(token.substr(a + 1, b == std::string_view::npos ? std::string_view::npos : b - a - 1), uvs.size());
                if (b != std::string_view::npos) c.n = resolve(token.substr(b + 1), normals.size());
                if (c.v < 0 || c.t == -2 || c.n == -2) return bad("invalid face index.");
                corners.push_back(c);
            }
            const auto key = std::make_pair(object, material);
            auto found = part_index.find(key);
            if (found == part_index.end()) {
                found = part_index.emplace(key, parts.size()).first;
                parts.push_back({});
                parts.back().object = object;
                parts.back().material = material;
                lookups.emplace_back();
            }
            ObjPart& part = parts[found->second];
            auto& lookup = lookups[found->second];
            // Faces without normals get a flat normal (their vertices are not shared).
            const Vec3 a = positions[size_t(corners[0].v)], b = positions[size_t(corners[1].v)], c = positions[size_t(corners[2].v)];
            Vec3 flat = (b - a).cross(c - a).normalized();
            if (swap) flat = -flat;
            ++face_number;
            std::vector<uint16_t> ids;
            for (const auto& corner : corners) {
                const int face = corner.n < 0 ? int(face_number) : -1;
                const auto vertex_key = std::make_tuple(corner.v, corner.t, corner.n, face);
                auto hit = lookup.find(vertex_key);
                if (hit == lookup.end()) {
                    if (part.mesh.vertices.size() >= MaxMeshVertices) return bad("part with more than 65535 vertices: split it in the 3D modelling program.");
                    MeshVertex vertex;
                    vertex.position = positions[size_t(corner.v)];
                    vertex.normal = corner.n >= 0 ? normals[size_t(corner.n)] : flat;
                    if (corner.t >= 0) { vertex.u = uvs[size_t(corner.t)].first; vertex.v = uvs[size_t(corner.t)].second; }
                    hit = lookup.emplace(vertex_key, uint16_t(part.mesh.vertices.size())).first;
                    part.mesh.vertices.push_back(vertex);
                }
                ids.push_back(hit->second);
            }
            for (size_t k = 1; k + 1 < ids.size(); ++k) {
                if (swap) part.mesh.indices.insert(part.mesh.indices.end(), {ids[0], ids[k + 1], ids[k]});
                else part.mesh.indices.insert(part.mesh.indices.end(), {ids[0], ids[k], ids[k + 1]});
            }
        }
    }
    if (parts.empty()) { error = "The OBJ file contains no faces."; return false; }
    // Unique node names: the object, plus the material when an object has several.
    std::map<std::string, int> per_object;
    for (const auto& p : parts) ++per_object[p.object];
    std::map<std::string, int> used;
    for (auto& p : parts) {
        std::string name = sanitize(per_object[p.object] > 1 && !p.material.empty() ? p.object + "_" + p.material : p.object);
        const int n = used[name]++;
        if (n) name += std::to_string(n + 1);
        p.name = name;
    }
    return true;
}

} // namespace jetstudio
