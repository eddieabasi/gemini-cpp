cmake_minimum_required(VERSION 3.25)

project(gemini_cpp
  VERSION 0.1.0
  DESCRIPTION "Modern C++ client for the Google Gemini API with a coding-agent evaluation harness"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(GEMINI_BUILD_CLI "Build the gemini-cpp command line tool" ON)
option(GEMINI_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(GEMINI_BUILD_EXAMPLES "Build example programs" ${PROJECT_IS_TOP_LEVEL})
option(GEMINI_WERROR "Treat compiler warnings as errors" OFF)
option(GEMINI_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

include(FetchContent)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    SYSTEM)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Shared compile settings
# ---------------------------------------------------------------------------
add_library(gemini_warnings INTERFACE)
if(MSVC)
  target_compile_options(gemini_warnings INTERFACE /W4 /permissive-)
  if(GEMINI_WERROR)
    target_compile_options(gemini_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(gemini_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align
    -Wunused -Woverloaded-virtual -Wnull-dereference -Wimplicit-fallthrough
    # Partial aggregate initialisation relying on default member initialisers is intended.
    -Wno-missing-field-initializers)
  if(GEMINI_WERROR)
    target_compile_options(gemini_warnings INTERFACE -Werror)
  endif()
endif()

if(GEMINI_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Core client library
# ---------------------------------------------------------------------------
add_library(gemini
  src/error.cpp
  src/types.cpp
  src/serialization.cpp
  src/sse.cpp
  src/retry.cpp
  src/curl_transport.cpp
  src/client.cpp
  src/chat.cpp
  src/tools.cpp
  src/logging.cpp)
add_library(gemini::gemini ALIAS gemini)
target_include_directories(gemini PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(gemini
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE CURL::libcurl Threads::Threads $<BUILD_INTERFACE:gemini_warnings>)
target_compile_definitions(gemini PRIVATE GEMINI_CPP_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# Evaluation harness (compiles and tests model-written C++)
# ---------------------------------------------------------------------------
add_library(gemini_eval
  src/eval/code_extract.cpp
  src/eval/process.cpp
  src/eval/task.cpp
  src/eval/runner.cpp
  src/eval/report.cpp)
add_library(gemini::eval ALIAS gemini_eval)
target_link_libraries(gemini_eval
  PUBLIC gemini Threads::Threads
  PRIVATE $<BUILD_INTERFACE:gemini_warnings>)

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
if(GEMINI_BUILD_CLI)
  add_executable(gemini-cpp apps/main.cpp apps/cli_args.cpp)
  target_link_libraries(gemini-cpp PRIVATE gemini gemini_eval gemini_warnings)
  target_compile_definitions(gemini-cpp PRIVATE GEMINI_CPP_VERSION="${PROJECT_VERSION}")
endif()

if(GEMINI_BUILD_EXAMPLES)
  add_executable(quickstart examples/quickstart.cpp)
  target_link_libraries(quickstart PRIVATE gemini gemini_warnings)
  set_target_properties(quickstart PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/examples)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(GEMINI_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS gemini gemini_eval
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(GEMINI_BUILD_CLI)
  install(TARGETS gemini-cpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY tasks/ DESTINATION ${CMAKE_INSTALL_DATADIR}/gemini-cpp/tasks)
endif()
