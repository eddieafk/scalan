add_library(
  cpp-frontend
  src/Ast.cpp
  src/AstValidator.cpp
  src/CompilerPipeline.cpp
  src/Lexer.cpp
  src/Parser.cpp
  src/Typechecker.cpp)

add_library(cpp-scalanative::frontend ALIAS cpp-frontend)

target_include_directories(
  cpp-frontend
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(cpp-frontend PUBLIC cpp-scalanative::support)

cpp_scalanative_target_defaults(cpp-frontend)
