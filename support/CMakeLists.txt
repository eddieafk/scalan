add_library(
  support
  src/Arena.cpp
  src/Diagnostics.cpp
  src/SourceManager.cpp)

add_library(scalanative::support ALIAS support)

target_include_directories(
  support
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

scalanative_target_defaults(support)

