function(cpp_scalanative_target_defaults target)
  target_compile_features(${target} PUBLIC cxx_std_20)

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      ${target}
      PRIVATE -Wall
              -Wextra
              -Wpedantic
              -Wconversion
              -Wshadow
              -Wnon-virtual-dtor)
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  endif()
endfunction()

