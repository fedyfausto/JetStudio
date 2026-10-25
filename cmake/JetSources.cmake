# Upstream's top-level CMakeLists targets ESP-IDF, so list the library sources here (Example.cpp and Sample.cpp are apps).
set(JET_SOURCES)
foreach(name BlendSpans Camera Light Material Object PostFX Primitives Renderer Scene Sprite2D Texture TiledSpan TrigLUT)
  list(APPEND JET_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../third_party/Jet/src/${name}.cpp")
endforeach()
