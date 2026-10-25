#include "js_internal.hpp"

#include <cmath>

namespace jetscript {

using detail::Heap;

const char* type_name(Type t) {
    switch (t) {
    case Type::Nil: return "Nil";
    case Type::Bool: return "bool";
    case Type::Int: return "int";
    case Type::Float: return "float";
    case Type::String: return "String";
    case Type::Vector3: return "Vector3";
    case Type::Color: return "Color";
    case Type::Array: return "Array";
    case Type::Dictionary: return "Dictionary";
    case Type::Object: return "Object";
    }
    return "Nil";
}

namespace detail {

// Containers freed while another one is being destroyed get queued, so deep nesting never recurses on the C++ stack.
static thread_local ContainerObj* t_dead = nullptr;
static thread_local bool t_draining = false;

void Heap::destroy(HeapObj* h) {
    if (h->kind == kStr) {
        static_cast<StrObj*>(h)->~StrObj();
        std::free(h);
        return;
    }
    ContainerObj* c = static_cast<ContainerObj*>(h);
    c->next_dead = t_dead;
    t_dead = c;
    if (t_draining) return;
    t_draining = true;
    while (t_dead) {
        ContainerObj* x = t_dead;
        t_dead = x->next_dead;
        if (x->kind == kArr) delete static_cast<ArrObj*>(x);
        else delete static_cast<DictObj*>(x);
    }
    t_draining = false;
}

uint32_t utf8_count(const char* s, size_t len) {
    uint32_t n = 0;
    for (size_t i = 0; i < len; ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0u) != 0x80u) ++n;
    return n;
}

size_t utf8_offset(std::string_view s, Int cp_index) {
    if (cp_index <= 0) return 0;
    Int n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0u) != 0x80u) {
            if (n == cp_index) return i;
            ++n;
        }
    }
    return s.size();
}

