add_library(
  nir
  src/Builder.cpp
  src/Nir.cpp
  src/Verifier.cpp)

add_library(scalanative::nir ALIAS nir)

target_include_directories(
  nir
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(nir PUBLIC scalanative::support)

scalanative_target_defaults(nir)

