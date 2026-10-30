#include "runtime.hpp"
#include "script_host.hpp"
#include <cmath>

namespace jetstudio {
namespace {
using jetscript::Value;

int find_animation(const NodeExtras& e, std::string_view name) {
    for (size_t i = 0; i < e.player_animations.size(); ++i)
        if (e.player_animations[i].name == name) return int(i);
    return -1;
}

// Eased by the earlier key's curve; outside the keys, the nearest one.
bool value_at(const AnimationTrack& track, float time, Value& out) {
    const auto& keys = track.keys;
    if (keys.empty()) return false;
    size_t next = 0;
    while (next < keys.size() && keys[next].time <= time) ++next;
    if (next == 0) { out = keys.front().value; return true; }
    if (next == keys.size()) { out = keys.back().value; return true; }
    const AnimationKey& a = keys[next - 1];
    const AnimationKey& b = keys[next];
    if (track.interpolation == 0 || b.time <= a.time) { out = a.value; return true; }
    out = interpolate(a.value, b.value, ease_curve((time - a.time) / (b.time - a.time), a.transition));
    return true;
}
} // namespace

// Paths start from the player's parent, Godot's default root_node "..".
int Runtime::animation_target(const Live& player, const std::string& path) const {
    int current = player.parent;
    size_t start = 0;
    while (current >= 0 && start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        const std::string_view part = std::string_view(path).substr(start, end - start);
        if (part == "..") {
            current = nodes_[size_t(current)].parent;
        } else if (!part.empty() && part != ".") {
            int found = -1;
            for (int child : nodes_[size_t(current)].children)
                if (!nodes_[size_t(child)].freed && nodes_[size_t(child)].d.name == part) found = child;
            current = found;
        }
        start = end + 1;
    }
    return current >= 0 && !nodes_[size_t(current)].freed ? current : -1;
}

void Runtime::apply_player(const Live& n, const Animation& a, float time) {
    for (const auto& track : a.tracks) {
        if (track.property.empty()) continue;
        Value value;
        const int target = animation_target(n, track.path);
        if (target < 0 || !value_at(track, time, value)) continue;
        if (track.property_id == -2) {
            // "position:x" animates one component; a name the engine doesn't know may be a script variable.
            const size_t colon = track.property.find(':');
            const std::string name = track.property.substr(0, colon);
            if (colon != std::string::npos) {
                const char c = colon + 1 < track.property.size() ? track.property[colon + 1] : '?';
                track.component = int8_t(c == 'x' || c == 'r' ? 0 : c == 'y' || c == 'g' ? 1 : c == 'z' || c == 'b' ? 2 : -1);
            }
            track.property_id = host_->property_id(name);
        }
        const uint32_t id = nodes_[size_t(target)].d.id;
        std::string error;
        if (track.property_id < 0) {
            vm_->set_member(id, track.property, value);
            continue;
        }
        if (track.component >= 0) {
            Value whole;
            if (host_->get_property(id, track.property_id, whole, error) != jetscript::Status::Ok) continue;
            value = with_component(whole, track.component, value.as_real());
        }
        host_->set_property(id, track.property_id, value, error);
    }
}

// Method keys passed between two instants, in the direction of play; they run with the other deferred calls.
void Runtime::fire_method_keys(const Live& n, const Animation& a, float from, float to) {
    if (!playing_) return;
    const bool forwards = to >= from;
    for (const auto& track : a.tracks) {
        if (!track.property.empty()) continue;
        const int target = animation_target(n, track.path);
        if (target < 0) continue;
        for (const auto& key : track.keys) {
            const bool passed = forwards ? key.time > from && key.time <= to : key.time < from && key.time >= to;
            if (passed && !key.method.empty())
                deferred_.push_back({0, nullptr, Value::callable(nodes_[size_t(target)].d.id, key.method), key.args});
        }
    }
}

bool Runtime::play_player(Live& n, std::string_view name, float speed, bool from_end) {
    auto& e = n.d.extras();
    int index = name.empty() ? e.player_current : find_animation(e, name);
    if (index < 0 && name.empty()) index = find_animation(e, e.animation);
    if (index < 0) return false;
    const Animation& a = e.player_animations[size_t(index)];
    // The same animation resumes where it is, unless it had ended.
    const bool ended = !e.playing && (e.anim_time >= a.length || e.anim_time <= 0);
    if (index != e.player_current || ended || from_end) {
        e.player_current = int16_t(index);
        e.anim_time = speed < 0 || from_end ? a.length : 0;
        e.player_fresh = true;
        defer_signal(n.d.id, "animation_started", {Value(a.name)});
    }
    e.player_speed = speed == 0 ? 1 : speed;
    e.playing = true;
    return true;
}

// As Godot's stop(): back to the start, which is shown unless keep_state.
void Runtime::stop_player(Live& n, bool keep_state) {
    auto& e = n.d.extras();
    e.playing = false;
    e.anim_time = 0;
    if (!keep_state && e.player_current >= 0) apply_player(n, e.player_animations[size_t(e.player_current)], 0);
}

void Runtime::seek_player(Live& n, float time, bool update) {
    auto& e = n.d.extras();
    if (e.player_current < 0) return;
    const Animation& a = e.player_animations[size_t(e.player_current)];
    e.anim_time = std::clamp(time, 0.0f, a.length);
    e.player_fresh = false;
    if (update) apply_player(n, a, e.anim_time);
}

// One frame: loops wrap, ping-pong turns back, the others stop at their end and emit animation_finished.
void Runtime::advance_player(Live& n, float dt) {
    auto& e = n.d.extras();
    if (e.player_current < 0 || size_t(e.player_current) >= e.player_animations.size()) {
        e.playing = false;
        return;
    }
    const Animation& a = e.player_animations[size_t(e.player_current)];
    const float length = std::max(a.length, 0.001f);
    float before = e.anim_time;
    if (e.player_fresh) before += e.player_speed > 0 ? -0.0001f : 0.0001f; // keys at the very start fire too
    e.player_fresh = false;
    float now = before + dt * e.speed_scale * e.player_speed;
    bool finished = false;
    if (a.loop == 1) {
        if (now > length) {
            fire_method_keys(n, a, before, length);
            before = -0.0001f;
            now = std::fmod(now, length);
        } else if (now < 0) {
            fire_method_keys(n, a, before, 0);
            before = length + 0.0001f;
            now = length + std::fmod(now, length);
        }
    } else if (a.loop == 2) {
        if (now > length || now < 0) {
            const float edge = now > length ? length : 0;
            fire_method_keys(n, a, before, edge);
            before = edge;
            now = 2 * edge - now;
            e.player_speed = -e.player_speed;
        }
    } else if (now >= length || now <= 0) {
        finished = (now >= length) == (e.player_speed > 0);
        now = std::clamp(now, 0.0f, length);
    }
    fire_method_keys(n, a, before, now);
    e.anim_time = std::clamp(now, 0.0f, length);
    apply_player(n, a, e.anim_time);
    if (finished) {
        e.playing = false;
        defer_signal(n.d.id, "animation_finished", {Value(a.name)});
    }
}

bool Runtime::preview_animation(uint32_t player, std::string_view name, float time) {
    const int index = index_of(player);
    if (index < 0 || nodes_[size_t(index)].d.type != NodeType::AnimationPlayer) return false;
    const Live& n = nodes_[size_t(index)];
    const int animation = find_animation(n.d.more(), name);
    if (animation < 0) return false;
    apply_player(n, n.d.more().player_animations[size_t(animation)], time);
    update_transforms();
    return true;
}

} // namespace jetstudio
