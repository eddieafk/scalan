add_library(tools-interflow src/InterflowOptimizer.cpp)

add_library(scalanative::tools-interflow ALIAS tools-interflow)

target_include_directories(
  tools-interflow
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(
  tools-interflow
  PUBLIC scalanative::tools-linker scalanative::support)

scalanative_target_defaults(tools-interflow)