void utf8_append(std::string& out, uint32_t cp) {
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFDu;
    if (cp < 0x80u) {
        out.push_back(char(cp));
    } else if (cp < 0x800u) {
        out.push_back(char(0xC0u | (cp >> 6)));
        out.push_back(char(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(char(0xE0u | (cp >> 12)));
        out.push_back(char(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(char(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(char(0xF0u | (cp >> 18)));
        out.push_back(char(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(char(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(char(0x80u | (cp & 0x3Fu)));
    }
}

StrObj* alloc_string(size_t len) {
    if (len > 0xFFFFFFF0u) len = 0xFFFFFFF0u;
    void* mem = std::malloc(sizeof(StrObj) + len + 1);
    if (!mem) std::abort();
    StrObj* s = new (mem) StrObj();
    s->kind = kStr;
    s->len = uint32_t(len);
    s->chars()[len] = '\0';
    return s;
}

StrObj* new_string(const char* data, size_t len) {
    if (len == 0) return nullptr;
    StrObj* s = alloc_string(len);
    std::memcpy(s->chars(), data, s->len);
    s->ncp = utf8_count(data, s->len);
    return s;
}

Value make_string(std::string_view s) { return Heap::make(Type::String, new_string(s.data(), s.size())); }

Value make_array(std::vector<Value>&& items) {
    ArrObj* a = new ArrObj();
    a->kind = kArr;
    a->items = std::move(items);
    return Heap::make(Type::Array, a);
}

uint32_t hash_bytes(const char* data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static uint32_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return uint32_t(x);
}

static uint32_t real_bits(Real f) {
    if (f == 0) f = 0; // -0 == 0
    uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    if (f != f) u = 0x7fc00000u; // canonical NaN
    return u;
}

uint32_t hash_value(const Value& v) {
    switch (v.type()) {
    case Type::Nil: return 0x9e3779b9u;
    case Type::Bool: return Heap::b(v) ? 0x51u : 0x52u;
    case Type::Int: return mix64(uint64_t(Heap::i(v)));
    case Type::Float: return mix64(real_bits(Heap::f(v)) ^ 0x1234567ull);
    case Type::String: {
        StrObj* s = Heap::str(v);
        if (!s) return hash_bytes("", 0);
        if (!s->hash) s->hash = hash_bytes(s->chars(), s->len);
        return s->hash;
    }
    case Type::Vector3:
    case Type::Color: {
        uint64_t h = real_bits(Heap::comp(v, 0));
        h = h * 31 + real_bits(Heap::comp(v, 1));
        h = h * 31 + real_bits(Heap::comp(v, 2));
        return mix64(h + uint64_t(v.type()));
    }
    case Type::Object: return mix64(uint64_t(Heap::o(v)) ^ 0xabcdefull);
    case Type::Array:
    case Type::Dictionary: return mix64(reinterpret_cast<uintptr_t>(Heap::p(v)));
    }
    return 0;
}

static bool real_same(Real a, Real b) { return a == b || (a != a && b != b); }

bool key_equal(const Value& a, const Value& b) {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
    case Type::Nil: return true;
    case Type::Bool: return Heap::b(a) == Heap::b(b);
    case Type::Int: return Heap::i(a) == Heap::i(b);
    case Type::Float: return real_same(Heap::f(a), Heap::f(b));
    case Type::String: return Heap::sv(a) == Heap::sv(b);
    case Type::Vector3:
    case Type::Color:
        return real_same(Heap::comp(a, 0), Heap::comp(b, 0)) && real_same(Heap::comp(a, 1), Heap::comp(b, 1)) &&
               real_same(Heap::comp(a, 2), Heap::comp(b, 2));
    case Type::Object: return Heap::o(a) == Heap::o(b);
    case Type::Array:
    case Type::Dictionary: return Heap::p(a) == Heap::p(b);
    }
    return false;
}

bool valid_dict_key(const Value& k) { return k.type() != Type::Array && k.type() != Type::Dictionary; }

static constexpr size_t kDictLinear = 8;

int DictObj::find(const Value& key) const {
    if (table.empty()) {
        for (size_t i = 0; i < entries.size(); ++i)
            if (key_equal(entries[i].key, key)) return int(i);
        return -1;
    }
    size_t mask = table.size() - 1;
    size_t i = hash_value(key) & mask;
    for (;;) {
        uint32_t e = table[i];
        if (!e) return -1;
        if (key_equal(entries[e - 1].key, key)) return int(e - 1);
        i = (i + 1) & mask;
    }
}

int DictObj::find_str(std::string_view key) const {
    if (table.empty()) {
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].key.type() == Type::String && Heap::sv(entries[i].key) == key) return int(i);
        return -1;
    }
    size_t mask = table.size() - 1;
    size_t i = hash_bytes(key.data(), key.size()) & mask;
    for (;;) {
        uint32_t e = table[i];
        if (!e) return -1;
        const Value& k = entries[e - 1].key;
        if (k.type() == Type::String && Heap::sv(k) == key) return int(e - 1);
        i = (i + 1) & mask;
    }
}

Value* DictObj::lookup(const Value& key) {
    int i = find(key);
    return i < 0 ? nullptr : &entries[size_t(i)].value;
}

void DictObj::rebuild() {
    table.clear();
    if (entries.size() <= kDictLinear) return;
    size_t cap = 16;
    while (cap < entries.size() * 2) cap <<= 1;
    table.assign(cap, 0);
    size_t mask = cap - 1;
    for (size_t n = 0; n < entries.size(); ++n) {
        size_t i = hash_value(entries[n].key) & mask;
        while (table[i]) i = (i + 1) & mask;
        table[i] = uint32_t(n + 1);
    }
}

void DictObj::set(const Value& key, const Value& value) {
    int i = find(key);
    if (i >= 0) {
        entries[size_t(i)].value = value;
        return;
    }
    DictEntry e;
    e.key = key;
    e.value = value;
    entries.push_back(std::move(e)); // `key` may alias an entry: do not use it below
    if (entries.size() > kDictLinear) {
        if (table.size() < entries.size() * 2) {
            rebuild();
        } else {
            size_t mask = table.size() - 1;
            size_t j = hash_value(entries.back().key) & mask;
            while (table[j]) j = (j + 1) & mask;
            table[j] = uint32_t(entries.size());
        }
    }
}

bool DictObj::erase(const Value& key) {
    int i = find(key);
    if (i < 0) return false;
    entries.erase(entries.begin() + i);
    rebuild();
    return true;
}

void DictObj::clear() {
    entries.clear();
    table.clear();
}

static const Host* g_default_host = nullptr;
const Host* default_host() { return g_default_host; }
void set_default_host(const Host* h, bool set) {
    if (set) g_default_host = h;
    else if (g_default_host == h) g_default_host = nullptr;
}

static void append_quoted(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default: out.push_back(c); break;
        }
    }
    out.push_back('"');
}

// Godot 4 Color::operator String uses String::num(x, 4).
static void append_color_comp(std::string& out, Real x) {
    if (x != x) { out += "nan"; return; }
    if (std::isinf(x)) { out += x < 0 ? "-inf" : "inf"; return; }
    std::string t;
    append_fixed(t, double(x), 4);
    if (t.find('.') != std::string::npos) {
        while (!t.empty() && t.back() == '0') t.pop_back();
        if (!t.empty() && t.back() == '.') t.pop_back();
    }
    if (t == "-0") t = "0";
    out += t;
}

void append_value(std::string& out, const Value& v, const Host* host, int depth, bool quoted) {
    if (out.size() > kMaxStringBytes) return; // bounded work, even for huge or shared structures
    switch (v.type()) {
    case Type::Nil: out += "<null>"; return;
    case Type::Bool: out += Heap::b(v) ? "true" : "false"; return;
    case Type::Int: out += format_int(Heap::i(v)); return;
    case Type::Float: out += format_real(Heap::f(v)); return;
    case Type::String:
        if (quoted) append_quoted(out, Heap::sv(v));
        else out += Heap::sv(v);
        return;
    case Type::Vector3:
        out.push_back('(');
        out += format_real(Heap::comp(v, 0));
        out += ", ";
        out += format_real(Heap::comp(v, 1));
        out += ", ";
        out += format_real(Heap::comp(v, 2));
        out.push_back(')');
        return;
    case Type::Color:
        out.push_back('(');
        append_color_comp(out, Heap::comp(v, 0));
        out += ", ";
        append_color_comp(out, Heap::comp(v, 1));
        out += ", ";
        append_color_comp(out, Heap::comp(v, 2));
        out += ", 1)";
        return;
    case Type::Array: {
        if (depth >= kMaxStringifyDepth) { out += "[...]"; return; }
        const ArrObj* a = Heap::arr(v);
        out.push_back('[');
        for (size_t i = 0; i < a->items.size(); ++i) {
            if (i) out += ", ";
            append_value(out, a->items[i], host, depth + 1, true);
        }
        out.push_back(']');
        return;
    }
    case Type::Dictionary: {
        if (depth >= kMaxStringifyDepth) { out += "{ ... }"; return; }
        const DictObj* d = Heap::dict(v);
        out += "{ ";
        for (size_t i = 0; i < d->entries.size(); ++i) {
            if (i) out += ", ";
            append_value(out, d->entries[i].key, host, depth + 1, true);
            out += ": ";
            append_value(out, d->entries[i].value, host, depth + 1, true);
        }
        out += " }";
        return;
    }
    case Type::Object:
        if (Heap::o(v) == 0) { out += "<null>"; return; }
        if (host) out += host->describe(Heap::o(v));
        else { out += "<Object#"; out += format_int(Int(Heap::o(v))); out += ">"; }
        return;
    }
}

std::string stringify(const Value& v, const Host* host) {
    if (v.type() == Type::String) return std::string(Heap::sv(v));
    std::string out;
    append_value(out, v, host, 0, false);
    return out;
}

Value deep_copy(const Value& v, bool deep, int depth) {
    int budget = kMaxDeepNodes;
    return deep_copy_budget(v, deep, depth, budget);
}

// Containers beyond the depth/work budget are shared instead of copied.
Value deep_copy_budget(const Value& v, bool deep, int depth, int& budget) {
    if (v.type() == Type::Array) {
        const ArrObj* a = Heap::arr(v);
        std::vector<Value> items;
        items.reserve(a->items.size());
        for (const Value& x : a->items) {
            bool rec = deep && depth < kMaxStringifyDepth && (x.type() == Type::Array || x.type() == Type::Dictionary) &&
                       --budget > 0;
            items.push_back(rec ? deep_copy_budget(x, true, depth + 1, budget) : x);
        }
        return make_array(std::move(items));
    }
    if (v.type() == Type::Dictionary) {
        const DictObj* d = Heap::dict(v);
        Value r = Value::new_dictionary();
        DictObj* nd = Heap::dict(r);
        nd->entries.reserve(d->entries.size());
        for (const DictEntry& e : d->entries) {
            DictEntry ne;
            ne.key = e.key;
            bool rec = deep && depth < kMaxStringifyDepth &&
                       (e.value.type() == Type::Array || e.value.type() == Type::Dictionary) && --budget > 0;
            ne.value = rec ? deep_copy_budget(e.value, true, depth + 1, budget) : e.value;
            nd->entries.push_back(std::move(ne));
        }
        nd->rebuild();
        return r;
    }
    return v;
}

} // namespace detail

Value::Value(bool b) : type_(Type::Bool) { u_.b = b; }
Value::Value(int i) : type_(Type::Int) { u_.i = i; }
Value::Value(Int i) : type_(Type::Int) { u_.i = i; }
Value::Value(Real f) : type_(Type::Float), f_(f) {}
Value::Value(double f) : type_(Type::Float), f_(Real(f)) {}
Value::Value(const Vector3& v) : type_(Type::Vector3), f_(v.x) { u_.f[0] = v.y; u_.f[1] = v.z; }
Value::Value(const Color& c) : type_(Type::Color), f_(c.r) { u_.f[0] = c.g; u_.f[1] = c.b; }
Value::Value(const char* s) : type_(Type::String) {
    u_.p = s ? detail::new_string(s, std::strlen(s)) : nullptr;
}
Value::Value(std::string_view s) : type_(Type::String) { u_.p = detail::new_string(s.data(), s.size()); }
Value::Value(const std::string& s) : type_(Type::String) { u_.p = detail::new_string(s.data(), s.size()); }

Value Value::object(ObjectId id) {
    Value v;
    if (id) { v.type_ = Type::Object; v.u_.o = id; }
    return v;
}

Value Value::new_array() {
    detail::ArrObj* a = new detail::ArrObj();
    a->kind = detail::kArr;
    return Heap::make(Type::Array, a);
}

Value Value::new_dictionary() {
    detail::DictObj* d = new detail::DictObj();
    d->kind = detail::kDict;
    return Heap::make(Type::Dictionary, d);
}

Value::Value(const Value& o) : type_(o.type_), f_(o.f_), u_(o.u_) { Heap::retain(*this); }
Value::Value(Value&& o) noexcept : type_(o.type_), f_(o.f_), u_(o.u_) { o.type_ = Type::Nil; }
Value& Value::operator=(const Value& o) {
    Heap::assign(*this, o);
    return *this;
}
Value& Value::operator=(Value&& o) noexcept {
    Heap::move_assign(*this, o);
    return *this;
}
Value::~Value() { Heap::release(*this); }

bool Value::truthy() const { return detail::truthy(*this); }
bool Value::as_bool() const { return type_ == Type::Bool && u_.b; }
Int Value::as_int() const {
    if (type_ == Type::Int) return u_.i;
    if (type_ == Type::Float) return detail::real_to_int(double(f_));
    return 0;
}
Real Value::as_real() const {
    if (type_ == Type::Float) return f_;
    if (type_ == Type::Int) return Real(u_.i);
    return 0;
}
Vector3 Value::as_vector3() const { return type_ == Type::Vector3 ? Heap::vec(*this) : Vector3(); }
Color Value::as_color() const { return type_ == Type::Color ? Heap::col(*this) : Color(); }
std::string_view Value::as_string() const { return Heap::sv(*this); }
ObjectId Value::as_object() const { return type_ == Type::Object ? u_.o : 0; }

size_t Value::array_size() const { return type_ == Type::Array ? Heap::arr(*this)->items.size() : 0; }
Value Value::array_get(size_t index) const {
    if (type_ != Type::Array) return Value();
    const auto& items = Heap::arr(*this)->items;
    return index < items.size() ? items[index] : Value();
}
void Value::array_push(const Value& v) {
    if (type_ != Type::Array) return;
    detail::ArrObj* a = Heap::arr(*this);
    if (a->items.size() < detail::kMaxArrayItems) a->items.push_back(v);
}

std::string Value::to_string() const { return detail::stringify(*this, detail::default_host()); }
bool Value::operator==(const Value& o) const { return detail::values_equal(*this, o, 0); }

} // namespace jetscript
