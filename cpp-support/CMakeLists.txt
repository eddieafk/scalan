add_library(
  cpp-support
  src/Arena.cpp
  src/Diagnostics.cpp
  src/SourceManager.cpp)

add_library(cpp-scalanative::support ALIAS cpp-support)

target_include_directories(
  cpp-support
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

cpp_scalanative_target_defaults(cpp-support)

