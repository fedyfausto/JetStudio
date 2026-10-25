#include "runtime.hpp"
#include <algorithm>
#include <unordered_map>

namespace jetstudio {
namespace {

struct Bucket { uint16_t color; uint32_t count; };
int channel(uint16_t c, int k) { return k == 0 ? ((c >> 11) & 31) << 3 : (k == 1 ? ((c >> 5) & 63) << 2 : (c & 31) << 3); }

// Splits the widest box at its weighted median until there are `slots` boxes; each colour maps to its box average.
std::unordered_map<uint16_t, uint16_t> median_cut(std::vector<Bucket> colors, size_t slots) {
    struct Box { size_t begin, end; };
    std::vector<Box> boxes{{0, colors.size()}};
    auto widest = [&](const Box& b, int& axis) {
        int best = -1;
        axis = 0;
        for (int k = 0; k < 3; ++k) {
            int lo = 255, hi = 0;
            for (size_t i = b.begin; i < b.end; ++i) { lo = std::min(lo, channel(colors[i].color, k)); hi = std::max(hi, channel(colors[i].color, k)); }
            if (hi - lo > best) { best = hi - lo; axis = k; }
        }
        return best;
    };
    while (boxes.size() < slots) {
        size_t pick = boxes.size();
        int range = 0, axis = 0;
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].end - boxes[i].begin < 2) continue;
            int a;
            const int r = widest(boxes[i], a);
            if (r > range) { range = r; pick = i; axis = a; }
        }
        if (pick == boxes.size()) break;
        Box& b = boxes[pick];
        std::sort(colors.begin() + long(b.begin), colors.begin() + long(b.end),
                  [&](const Bucket& x, const Bucket& y) { return channel(x.color, axis) < channel(y.color, axis); });
        uint64_t total = 0, running = 0;
        for (size_t i = b.begin; i < b.end; ++i) total += colors[i].count;
        size_t split = b.begin + 1;
        for (size_t i = b.begin; i < b.end - 1; ++i) {
            running += colors[i].count;
            split = i + 1;
            if (running * 2 >= total) break;
        }
        boxes.push_back({split, b.end});
        boxes[pick].end = split;
    }
    std::unordered_map<uint16_t, uint16_t> map;
    for (const auto& b : boxes) {
        uint64_t sum[3] = {0, 0, 0}, weight = 0;
        for (size_t i = b.begin; i < b.end; ++i) {
            for (int k = 0; k < 3; ++k) sum[k] += uint64_t(channel(colors[i].color, k)) * colors[i].count;
            weight += colors[i].count;
        }
        const uint32_t r = uint32_t(sum[0] / weight), g = uint32_t(sum[1] / weight), bl = uint32_t(sum[2] / weight);
        const uint16_t average = uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (bl >> 3));
        for (size_t i = b.begin; i < b.end; ++i) map[colors[i].color] = average;
    }
    return map;
}

} // namespace

bool make_texture(std::string path, int width, int height, const uint8_t* rgba, TextureAsset& out, std::string& error, PaletteMode mode) {
    if (width < 1 || height < 1 || width > MaxTextureSide || height > MaxTextureSide || !rgba) {
        error = "Texture " + path + ": unsupported size (1.." + std::to_string(MaxTextureSide) + " pixels per side).";
        return false;
    }
    TextureAsset t;
    t.path = std::move(path);
    t.width = uint16_t(width);
    t.height = uint16_t(height);
    const size_t count = size_t(width) * size_t(height);
    std::vector<uint16_t> colors(count);
    std::vector<bool> used(65536, false);
    bool transparent = false;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = rgba + i * 4;
        if (p[3] < 128) { transparent = true; continue; }
        const uint16_t c = uint16_t(((p[0] * 31 + 127) / 255) << 11 | ((p[1] * 63 + 127) / 255) << 5 | ((p[2] * 31 + 127) / 255));
        colors[i] = c;
        used[c] = true;
    }
    if (transparent) {
        // Colour key: magenta when the opaque pixels never use it, else any free value.
        uint32_t key = 0xF81F;
        while (key < 65536 && used[key]) ++key;
        if (key == 65536) for (key = 0; used[key]; ++key) {}
        t.has_alpha = true;
        t.alpha_color = uint16_t(key);
        for (size_t i = 0; i < count; ++i)
            if (rgba[i * 4 + 3] < 128) colors[i] = t.alpha_color;
    }
    if (mode == PaletteMode::Quantize) {
        // Reserve one palette slot for the transparent key colour.
        std::unordered_map<uint16_t, uint32_t> histogram;
        for (size_t i = 0; i < count; ++i)
            if (!(transparent && colors[i] == t.alpha_color)) ++histogram[colors[i]];
        const size_t slots = transparent ? 255 : 256;
        if (histogram.size() > slots) {
            std::vector<Bucket> buckets;
            for (const auto& [c, n] : histogram) buckets.push_back({c, n});
            const auto map = median_cut(std::move(buckets), slots);
            for (size_t i = 0; i < count; ++i)
                if (!(transparent && colors[i] == t.alpha_color)) colors[i] = map.at(colors[i]);
            // A reduced colour could now equal the key: nudge it.
            if (transparent)
                for (size_t i = 0; i < count; ++i)
                    if (rgba[i * 4 + 3] >= 128 && colors[i] == t.alpha_color) colors[i] ^= 1;
        }
    }
    std::unordered_map<uint16_t, uint8_t> index;
    bool indexed = mode != PaletteMode::Rgb565;
    for (uint16_t c : colors) {
        if (!indexed) break;
        if (index.count(c)) continue;
        if (index.size() == 256) { indexed = false; break; }
        index.emplace(c, uint8_t(index.size()));
    }
    if (indexed) {
        t.palette.resize(index.size());
        for (const auto& [c, i] : index) t.palette[i] = c;
        t.indices.resize(count);
        for (size_t i = 0; i < count; ++i) t.indices[i] = index[colors[i]];
    } else {
        t.pixels = std::move(colors);
    }
    out = std::move(t);
    return true;
}

} // namespace jetstudio
