add_library(cpp-tools-build src/BuildCache.cpp src/BuildConfig.cpp src/BuildDriver.cpp
                            src/BuildReport.cpp src/Toolchain.cpp)

add_library(cpp-scalanative::tools-build ALIAS cpp-tools-build)

target_include_directories(
  cpp-tools-build
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(
  cpp-tools-build
  PUBLIC cpp-scalanative::frontend
         cpp-scalanative::nscplugin
         cpp-scalanative::tools-checker
         cpp-scalanative::tools-linker
         cpp-scalanative::tools-interflow
         cpp-scalanative::tools-codegen
         cpp-scalanative::support)

cpp_scalanative_target_defaults(cpp-tools-build)
