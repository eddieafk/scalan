add_library(nscplugin src/NirEmitter.cpp)

add_library(scalanative::nscplugin ALIAS nscplugin)

target_include_directories(
  nscplugin
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(
  nscplugin
  PUBLIC scalanative::frontend scalanative::nir
         scalanative::support)

scalanative_target_defaults(nscplugin)
