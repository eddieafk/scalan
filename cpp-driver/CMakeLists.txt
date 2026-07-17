add_executable(cpp-scalanative src/main.cpp)

target_link_libraries(cpp-scalanative PRIVATE cpp-scalanative::tools-build)

cpp_scalanative_target_defaults(cpp-scalanative)

