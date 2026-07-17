add_library(cpp-tools-interflow src/InterflowOptimizer.cpp)

add_library(cpp-scalanative::tools-interflow ALIAS cpp-tools-interflow)

target_include_directories(
  cpp-tools-interflow
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(
  cpp-tools-interflow
  PUBLIC cpp-scalanative::tools-linker cpp-scalanative::support)

cpp_scalanative_target_defaults(cpp-tools-interflow)

