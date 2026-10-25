include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/JetSources.cmake")
idf_component_register(SRCS ${JET_SOURCES}
  # Device rendering profile (half-width fields, no z-buffer), see firmware/config.
  INCLUDE_DIRS "../../../third_party/Jet/src" "../../config")
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-math-errno)
