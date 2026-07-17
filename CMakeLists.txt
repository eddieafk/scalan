cmake_minimum_required(VERSION 3.24)

project(
  cpp_scalanative
  VERSION 0.1.0
  DESCRIPTION "A C++ bootstrap implementation of a Scala Native compiler"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CppScalaNativeOptions)

add_subdirectory(cpp-support)
add_subdirectory(cpp-frontend)
add_subdirectory(cpp-nir)
add_subdirectory(cpp-nscplugin)
add_subdirectory(cpp-runtime)
add_subdirectory(cpp-tools/checker)
add_subdirectory(cpp-tools/linker)
add_subdirectory(cpp-tools/interflow)
add_subdirectory(cpp-tools/codegen)
add_subdirectory(cpp-tools/build)
add_subdirectory(cpp-driver)

enable_testing()
add_subdirectory(cpp-tests/smoke)

