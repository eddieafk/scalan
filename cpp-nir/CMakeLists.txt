add_library(
  cpp-nir
  src/Builder.cpp
  src/Nir.cpp
  src/Verifier.cpp)

add_library(cpp-scalanative::nir ALIAS cpp-nir)

target_include_directories(
  cpp-nir
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(cpp-nir PUBLIC cpp-scalanative::support)

cpp_scalanative_target_defaults(cpp-nir)

