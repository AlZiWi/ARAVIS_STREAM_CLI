cmake_minimum_required(VERSION 3.16)

project(ARAVIS_STREAM_CLI VERSION 0.1.0 LANGUAGES C CXX)

option(ARAVIS_ENABLE_PREVIEW "Build the optional OpenCV preview window" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ARAVIS REQUIRED IMPORTED_TARGET aravis-0.8)
pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)

add_executable(aravis_stream_cli
  src/aravis_stream_cli.c
)

target_link_libraries(aravis_stream_cli PRIVATE PkgConfig::ARAVIS PkgConfig::JPEG)
target_compile_features(aravis_stream_cli PRIVATE c_std_99)

if(ARAVIS_ENABLE_PREVIEW)
  find_package(OpenCV CONFIG REQUIRED COMPONENTS core highgui imgproc)
  target_sources(aravis_stream_cli PRIVATE src/preview.cpp)
  target_link_libraries(aravis_stream_cli PRIVATE ${OpenCV_LIBS})
  target_include_directories(aravis_stream_cli PRIVATE ${OpenCV_INCLUDE_DIRS})
  target_compile_definitions(aravis_stream_cli PRIVATE ARAVIS_CLI_HAS_PREVIEW=1)
  target_compile_features(aravis_stream_cli PRIVATE cxx_std_11)
endif()

if(APPLE)
  target_compile_options(aravis_stream_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()
