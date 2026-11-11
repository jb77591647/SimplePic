cmake_minimum_required(VERSION 3.20)
project(SimplePic CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(SimplePic WIN32
  src/main.cpp
  src/codec/codec.cpp
  src/codec/jpeg_meta.cpp
  src/ops/ops.cpp
  src/pipeline/pipeline.cpp
  src/ui/main_window.cpp
  src/app.manifest
)

target_compile_definitions(SimplePic PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_WARNINGS
)

target_compile_options(SimplePic PRIVATE
  $<$<CONFIG:Release>:/MT /O2 /W3 /EHsc /GL>
  $<$<CONFIG:Debug>:/MTd /W3 /EHsc>
)
target_link_options(SimplePic PRIVATE $<$<CONFIG:Release>:/LTCG>)

target_link_libraries(SimplePic PRIVATE comctl32 comdlg32 shell32 ole32 shlwapi user32 gdi32)
target_include_directories(SimplePic PRIVATE third_party src)
