#pragma once
#include "runtime.hpp"

namespace jetstudio {

// Node ids double as script ObjectIds. Input and the SceneTree get reserved ones.
class ScriptHost final : public jetscript::Host {
public:
    static constexpr jetscript::ObjectId InputId = 0xfffffff0u, TreeId = 0xfffffff1u;
    // PackedScenes from load()/preload(): SceneIdBase is the main scene, then the package's other scenes.
    static constexpr jetscript::ObjectId SceneIdBase = 0x7ffe0000u;
    explicit ScriptHost(Runtime* runtime = nullptr);

    const jetscript::ClassInfo* find_class(std::string_view name) const override;
    std::string_view class_of(jetscript::ObjectId) const override;
    int property_id(std::string_view name) const override;
    int method_id(std::string_view name) const override;
    jetscript::ObjectId singleton(std::string_view name) const override;
    jetscript::Status get_property(jetscript::ObjectId, int property, jetscript::Value& out, std::string& error) override;
    jetscript::Status set_property(jetscript::ObjectId, int property, const jetscript::Value& value, std::string& error) override;
    jetscript::Status call_method(jetscript::ObjectId, int method, const jetscript::Value* args, int count,
                                  jetscript::Value& result, std::string& error) override;
    bool has_signal(jetscript::ObjectId, std::string_view name) const override;
    bool has_method(jetscript::ObjectId, std::string_view name) const override;
    std::string describe(jetscript::ObjectId) const override;
    jetscript::ObjectId resource(std::string_view path) const override;
    void print(std::string_view text, bool is_error) override;

    const std::vector<jetscript::ClassInfo>& classes() const;
    // For compiling without a runtime (editor, exports): the autoload names scripts may reference.
    void set_autoloads(std::vector<std::string> names) { autoload_names_ = std::move(names); }

private:
    std::string_view scene_path(jetscript::ObjectId id) const;
    Runtime* runtime_;
    std::vector<std::string> autoload_names_;
};

} // namespace jetstudio
