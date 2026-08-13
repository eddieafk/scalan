add_library(tools-build src/BuildCache.cpp src/BuildConfig.cpp src/BuildDriver.cpp
                            src/BuildReport.cpp src/Toolchain.cpp)

add_library(scalanative::tools-build ALIAS tools-build)

target_include_directories(
  tools-build
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(
  tools-build
  PUBLIC scalanative::frontend
         scalanative::nscplugin
         scalanative::tools-checker
         scalanative::tools-linker
         scalanative::tools-interflow
         scalanative::tools-codegen
         scalanative::support)

scalanative_target_defaults(tools-build)
