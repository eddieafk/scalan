add_library(cpp-tools-linker src/Linker.cpp)

add_library(cpp-scalanative::tools-linker ALIAS cpp-tools-linker)

target_include_directories(
  cpp-tools-linker
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(
  cpp-tools-linker
  PUBLIC cpp-scalanative::nir cpp-scalanative::support)

cpp_scalanative_target_defaults(cpp-tools-linker)

