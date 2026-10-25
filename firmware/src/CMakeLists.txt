# Only the JetScript VM: the editor ships scripts as bytecode inside the package.
file(GLOB JET_SCRIPT_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../../runtime/script/*.cpp")
list(FILTER JET_SCRIPT_SOURCES EXCLUDE REGEX "js_(lexer|parser|compiler)\\.cpp$")
idf_component_register(SRCS "main.cpp" "../../runtime/runtime.cpp" "../../runtime/physics.cpp" "../../runtime/particles.cpp" "../../runtime/script_host.cpp" "../../runtime/pack.cpp"
  ${JET_SCRIPT_SOURCES}
  # JetConfig.hpp comes from the Jet component (firmware/config), not runtime/config.
  INCLUDE_DIRS "." "../../runtime" "../../runtime/script"
  REQUIRES Jet LovyanGFX esp_timer)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PUBLIC JETSCRIPT_NO_COMPILER)
