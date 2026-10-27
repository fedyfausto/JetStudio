#include "js_internal.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>

namespace jetscript {
namespace detail {

namespace {

constexpr Real kCmpEpsilon = 0.00001f;
constexpr Real kPi = 3.14159265358979323846f;
constexpr Real kTau = 6.28318530717958647692f;

bool is_num(const Value& v) { return v.type() == Type::Int || v.type() == Type::Float; }
double num(const Value& v) { return v.type() == Type::Int ? double(Heap::i(v)) : double(Heap::f(v)); }

bool approx_eq(Real a, Real b) {
    if (a == b) return true;
    Real tol = kCmpEpsilon * std::fabs(a);
    if (tol < kCmpEpsilon) tol = kCmpEpsilon;
    return std::fabs(a - b) < tol;
}

Vector3 vec3(Real x, Real y, Real z) {
    Vector3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}
Color col3(Real r, Real g, Real b) {
    Color c;
    c.r = r;
    c.g = g;
    c.b = b;
    return c;
}
Vector3 vsub(const Vector3& a, const Vector3& b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
Vector3 vadd(const Vector3& a, const Vector3& b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
Vector3 vmul(const Vector3& a, Real s) { return vec3(a.x * s, a.y * s, a.z * s); }
Real vdot(const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vector3 vcross(const Vector3& a, const Vector3& b) {
    return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
Real vlen(const Vector3& a) { return std::sqrt(vdot(a, a)); }
Vector3 vnorm(const Vector3& a) {
    Real l = vlen(a);
    return l == 0 ? Vector3() : vmul(a, 1 / l);
}

bool fail(BuiltinCtx& ctx, std::string msg) {
    ctx.error = std::move(msg);
    return false;
}

bool util_type_error(BuiltinCtx& ctx, const char* fn, int index, const Value& v, const char* expected) {
    std::string m = "Invalid type in utility function '";
    m += fn;
    m += "'. Cannot convert argument ";
    m += format_int(index + 1);
    m += " from ";
    m += type_name(v.type());
    m += " to ";
    m += expected;
    m += ".";
    return fail(ctx, std::move(m));
}

bool get_real(BuiltinCtx& ctx, const char* fn, const Value* args, int i, Real& out) {
    if (!is_num(args[i])) return util_type_error(ctx, fn, i, args[i], "float");
    out = args[i].as_real();
    return true;
}
bool get_int(BuiltinCtx& ctx, const char* fn, const Value* args, int i, Int& out) {
    if (!is_num(args[i])) return util_type_error(ctx, fn, i, args[i], "int");
    out = args[i].as_int();
    return true;
}

std::string join_args(const Value* args, int argc, const Host* host, const char* sep) {
    std::string s;
    for (int i = 0; i < argc; ++i) {
        if (i && sep) s += sep;
        append_value(s, args[i], host, 0, false);
    }
    return s;
}

bool bf_print(BuiltinCtx& ctx, const Value* a, int n, Value&) {
    if (ctx.host) ctx.host->print(join_args(a, n, ctx.host, nullptr), false);
    return true;
}
bool bf_printerr(BuiltinCtx& ctx, const Value* a, int n, Value&) {
    if (ctx.host) ctx.host->print(join_args(a, n, ctx.host, nullptr), true);
    return true;
}
bool bf_prints(BuiltinCtx& ctx, const Value* a, int n, Value&) {
    if (ctx.host) ctx.host->print(join_args(a, n, ctx.host, " "), false);
    return true;
}
bool bf_printt(BuiltinCtx& ctx, const Value* a, int n, Value&) {
    if (ctx.host) ctx.host->print(join_args(a, n, ctx.host, "\t"), false);
    return true;
}
bool bf_push_error(BuiltinCtx& ctx, const Value* a, int n, Value&) {
    if (ctx.host) ctx.host->print("ERROR: " + join_args(a, n, ctx.host, nullptr), true);
    return true;
}
bool bf_push_warning(BuiltinCtx& ctx, const Value* a, int n, Value&) {
    if (ctx.host) ctx.host->print("WARNING: " + join_args(a, n, ctx.host, nullptr), true);
    return true;
}
bool bf_str(BuiltinCtx& ctx, const Value* a, int n, Value& out) {
    out = Value(join_args(a, n, ctx.host ? ctx.host : default_host(), nullptr));
    return true;
}

Int string_to_int(std::string_view s) {
    uint64_t v = 0;
    bool neg = false, any = false;
    for (char c : s) {
        if (c == '.') break;
        if (c >= '0' && c <= '9') {
            if (v > (UINT64_C(9223372036854775807) - uint64_t(c - '0')) / 10) return neg ? INT64_MIN : INT64_MAX;
            v = v * 10 + uint64_t(c - '0');
            any = true;
        } else if (c == '-' && !any) {
            neg = !neg;
        }
    }
    return neg ? Int(0 - v) : Int(v);
}

double string_to_float(std::string_view s, bool* full = nullptr) {
    std::string tmp(s);
    const char* begin = tmp.c_str();
    char* end = nullptr;
    double d = std::strtod(begin, &end);
    if (full) {
        const char* p = end;
        *full = end != begin;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (*p) *full = false;
    }
    return end == begin ? 0.0 : d;
}

bool bf_int(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    switch (a[0].type()) {
    case Type::Int: out = a[0]; return true;
    case Type::Float: out = Value(real_to_int(Heap::f(a[0]))); return true;
    case Type::Bool: out = Value(Int(Heap::b(a[0]) ? 1 : 0)); return true;
    case Type::String: out = Value(string_to_int(Heap::sv(a[0]))); return true;
    default: return fail(ctx, std::string("Cannot convert a value of type '") + type_name(a[0].type()) + "' to int.");
    }
}
bool bf_float(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    switch (a[0].type()) {
    case Type::Int:
    case Type::Float: out = Value(a[0].as_real()); return true;
    case Type::Bool: out = Value(Real(Heap::b(a[0]) ? 1 : 0)); return true;
    case Type::String: out = Value(Real(string_to_float(Heap::sv(a[0])))); return true;
    default: return fail(ctx, std::string("Cannot convert a value of type '") + type_name(a[0].type()) + "' to float.");
    }
}
bool bf_bool(BuiltinCtx&, const Value* a, int, Value& out) {
    out = Value(truthy(a[0]));
    return true;
}
bool bf_len(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    switch (a[0].type()) {
    case Type::String: out = Value(Int(Heap::str(a[0]) ? Heap::str(a[0])->ncp : 0)); return true;
    case Type::Array: out = Value(Int(Heap::arr(a[0])->items.size())); return true;
    case Type::Dictionary: out = Value(Int(Heap::dict(a[0])->entries.size())); return true;
    default: return fail(ctx, std::string("Value of type '") + type_name(a[0].type()) + "' can't provide a length.");
    }
}
bool bf_range(BuiltinCtx& ctx, const Value* a, int n, Value& out) {
    Int from = 0, to = 0, step = 1;
    if (n == 1) {
        if (!get_int(ctx, "range", a, 0, to)) return false;
    } else {
        if (!get_int(ctx, "range", a, 0, from) || !get_int(ctx, "range", a, 1, to)) return false;
        if (n == 3 && !get_int(ctx, "range", a, 2, step)) return false;
    }
    if (step == 0) return fail(ctx, "Step argument is zero!");
    double count = (double(to) - double(from)) / double(step);
    if (count > double(kMaxArrayItems)) return fail(ctx, "range() would create a too large Array.");
    std::vector<Value> items;
    if (count > 0) items.reserve(size_t(std::ceil(count)));
    if (step > 0) {
        for (Int i = from; i < to; i = Int(uint64_t(i) + uint64_t(step))) {
            items.push_back(Value(i));
            if (Int(uint64_t(i) + uint64_t(step)) < i) break;
        }
    } else {
        for (Int i = from; i > to; i = Int(uint64_t(i) + uint64_t(step))) {
            items.push_back(Value(i));
            if (Int(uint64_t(i) + uint64_t(step)) > i) break;
        }
    }
    out = make_array(std::move(items));
    return true;
}

// Numeric helpers that keep ints as ints and map over Vector3 components.
template <typename FI, typename FF>
bool map_numeric(BuiltinCtx& ctx, const char* name, const Value& v, Value& out, FI fi, FF ff) {
    switch (v.type()) {
    case Type::Int: out = Value(fi(Heap::i(v))); return true;
    case Type::Float: out = Value(ff(Heap::f(v))); return true;
    case Type::Vector3: {
        Vector3 x = Heap::vec(v);
        out = Value(vec3(ff(x.x), ff(x.y), ff(x.z)));
        return true;
    }
    default: return util_type_error(ctx, name, 0, v, "float");
    }
}

Int iabs(Int x) { return x < 0 ? Int(0 - uint64_t(x)) : x; }
Int isign(Int x) { return x > 0 ? 1 : (x < 0 ? -1 : 0); }
Real fsign(Real x) { return x > 0 ? 1.0f : (x < 0 ? -1.0f : 0.0f); }
Int iself(Int x) { return x; }

bool bf_abs(BuiltinCtx& c, const Value* a, int, Value& o) {
    return map_numeric(c, "abs", a[0], o, iabs, [](Real x) { return std::fabs(x); });
}
bool bf_sign(BuiltinCtx& c, const Value* a, int, Value& o) { return map_numeric(c, "sign", a[0], o, isign, fsign); }
bool bf_floor(BuiltinCtx& c, const Value* a, int, Value& o) {
    return map_numeric(c, "floor", a[0], o, iself, [](Real x) { return std::floor(x); });
}
bool bf_ceil(BuiltinCtx& c, const Value* a, int, Value& o) {
    return map_numeric(c, "ceil", a[0], o, iself, [](Real x) { return std::ceil(x); });
}
bool bf_round(BuiltinCtx& c, const Value* a, int, Value& o) {
    return map_numeric(c, "round", a[0], o, iself, [](Real x) { return std::round(x); });
}

#define JS_FLOAT_FN(fname, expr)                                          \
    bool bf_##fname(BuiltinCtx& ctx, const Value* a, int, Value& out) {   \
        Real x = 0;                                                           \
        if (!get_real(ctx, #fname, a, 0, x)) return false;                \
        out = Value(Real(expr));                                          \
        return true;                                                      \
    }
JS_FLOAT_FN(sqrt, std::sqrt(x))
JS_FLOAT_FN(exp, std::exp(x))
JS_FLOAT_FN(log, std::log(x))
JS_FLOAT_FN(sin, std::sin(x))
JS_FLOAT_FN(cos, std::cos(x))
JS_FLOAT_FN(tan, std::tan(x))
JS_FLOAT_FN(asin, std::asin(x < -1 ? -1.0f : (x > 1 ? 1.0f : x)))
JS_FLOAT_FN(acos, std::acos(x < -1 ? -1.0f : (x > 1 ? 1.0f : x)))
JS_FLOAT_FN(atan, std::atan(x))
JS_FLOAT_FN(sinh, std::sinh(x))
JS_FLOAT_FN(cosh, std::cosh(x))
JS_FLOAT_FN(tanh, std::tanh(x))
JS_FLOAT_FN(deg_to_rad, x * (kPi / 180.0f))
JS_FLOAT_FN(rad_to_deg, x * (180.0f / kPi))
JS_FLOAT_FN(absf, std::fabs(x))
JS_FLOAT_FN(signf, fsign(x))
JS_FLOAT_FN(floorf, std::floor(x))
JS_FLOAT_FN(ceilf, std::ceil(x))
JS_FLOAT_FN(roundf, std::round(x))
#undef JS_FLOAT_FN

#define JS_INT_FN(fname, expr)                                            \
    bool bf_##fname(BuiltinCtx& ctx, const Value* a, int, Value& out) {   \
        Real x = 0;                                                           \
        if (!get_real(ctx, #fname, a, 0, x)) return false;                \
        out = Value(Int(expr));                                           \
        return true;                                                      \
    }
JS_INT_FN(floori, a[0].type() == Type::Int ? Heap::i(a[0]) : real_to_int(std::floor(x)))
JS_INT_FN(ceili, a[0].type() == Type::Int ? Heap::i(a[0]) : real_to_int(std::ceil(x)))
JS_INT_FN(roundi, a[0].type() == Type::Int ? Heap::i(a[0]) : real_to_int(std::round(x)))
JS_INT_FN(signi, a[0].type() == Type::Int ? isign(Heap::i(a[0])) : Int(fsign(x)))
#undef JS_INT_FN

bool bf_absi(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Int x = 0;
    if (!get_int(ctx, "absi", a, 0, x)) return false;
    out = Value(iabs(x));
    return true;
}

#define JS_FLOAT2_FN(fname, expr)                                                                     \
    bool bf_##fname(BuiltinCtx& ctx, const Value* a, int, Value& out) {                               \
        Real x = 0, y = 0;                                                                                    \
        if (!get_real(ctx, #fname, a, 0, x) || !get_real(ctx, #fname, a, 1, y)) return false;         \
        out = Value(Real(expr));                                                                      \
        return true;                                                                                  \
    }
JS_FLOAT2_FN(pow, std::pow(x, y))
JS_FLOAT2_FN(atan2, std::atan2(x, y))
JS_FLOAT2_FN(fmod, std::fmod(x, y))
JS_FLOAT2_FN(minf, x < y ? x : y)
JS_FLOAT2_FN(maxf, x > y ? x : y)
#undef JS_FLOAT2_FN

bool bf_fposmod(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Real x = 0, y = 0;
    if (!get_real(ctx, "fposmod", a, 0, x) || !get_real(ctx, "fposmod", a, 1, y)) return false;
    Real v = std::fmod(x, y);
    if ((v < 0 && y > 0) || (v > 0 && y < 0)) v += y;
    out = Value(Real(v + 0.0f));
    return true;
}
bool bf_posmod(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Int x = 0, y = 0;
    if (!get_int(ctx, "posmod", a, 0, x) || !get_int(ctx, "posmod", a, 1, y)) return false;
    if (y == 0) return fail(ctx, "Modulo by zero error in function 'posmod'.");
    Int v = y == -1 ? 0 : x % y;
    if ((v < 0 && y > 0) || (v > 0 && y < 0)) v += y;
    out = Value(v);
    return true;
}
bool bf_mini(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Int x = 0, y = 0;
    if (!get_int(ctx, "mini", a, 0, x) || !get_int(ctx, "mini", a, 1, y)) return false;
    out = Value(x < y ? x : y);
    return true;
}
bool bf_maxi(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Int x = 0, y = 0;
    if (!get_int(ctx, "maxi", a, 0, x) || !get_int(ctx, "maxi", a, 1, y)) return false;
    out = Value(x > y ? x : y);
    return true;
}

bool minmax(BuiltinCtx& ctx, const char* name, const Value* a, int n, Value& out, bool want_max) {
    bool all_int = true;
    for (int i = 0; i < n; ++i) {
        if (!is_num(a[i])) return util_type_error(ctx, name, i, a[i], "float");
        if (a[i].type() != Type::Int) all_int = false;
    }
    if (all_int) {
        Int r = Heap::i(a[0]);
        for (int i = 1; i < n; ++i) {
            Int x = Heap::i(a[i]);
            if (want_max ? x > r : x < r) r = x;
        }
        out = Value(r);
    } else {
        Real r = a[0].as_real();
        for (int i = 1; i < n; ++i) {
            Real x = a[i].as_real();
            if (want_max ? x > r : x < r) r = x;
        }
        out = Value(r);
    }
    return true;
}
bool bf_min(BuiltinCtx& c, const Value* a, int n, Value& o) { return minmax(c, "min", a, n, o, false); }
bool bf_max(BuiltinCtx& c, const Value* a, int n, Value& o) { return minmax(c, "max", a, n, o, true); }

Real clampr(Real v, Real lo, Real hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool bf_clamp(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    if (a[0].type() == Type::Vector3 && a[1].type() == Type::Vector3 && a[2].type() == Type::Vector3) {
        Vector3 v = Heap::vec(a[0]), lo = Heap::vec(a[1]), hi = Heap::vec(a[2]);
        out = Value(vec3(clampr(v.x, lo.x, hi.x), clampr(v.y, lo.y, hi.y), clampr(v.z, lo.z, hi.z)));
        return true;
    }
    for (int i = 0; i < 3; ++i)
        if (!is_num(a[i])) return util_type_error(ctx, "clamp", i, a[i], "float");
    if (a[0].type() == Type::Int && a[1].type() == Type::Int && a[2].type() == Type::Int) {
        Int v = Heap::i(a[0]), lo = Heap::i(a[1]), hi = Heap::i(a[2]);
        out = Value(v < lo ? lo : (v > hi ? hi : v));
    } else {
        out = Value(clampr(a[0].as_real(), a[1].as_real(), a[2].as_real()));
    }
    return true;
}
bool bf_clampf(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Real v = 0, lo = 0, hi = 0;
    if (!get_real(ctx, "clampf", a, 0, v) || !get_real(ctx, "clampf", a, 1, lo) || !get_real(ctx, "clampf", a, 2, hi))
        return false;
    out = Value(clampr(v, lo, hi));
    return true;
}
bool bf_clampi(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Int v = 0, lo = 0, hi = 0;
    if (!get_int(ctx, "clampi", a, 0, v) || !get_int(ctx, "clampi", a, 1, lo) || !get_int(ctx, "clampi", a, 2, hi))
        return false;
    out = Value(v < lo ? lo : (v > hi ? hi : v));
    return true;
}
bool bf_lerp(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Real t = 0;
    if (!get_real(ctx, "lerp", a, 2, t)) return false;
    Type ta = a[0].type();
    if ((ta == Type::Vector3 || ta == Type::Color) && a[1].type() == ta) {
        Real r[3];
        for (int k = 0; k < 3; ++k) r[k] = Heap::comp(a[0], k) + (Heap::comp(a[1], k) - Heap::comp(a[0], k)) * t;
        if (ta == Type::Color) out = Value(col3(r[0], r[1], r[2]));
        else out = Value(vec3(r[0], r[1], r[2]));
        return true;
    }
    Real x = 0, y = 0;
    if (!get_real(ctx, "lerp", a, 0, x) || !get_real(ctx, "lerp", a, 1, y)) return false;
    out = Value(x + (y - x) * t);
    return true;
}
bool bf_lerpf(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Real x = 0, y = 0, t = 0;
    if (!get_real(ctx, "lerpf", a, 0, x) || !get_real(ctx, "lerpf", a, 1, y) || !get_real(ctx, "lerpf", a, 2, t))
        return false;
    out = Value(x + (y - x) * t);
    return true;
}
bool bf_inverse_lerp(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Real x = 0, y = 0, v = 0;
    if (!get_real(ctx, "inverse_lerp", a, 0, x) || !get_real(ctx, "inverse_lerp", a, 1, y) ||
        !get_real(ctx, "inverse_lerp", a, 2, v))
        return false;
    out = Value((v - x) / (y - x));
    return true;
}
bool bf_move_toward(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Real from = 0, to = 0, d = 0;
    if (!get_real(ctx, "move_toward", a, 0, from) || !get_real(ctx, "move_toward", a, 1, to) ||
        !get_real(ctx, "move_toward", a, 2, d))
        return false;
    out = Value(std::fabs(to - from) <= d ? to : from + fsign(to - from) * d);
    return true;
}
bool bf_smoothstep(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Real from = 0, to = 0, x = 0;
    if (!get_real(ctx, "smoothstep", a, 0, from) || !get_real(ctx, "smoothstep", a, 1, to) ||
        !get_real(ctx, "smoothstep", a, 2, x))
        return false;
    if (approx_eq(from, to)) {
        out = Value(Real(from));
        return true;
    }
    Real s = clampr((x - from) / (to - from), 0, 1);
    out = Value(s * s * (3.0f - 2.0f * s));
    return true;
}
bool bf_wrapf(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Real v = 0, lo = 0, hi = 0;
    if (!get_real(ctx, "wrapf", a, 0, v) || !get_real(ctx, "wrapf", a, 1, lo) || !get_real(ctx, "wrapf", a, 2, hi))
        return false;
    Real range = hi - lo;
    if (std::fabs(range) < kCmpEpsilon) {
        out = Value(lo);
        return true;
    }
    Real r = v - range * std::floor((v - lo) / range);
    if (approx_eq(r, hi)) r = lo;
    out = Value(r);
    return true;
}
bool bf_wrapi(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Int v = 0, lo = 0, hi = 0;
    if (!get_int(ctx, "wrapi", a, 0, v) || !get_int(ctx, "wrapi", a, 1, lo) || !get_int(ctx, "wrapi", a, 2, hi))
        return false;
    Int range = Int(uint64_t(hi) - uint64_t(lo));
    if (range == 0) {
        out = Value(lo);
        return true;
    }
    Int d = Int(uint64_t(v) - uint64_t(lo));
    Int m = range == -1 ? 0 : d % range;
    if (m < 0 && range > 0) m += range;
    if (m > 0 && range < 0) m += range;
    out = Value(Int(uint64_t(lo) + uint64_t(m)));
    return true;
}
bool bf_snapped(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    if (a[0].type() == Type::Int && a[1].type() == Type::Int) {
        Int v = Heap::i(a[0]), s = Heap::i(a[1]);
        if (s != 0) v = real_to_int(std::floor(double(v) / double(s) + 0.5) * double(s));
        out = Value(v);
        return true;
    }
    Real v = 0, s = 0;
    if (!get_real(ctx, "snapped", a, 0, v) || !get_real(ctx, "snapped", a, 1, s)) return false;
    if (s != 0) v = std::floor(v / s + 0.5f) * s;
    out = Value(v);
    return true;
}
bool bf_is_equal_approx(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Real x = 0, y = 0;
    if (!get_real(ctx, "is_equal_approx", a, 0, x) || !get_real(ctx, "is_equal_approx", a, 1, y)) return false;
    out = Value(approx_eq(x, y));
    return true;
}
bool bf_is_zero_approx(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Real x = 0;
    if (!get_real(ctx, "is_zero_approx", a, 0, x)) return false;
    out = Value(std::fabs(x) < kCmpEpsilon);
    return true;
}
bool bf_is_nan(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Real x = 0;
    if (!get_real(ctx, "is_nan", a, 0, x)) return false;
    out = Value(x != x);
    return true;
}
bool bf_is_inf(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Real x = 0;
    if (!get_real(ctx, "is_inf", a, 0, x)) return false;
    out = Value(bool(std::isinf(x)));
    return true;
}
bool bf_is_finite(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Real x = 0;
    if (!get_real(ctx, "is_finite", a, 0, x)) return false;
    out = Value(bool(std::isfinite(x)));
    return true;
}
bool bf_randomize(BuiltinCtx& ctx, const Value*, int, Value&) {
    uint64_t t = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    if (ctx.rng) ctx.rng->seed(t ^ (uint64_t(reinterpret_cast<uintptr_t>(&ctx)) << 16));
    return true;
}
bool bf_seed(BuiltinCtx& ctx, const Value* a, int, Value&) {
    Int s = 0;
    if (!get_int(ctx, "seed", a, 0, s)) return false;
    if (ctx.rng) ctx.rng->seed(uint64_t(s));
    return true;
}
bool bf_randf(BuiltinCtx& ctx, const Value*, int, Value& out) {
    out = Value(Real(ctx.rng ? ctx.rng->unit() : 0.0));
    return true;
}
bool bf_randi(BuiltinCtx& ctx, const Value*, int, Value& out) {
    out = Value(Int(ctx.rng ? ctx.rng->next() : 0));
    return true;
}
bool bf_randf_range(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Real x = 0, y = 0;
    if (!get_real(ctx, "randf_range", a, 0, x) || !get_real(ctx, "randf_range", a, 1, y)) return false;
    out = Value(Real(x + (y - x) * Real(ctx.rng ? ctx.rng->unit() : 0.0)));
    return true;
}
bool bf_randi_range(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Int x = 0, y = 0;
    if (!get_int(ctx, "randi_range", a, 0, x) || !get_int(ctx, "randi_range", a, 1, y)) return false;
    if (x > y) std::swap(x, y);
    uint64_t span = uint64_t(y) - uint64_t(x) + 1u;
    uint64_t r = ctx.rng ? (uint64_t(ctx.rng->next()) << 32 | ctx.rng->next()) : 0;
    out = Value(Int(uint64_t(x) + (span ? r % span : r)));
    return true;
}
bool bf_is_instance_valid(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    out = Value(a[0].type() == Type::Object && ctx.host && !ctx.host->class_of(Heap::o(a[0])).empty());
    return true;
}
bool bf_typeof(BuiltinCtx&, const Value* a, int, Value& out) {
    out = Value(type_name(a[0].type()));
    return true;
}
bool bf_char(BuiltinCtx& ctx, const Value* a, int, Value& out) {
    Int c = 0;
    if (!get_int(ctx, "char", a, 0, c)) return false;
    std::string s;
    utf8_append(s, (c < 0 || c > 0x10FFFF) ? 0xFFFDu : uint32_t(c));
    out = Value(s);
    return true;
}
bool bf_assert(BuiltinCtx& ctx, const Value* a, int n, Value&) {
    if (truthy(a[0])) return true;
    std::string m = "Assertion failed";
    if (n > 1) {
        m += ": ";
        m += stringify(a[1], ctx.host);
    } else {
        m += ".";
    }
    return fail(ctx, std::move(m));
}
bool bf_vector3(BuiltinCtx& ctx, const Value* a, int n, Value& out) {
    if (n == 0) {
        out = Value(Vector3());
        return true;
    }
    if (n == 1 && a[0].type() == Type::Vector3) {
        out = a[0];
        return true;
    }
    if (n == 3) {
        Real x = 0, y = 0, z = 0;
        if (!get_real(ctx, "Vector3", a, 0, x) || !get_real(ctx, "Vector3", a, 1, y) || !get_real(ctx, "Vector3", a, 2, z))
            return false;
        out = Value(vec3(x, y, z));
        return true;
    }
    return fail(ctx, "No constructor of \"Vector3\" matches the given arguments (expected (), (Vector3) or (x, y, z)).");
}
bool bf_color(BuiltinCtx& ctx, const Value* a, int n, Value& out) {
    if ((n == 1 || n == 2) && a[0].type() == Type::Color) {
        if (n == 2 && !is_num(a[1])) return util_type_error(ctx, "Color", 1, a[1], "float");
        out = a[0];
        return true;
    }
    if (n == 1 && a[0].type() == Type::String) {
        Color c;
        if (!parse_color_string(Heap::sv(a[0]), c))
            return fail(ctx, "Invalid color code: \"" + std::string(Heap::sv(a[0])) + "\".");
        out = Value(c);
        return true;
    }
    if (n == 3 || n == 4) {
        Real r = 0, g = 0, b = 0, al = 0;
        if (!get_real(ctx, "Color", a, 0, r) || !get_real(ctx, "Color", a, 1, g) || !get_real(ctx, "Color", a, 2, b))
            return false;
        if (n == 4 && !get_real(ctx, "Color", a, 3, al)) return false;
        out = Value(col3(r, g, b));
        return true;
    }
    return fail(ctx, "No constructor of \"Color\" matches the given arguments (expected (r, g, b[, a]), (Color) or (String)).");
}
bool bf_array(BuiltinCtx& ctx, const Value* a, int n, Value& out) {
    if (n == 0) {
        out = Value::new_array();
        return true;
    }
    if (a[0].type() == Type::Array) {
        out = a[0];
        return true;
    }
    return util_type_error(ctx, "Array", 0, a[0], "Array");
}
bool bf_dictionary(BuiltinCtx& ctx, const Value* a, int n, Value& out) {
    if (n == 0) {
        out = Value::new_dictionary();
        return true;
    }
    if (a[0].type() == Type::Dictionary) {
        out = a[0];
        return true;
    }
    return util_type_error(ctx, "Dictionary", 0, a[0], "Dictionary");
}

constexpr SType U = kUnknown;
constexpr SType T_NIL = SType(Type::Nil), T_BOOL = SType(Type::Bool), T_INT = SType(Type::Int),
                T_FLOAT = SType(Type::Float), T_STR = SType(Type::String), T_VEC = SType(Type::Vector3),
                T_COL = SType(Type::Color), T_ARR = SType(Type::Array), T_DICT = SType(Type::Dictionary);

const BuiltinFunc kFuncs[] = {
    {"print", 0, -1, T_NIL, 0, bf_print},
    {"printerr", 0, -1, T_NIL, 0, bf_printerr},
    {"prints", 0, -1, T_NIL, 0, bf_prints},
    {"printt", 0, -1, T_NIL, 0, bf_printt},
    {"push_error", 1, -1, T_NIL, 0, bf_push_error},
    {"push_warning", 1, -1, T_NIL, 0, bf_push_warning},
    {"str", 0, -1, T_STR, kPure, bf_str},
    {"int", 1, 1, T_INT, kPure, bf_int},
    {"float", 1, 1, T_FLOAT, kPure, bf_float},
    {"bool", 1, 1, T_BOOL, kPure, bf_bool},
    {"len", 1, 1, T_INT, kPure, bf_len},
    {"range", 1, 3, T_ARR, 0, bf_range},
    {"abs", 1, 1, U, kPure | kRetFirst, bf_abs},
    {"absf", 1, 1, T_FLOAT, kPure, bf_absf},
    {"absi", 1, 1, T_INT, kPure, bf_absi},
    {"sign", 1, 1, U, kPure | kRetFirst, bf_sign},
    {"signf", 1, 1, T_FLOAT, kPure, bf_signf},
    {"signi", 1, 1, T_INT, kPure, bf_signi},
    {"floor", 1, 1, U, kPure | kRetFirst, bf_floor},
    {"ceil", 1, 1, U, kPure | kRetFirst, bf_ceil},
    {"round", 1, 1, U, kPure | kRetFirst, bf_round},
    {"floorf", 1, 1, T_FLOAT, kPure, bf_floorf},
    {"ceilf", 1, 1, T_FLOAT, kPure, bf_ceilf},
    {"roundf", 1, 1, T_FLOAT, kPure, bf_roundf},
    {"floori", 1, 1, T_INT, kPure, bf_floori},
    {"ceili", 1, 1, T_INT, kPure, bf_ceili},
    {"roundi", 1, 1, T_INT, kPure, bf_roundi},
    {"sqrt", 1, 1, T_FLOAT, kPure, bf_sqrt},
    {"pow", 2, 2, T_FLOAT, kPure, bf_pow},
    {"exp", 1, 1, T_FLOAT, kPure, bf_exp},
    {"log", 1, 1, T_FLOAT, kPure, bf_log},
    {"sin", 1, 1, T_FLOAT, kPure, bf_sin},
    {"cos", 1, 1, T_FLOAT, kPure, bf_cos},
    {"tan", 1, 1, T_FLOAT, kPure, bf_tan},
    {"asin", 1, 1, T_FLOAT, kPure, bf_asin},
    {"acos", 1, 1, T_FLOAT, kPure, bf_acos},
    {"atan", 1, 1, T_FLOAT, kPure, bf_atan},
    {"atan2", 2, 2, T_FLOAT, kPure, bf_atan2},
    {"sinh", 1, 1, T_FLOAT, kPure, bf_sinh},
    {"cosh", 1, 1, T_FLOAT, kPure, bf_cosh},
    {"tanh", 1, 1, T_FLOAT, kPure, bf_tanh},
    {"deg_to_rad", 1, 1, T_FLOAT, kPure, bf_deg_to_rad},
    {"rad_to_deg", 1, 1, T_FLOAT, kPure, bf_rad_to_deg},
    {"min", 2, -1, U, kPure | kRetNumeric, bf_min},
    {"max", 2, -1, U, kPure | kRetNumeric, bf_max},
    {"minf", 2, 2, T_FLOAT, kPure, bf_minf},
    {"maxf", 2, 2, T_FLOAT, kPure, bf_maxf},
    {"mini", 2, 2, T_INT, kPure, bf_mini},
    {"maxi", 2, 2, T_INT, kPure, bf_maxi},
    {"clamp", 3, 3, U, kPure | kRetNumeric, bf_clamp},
    {"clampf", 3, 3, T_FLOAT, kPure, bf_clampf},
    {"clampi", 3, 3, T_INT, kPure, bf_clampi},
    {"lerp", 3, 3, U, kPure | kRetFirst, bf_lerp},
    {"lerpf", 3, 3, T_FLOAT, kPure, bf_lerpf},
    {"inverse_lerp", 3, 3, T_FLOAT, kPure, bf_inverse_lerp},
    {"move_toward", 3, 3, T_FLOAT, kPure, bf_move_toward},
    {"smoothstep", 3, 3, T_FLOAT, kPure, bf_smoothstep},
    {"wrapf", 3, 3, T_FLOAT, kPure, bf_wrapf},
    {"wrapi", 3, 3, T_INT, kPure, bf_wrapi},
    {"fmod", 2, 2, T_FLOAT, kPure, bf_fmod},
    {"fposmod", 2, 2, T_FLOAT, kPure, bf_fposmod},
    {"posmod", 2, 2, T_INT, kPure, bf_posmod},
    {"snapped", 2, 2, U, kPure | kRetNumeric, bf_snapped},
    {"is_equal_approx", 2, 2, T_BOOL, kPure, bf_is_equal_approx},
    {"is_zero_approx", 1, 1, T_BOOL, kPure, bf_is_zero_approx},
    {"is_nan", 1, 1, T_BOOL, kPure, bf_is_nan},
    {"is_inf", 1, 1, T_BOOL, kPure, bf_is_inf},
    {"is_finite", 1, 1, T_BOOL, kPure, bf_is_finite},
    {"randomize", 0, 0, T_NIL, 0, bf_randomize},
    {"seed", 1, 1, T_NIL, 0, bf_seed},
    {"randf", 0, 0, T_FLOAT, 0, bf_randf},
    {"randi", 0, 0, T_INT, 0, bf_randi},
    {"randf_range", 2, 2, T_FLOAT, 0, bf_randf_range},
    {"randi_range", 2, 2, T_INT, 0, bf_randi_range},
    {"is_instance_valid", 1, 1, T_BOOL, 0, bf_is_instance_valid},
    {"typeof", 1, 1, T_STR, kPure, bf_typeof},
    {"char", 1, 1, T_STR, kPure, bf_char},
    {"assert", 1, 2, T_NIL, 0, bf_assert},
    {"Vector3", 0, 3, T_VEC, kPure, bf_vector3},
    {"Color", 1, 4, T_COL, kPure, bf_color},
    {"Array", 0, 1, T_ARR, 0, bf_array},
    {"Dictionary", 0, 1, T_DICT, 0, bf_dictionary},
};

enum BM : int {
    BM_length, BM_length_squared, BM_normalized, BM_dot, BM_cross, BM_distance_to, BM_distance_squared_to,
    BM_lerp, BM_move_toward, BM_abs, BM_rotated, BM_angle_to, BM_is_zero_approx, BM_is_equal_approx,
    BM_direction_to, BM_floor, BM_ceil, BM_round, BM_sign, BM_limit_length, BM_to_html, BM_inverted,
    BM_lightened, BM_darkened, BM_to_upper, BM_to_lower, BM_begins_with, BM_ends_with, BM_contains, BM_find,
    BM_rfind, BM_substr, BM_split, BM_strip_edges, BM_replace, BM_to_int, BM_to_float, BM_is_empty, BM_left,
    BM_right, BM_repeat, BM_join, BM_is_valid_int, BM_is_valid_float, BM_unicode_at, BM_count, BM_size,
    BM_append, BM_push_back, BM_push_front, BM_pop_back, BM_pop_front, BM_insert, BM_remove_at, BM_erase,
    BM_has, BM_clear, BM_front, BM_back, BM_duplicate, BM_reverse, BM_sort, BM_pick_random, BM_slice, BM_max,
    BM_min, BM_resize, BM_fill, BM_append_array, BM_get, BM_keys, BM_values, BM_merge, BM_find_key,
    BM_COUNT
};

const char* const kMethodNames[] = {
    "length", "length_squared", "normalized", "dot", "cross", "distance_to", "distance_squared_to",
    "lerp", "move_toward", "abs", "rotated", "angle_to", "is_zero_approx", "is_equal_approx",
    "direction_to", "floor", "ceil", "round", "sign", "limit_length", "to_html", "inverted",
    "lightened", "darkened", "to_upper", "to_lower", "begins_with", "ends_with", "contains", "find",
    "rfind", "substr", "split", "strip_edges", "replace", "to_int", "to_float", "is_empty", "left",
    "right", "repeat", "join", "is_valid_int", "is_valid_float", "unicode_at", "count", "size",
    "append", "push_back", "push_front", "pop_back", "pop_front", "insert", "remove_at", "erase",
    "has", "clear", "front", "back", "duplicate", "reverse", "sort", "pick_random", "slice", "max",
    "min", "resize", "fill", "append_array", "get", "keys", "values", "merge", "find_key",
};
static_assert(sizeof(kMethodNames) / sizeof(kMethodNames[0]) == BM_COUNT, "method name table out of sync");

struct MCall {
    BuiltinCtx& ctx;
    int id;
    const Value& self;
    const Value* args;
    int argc;
    Value& out;

    const char* base() const { return type_name(self.type()); }
    bool arity(int lo, int hi) {
        if (argc >= lo && argc <= hi) return true;
        std::string m = "Invalid call to function '";
        m += kMethodNames[id];
        m += "' in base '";
        m += base();
        m += "'. Expected ";
        m += format_int(argc < lo ? lo : hi);
        m += " arguments.";
        ctx.error = std::move(m);
        return false;
    }
    bool type_error(int i, const char* expected) {
        std::string m = "Invalid type in function '";
        m += kMethodNames[id];
        m += "' in base '";
        m += base();
        m += "'. Cannot convert argument ";
        m += format_int(i + 1);
        m += " from ";
        m += type_name(args[i].type());
        m += " to ";
        m += expected;
        m += ".";
        ctx.error = std::move(m);
        return false;
    }
    bool real(int i, Real& r) {
        if (!is_num(args[i])) return type_error(i, "float");
        r = args[i].as_real();
        return true;
    }
    bool integer(int i, Int& r) {
        if (!is_num(args[i])) return type_error(i, "int");
        r = args[i].as_int();
        return true;
    }
    bool boolean(int i, bool& r) {
        if (args[i].type() != Type::Bool) return type_error(i, "bool");
        r = Heap::b(args[i]);
        return true;
    }
    bool str(int i, std::string_view& r) {
        if (args[i].type() != Type::String) return type_error(i, "String");
        r = Heap::sv(args[i]);
        return true;
    }
    bool vec(int i, Vector3& r) {
        if (args[i].type() != Type::Vector3) return type_error(i, "Vector3");
        r = Heap::vec(args[i]);
        return true;
    }
    bool color(int i, Color& r) {
        if (args[i].type() != Type::Color) return type_error(i, "Color");
        r = Heap::col(args[i]);
        return true;
    }
    bool err(std::string m) {
        ctx.error = std::move(m);
        return false;
    }
};

enum { kOk = 0, kNoMethod = 1, kFail = 2 };
int res(bool ok) { return ok ? kOk : kFail; }

int vector3_method(MCall& c) {
    Vector3 v = Heap::vec(c.self);
    Vector3 w;
    Real t = 0;
    switch (c.id) {
    case BM_length: if (!c.arity(0, 0)) return kFail; c.out = Value(vlen(v)); return kOk;
    case BM_length_squared: if (!c.arity(0, 0)) return kFail; c.out = Value(vdot(v, v)); return kOk;
    case BM_normalized: if (!c.arity(0, 0)) return kFail; c.out = Value(vnorm(v)); return kOk;
    case BM_dot: if (!c.arity(1, 1) || !c.vec(0, w)) return kFail; c.out = Value(vdot(v, w)); return kOk;
    case BM_cross: if (!c.arity(1, 1) || !c.vec(0, w)) return kFail; c.out = Value(vcross(v, w)); return kOk;
    case BM_distance_to: if (!c.arity(1, 1) || !c.vec(0, w)) return kFail; c.out = Value(vlen(vsub(w, v))); return kOk;
    case BM_distance_squared_to: {
        if (!c.arity(1, 1) || !c.vec(0, w)) return kFail;
        Vector3 d = vsub(w, v);
        c.out = Value(vdot(d, d));
        return kOk;
    }
    case BM_direction_to: if (!c.arity(1, 1) || !c.vec(0, w)) return kFail; c.out = Value(vnorm(vsub(w, v))); return kOk;
    case BM_lerp:
        if (!c.arity(2, 2) || !c.vec(0, w) || !c.real(1, t)) return kFail;
        c.out = Value(vadd(v, vmul(vsub(w, v), t)));
        return kOk;
    case BM_move_toward: {
        if (!c.arity(2, 2) || !c.vec(0, w) || !c.real(1, t)) return kFail;
        Vector3 d = vsub(w, v);
        Real len = vlen(d);
        c.out = Value(len <= t || len < kCmpEpsilon ? w : vadd(v, vmul(d, t / len)));
        return kOk;
    }
    case BM_abs: if (!c.arity(0, 0)) return kFail; c.out = Value(vec3(std::fabs(v.x), std::fabs(v.y), std::fabs(v.z))); return kOk;
    case BM_floor: if (!c.arity(0, 0)) return kFail; c.out = Value(vec3(std::floor(v.x), std::floor(v.y), std::floor(v.z))); return kOk;
    case BM_ceil: if (!c.arity(0, 0)) return kFail; c.out = Value(vec3(std::ceil(v.x), std::ceil(v.y), std::ceil(v.z))); return kOk;
    case BM_round: if (!c.arity(0, 0)) return kFail; c.out = Value(vec3(std::round(v.x), std::round(v.y), std::round(v.z))); return kOk;
    case BM_sign: if (!c.arity(0, 0)) return kFail; c.out = Value(vec3(fsign(v.x), fsign(v.y), fsign(v.z))); return kOk;
    case BM_rotated: {
        if (!c.arity(2, 2) || !c.vec(0, w) || !c.real(1, t)) return kFail;
        Real l = vlen(w);
        if (l == 0) return res(c.err("The axis Vector3 must be normalized."));
        Vector3 k = vmul(w, 1 / l);
        Real cs = std::cos(t), sn = std::sin(t);
        Vector3 r = vadd(vadd(vmul(v, cs), vmul(vcross(k, v), sn)), vmul(k, vdot(k, v) * (1 - cs)));
        c.out = Value(r);
        return kOk;
    }
    case BM_angle_to:
        if (!c.arity(1, 1) || !c.vec(0, w)) return kFail;
        c.out = Value(Real(std::atan2(vlen(vcross(v, w)), vdot(v, w))));
        return kOk;
    case BM_is_zero_approx:
        if (!c.arity(0, 0)) return kFail;
        c.out = Value(std::fabs(v.x) < kCmpEpsilon && std::fabs(v.y) < kCmpEpsilon && std::fabs(v.z) < kCmpEpsilon);
        return kOk;
    case BM_is_equal_approx:
        if (!c.arity(1, 1) || !c.vec(0, w)) return kFail;
        c.out = Value(approx_eq(v.x, w.x) && approx_eq(v.y, w.y) && approx_eq(v.z, w.z));
        return kOk;
    case BM_limit_length: {
        if (!c.arity(0, 1)) return kFail;
        t = 1;
        if (c.argc == 1 && !c.real(0, t)) return kFail;
        Real l = vlen(v);
        if (l > 0 && t < l) v = vmul(v, t / l);
        c.out = Value(v);
        return kOk;
    }
    default: return kNoMethod;
    }
}

int hex_byte(Real x) {
    int v = int(std::round(x * 255.0f));
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

int color_method(MCall& c) {
    Color v = Heap::col(c.self);
    Color w;
    Real t = 0;
    switch (c.id) {
    case BM_lerp:
        if (!c.arity(2, 2) || !c.color(0, w) || !c.real(1, t)) return kFail;
        c.out = Value(col3(v.r + (w.r - v.r) * t, v.g + (w.g - v.g) * t, v.b + (w.b - v.b) * t));
        return kOk;
    case BM_to_html: {
        if (!c.arity(0, 1)) return kFail;
        bool alpha = true;
        if (c.argc == 1 && !c.boolean(0, alpha)) return kFail;
        static const char* hex = "0123456789abcdef";
        std::string s;
        int comps[4] = {hex_byte(v.r), hex_byte(v.g), hex_byte(v.b), 255};
        for (int k = 0; k < (alpha ? 4 : 3); ++k) {
            s.push_back(hex[comps[k] >> 4]);
            s.push_back(hex[comps[k] & 15]);
        }
        c.out = Value(s);
        return kOk;
    }
    case BM_inverted: if (!c.arity(0, 0)) return kFail; c.out = Value(col3(1 - v.r, 1 - v.g, 1 - v.b)); return kOk;
    case BM_lightened:
        if (!c.arity(1, 1) || !c.real(0, t)) return kFail;
        c.out = Value(col3(v.r + (1 - v.r) * t, v.g + (1 - v.g) * t, v.b + (1 - v.b) * t));
        return kOk;
    case BM_darkened:
        if (!c.arity(1, 1) || !c.real(0, t)) return kFail;
        c.out = Value(col3(v.r * (1 - t), v.g * (1 - t), v.b * (1 - t)));
        return kOk;
    case BM_is_equal_approx:
        if (!c.arity(1, 1) || !c.color(0, w)) return kFail;
        c.out = Value(approx_eq(v.r, w.r) && approx_eq(v.g, w.g) && approx_eq(v.b, w.b));
        return kOk;
    default: return kNoMethod;
    }
}

Int cp_index_of(std::string_view s, size_t byte_pos) { return Int(utf8_count(s.data(), byte_pos)); }

bool is_space(char ch) { return static_cast<unsigned char>(ch) <= 32; }

int string_method(MCall& c) {
    std::string_view s = Heap::sv(c.self);
    const StrObj* so = Heap::str(c.self);
    Int ncp = so ? Int(so->ncp) : 0;
    std::string_view w;
    Int n = 0;
    switch (c.id) {
    case BM_length: if (!c.arity(0, 0)) return kFail; c.out = Value(ncp); return kOk;
    case BM_is_empty: if (!c.arity(0, 0)) return kFail; c.out = Value(s.empty()); return kOk;
    case BM_to_upper:
    case BM_to_lower: {
        if (!c.arity(0, 0)) return kFail;
        std::string r(s);
        for (char& ch : r) {
            if (c.id == BM_to_upper && ch >= 'a' && ch <= 'z') ch = char(ch - 32);
            if (c.id == BM_to_lower && ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
        }
        c.out = Value(r);
        return kOk;
    }
    case BM_begins_with:
        if (!c.arity(1, 1) || !c.str(0, w)) return kFail;
        c.out = Value(s.substr(0, w.size()) == w);
        return kOk;
    case BM_ends_with:
        if (!c.arity(1, 1) || !c.str(0, w)) return kFail;
        c.out = Value(s.size() >= w.size() && s.substr(s.size() - w.size()) == w);
        return kOk;
    case BM_contains:
        if (!c.arity(1, 1) || !c.str(0, w)) return kFail;
        c.out = Value(s.find(w) != std::string_view::npos);
        return kOk;
    case BM_find: {
        if (!c.arity(1, 2) || !c.str(0, w)) return kFail;
        if (c.argc == 2 && !c.integer(1, n)) return kFail;
        if (n < 0) n += ncp;
        if (n < 0) n = 0;
        size_t pos = s.find(w, utf8_offset(s, n));
        c.out = Value(pos == std::string_view::npos ? Int(-1) : cp_index_of(s, pos));
        return kOk;
    }
    case BM_rfind: {
        if (!c.arity(1, 2) || !c.str(0, w)) return kFail;
        n = -1;
        if (c.argc == 2 && !c.integer(1, n)) return kFail;
        size_t from = std::string_view::npos;
        if (n >= 0) from = utf8_offset(s, n);
        size_t pos = s.rfind(w, from);
        c.out = Value(pos == std::string_view::npos ? Int(-1) : cp_index_of(s, pos));
        return kOk;
    }
    case BM_count: {
        if (!c.arity(1, 1) || !c.str(0, w)) return kFail;
        Int k = 0;
        if (!w.empty())
            for (size_t pos = s.find(w); pos != std::string_view::npos; pos = s.find(w, pos + w.size())) ++k;
        c.out = Value(k);
        return kOk;
    }
    case BM_substr: {
        Int len = -1;
        if (!c.arity(1, 2) || !c.integer(0, n)) return kFail;
        if (c.argc == 2 && !c.integer(1, len)) return kFail;
        if (len == -1) len = ncp - n;
        if (n < 0 || n >= ncp || len <= 0) {
            c.out = Value(std::string_view());
            return kOk;
        }
        size_t a = utf8_offset(s, n);
        size_t b = utf8_offset(s, len > ncp - n ? ncp : n + len);
        c.out = Value(s.substr(a, b - a));
        return kOk;
    }
    case BM_left:
    case BM_right: {
        if (!c.arity(1, 1) || !c.integer(0, n)) return kFail;
        Int keep = n < 0 ? ncp + n : n;
        if (keep < 0) keep = 0;
        if (keep > ncp) keep = ncp;
        if (c.id == BM_left) c.out = Value(s.substr(0, utf8_offset(s, keep)));
        else c.out = Value(s.substr(utf8_offset(s, ncp - keep)));
        return kOk;
    }
    case BM_repeat: {
        if (!c.arity(1, 1) || !c.integer(0, n)) return kFail;
        if (n < 0) n = 0;
        if (double(n) * double(s.size()) > double(kMaxStringBytes)) return res(c.err("String is too long."));
        std::string r;
        r.reserve(size_t(n) * s.size());
        for (Int i = 0; i < n; ++i) r += s;
        c.out = Value(r);
        return kOk;
    }
    case BM_split: {
        bool allow_empty = true;
        Int maxsplit = 0;
        if (!c.arity(0, 3)) return kFail;
        if (c.argc >= 1 && !c.str(0, w)) return kFail;
        if (c.argc >= 2 && !c.boolean(1, allow_empty)) return kFail;
        if (c.argc >= 3 && !c.integer(2, maxsplit)) return kFail;
        std::vector<Value> parts;
        if (w.empty()) {
            for (size_t i = 0; i < s.size();) {
                size_t j = utf8_offset(s.substr(i), 1);
                if (j == 0) j = s.size() - i;
                parts.push_back(Value(s.substr(i, j)));
                i += j;
            }
        } else {
            size_t start = 0;
            for (;;) {
                size_t pos = s.find(w, start);
                if (pos == std::string_view::npos || (maxsplit > 0 && Int(parts.size()) >= maxsplit)) {
                    std::string_view part = s.substr(start);
                    if (allow_empty || !part.empty()) parts.push_back(Value(part));
                    break;
                }
                std::string_view part = s.substr(start, pos - start);
                if (allow_empty || !part.empty()) parts.push_back(Value(part));
                start = pos + w.size();
                if (parts.size() > kMaxArrayItems) break;
            }
        }
        c.out = make_array(std::move(parts));
        return kOk;
    }
    case BM_strip_edges: {
        bool left = true, right = true;
        if (!c.arity(0, 2)) return kFail;
        if (c.argc >= 1 && !c.boolean(0, left)) return kFail;
        if (c.argc >= 2 && !c.boolean(1, right)) return kFail;
        size_t a = 0, b = s.size();
        if (left) while (a < b && is_space(s[a])) ++a;
        if (right) while (b > a && is_space(s[b - 1])) --b;
        c.out = Value(s.substr(a, b - a));
        return kOk;
    }
    case BM_replace: {
        std::string_view with;
        if (!c.arity(2, 2) || !c.str(0, w) || !c.str(1, with)) return kFail;
        if (w.empty()) {
            c.out = c.self;
            return kOk;
        }
        std::string r;
        size_t start = 0;
        for (size_t pos = s.find(w); pos != std::string_view::npos; pos = s.find(w, start)) {
            r.append(s.data() + start, pos - start);
            r.append(with.data(), with.size());
            start = pos + w.size();
            if (r.size() > kMaxStringBytes) return res(c.err("String is too long."));
        }
        r.append(s.data() + start, s.size() - start);
        c.out = Value(r);
        return kOk;
    }
    case BM_to_int: if (!c.arity(0, 0)) return kFail; c.out = Value(string_to_int(s)); return kOk;
    case BM_to_float: if (!c.arity(0, 0)) return kFail; c.out = Value(Real(string_to_float(s))); return kOk;
    case BM_is_valid_int: {
        if (!c.arity(0, 0)) return kFail;
        size_t i = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
        bool ok = i < s.size();
        for (; i < s.size(); ++i)
            if (s[i] < '0' || s[i] > '9') ok = false;
        c.out = Value(ok);
        return kOk;
    }
    case BM_is_valid_float: {
        if (!c.arity(0, 0)) return kFail;
        bool full = false;
        string_to_float(s, &full);
        c.out = Value(full && !s.empty() && !is_space(s[0]));
        return kOk;
    }
    case BM_unicode_at: {
        if (!c.arity(1, 1) || !c.integer(0, n)) return kFail;
        if (n < 0 || n >= ncp) return res(c.err("Index out of bounds in unicode_at()."));
        size_t a = utf8_offset(s, n);
        const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data() + a);
        size_t rem = s.size() - a;
        uint32_t cp = p[0];
        if (cp >= 0xF0 && rem >= 4) cp = ((cp & 7u) << 18) | ((p[1] & 63u) << 12) | ((p[2] & 63u) << 6) | (p[3] & 63u);
        else if (cp >= 0xE0 && rem >= 3) cp = ((cp & 15u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u);
        else if (cp >= 0xC0 && rem >= 2) cp = ((cp & 31u) << 6) | (p[1] & 63u);
        c.out = Value(Int(cp));
        return kOk;
    }
    case BM_join: {
        if (!c.arity(1, 1)) return kFail;
        if (c.args[0].type() != Type::Array) return res(c.type_error(0, "Array"));
        std::string r;
        const auto& items = Heap::arr(c.args[0])->items;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i) r += s;
            append_value(r, items[i], c.ctx.host, 0, false);
            if (r.size() > kMaxStringBytes) return res(c.err("String is too long."));
        }
        c.out = Value(r);
        return kOk;
    }
    default: return kNoMethod;
    }
}

bool sort_less(const Value& a, const Value& b) {
    if (is_num(a) && is_num(b)) {
        if (a.type() == Type::Int && b.type() == Type::Int) return Heap::i(a) < Heap::i(b);
        Real x = 0, y = 0;
        if (exact_real(a, x) && exact_real(b, y)) return x < y;
        return num(a) < num(b);
    }
    if (a.type() != b.type()) return int(a.type()) < int(b.type());
    switch (a.type()) {
    case Type::String: return Heap::sv(a) < Heap::sv(b);
    case Type::Bool: return !Heap::b(a) && Heap::b(b);
    case Type::Vector3:
    case Type::Color:
        for (int k = 0; k < 3; ++k) {
            Real x = Heap::comp(a, k), y = Heap::comp(b, k);
            if (x != y) return x < y;
        }
        return false;
    case Type::Object: return Heap::o(a) < Heap::o(b);
    default: return false;
    }
}

// Bottom-up merge sort instead of std::sort, so an inconsistent comparator can't cause UB.
void merge_sort(std::vector<Value>& v) {
    size_t n = v.size();
    if (n < 2) return;
    std::vector<Value> tmp(n);
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * width) {
            size_t mid = lo + width < n ? lo + width : n;
            size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = sort_less(v[j], v[i]) ? std::move(v[j++]) : std::move(v[i++]);
            while (i < mid) tmp[k++] = std::move(v[i++]);
            while (j < hi) tmp[k++] = std::move(v[j++]);
        }
        v.swap(tmp);
    }
}

bool normalize_index(Int& i, size_t size, bool allow_end) {
    Int n = Int(size);
    if (i < 0) i += n;
    return i >= 0 && (allow_end ? i <= n : i < n);
}

int array_method(MCall& c) {
    ArrObj* a = Heap::arr(c.self);
    auto& items = a->items;
    Int n = 0;
    auto mutating = [&]() {
        if (!a->read_only) return true;
        c.ctx.error = "Array is in read-only state.";
        return false;
    };
    switch (c.id) {
    case BM_size: if (!c.arity(0, 0)) return kFail; c.out = Value(Int(items.size())); return kOk;
    case BM_is_empty: if (!c.arity(0, 0)) return kFail; c.out = Value(items.empty()); return kOk;
    case BM_append:
    case BM_push_back:
        if (!c.arity(1, 1) || !mutating()) return kFail;
        if (items.size() >= kMaxArrayItems) return res(c.err("Array is too large."));
        items.push_back(c.args[0]);
        return kOk;
    case BM_push_front:
        if (!c.arity(1, 1) || !mutating()) return kFail;
        if (items.size() >= kMaxArrayItems) return res(c.err("Array is too large."));
        items.insert(items.begin(), c.args[0]);
        return kOk;
    case BM_pop_back:
        if (!c.arity(0, 0) || !mutating()) return kFail;
        if (!items.empty()) {
            c.out = std::move(items.back());
            items.pop_back();
        }
        return kOk;
    case BM_pop_front:
        if (!c.arity(0, 0) || !mutating()) return kFail;
        if (!items.empty()) {
            c.out = std::move(items.front());
            items.erase(items.begin());
        }
        return kOk;
    case BM_insert: {
        if (!c.arity(2, 2) || !c.integer(0, n) || !mutating()) return kFail;
        if (!normalize_index(n, items.size(), true))
            return res(c.err("Out of bounds index " + format_int(c.args[0].as_int()) + " in insert() (size " +
                             format_int(Int(items.size())) + ")."));
        if (items.size() >= kMaxArrayItems) return res(c.err("Array is too large."));
        Value v = c.args[1];
        items.insert(items.begin() + n, std::move(v));
        return kOk;
    }
    case BM_remove_at:
        if (!c.arity(1, 1) || !c.integer(0, n) || !mutating()) return kFail;
        if (!normalize_index(n, items.size(), false))
            return res(c.err("Out of bounds index " + format_int(c.args[0].as_int()) + " in remove_at() (size " +
                             format_int(Int(items.size())) + ")."));
        items.erase(items.begin() + n);
        return kOk;
    case BM_erase:
        if (!c.arity(1, 1) || !mutating()) return kFail;
        for (size_t i = 0; i < items.size(); ++i)
            if (values_equal(items[i], c.args[0])) {
                items.erase(items.begin() + Int(i));
                break;
            }
        return kOk;
    case BM_has:
    case BM_find:
    case BM_count: {
        if (!c.arity(1, c.id == BM_find ? 2 : 1)) return kFail;
        Int from = 0;
        if (c.argc == 2 && !c.integer(1, from)) return kFail;
        if (from < 0) from += Int(items.size());
        if (from < 0) from = 0;
        Int found = -1, count = 0;
        for (size_t i = size_t(from); i < items.size(); ++i)
            if (values_equal(items[i], c.args[0])) {
                if (found < 0) found = Int(i);
                ++count;
                if (c.id != BM_count) break;
            }
        if (c.id == BM_has) c.out = Value(found >= 0);
        else if (c.id == BM_find) c.out = Value(found);
        else c.out = Value(count);
        return kOk;
    }
    case BM_clear: if (!c.arity(0, 0) || !mutating()) return kFail; items.clear(); return kOk;
    case BM_front: if (!c.arity(0, 0)) return kFail; if (!items.empty()) c.out = items.front(); return kOk;
    case BM_back: if (!c.arity(0, 0)) return kFail; if (!items.empty()) c.out = items.back(); return kOk;
    case BM_duplicate: {
        bool deep = false;
        if (!c.arity(0, 1)) return kFail;
        if (c.argc == 1 && !c.boolean(0, deep)) return kFail;
        c.out = deep_copy(c.self, deep);
        return kOk;
    }
    case BM_reverse:
        if (!c.arity(0, 0) || !mutating()) return kFail;
        for (size_t i = 0, j = items.size(); i + 1 < j; ++i, --j) std::swap(items[i], items[j - 1]);
        return kOk;
    case BM_sort: if (!c.arity(0, 0) || !mutating()) return kFail; merge_sort(items); return kOk;
    case BM_pick_random:
        if (!c.arity(0, 0)) return kFail;
        if (!items.empty()) c.out = items[c.ctx.rng ? c.ctx.rng->next() % items.size() : 0];
        return kOk;
    case BM_slice: {
        Int begin = 0, end = INT64_MAX, step = 1;
        if (!c.arity(0, 3)) return kFail;
        if (c.argc >= 1 && !c.integer(0, begin)) return kFail;
        if (c.argc >= 2 && !c.integer(1, end)) return kFail;
        if (c.argc >= 3 && !c.integer(2, step)) return kFail;
        if (step == 0) return res(c.err("Slice step cannot be zero."));
        Int size = Int(items.size());
        auto fix = [&](Int x) {
            if (x < 0) x += size;
            return x < 0 ? Int(step > 0 ? 0 : -1) : (x > size ? (step > 0 ? size : size - 1) : x);
        };
        begin = fix(begin);
        end = fix(end);
        std::vector<Value> r;
        if (step > 0) {
            if (begin >= size) begin = size;
            for (Int i = begin; i < end; i += step) r.push_back(items[size_t(i)]);
        } else {
            if (begin >= size) begin = size - 1;
            for (Int i = begin; i > end && i >= 0; i += step) r.push_back(items[size_t(i)]);
        }
        c.out = make_array(std::move(r));
        return kOk;
    }
    case BM_max:
    case BM_min: {
        if (!c.arity(0, 0)) return kFail;
        if (items.empty()) return kOk;
        size_t best = 0;
        for (size_t i = 1; i < items.size(); ++i) {
            bool better = c.id == BM_max ? sort_less(items[best], items[i]) : sort_less(items[i], items[best]);
            if (better) best = i;
        }
        c.out = items[best];
        return kOk;
    }
    case BM_resize:
        if (!c.arity(1, 1) || !c.integer(0, n) || !mutating()) return kFail;
        if (n < 0 || size_t(n) > kMaxArrayItems) return res(c.err("Invalid size for resize()."));
        items.resize(size_t(n));
        return kOk;
    case BM_fill:
        if (!c.arity(1, 1) || !mutating()) return kFail;
        for (Value& x : items) x = c.args[0];
        return kOk;
    case BM_append_array: {
        if (!c.arity(1, 1) || !mutating()) return kFail;
        if (c.args[0].type() != Type::Array) return res(c.type_error(0, "Array"));
        std::vector<Value> other = Heap::arr(c.args[0])->items; // copy: may alias `items`
        if (items.size() + other.size() > kMaxArrayItems) return res(c.err("Array is too large."));
        for (Value& x : other) items.push_back(std::move(x));
        return kOk;
    }
    default: return kNoMethod;
    }
}

int dictionary_method(MCall& c) {
    DictObj* d = Heap::dict(c.self);
    auto mutating = [&]() {
        if (!d->read_only) return true;
        c.ctx.error = "Dictionary is in read-only state.";
        return false;
    };
    switch (c.id) {
    case BM_size: if (!c.arity(0, 0)) return kFail; c.out = Value(Int(d->entries.size())); return kOk;
    case BM_is_empty: if (!c.arity(0, 0)) return kFail; c.out = Value(d->entries.empty()); return kOk;
    case BM_has: if (!c.arity(1, 1)) return kFail; c.out = Value(d->find(c.args[0]) >= 0); return kOk;
    case BM_get: {
        if (!c.arity(1, 2)) return kFail;
        int i = d->find(c.args[0]);
        if (i >= 0) c.out = d->entries[size_t(i)].value;
        else if (c.argc == 2) c.out = c.args[1];
        return kOk;
    }
    case BM_keys:
    case BM_values: {
        if (!c.arity(0, 0)) return kFail;
        std::vector<Value> r;
        r.reserve(d->entries.size());
        for (const DictEntry& e : d->entries) r.push_back(c.id == BM_keys ? e.key : e.value);
        c.out = make_array(std::move(r));
        return kOk;
    }
    case BM_erase:
        if (!c.arity(1, 1) || !mutating()) return kFail;
        c.out = Value(d->erase(c.args[0]));
        return kOk;
    case BM_clear: if (!c.arity(0, 0) || !mutating()) return kFail; d->clear(); return kOk;
    case BM_duplicate: {
        bool deep = false;
        if (!c.arity(0, 1)) return kFail;
        if (c.argc == 1 && !c.boolean(0, deep)) return kFail;
        c.out = deep_copy(c.self, deep);
        return kOk;
    }
    case BM_merge: {
        bool overwrite = false;
        if (!c.arity(1, 2) || !mutating()) return kFail;
        if (c.args[0].type() != Type::Dictionary) return res(c.type_error(0, "Dictionary"));
        if (c.argc == 2 && !c.boolean(1, overwrite)) return kFail;
        std::vector<DictEntry> other = Heap::dict(c.args[0])->entries; // copy: may alias
        for (const DictEntry& e : other)
            if (overwrite || d->find(e.key) < 0) d->set(e.key, e.value);
        return kOk;
    }
    case BM_find_key:
        if (!c.arity(1, 1)) return kFail;
        for (const DictEntry& e : d->entries)
            if (values_equal(e.value, c.args[0])) {
                c.out = e.key;
                break;
            }
        return kOk;
    default: return kNoMethod;
    }
}

} // namespace

uint32_t Rng::next() {
    uint64_t old = state;
    state = old * 6364136223846793005ULL + (inc | 1u);
    uint32_t xorshifted = uint32_t(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = uint32_t(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

void Rng::seed(uint64_t s) {
    state = 0;
    inc = (s << 1u) | 1u;
    next();
    state += s ^ 0x853c49e6748fea9bULL;
    next();
}

double Rng::unit() { return double(next()) / 4294967296.0; }

const BuiltinFunc* builtin_funcs() { return kFuncs; }
int builtin_func_count() { return int(sizeof(kFuncs) / sizeof(kFuncs[0])); }

int find_builtin_func(std::string_view name) {
    for (int i = 0; i < builtin_func_count(); ++i)
        if (name == kFuncs[i].name) return i;
    return -1;
}

int find_builtin_method(std::string_view name) {
    for (int i = 0; i < BM_COUNT; ++i)
        if (name == kMethodNames[i]) return i;
    return -1;
}

const char* builtin_method_name(int id) { return id >= 0 && id < BM_COUNT ? kMethodNames[id] : "?"; }

int call_builtin_method(BuiltinCtx& ctx, int id, const Value& self, const Value* args, int argc, Value& out) {
    if (id < 0 || id >= BM_COUNT) return kNoMethod;
    MCall c{ctx, id, self, args, argc, out};
    switch (self.type()) {
    case Type::Vector3: return vector3_method(c);
    case Type::Color: return color_method(c);
    case Type::String: return string_method(c);
    case Type::Array: return array_method(c);
    case Type::Dictionary: return dictionary_method(c);
    default: return kNoMethod;
    }
}

Value sample_value(Type t) {
    switch (t) {
    case Type::Bool: return Value(true);
    case Type::Int: return Value(Int(1));
    case Type::Float: return Value(Real(1.5f));
    case Type::String: return Value("a");
    case Type::Vector3: return Value(vec3(1, 1, 1));
    case Type::Color: return Value(col3(1, 1, 1));
    case Type::Array: return Value::new_array();
    case Type::Dictionary: return Value::new_dictionary();
    case Type::Object: return Value::object(1);
    default: return Value();
    }
}

bool builtin_method_exists(Type receiver, int id) {
    if (id < 0 || id >= BM_COUNT) return false;
    if (receiver != Type::Vector3 && receiver != Type::Color && receiver != Type::String && receiver != Type::Array &&
        receiver != Type::Dictionary)
        return false;
    BuiltinCtx ctx; // no host or rng: the call only tells whether the type has this method
    Value self = sample_value(receiver), out;
    return call_builtin_method(ctx, id, self, nullptr, 0, out) != kNoMethod;
}

SType builtin_method_return(Type receiver, int id) {
    const SType F = SType(Type::Float), I = SType(Type::Int), B = SType(Type::Bool), S = SType(Type::String),
                V = SType(Type::Vector3), C = SType(Type::Color), A = SType(Type::Array), D = SType(Type::Dictionary);
    switch (receiver) {
    case Type::Vector3:
        switch (id) {
        case BM_length: case BM_length_squared: case BM_dot: case BM_distance_to: case BM_distance_squared_to:
        case BM_angle_to: return F;
        case BM_normalized: case BM_cross: case BM_lerp: case BM_move_toward: case BM_abs: case BM_rotated:
        case BM_direction_to: case BM_floor: case BM_ceil: case BM_round: case BM_sign: case BM_limit_length: return V;
        case BM_is_zero_approx: case BM_is_equal_approx: return B;
        default: return kUnknown;
        }
    case Type::Color:
        switch (id) {
        case BM_lerp: case BM_inverted: case BM_lightened: case BM_darkened: return C;
        case BM_to_html: return S;
        case BM_is_equal_approx: return B;
        default: return kUnknown;
        }
    case Type::String:
        switch (id) {
        case BM_length: case BM_find: case BM_rfind: case BM_count: case BM_to_int: case BM_unicode_at: return I;
        case BM_to_float: return F;
        case BM_to_upper: case BM_to_lower: case BM_substr: case BM_strip_edges: case BM_replace: case BM_left:
        case BM_right: case BM_repeat: case BM_join: return S;
        case BM_begins_with: case BM_ends_with: case BM_contains: case BM_is_empty: case BM_is_valid_int:
        case BM_is_valid_float: return B;
        case BM_split: return A;
        default: return kUnknown;
        }
    case Type::Array:
        switch (id) {
        case BM_size: case BM_find: case BM_count: return I;
        case BM_is_empty: case BM_has: return B;
        case BM_duplicate: case BM_slice: return A;
        default: return kUnknown;
        }
    case Type::Dictionary:
        switch (id) {
        case BM_size: return I;
        case BM_is_empty: case BM_has: case BM_erase: return B;
        case BM_keys: case BM_values: return A;
        case BM_duplicate: return D;
        default: return kUnknown;
        }
    default: return kUnknown;
    }
}

int find_builtin_field(std::string_view name) {
    if (name.size() != 1) return -1;
    switch (name[0]) {
    case 'x': return 0;
    case 'y': return 1;
    case 'z': return 2;
    case 'r': return 3;
    case 'g': return 4;
    case 'b': return 5;
    case 'a': return 6;
    default: return -1;
    }
}

struct NamedColor { const char* name; Real r, g, b; };
static const NamedColor kColors[] = {
    {"BLACK", 0, 0, 0},        {"WHITE", 1, 1, 1},           {"RED", 1, 0, 0},
    {"GREEN", 0, 1, 0},        {"BLUE", 0, 0, 1},            {"YELLOW", 1, 1, 0},
    {"CYAN", 0, 1, 1},         {"MAGENTA", 1, 0, 1},         {"ORANGE", 1, 0.647059f, 0},
    {"GRAY", 0.745098f, 0.745098f, 0.745098f},
};

bool builtin_type_constant(Type t, std::string_view name, Value& out) {
    if (t == Type::Vector3) {
        struct V { const char* n; Real x, y, z; };
        static const V vs[] = {{"ZERO", 0, 0, 0},  {"ONE", 1, 1, 1},      {"UP", 0, 1, 0},
                               {"DOWN", 0, -1, 0}, {"LEFT", -1, 0, 0},    {"RIGHT", 1, 0, 0},
                               {"FORWARD", 0, 0, -1}, {"BACK", 0, 0, 1}};
        for (const V& v : vs)
            if (name == v.n) {
                out = Value(vec3(v.x, v.y, v.z));
                return true;
            }
        return false;
    }
    if (t == Type::Color) {
        for (const NamedColor& c : kColors)
            if (name == c.name) {
                out = Value(col3(c.r, c.g, c.b));
                return true;
            }
    }
    return false;
}

bool builtin_global_constant(std::string_view name, Value& out) {
    if (name == "PI") out = Value(kPi);
    else if (name == "TAU") out = Value(kTau);
    else if (name == "INF") out = Value(Real(HUGE_VALF));
    else if (name == "NAN") out = Value(Real(std::nanf("")));
    else return false;
    return true;
}

bool builtin_type_from_name(std::string_view name, Type& out, bool& variant) {
    variant = false;
    static const struct { const char* n; Type t; } names[] = {
        {"bool", Type::Bool},     {"int", Type::Int},         {"float", Type::Float},
        {"String", Type::String}, {"Vector3", Type::Vector3}, {"Color", Type::Color},
        {"Array", Type::Array},   {"Dictionary", Type::Dictionary}, {"Object", Type::Object},
    };
    for (const auto& e : names)
        if (name == e.n) {
            out = e.t;
            return true;
        }
    if (name == "Variant") {
        out = Type::Nil;
        variant = true;
        return true;
    }
    return false;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_color_string(std::string_view s, Color& out) {
    std::string upper;
    for (char ch : s) upper.push_back(ch >= 'a' && ch <= 'z' ? char(ch - 32) : ch);
    for (const NamedColor& c : kColors)
        if (upper == c.name) {
            out = col3(c.r, c.g, c.b);
            return true;
        }
    if (!s.empty() && s[0] == '#') s.remove_prefix(1);
    for (char ch : s)
        if (hexval(ch) < 0) return false;
    if (s.size() == 3 || s.size() == 4) {
        out = col3(Real(hexval(s[0])) / 15.0f, Real(hexval(s[1])) / 15.0f, Real(hexval(s[2])) / 15.0f);
        return true;
    }
    if (s.size() == 6 || s.size() == 8) {
        auto byte = [&](size_t i) { return Real(hexval(s[i]) * 16 + hexval(s[i + 1])) / 255.0f; };
        out = col3(byte(0), byte(2), byte(4));
        return true;
    }
    return false;
}

} // namespace detail
} // namespace jetscript
