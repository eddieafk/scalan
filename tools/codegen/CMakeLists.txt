set(SCALANATIVE_RUNTIME_RESOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/runtime")
set(SCALANATIVE_RUNTIME_LIFECYCLE_RESOURCE
    "${SCALANATIVE_RUNTIME_RESOURCE_DIR}/lifecycle.ll")
set(SCALANATIVE_RUNTIME_EXCEPTIONS_RESOURCE
    "${SCALANATIVE_RUNTIME_RESOURCE_DIR}/exceptions.ll")

set_property(
  DIRECTORY APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS
           "${SCALANATIVE_RUNTIME_LIFECYCLE_RESOURCE}"
           "${SCALANATIVE_RUNTIME_EXCEPTIONS_RESOURCE}")

file(READ "${SCALANATIVE_RUNTIME_LIFECYCLE_RESOURCE}"
     SCALANATIVE_RUNTIME_LIFECYCLE_LLVM)
file(READ "${SCALANATIVE_RUNTIME_EXCEPTIONS_RESOURCE}"
     SCALANATIVE_RUNTIME_EXCEPTIONS_LLVM)

set(SCALANATIVE_CODEGEN_GENERATED_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY
     "${SCALANATIVE_CODEGEN_GENERATED_INCLUDE_DIR}/scalanative/tools/codegen")
configure_file(
  cmake/RuntimeLlvmResources.h.in
  "${SCALANATIVE_CODEGEN_GENERATED_INCLUDE_DIR}/scalanative/tools/codegen/RuntimeLlvmResources.h"
  @ONLY)

add_library(tools-codegen src/LlvmCodegen.cpp)

add_library(scalanative::tools-codegen ALIAS tools-codegen)

target_include_directories(
  tools-codegen
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE "${SCALANATIVE_CODEGEN_GENERATED_INCLUDE_DIR}")

target_link_libraries(
  tools-codegen
  PUBLIC scalanative::tools-linker scalanative::runtime)

scalanative_target_defaults(tools-codegen)
