set(CPP_SCALANATIVE_RUNTIME_RESOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/runtime")
set(CPP_SCALANATIVE_RUNTIME_LIFECYCLE_RESOURCE
    "${CPP_SCALANATIVE_RUNTIME_RESOURCE_DIR}/lifecycle.ll")
set(CPP_SCALANATIVE_RUNTIME_EXCEPTIONS_RESOURCE
    "${CPP_SCALANATIVE_RUNTIME_RESOURCE_DIR}/exceptions.ll")

set_property(
  DIRECTORY APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS
           "${CPP_SCALANATIVE_RUNTIME_LIFECYCLE_RESOURCE}"
           "${CPP_SCALANATIVE_RUNTIME_EXCEPTIONS_RESOURCE}")

file(READ "${CPP_SCALANATIVE_RUNTIME_LIFECYCLE_RESOURCE}"
     CPP_SCALANATIVE_RUNTIME_LIFECYCLE_LLVM)
file(READ "${CPP_SCALANATIVE_RUNTIME_EXCEPTIONS_RESOURCE}"
     CPP_SCALANATIVE_RUNTIME_EXCEPTIONS_LLVM)

set(CPP_SCALANATIVE_CODEGEN_GENERATED_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY
     "${CPP_SCALANATIVE_CODEGEN_GENERATED_INCLUDE_DIR}/scalanative/tools/codegen")
configure_file(
  cmake/RuntimeLlvmResources.h.in
  "${CPP_SCALANATIVE_CODEGEN_GENERATED_INCLUDE_DIR}/scalanative/tools/codegen/RuntimeLlvmResources.h"
  @ONLY)

add_library(cpp-tools-codegen src/LlvmCodegen.cpp)

add_library(cpp-scalanative::tools-codegen ALIAS cpp-tools-codegen)

target_include_directories(
  cpp-tools-codegen
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE "${CPP_SCALANATIVE_CODEGEN_GENERATED_INCLUDE_DIR}")

target_link_libraries(
  cpp-tools-codegen
  PUBLIC cpp-scalanative::tools-linker cpp-scalanative::runtime)

cpp_scalanative_target_defaults(cpp-tools-codegen)
