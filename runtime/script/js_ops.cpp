// Shared by constant folding and the VM, so folded and executed code always agree.
#include "js_internal.hpp"

#include <cmath>
#include <cstdlib>

namespace jetscript {
namespace detail {

Int real_to_int(double x) {
    if (x != x) return 0;
    if (x >= 9223372036854775807.0) return INT64_MAX;
    if (x <= -9223372036854775808.0) return INT64_MIN;
    return Int(x);
}

Int real_to_int(Real x) {
    if (x > -2147483648.0f && x < 2147483648.0f) return Int(int32_t(x));
    if (x != x) return 0;
    if (x >= 9223372036854775808.0f) return INT64_MAX;
    if (x <= -9223372036854775808.0f) return INT64_MIN;
    return Int(x);
}

static void append_u64(std::string& out, uint64_t u) {
    char buf[24];
    int n = 0;
    do {
        buf[n++] = char('0' + u % 10);
        u /= 10;
    } while (u);
    while (n) out.push_back(buf[--n]);
}

std::string format_int(Int v) {
    std::string s;
    if (v < 0) {
        s.push_back('-');
        append_u64(s, 0 - uint64_t(v));
    } else {
        append_u64(s, uint64_t(v));
    }
    return s;
}

// Floats are formatted by hand because the ESP-IDF libc may be built without printf float support.
static double pow10d(int k) {
    static const double table[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                   1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    if (k >= 0 && k <= 22) return table[k];
    return std::pow(10.0, double(k));
}

static double scale10(double d, int k) { return k >= 0 ? d * pow10d(k) : d / pow10d(-k); }

// Shortest digits that round-trip through float32. `point` is how many of them come before the decimal point.
static void shortest_digits(float x, std::string& digits, int& point) {
    double d = double(x);
    int e = int(std::floor(std::log10(d)));
    if (d >= pow10d(e + 1)) ++e;
    else if (d < scale10(1.0, e)) --e;
    uint64_t best = 0;
    int best_exp = 0;
    bool found = false;
    for (int p = 1; p <= 9 && !found; ++p) {
        double m = std::nearbyint(scale10(d, p - 1 - e));
        int ee = e;
        if (m >= pow10d(p)) {
            m = std::nearbyint(m / 10.0);
            ++ee;
        }
        uint64_t mi = uint64_t(m);
        int exp10 = ee - p + 1; // value = mi * 10^exp10
        std::string s;
        append_u64(s, mi);
        s.push_back('e');
        s += format_int(exp10);
        if (std::strtof(s.c_str(), nullptr) == x || p == 9) {
            best = mi;
            best_exp = exp10;
            found = true;
        }
    }
    while (best && best % 10 == 0) {
        best /= 10;
        ++best_exp;
    }
    digits.clear();
    append_u64(digits, best);
    point = int(digits.size()) + best_exp;
}

std::string format_real(Real v) {
    if (v != v) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
    if (v == 0) return std::signbit(v) ? "-0.0" : "0.0";
    std::string out;
    if (v < 0) {
        out.push_back('-');
        v = -v;
    }
    std::string d;
    int k = 0;
    shortest_digits(v, d, k);
    int n = int(d.size());
    if (k > 0 && k <= 21) {
        if (n <= k) {
            out += d;
            out.append(size_t(k - n), '0');
            out += ".0";
        } else {
            out.append(d, 0, size_t(k));
            out.push_back('.');
            out.append(d, size_t(k), std::string::npos);
        }
    } else if (k <= 0 && k > -6) {
        out += "0.";
        out.append(size_t(-k), '0');
        out += d;
    } else {
        out.push_back(d[0]);
        if (n > 1) {
            out.push_back('.');
            out.append(d, 1, std::string::npos);
        }
        out.push_back('e');
        int ex = k - 1;
        out.push_back(ex < 0 ? '-' : '+');
        out += format_int(ex < 0 ? -ex : ex);
    }
    return out;
}

void append_fixed(std::string& out, double v, int decimals) {
    if (v != v) { out += "nan"; return; }
    if (std::isinf(v)) { out += v < 0 ? "-inf" : "inf"; return; }
    if (decimals < 0) decimals = 0;
    if (decimals > 64) decimals = 64;
    int dec = decimals > 9 ? 9 : decimals;
    bool neg = v < 0;
    double a = std::fabs(v);
    std::string body;
    double scaled = a * pow10d(dec);
    if (scaled < 9.0e18) {
        uint64_t m = uint64_t(std::nearbyint(scaled));
        uint64_t p = uint64_t(pow10d(dec));
        append_u64(body, m / p);
        if (decimals > 0) {
            body.push_back('.');
            std::string frac;
            append_u64(frac, m % p);
            if (dec > 0) body.append(size_t(dec) - frac.size(), '0');
            if (dec > 0) body += frac;
            body.append(size_t(decimals - dec), '0');
        }
        if (m == 0) neg = false;
    } else {
        if (a < 9.0e18) {
            append_u64(body, uint64_t(a));
        } else {
            std::string d;
            int k = 0;
            shortest_digits(float(a), d, k);
            body += d;
            if (k > int(d.size())) body.append(size_t(k) - d.size(), '0');
        }
        if (decimals > 0) {
            body.push_back('.');
            body.append(size_t(decimals), '0');
        }
    }
    if (neg) out.push_back('-');
    out += body;
}

bool truthy(const Value& v) {
    switch (v.type()) {
    case Type::Nil: return false;
    case Type::Bool: return Heap::b(v);
    case Type::Int: return Heap::i(v) != 0;
    case Type::Float: return Heap::f(v) != 0;
    case Type::String: return Heap::str(v) != nullptr;
    case Type::Vector3:
    case Type::Color: return Heap::comp(v, 0) != 0 || Heap::comp(v, 1) != 0 || Heap::comp(v, 2) != 0;
    case Type::Array: return !Heap::arr(v)->items.empty();
    case Type::Dictionary: return !Heap::dict(v)->entries.empty();
    case Type::Object: return Heap::o(v) != 0;
    }
    return false;
}

bool convert_to(Type t, Value& v) {
    Type s = v.type();
    if (s == t) return true;
    switch (t) {
    case Type::Nil: return true; // Variant
    case Type::Int:
        if (s == Type::Float) { Heap::set_int(v, real_to_int(Heap::f(v))); return true; }
        return false;
    case Type::Float:
        if (s == Type::Int) { Heap::set_float(v, int_to_real(Heap::i(v))); return true; }
        return false;
    case Type::Object: return s == Type::Nil;
    default: return false;
    }
}

Value type_default(Type t) {
    switch (t) {
    case Type::Bool: return Value(false);
    case Type::Int: return Value(Int(0));
    case Type::Float: return Value(Real(0));
    case Type::String: return Value(std::string_view());
    case Type::Vector3: return Value(Vector3());
    case Type::Color: return Value(Color());
    case Type::Array: return Value::new_array();
    case Type::Dictionary: return Value::new_dictionary();
    default: return Value();
    }
}

static bool is_num(Type t) { return t == Type::Int || t == Type::Float; }
static double num(const Value& v) { return v.type() == Type::Int ? double(Heap::i(v)) : double(Heap::f(v)); }

bool values_equal(const Value& a, const Value& b, int depth) {
    int budget = kMaxDeepNodes;
    return values_equal_budget(a, b, depth, budget);
}

// Deep comparison with bounded work: structures larger than the budget compare unequal.
bool values_equal_budget(const Value& a, const Value& b, int depth, int& budget) {
    if (--budget < 0) return false;
    Type ta = a.type(), tb = b.type();
    if (ta != tb) {
        if (!is_num(ta) || !is_num(tb)) return false;
        Real x = 0, y = 0;
        if (exact_real(a, x) && exact_real(b, y)) return x == y;
        return num(a) == num(b);
    }
    switch (ta) {
    case Type::Nil: return true;
    case Type::Bool: return Heap::b(a) == Heap::b(b);
    case Type::Int: return Heap::i(a) == Heap::i(b);
    case Type::Float: return Heap::f(a) == Heap::f(b);
    case Type::String: return Heap::sv(a) == Heap::sv(b);
    case Type::Vector3:
    case Type::Color:
        return Heap::comp(a, 0) == Heap::comp(b, 0) && Heap::comp(a, 1) == Heap::comp(b, 1) &&
               Heap::comp(a, 2) == Heap::comp(b, 2);
    case Type::Object: return Heap::o(a) == Heap::o(b);
    case Type::Array: {
        const ArrObj* x = Heap::arr(a);
        const ArrObj* y = Heap::arr(b);
        if (x == y) return true;
        if (x->items.size() != y->items.size() || depth >= kMaxStringifyDepth) return false;
        for (size_t i = 0; i < x->items.size(); ++i)
            if (!values_equal_budget(x->items[i], y->items[i], depth + 1, budget)) return false;
        return true;
    }
    case Type::Dictionary: {
        const DictObj* x = Heap::dict(a);
        const DictObj* y = Heap::dict(b);
        if (x == y) return true;
        if (x->entries.size() != y->entries.size() || depth >= kMaxStringifyDepth) return false;
        for (const DictEntry& e : x->entries) {
            int j = y->find(e.key);
            if (j < 0 || !values_equal_budget(e.value, y->entries[size_t(j)].value, depth + 1, budget)) return false;
        }
        return true;
    }
    }
    return false;
}

bool strict_equal(const Value& a, const Value& b) { return a.type() == b.type() && values_equal(a, b, 0); }

const char* binop_name(BinOp op) {
    switch (op) {
    case BinOp::Add: return "+";
    case BinOp::Sub: return "-";
    case BinOp::Mul: return "*";
    case BinOp::Div: return "/";
    case BinOp::Mod: return "%";
    case BinOp::Pow: return "**";
    case BinOp::Shl: return "<<";
    case BinOp::Shr: return ">>";
    case BinOp::BitAnd: return "&";
    case BinOp::BitOr: return "|";
    case BinOp::BitXor: return "^";
    case BinOp::Eq: return "==";
    case BinOp::Ne: return "!=";
    case BinOp::Lt: return "<";
    case BinOp::Le: return "<=";
    case BinOp::Gt: return ">";
    case BinOp::Ge: return ">=";
    case BinOp::In: return "in";
    case BinOp::NotIn: return "not in";
    case BinOp::Count: break;
    }
    return "?";
}

const char* unop_name(UnOp op) {
    switch (op) {
    case UnOp::Neg: return "-";
    case UnOp::Pos: return "+";
    case UnOp::BitNot: return "~";
    case UnOp::Not: return "not";
    }
    return "?";
}

static bool operand_error(BinOp op, const Value& a, const Value& b, std::string& err) {
    err = "Invalid operands '";
    err += type_name(a.type());
    err += "' and '";
    err += type_name(b.type());
    err += "' in operator '";
    err += binop_name(op);
    err += "'.";
    return false;
}

static Int ipow(Int base, Int exp) {
    uint64_t result = 1, b = uint64_t(base);
    uint64_t e = uint64_t(exp);
    while (e) {
        if (e & 1u) result *= b;
        b *= b;
        e >>= 1;
    }
    return Int(result);
}

static bool int_op(BinOp op, Int a, Int b, Value& out, std::string& err) {
    switch (op) {
    case BinOp::Add: out = Value(Int(uint64_t(a) + uint64_t(b))); return true;
    case BinOp::Sub: out = Value(Int(uint64_t(a) - uint64_t(b))); return true;
    case BinOp::Mul: out = Value(Int(uint64_t(a) * uint64_t(b))); return true;
    case BinOp::Div:
        if (b == 0) { err = "Division by zero error in operator '/'."; return false; }
        // b == -1 stays on 64 bits: INT32_MIN / -1 overflows int32.
        if (fits32(a) && fits32(b) && b != -1) out = Value(Int(int32_t(a) / int32_t(b)));
        else out = Value((a == INT64_MIN && b == -1) ? a : a / b);
        return true;
    case BinOp::Mod:
        if (b == 0) { err = "Modulo by zero error in operator '%'."; return false; }
        if (b == -1) out = Value(Int(0));
        else out = Value(fits32(a) && fits32(b) ? Int(int32_t(a) % int32_t(b)) : a % b);
        return true;
    case BinOp::Pow:
        if (b >= 0) out = Value(ipow(a, b));
        else out = Value(real_to_int(std::pow(double(a), double(b))));
        return true;
    case BinOp::Shl:
        if (b < 0) { err = "Invalid shift amount in operator '<<' (negative)."; return false; }
        out = Value(b >= 64 ? Int(0) : Int(uint64_t(a) << b));
        return true;
    case BinOp::Shr:
        if (b < 0) { err = "Invalid shift amount in operator '>>' (negative)."; return false; }
        if (b >= 64) out = Value(a < 0 ? Int(-1) : Int(0));
        else out = Value(a >= 0 ? (a >> b) : ~((~a) >> b));
        return true;
    case BinOp::BitAnd: out = Value(Int(a & b)); return true;
    case BinOp::BitOr: out = Value(Int(a | b)); return true;
    case BinOp::BitXor: out = Value(Int(a ^ b)); return true;
    case BinOp::Lt: out = Value(a < b); return true;
    case BinOp::Le: out = Value(a <= b); return true;
    case BinOp::Gt: out = Value(a > b); return true;
    case BinOp::Ge: out = Value(a >= b); return true;
    default: return false;
    }
}

static Vector3 vop(BinOp op, const Vector3& a, const Vector3& b) {
    Vector3 r;
    switch (op) {
    case BinOp::Add: r.x = a.x + b.x; r.y = a.y + b.y; r.z = a.z + b.z; break;
    case BinOp::Sub: r.x = a.x - b.x; r.y = a.y - b.y; r.z = a.z - b.z; break;
    case BinOp::Mul: r.x = a.x * b.x; r.y = a.y * b.y; r.z = a.z * b.z; break;
    default: r.x = a.x / b.x; r.y = a.y / b.y; r.z = a.z / b.z; break;
    }
    return r;
}

static Vector3 to_vec(const Value& v) {
    Vector3 r;
    r.x = Heap::comp(v, 0);
    r.y = Heap::comp(v, 1);
    r.z = Heap::comp(v, 2);
    return r;
}
static Vector3 splat(Real s) {
    Vector3 r;
    r.x = r.y = r.z = s;
    return r;
}
static Value from_vec(Type t, const Vector3& v) {
    if (t == Type::Color) {
        Color c;
        c.r = v.x;
        c.g = v.y;
        c.b = v.z;
        return Value(c);
    }
    return Value(v);
}

static bool compare_result(BinOp op, int cmp, Value& out) {
    switch (op) {
    case BinOp::Lt: out = Value(cmp < 0); return true;
    case BinOp::Le: out = Value(cmp <= 0); return true;
    case BinOp::Gt: out = Value(cmp > 0); return true;
    case BinOp::Ge: out = Value(cmp >= 0); return true;
    default: return false;
    }
}

static bool contains_op(const Value& needle, const Value& hay, bool& result, std::string& err) {
    switch (hay.type()) {
    case Type::Array:
        result = false;
        for (const Value& x : Heap::arr(hay)->items)
            if (values_equal(x, needle)) { result = true; break; }
        return true;
    case Type::Dictionary: result = Heap::dict(hay)->find(needle) >= 0; return true;
    case Type::String:
        if (needle.type() != Type::String) break;
        result = Heap::sv(hay).find(Heap::sv(needle)) != std::string_view::npos;
        return true;
    default: break;
    }
    return operand_error(BinOp::In, needle, hay, err);
}

bool binary_op(BinOp op, const Value& a, const Value& b, Value& out, std::string& err) {
    Type ta = a.type(), tb = b.type();
    Value r;
    switch (op) {
    case BinOp::Eq: out = Value(values_equal(a, b)); return true;
    case BinOp::Ne: out = Value(!values_equal(a, b)); return true;
    case BinOp::In:
    case BinOp::NotIn: {
        bool res = false;
        if (!contains_op(a, b, res, err)) {
            if (op == BinOp::NotIn) operand_error(op, a, b, err);
            return false;
        }
        out = Value(op == BinOp::In ? res : !res);
        return true;
    }
    default: break;
    }
    if (is_num(ta) && is_num(tb)) {
        if (ta == Type::Int && tb == Type::Int) {
            if (!int_op(op, Heap::i(a), Heap::i(b), r, err)) {
                if (err.empty()) return operand_error(op, a, b, err);
                return false;
            }
            out = std::move(r);
            return true;
        }
        Real fx = 0, fy = 0;
        if (exact_real(a, fx) && exact_real(b, fy)) {
            switch (op) {
            case BinOp::Lt: out = Value(fx < fy); return true;
            case BinOp::Le: out = Value(fx <= fy); return true;
            case BinOp::Gt: out = Value(fx > fy); return true;
            case BinOp::Ge: out = Value(fx >= fy); return true;
            default: break;
            }
        }
        switch (op) {
        case BinOp::Lt: out = Value(num(a) < num(b)); return true;
        case BinOp::Le: out = Value(num(a) <= num(b)); return true;
        case BinOp::Gt: out = Value(num(a) > num(b)); return true;
        case BinOp::Ge: out = Value(num(a) >= num(b)); return true;
        default: break;
        }
        Real x = a.as_real(), y = b.as_real();
        switch (op) {
        case BinOp::Add: out = Value(x + y); return true;
        case BinOp::Sub: out = Value(x - y); return true;
        case BinOp::Mul: out = Value(x * y); return true;
        case BinOp::Div: out = Value(x / y); return true;
        case BinOp::Mod: out = Value(std::fmod(x, y)); return true;
        case BinOp::Pow: out = Value(std::pow(x, y)); return true;
        default: return operand_error(op, a, b, err);
        }
    }
    if (ta == Type::String && tb == Type::String) {
        std::string_view x = Heap::sv(a), y = Heap::sv(b);
        if (op == BinOp::Add) {
            if (x.size() + y.size() > kMaxStringBytes) { err = "String is too long."; return false; }
            if (x.empty()) { out = b; return true; }
            if (y.empty()) { out = a; return true; }
            StrObj* s = alloc_string(x.size() + y.size());
            std::memcpy(s->chars(), x.data(), x.size());
            std::memcpy(s->chars() + x.size(), y.data(), y.size());
            s->ncp = Heap::str(a)->ncp + Heap::str(b)->ncp;
            out = Heap::make(Type::String, s);
            return true;
        }
        int c = x.compare(y);
        if (compare_result(op, c, out)) return true;
    }
    if (ta == Type::String && op == BinOp::Mod) {
        std::string text;
        if (!format_percent(Heap::sv(a), b, text, err)) return false;
        out = Value(text);
        return true;
    }
    bool va = ta == Type::Vector3 || ta == Type::Color;
    bool vb = tb == Type::Vector3 || tb == Type::Color;
    if (va && vb && ta == tb) {
        switch (op) {
        case BinOp::Add:
        case BinOp::Sub:
        case BinOp::Mul:
        case BinOp::Div: out = from_vec(ta, vop(op, to_vec(a), to_vec(b))); return true;
        case BinOp::Lt:
        case BinOp::Le:
        case BinOp::Gt:
        case BinOp::Ge: {
            int c = 0;
            for (int k = 0; k < 3 && c == 0; ++k) {
                Real x = Heap::comp(a, k), y = Heap::comp(b, k);
                c = x < y ? -1 : (x > y ? 1 : 0);
            }
            return compare_result(op, c, out);
        }
        default: break;
        }
    }
    if (va && is_num(tb) && (op == BinOp::Mul || op == BinOp::Div)) {
        out = from_vec(ta, vop(op, to_vec(a), splat(b.as_real())));
        return true;
    }
    if (is_num(ta) && vb && op == BinOp::Mul) {
        out = from_vec(tb, vop(op, splat(a.as_real()), to_vec(b)));
        return true;
    }
    if (ta == Type::Array && tb == Type::Array && op == BinOp::Add) {
        const auto& x = Heap::arr(a)->items;
        const auto& y = Heap::arr(b)->items;
        if (x.size() + y.size() > kMaxArrayItems) { err = "Array is too large."; return false; }
        std::vector<Value> items;
        items.reserve(x.size() + y.size());
        items.insert(items.end(), x.begin(), x.end());
        items.insert(items.end(), y.begin(), y.end());
        out = make_array(std::move(items));
        return true;
    }
    if (ta == Type::Bool && tb == Type::Bool) {
        int c = int(Heap::b(a)) - int(Heap::b(b));
        if (compare_result(op, c, out)) return true;
    }
    return operand_error(op, a, b, err);
}

bool unary_op(UnOp op, const Value& a, Value& out, std::string& err) {
    Type t = a.type();
    switch (op) {
    case UnOp::Not: out = Value(!truthy(a)); return true;
    case UnOp::Neg:
        if (t == Type::Int) { out = Value(Int(0 - uint64_t(Heap::i(a)))); return true; }
        if (t == Type::Float) { out = Value(-Heap::f(a)); return true; }
        if (t == Type::Vector3 || t == Type::Color) {
            Vector3 v = to_vec(a);
            v.x = -v.x;
            v.y = -v.y;
            v.z = -v.z;
            out = from_vec(t, v);
            return true;
        }
        break;
    case UnOp::Pos:
        if (t == Type::Int || t == Type::Float || t == Type::Vector3 || t == Type::Color) { out = a; return true; }
        break;
    case UnOp::BitNot:
        if (t == Type::Int) { out = Value(Int(~Heap::i(a))); return true; }
        break;
    }
    err = "Invalid operand of type '";
    err += type_name(t);
    err += "' for unary operator '";
    err += unop_name(op);
    err += "'.";
    return false;
}

static void pad_to(std::string& out, const std::string& body, int width, bool left, char padc) {
    int n = int(utf8_count(body.data(), body.size()));
    if (width > n && !left) {
        if (padc == '0' && !body.empty() && (body[0] == '-' || body[0] == '+')) {
            out.push_back(body[0]);
            out.append(size_t(width - n), '0');
            out.append(body, 1, std::string::npos);
            return;
        }
        out.append(size_t(width - n), padc);
    }
    out += body;
    if (width > n && left) out.append(size_t(width - n), ' ');
}

static void append_radix(std::string& out, Int v, unsigned radix, bool upper) {
    const char* dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    uint64_t u = v < 0 ? 0 - uint64_t(v) : uint64_t(v);
    char buf[70];
    int n = 0;
    do {
        buf[n++] = dig[u % radix];
        u /= radix;
    } while (u);
    if (v < 0) out.push_back('-');
    while (n) out.push_back(buf[--n]);
}

bool format_percent(std::string_view fmt, const Value& args, std::string& out, std::string& err) {
    const Value* list = &args;
    size_t count = 1;
    if (args.type() == Type::Array) {
        list = Heap::arr(args)->items.data();
        count = Heap::arr(args)->items.size();
    }
    size_t used = 0;
    auto fail = [&](const char* msg) {
        err = "String formatting error: ";
        err += msg;
        return false;
    };
    for (size_t i = 0; i < fmt.size(); ++i) {
        char c = fmt[i];
        if (c != '%') {
            out.push_back(c);
            continue;
        }
        if (++i >= fmt.size()) return fail("incomplete format.");
        if (fmt[i] == '%') {
            out.push_back('%');
            continue;
        }
        bool left = false, plus = false, zero = false;
        int width = 0, prec = -1;
        for (; i < fmt.size(); ++i) {
            if (fmt[i] == '-') left = true;
            else if (fmt[i] == '+') plus = true;
            else if (fmt[i] == '0') zero = true;
            else break;
        }
        if (i < fmt.size() && fmt[i] == '*') {
            if (used >= count) return fail("not enough arguments for format string.");
            if (!list[used].is_number()) return fail("* wants number.");
            width = int(list[used++].as_int());
            ++i;
        } else {
            while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
                width = width * 10 + (fmt[i] - '0');
                if (width > 1024) return fail("width is too large.");
                ++i;
            }
        }
        if (i < fmt.size() && fmt[i] == '.') {
            ++i;
            prec = 0;
            if (i < fmt.size() && fmt[i] == '*') {
                if (used >= count) return fail("not enough arguments for format string.");
                if (!list[used].is_number()) return fail("* wants number.");
                prec = int(list[used++].as_int());
                ++i;
            } else {
                while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
                    prec = prec * 10 + (fmt[i] - '0');
                    if (prec > 64) return fail("precision is too large.");
                    ++i;
                }
            }
        }
        if (width < 0) { left = true; width = -width; }
        if (width > 1024) return fail("width is too large.");
        if (prec > 64) prec = 64;
        if (i >= fmt.size()) return fail("incomplete format.");
        char spec = fmt[i];
        if (used >= count) return fail("not enough arguments for format string.");
        const Value& arg = list[used++];
        std::string body;
        switch (spec) {
        case 's':
            body = stringify(arg, default_host());
            if (prec >= 0 && size_t(prec) < body.size()) body.resize(utf8_offset(body, prec));
            pad_to(out, body, width, left, ' ');
            break;
        case 'c':
            if (arg.type() == Type::String) {
                std::string_view s = Heap::sv(arg);
                body.assign(s.substr(0, utf8_offset(s, 1)));
            } else if (arg.is_number()) {
                Int cp = arg.as_int();
                utf8_append(body, (cp < 0 || cp > 0x10FFFF) ? 0xFFFDu : uint32_t(cp));
            } else {
                return fail("%c requires number or single-character string.");
            }
            pad_to(out, body, width, left, ' ');
            break;
        case 'd':
        case 'i':
        case 'o':
        case 'x':
        case 'X': {
            if (!arg.is_number()) return fail("a number is required.");
            Int v = arg.as_int();
            if (plus && v >= 0) body.push_back('+');
            if (spec == 'o') append_radix(body, v, 8, false);
            else if (spec == 'x') append_radix(body, v, 16, false);
            else if (spec == 'X') append_radix(body, v, 16, true);
            else append_radix(body, v, 10, false);
            pad_to(out, body, width, left, zero && !left ? '0' : ' ');
            break;
        }
        case 'f': {
            if (!arg.is_number()) return fail("a number is required.");
            double v = arg.type() == Type::Int ? double(Heap::i(arg)) : double(Heap::f(arg));
            if (plus && v >= 0) body.push_back('+');
            append_fixed(body, v, prec < 0 ? 6 : prec);
            pad_to(out, body, width, left, zero && !left ? '0' : ' ');
            break;
        }
        case 'v': {
            if (arg.type() != Type::Vector3 && arg.type() != Type::Color && !arg.is_number())
                return fail("%v requires a vector or a number.");
            if (arg.is_number()) {
                append_fixed(body, double(arg.as_real()), prec < 0 ? 6 : prec);
            } else {
                body.push_back('(');
                for (int k = 0; k < 3; ++k) {
                    if (k) body += ", ";
                    if (prec < 0) body += format_real(Heap::comp(arg, k));
                    else append_fixed(body, double(Heap::comp(arg, k)), prec);
                }
                body.push_back(')');
            }
            pad_to(out, body, width, left, ' ');
            break;
        }
        default:
            return fail("unsupported format character.");
        }
        if (out.size() > kMaxStringBytes) return fail("result is too long.");
    }
    if (used < count) return fail("not all arguments converted during string formatting.");
    return true;
}

} // namespace detail
} // namespace jetscript
