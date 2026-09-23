include(FetchContent)

find_package(GTest 1.14 QUIET)
if(NOT GTest_FOUND)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
    SYSTEM)
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(gemini_tests
  test_serialization.cpp
  test_sse.cpp
  test_retry.cpp
  test_client.cpp
  test_chat.cpp
  test_tools.cpp
  test_code_extract.cpp
  test_process.cpp
  test_runner.cpp
  test_live.cpp)
target_link_libraries(gemini_tests PRIVATE gemini gemini_eval GTest::gtest_main gemini_warnings)
target_compile_definitions(gemini_tests PRIVATE GEMINI_TASKS_DIR="${PROJECT_SOURCE_DIR}/tasks")

include(GoogleTest)
gtest_discover_tests(gemini_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)

# Validates the benchmark itself: every task's hidden tests must pass against its
# reference solution, otherwise model scores would be meaningless.
if(TARGET gemini-cpp)
  add_test(NAME tasks.reference_solutions_pass
    COMMAND gemini-cpp eval --reference --jobs 4 --tasks ${PROJECT_SOURCE_DIR}/tasks)
  set_tests_properties(tasks.reference_solutions_pass PROPERTIES TIMEOUT 600)
endif()
