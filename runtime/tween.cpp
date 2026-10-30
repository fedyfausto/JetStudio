#include "runtime.hpp"
#include "script_host.hpp"
#include <cmath>

namespace jetstudio {
namespace {
using jetscript::Type;
using jetscript::Value;

constexpr float Pi = 3.14159265f;
constexpr size_t MaxTweens = 256;

float bounce_out(float t) {
    if (t < 1 / 2.75f) return 7.5625f * t * t;
    if (t < 2 / 2.75f) { t -= 1.5f / 2.75f; return 7.5625f * t * t + 0.75f; }
    if (t < 2.5f / 2.75f) { t -= 2.25f / 2.75f; return 7.5625f * t * t + 0.9375f; }
    t -= 2.625f / 2.75f;
    return 7.5625f * t * t + 0.984375f;
}

// Godot's spring, an "out" curve that overshoots and settles.
float spring_out(float t) {
    return (std::sin(t * Pi * (0.2f + 2.5f * t * t * t)) * std::pow(1 - t, 2.2f) + t) * (1 + 1.2f * (1 - t));
}

// The "in" curve of each Tween.TRANS_*, in Godot's order; the other eases are built from it.
float curve_in(int trans, float t) {
    switch (trans) {
    case 1: return 1 - std::cos(t * Pi * 0.5f);
    case 2: return t * t * t * t * t;
    case 3: return t * t * t * t;
    case 4: return t * t;
    case 5: return t <= 0 ? 0 : std::pow(2.0f, 10 * (t - 1));
    case 6: {
        if (t <= 0 || t >= 1) return t;
        const float p = 0.3f, s = p / 4;
        return -(std::pow(2.0f, 10 * (t - 1)) * std::sin((t - 1 - s) * 2 * Pi / p));
    }
    case 7: return t * t * t;
    case 8: return 1 - std::sqrt(std::max(0.0f, 1 - t * t));
    case 9: return 1 - bounce_out(1 - t);
    case 10: { const float s = 1.70158f; return t * t * ((s + 1) * t - s); }
    case 11: return 1 - spring_out(1 - t);
    default: return t;
    }
}

// Tween.EASE_IN, EASE_OUT, EASE_IN_OUT, EASE_OUT_IN.
float eased(int trans, int ease, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto out = [&](float x) { return 1 - curve_in(trans, 1 - x); };
    switch (ease) {
    case 0: return curve_in(trans, t);
    case 1: return out(t);
    case 3: return t < 0.5f ? out(t * 2) * 0.5f : 0.5f + curve_in(trans, t * 2 - 1) * 0.5f;
    default: return t < 0.5f ? curve_in(trans, t * 2) * 0.5f : 0.5f + out(t * 2 - 1) * 0.5f;
    }
}

} // namespace

// ints stay ints; vectors and colours go component by component; anything else jumps at the end.
Value interpolate(const Value& a, const Value& b, float w) {
    if (a.is_number() && b.is_number()) {
        const float x = a.as_real() + (b.as_real() - a.as_real()) * w;
        return a.type() == Type::Int && b.type() == Type::Int ? Value(jetscript::Int(std::lround(x))) : Value(x);
    }
    if (a.type() != b.type()) return w >= 1 ? b : a;
    auto lerp = [&](float x, float y) { return x + (y - x) * w; };
    switch (a.type()) {
    case Type::Vector2: {
        const auto p = a.as_vector2(), q = b.as_vector2();
        return Value(jetscript::Vector2{lerp(p.x, q.x), lerp(p.y, q.y)});
    }
    case Type::Vector3: {
        const auto p = a.as_vector3(), q = b.as_vector3();
        return Value(jetscript::Vector3{lerp(p.x, q.x), lerp(p.y, q.y), lerp(p.z, q.z)});
    }
    case Type::Color: {
        const auto p = a.as_color(), q = b.as_color();
        return Value(jetscript::Color{lerp(p.r, q.r), lerp(p.g, q.g), lerp(p.b, q.b)});
    }
    default: return w >= 1 ? b : a;
    }
}

namespace {
// as_relative(): the end value is the start plus the given one.
Value plus(const Value& a, const Value& b) {
    if (a.is_number() && b.is_number())
        return a.type() == Type::Int && b.type() == Type::Int ? Value(a.as_int() + b.as_int()) : Value(a.as_real() + b.as_real());
    if (a.type() != b.type()) return b;
    switch (a.type()) {
    case Type::Vector2: { const auto p = a.as_vector2(), q = b.as_vector2(); return Value(jetscript::Vector2{p.x + q.x, p.y + q.y}); }
    case Type::Vector3: { const auto p = a.as_vector3(), q = b.as_vector3(); return Value(jetscript::Vector3{p.x + q.x, p.y + q.y, p.z + q.z}); }
    case Type::Color: { const auto p = a.as_color(), q = b.as_color(); return Value(jetscript::Color{p.r + q.r, p.g + q.g, p.b + q.b}); }
    default: return b;
    }
}

} // namespace

float component_of(const Value& v, int c) {
    switch (v.type()) {
    case Type::Vector2: return c == 0 ? v.as_vector2().x : v.as_vector2().y;
    case Type::Vector3: { const auto p = v.as_vector3(); return c == 0 ? p.x : c == 1 ? p.y : p.z; }
    case Type::Color: { const auto p = v.as_color(); return c == 0 ? p.r : c == 1 ? p.g : p.b; }
    default: return v.as_real();
    }
}

Value with_component(const Value& v, int c, float x) {
    switch (v.type()) {
    case Type::Vector2: { auto p = v.as_vector2(); (c == 0 ? p.x : p.y) = x; return Value(p); }
    case Type::Vector3: { auto p = v.as_vector3(); (c == 0 ? p.x : c == 1 ? p.y : p.z) = x; return Value(p); }
    case Type::Color: { auto p = v.as_color(); (c == 0 ? p.r : c == 1 ? p.g : p.b) = x; return Value(p); }
    default: return v;
    }
}

// Godot's ease().
float ease_curve(float x, float c) {
    x = std::clamp(x, 0.0f, 1.0f);
    if (c > 0) return c < 1 ? 1 - std::pow(1 - x, 1 / c) : std::pow(x, c);
    if (c < 0) return x < 0.5f ? std::pow(x * 2, -c) * 0.5f : (1 - std::pow(1 - (x - 0.5f) * 2, -c)) * 0.5f + 0.5f;
    return 0;
}

uint32_t Runtime::create_tween(uint32_t bound) {
    if (tweens_.size() >= MaxTweens) return 0;
    Tween t;
    t.id = next_tween_id_++;
    t.bound = bound;
    tweens_.push_back(std::move(t));
    return tweens_.back().id;
}

Runtime::Tween* Runtime::tween_of(uint32_t id) {
    for (auto& t : tweens_)
        if (t.id == id) return t.done ? nullptr : &t;
    return nullptr;
}

Runtime::Tweener* Runtime::tweener_of(uint32_t id) {
    for (auto& t : tweens_)
        for (auto& tw : t.tweeners)
            if (tw.id == id) return &tw;
    return nullptr;
}

// Each tweener is a step of its own, unless parallel() or set_parallel() put it with the previous one.
Runtime::Tweener& Runtime::add_tweener(Tween& t, Tweener::Kind kind) {
    Tweener tw;
    tw.kind = kind;
    tw.id = next_tweener_id_++;
    tw.trans = t.trans;
    tw.ease = t.ease;
    if (t.tweeners.empty()) {
        tw.step = 0;
        t.steps = 1;
    } else if ((t.parallel || t.join_next) && !t.new_step) {
        tw.step = t.steps - 1;
    } else {
        tw.step = t.steps++;
    }
    t.join_next = t.new_step = false;
    t.tweeners.push_back(std::move(tw));
    return t.tweeners.back();
}

void Runtime::start_tweener(Tweener& tw) {
    tw.started = true;
    if (tw.kind == Tweener::Kind::Method) {
        tw.start = tw.from;
        tw.end = tw.to;
        return;
    }
    if (tw.kind != Tweener::Kind::Property) return;
    Value current;
    std::string error;
    if (!tw.has_from && host_->get_property(tw.target, tw.property, current, error) != jetscript::Status::Ok) {
        tw.finished = true; // the object went away
        return;
    }
    if (!tw.has_from && tw.component >= 0) current = Value(component_of(current, tw.component));
    tw.start = tw.has_from ? tw.from : current;
    tw.end = tw.relative ? plus(tw.start, tw.to) : tw.to;
}

void Runtime::apply_tweener(Tweener& tw, float progress) {
    const float w = eased(tw.trans, tw.ease, progress);
    if (tw.kind == Tweener::Kind::Method) {
        deferred_.push_back({0, nullptr, tw.callable, {interpolate(tw.start, tw.end, w)}});
        return;
    }
    if (tw.kind != Tweener::Kind::Property || tw.finished) return;
    Value value = interpolate(tw.start, tw.end, w);
    std::string error;
    if (tw.component >= 0) {
        Value whole;
        if (host_->get_property(tw.target, tw.property, whole, error) != jetscript::Status::Ok) { tw.finished = true; return; }
        value = with_component(whole, tw.component, value.as_real());
    }
    if (host_->set_property(tw.target, tw.property, value, error) != jetscript::Status::Ok) {
        tw.finished = true;
        if (!error.empty()) log("Tween: " + error, true);
    }
}

// Runs `time` seconds of the tween; false once it has finished. The time a step overruns goes to the next one.
bool Runtime::advance_tween(Tween& t, float time) {
    for (int guard = 0; guard < 1000; ++guard) {
        t.step_time += time;
        float end = 0;
        for (auto& tw : t.tweeners) {
            if (tw.step != t.step) continue;
            end = std::max(end, tw.delay + tw.duration);
            if (tw.finished || t.step_time < tw.delay) continue;
            if (!tw.started) start_tweener(tw);
            if (tw.finished) continue;
            const float active = t.step_time - tw.delay;
            if (active < tw.duration) {
                apply_tweener(tw, active / tw.duration);
                continue;
            }
            apply_tweener(tw, 1);
            tw.finished = true;
            if (tw.kind == Tweener::Kind::Callback) deferred_.push_back({0, nullptr, tw.callable, {}});
            defer_signal(tw.id, "finished");
        }
        if (t.step_time < end) return true;

        time = t.step_time - end;
        t.step_time = 0;
        defer_signal(t.id, "step_finished", {Value(jetscript::Int(t.step))});
        if (++t.step < t.steps) continue;

        // A loop is over: again, or the end.
        ++t.loop;
        defer_signal(t.id, "loop_finished", {Value(jetscript::Int(t.loop))});
        if (t.loops != 0 && t.loop >= t.loops) {
            t.done = true;
            defer_signal(t.id, "finished");
            return false;
        }
        t.step = 0;
        for (auto& tw : t.tweeners) tw.started = tw.finished = false;
        if (time <= 0 && end <= 0) break; // an endless loop of instant steps: once per frame
    }
    return true;
}

// After the scripts' _process(), as Godot's idle tweens. Callables and signals run once every tween has moved.
void Runtime::update_tweens(float dt) {
    if (tweens_.empty()) return;
    for (size_t i = 0; i < tweens_.size(); ++i) {
        Tween& t = tweens_[i];
        if (t.done || !t.running) continue;
        // A node's tween dies with it, without finishing.
        const int bound = t.bound ? index_of(t.bound) : 0;
        if (t.tweeners.empty() || bound < 0 || nodes_[size_t(bound)].freed) {
            t.done = true;
            continue;
        }
        t.started = true;
        t.elapsed += dt * t.speed;
        advance_tween(t, dt * t.speed);
    }
    run_deferred();
    tweens_.erase(std::remove_if(tweens_.begin(), tweens_.end(), [](const Tween& t) { return t.done; }), tweens_.end());
}

// Scene changes keep only the autoloads' tweens: the next scene's nodes may get the old ids.
void Runtime::kill_tweens(bool keep_autoloads) {
    tweens_.erase(std::remove_if(tweens_.begin(), tweens_.end(),
                                 [&](const Tween& t) { return !keep_autoloads || t.bound < AutoloadIdBase; }),
                  tweens_.end());
    deferred_.clear();
}

} // namespace jetstudio
