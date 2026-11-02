#pragma once
#include "runtime.hpp"

namespace jetstudio{

    // Node ids double as script ObjectIds. Input and the SceneTree get reserved ones.
    class ScriptHost final : public jetscript::Host{
        public:
            static constexpr jetscript::ObjectId InputId = 0xfffffff0u, TreeId = 0xfffffff1u;
            // PackedScenes from load()/preload(): SceneIdBase is the main scene, then the package's other scenes.
            static constexpr jetscript::ObjectId SceneIdBase = 0x7ffe0000u;
            explicit ScriptHost(Runtime* runtime = nullptr);

            const jetscript::ClassInfo* findClass(std::string_view name) const override;
            std::string_view classOf(jetscript::ObjectId) const override;
            int propertyId(std::string_view name) const override;
            int methodId(std::string_view name) const override;
            jetscript::ObjectId singleton(std::string_view name) const override;
            jetscript::Status getProperty(jetscript::ObjectId, int property, jetscript::Value& out, std::string& error) override;
            jetscript::Status setProperty(jetscript::ObjectId, int property, const jetscript::Value& value, std::string& error) override;
            jetscript::Status callMethod(jetscript::ObjectId, int method, const jetscript::Value* args, int count,
                                          jetscript::Value& result, std::string& error) override;
            bool has_signal(jetscript::ObjectId, std::string_view name) const override;
            bool has_method(jetscript::ObjectId, std::string_view name) const override;
            std::string describe(jetscript::ObjectId) const override;
            jetscript::ObjectId resource(std::string_view path) const override;
            void print(std::string_view text, bool isError) override;

            const std::vector<jetscript::ClassInfo>& classes() const;
            // For compiling without a runtime (editor, exports): the autoload names scripts may reference.
            void set_autoloads(std::vector<std::string> names){ autoloadNames_ = std::move(names); }

        private:
            std::string_view scenePath(jetscript::ObjectId id) const;
            bool isTween(jetscript::ObjectId id) const;
            jetscript::Status tweenCall(jetscript::ObjectId, int method, const jetscript::Value* args, int count,
                                         jetscript::Value& result, std::string& error);
            Runtime* runtime_;
            std::vector<std::string> autoloadNames_;
    };

}
