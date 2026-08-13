add_library(tools-checker src/NirChecker.cpp)

add_library(scalanative::tools-checker ALIAS tools-checker)

target_include_directories(
  tools-checker
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(
  tools-checker
  PUBLIC scalanative::nir scalanative::support)

scalanative_target_defaults(tools-checker)
