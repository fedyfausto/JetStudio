// Shared by the compiler and the bytecode loader.
#include "js_internal.hpp"

#include <atomic>

namespace jetscript {

namespace detail {

int FunctionData::line_at(uint32_t pc) const {
    int line = this->line;
    for (const LineEntry& e : lines) {
        if (e.pc > pc) break;
        line = e.line;
    }
    return line;
}

uint32_t next_script_serial() {
    static std::atomic<uint32_t> serial{0};
    return ++serial;
}

} // namespace detail

const detail::SymEntry* ScriptData::find_symbol(std::string_view name) const {
    size_t lo = 0, hi = symbols.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = std::string_view(symbols[mid].name).compare(name);
        if (c == 0) return &symbols[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

Script::Script(std::unique_ptr<ScriptData> data) : data_(std::move(data)) {}
Script::~Script() = default;
const std::string& Script::path() const { return data_->path; }
const std::string& Script::base_class() const { return data_->base_class; }
const std::vector<ExportInfo>& Script::exports() const { return data_->exports; }
const std::vector<MemberInfo>& Script::members() const { return data_->members; }
const std::vector<FunctionInfo>& Script::functions() const { return data_->functions; }
bool Script::has_function(std::string_view name) const {
    const detail::SymEntry* s = data_->find_symbol(name);
    return s && s->kind == detail::SymKind::Func;
}

} // namespace jetscript
