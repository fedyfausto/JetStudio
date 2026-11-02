#include "js_internal.hpp"

#include <atomic>

// Shared by the compiler and the bytecode loader.
namespace jetscript{

    namespace detail{

        int FunctionData::lineAt(uint32_t pc) const{
            int line = this->line;

            for(const LineEntry& e : lines){
                if(e.pc > pc) break;

                line = e.line;
            }

            return line;
        }

        uint32_t nextScriptSerial(){
            static std::atomic<uint32_t> serial{0};
            return ++serial;
        }

        static const char* const kSignalMethods[SM_COUNT] = {
            "connect", "disconnect", "is_connected", "emit_signal", "has_signal",
            "emit", "get_name", "get_object", "is_null", "call", "callv", "get_method", "is_valid",
        };

        int findSignalMethod(std::string_view name){
            for(int i = 0; i < SM_COUNT; ++i)
                if(name == kSignalMethods[i]) return i;

            return -1;
        }

        const char* signalMethodName(int id){ return id >= 0 && id < SM_COUNT ? kSignalMethods[id] : "?"; }

        bool signalMethodInfo(int id, Type receiver, int& min_args, int& max_args, SType& ret){
            auto set = [&](int lo, int hi, SType r){
                min_args = lo;
                max_args = hi;
                ret = r;
                return true;
            };

            const SType B = SType(Type::Bool), I = SType(Type::Int), S = SType(Type::String), O = SType(Type::Object);

            if(receiver == Type::Object){
                switch(id){
                    case SM_connect: return set(2, 3, I);
                    case SM_disconnect: return set(2, 2, SType(Type::Nil));
                    case SM_is_connected: return set(2, 2, B);
                    case SM_emit_signal: return set(1, -1, I);
                    case SM_has_signal: return set(1, 1, B);
                    default: return false;
                }
            }

            if(receiver == Type::Signal){
                switch(id){
                    case SM_connect: return set(1, 2, I);
                    case SM_disconnect: return set(1, 1, SType(Type::Nil));
                    case SM_is_connected: return set(1, 1, B);
                    case SM_emit: return set(0, -1, SType(Type::Nil));
                    case SM_get_name: return set(0, 0, S);
                    case SM_get_object: return set(0, 0, O);
                    case SM_is_null: return set(0, 0, B);
                    default: return false;
                }
            }

            if(receiver == Type::Callable){
                switch(id){
                    case SM_call: return set(0, -1, kUnknown);
                    case SM_callv: return set(1, 1, kUnknown);
                    case SM_get_method: return set(0, 0, S);
                    case SM_get_object: return set(0, 0, O);
                    case SM_is_valid: case SM_is_null: return set(0, 0, B);
                    default: return false;
                }
            }

            return false;
        }

    }

    const detail::SymEntry* ScriptData::findSymbol(std::string_view name) const{
        size_t lo = 0, hi = symbols.size();

        while(lo < hi){
            size_t mid = (lo + hi) / 2;
            int c = std::string_view(symbols[mid].name).compare(name);

            if(c == 0) return &symbols[mid];

            if(c < 0) lo = mid + 1;
            else hi = mid;
        }

        return nullptr;
    }

    Script::Script(std::unique_ptr<ScriptData> data) : data_(std::move(data)){}
    Script::~Script() = default;
    const std::string& Script::path() const{ return data_->path; }
    const std::string& Script::baseClass() const{ return data_->baseClass; }
    const std::vector<ExportInfo>& Script::exports() const{ return data_->exports; }
    const std::vector<MemberInfo>& Script::members() const{ return data_->members; }
    const std::vector<FunctionInfo>& Script::functions() const{ return data_->functions; }
    const std::vector<SignalInfo>& Script::signals() const{ return data_->signals; }
    bool Script::hasFunction(std::string_view name) const{
        const detail::SymEntry* s = data_->findSymbol(name);
        return s && s->kind == detail::SymKind::Func;
    }

}
