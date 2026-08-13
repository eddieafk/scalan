add_library(tools-linker src/Linker.cpp)

add_library(scalanative::tools-linker ALIAS tools-linker)

target_include_directories(
  tools-linker
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(
  tools-linker
  PUBLIC scalanative::nir scalanative::support)

scalanative_target_defaults(tools-linker)
