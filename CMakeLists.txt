cmake_minimum_required(VERSION 3.24)

project(
  scalanative
  VERSION 0.1.0
  DESCRIPTION "A C++ bootstrap implementation of a Scala Native compiler"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ScalaNativeOptions)

add_subdirectory(support)
add_subdirectory(frontend)
add_subdirectory(nir)
add_subdirectory(nscplugin)
add_subdirectory(runtime)
add_subdirectory(tools/checker)
add_subdirectory(tools/linker)
add_subdirectory(tools/interflow)
add_subdirectory(tools/codegen)
add_subdirectory(tools/build)
add_subdirectory(driver)

enable_testing()
add_subdirectory(tests)
