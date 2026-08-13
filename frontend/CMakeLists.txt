add_library(
  frontend
  src/Ast.cpp
  src/AstValidator.cpp
  src/CompilerPipeline.cpp
  src/Lexer.cpp
  src/Parser.cpp
  src/Typechecker.cpp)

add_library(scalanative::frontend ALIAS frontend)

target_include_directories(
  frontend
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(frontend PUBLIC scalanative::support)

scalanative_target_defaults(frontend)
