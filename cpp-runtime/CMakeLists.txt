add_library(cpp-runtime src/RuntimeConfig.cpp)

add_library(cpp-scalanative::runtime ALIAS cpp-runtime)

target_include_directories(
  cpp-runtime
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(cpp-runtime PUBLIC cpp-scalanative::support)

cpp_scalanative_target_defaults(cpp-runtime)

