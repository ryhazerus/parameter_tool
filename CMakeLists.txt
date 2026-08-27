cmake_minimum_required(VERSION 3.20)
project(parameter_tool LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Core logic lives in a library so the unit tests can link it without main().
add_library(paramtool_core STATIC
  src/sha256.cpp
  src/json.cpp
  src/scan.cpp
  src/pool.cpp
  src/store.cpp
  src/manifest.cpp
  src/cli.cpp
  src/commands/cmd_common.cpp
  src/commands/cmd_snap.cpp
  src/commands/cmd_restore.cpp
  src/commands/cmd_list.cpp
  src/commands/cmd_show.cpp
  src/commands/cmd_diff.cpp
  src/commands/cmd_extract.cpp
  src/commands/cmd_delete.cpp
  src/commands/cmd_cleanup.cpp
  src/commands/cmd_verify.cpp
  src/commands/cmd_gc.cpp
)
target_include_directories(paramtool_core PUBLIC src)

# The version reported by `parametertool --version`. Release builds override this with the git tag
# so a tagged binary identifies itself correctly; a plain local build reports the project version.
set(PARAMTOOL_VERSION "${PROJECT_VERSION}" CACHE STRING "Version string reported by --version")
target_compile_definitions(paramtool_core PUBLIC PARAMTOOL_VERSION="${PARAMTOOL_VERSION}")

find_package(Threads REQUIRED)
target_link_libraries(paramtool_core PUBLIC Threads::Threads)

if(MSVC)
  target_compile_options(paramtool_core PRIVATE /W4)
else()
  target_compile_options(paramtool_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

add_executable(parametertool src/main.cpp)
target_link_libraries(parametertool PRIVATE paramtool_core)

install(TARGETS parametertool RUNTIME DESTINATION bin)

# ---- tests ----
include(CTest)
if(BUILD_TESTING)
  foreach(t sha256 json glob)
    add_executable(test_${t} tests/test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE paramtool_core)
    add_test(NAME unit_${t} COMMAND test_${t})
  endforeach()

  add_test(NAME e2e
           COMMAND ${CMAKE_COMMAND} -E env
                   PARAMTOOL=$<TARGET_FILE:parametertool>
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e.sh)
endif()
