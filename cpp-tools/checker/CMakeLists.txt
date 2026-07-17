add_library(cpp-tools-checker src/NirChecker.cpp)

add_library(cpp-scalanative::tools-checker ALIAS cpp-tools-checker)

target_include_directories(
  cpp-tools-checker
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(
  cpp-tools-checker
  PUBLIC cpp-scalanative::nir cpp-scalanative::support)

cpp_scalanative_target_defaults(cpp-tools-checker)

