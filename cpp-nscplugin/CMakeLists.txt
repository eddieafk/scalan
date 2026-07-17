add_library(cpp-nscplugin src/NirEmitter.cpp)

add_library(cpp-scalanative::nscplugin ALIAS cpp-nscplugin)

target_include_directories(
  cpp-nscplugin
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(
  cpp-nscplugin
  PUBLIC cpp-scalanative::frontend cpp-scalanative::nir
         cpp-scalanative::support)

cpp_scalanative_target_defaults(cpp-nscplugin)

