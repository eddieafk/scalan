add_executable(scalanative src/main.cpp)

target_link_libraries(scalanative PRIVATE scalanative::tools-build)

scalanative_target_defaults(scalanative)
