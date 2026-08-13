add_library(runtime src/RuntimeConfig.cpp)

add_library(scalanative::runtime ALIAS runtime)

target_include_directories(
  runtime
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(runtime PUBLIC scalanative::support)

scalanative_target_defaults(runtime)
