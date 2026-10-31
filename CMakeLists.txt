cmake_minimum_required(VERSION 3.20)
project(myMcpTools VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
include(FetchContent)
# Pinned reviewed upstream revisions; header-only components are fetched at configure time.
FetchContent_Declare(cpp_httplib
 GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
 GIT_TAG a18895f42e4768062b858c6847b1ae598b2a3be3
 GIT_SHALLOW FALSE)
FetchContent_Declare(jsoncons
 GIT_REPOSITORY https://github.com/danielaparker/jsoncons.git
 GIT_TAG 91c5e03f9169f2accecf3d5c082163d6a2f34b18
 GIT_SHALLOW FALSE)
FetchContent_GetProperties(cpp_httplib)
if(NOT cpp_httplib_POPULATED)
 FetchContent_Populate(cpp_httplib)
endif()
FetchContent_GetProperties(jsoncons)
if(NOT jsoncons_POPULATED)
 FetchContent_Populate(jsoncons)
endif()
add_library(mymcp STATIC src/mcp.cpp src/schema.cpp src/oauth.cpp src/oauth_client.cpp)
target_include_directories(mymcp PUBLIC
 $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
 $<INSTALL_INTERFACE:include>
 $<BUILD_INTERFACE:${jsoncons_SOURCE_DIR}/include>
 $<BUILD_INTERFACE:${cpp_httplib_SOURCE_DIR}>)
target_compile_definitions(mymcp PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(mymcp PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
 target_link_libraries(mymcp PUBLIC ws2_32 crypt32)
endif()
add_executable(mymcp_example_server tests/fixtures/server.cpp)
target_link_libraries(mymcp_example_server PRIVATE mymcp)
add_executable(mymcp_example_client tests/fixtures/client.cpp)
target_link_libraries(mymcp_example_client PRIVATE mymcp)
add_executable(mymcp_listen_client tests/listen_client.cpp)
target_link_libraries(mymcp_listen_client PRIVATE mymcp)
enable_testing()
foreach(test IN ITEMS protocol extended tasks_schema oauth)
 if(test STREQUAL "tasks_schema")
   set(path tests/test_tasks_schema.cpp)
 else()
   set(path tests/test_${test}.cpp)
 endif()
 add_executable(mymcp_${test}_tests ${path})
 target_link_libraries(mymcp_${test}_tests PRIVATE mymcp)
 add_test(NAME ${test} COMMAND mymcp_${test}_tests)
endforeach()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
 foreach(test IN ITEMS http_smoke stdio_subscription auth_http progress_cancel sse_smoke)
  if(test STREQUAL "sse_smoke")
   add_test(NAME ${test} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/${test}.py" "$<TARGET_FILE:mymcp_example_server>" "$<TARGET_FILE:mymcp_listen_client>")
  else()
   add_test(NAME ${test} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/${test}.py" "$<TARGET_FILE:mymcp_example_server>")
  endif()
 endforeach()
endif()
install(TARGETS mymcp EXPORT mymcpTargets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT mymcpTargets NAMESPACE mymcp:: DESTINATION lib/cmake/mymcp)

# Local mock OAuth authorization server: discovery, registration, PKCE, token exchange and refresh.
add_executable(mymcp_oauth_session_tests tests/test_oauth_session.cpp)
target_link_libraries(mymcp_oauth_session_tests PRIVATE mymcp)
add_test(NAME oauth_session COMMAND mymcp_oauth_session_tests)

# Comprehensive, opt-in executable examples. Never used as production credential stores.
foreach(example IN ITEMS showcase_server showcase_client showcase_subscribe oauth_login oauth_resource_server)
  add_executable(mymcp_${example} tests/fixtures/${example}.cpp)
  target_link_libraries(mymcp_${example} PRIVATE mymcp)
endforeach()
if(Python3_Interpreter_FOUND)
  add_test(NAME showcase_examples COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/showcase_smoke.py"
    "$<TARGET_FILE:mymcp_showcase_server>" "$<TARGET_FILE:mymcp_showcase_client>"
    "$<TARGET_FILE:mymcp_showcase_subscribe>")
  add_test(NAME oauth_resource_example COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/oauth_resource_smoke.py"
    "$<TARGET_FILE:mymcp_oauth_resource_server>")
  set_tests_properties(showcase_examples PROPERTIES TIMEOUT 75)
  set_tests_properties(oauth_resource_example PROPERTIES TIMEOUT 25)
endif()

# No-argument, single-purpose demos. Every source defines run_xxxx_demo() and a
# no-argument main() that calls it; the command line is never used for setup.
foreach(demo IN ITEMS
  tools resources prompts mrtr tasks extensions stdio_server stdio_client
  http_server http_client subscriptions oauth oauth_login oauth_resource oauth_resource_client progress)
  add_executable(run_${demo}_demo examples/run_${demo}_demo.cpp)
  target_link_libraries(run_${demo}_demo PRIVATE mymcp)
  target_include_directories(run_${demo}_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples)
endforeach()
# CMake fills in the correct Debug/Release path to the demo stdio server on
# Linux, macOS and Windows. No argv, cwd assumptions or handwritten .exe paths.
add_dependencies(run_stdio_client_demo run_stdio_server_demo)
add_dependencies(run_progress_demo run_stdio_server_demo)
target_compile_definitions(run_stdio_client_demo PRIVATE
  MYMCP_DEMO_STDIO_SERVER="$<TARGET_FILE:run_stdio_server_demo>")
target_compile_definitions(run_progress_demo PRIVATE
  MYMCP_DEMO_STDIO_SERVER="$<TARGET_FILE:run_stdio_server_demo>")

foreach(demo IN ITEMS tools resources prompts mrtr tasks extensions stdio_client progress oauth oauth_login)
  add_test(NAME demo_${demo} COMMAND run_${demo}_demo)
  set_tests_properties(demo_${demo} PROPERTIES TIMEOUT 20)
endforeach()
if(Python3_Interpreter_FOUND)
  add_test(NAME demo_http_and_sse
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/run_demo_smoke.py"
      "$<TARGET_FILE:run_http_server_demo>" "$<TARGET_FILE:run_http_client_demo>"
      "$<TARGET_FILE:run_subscriptions_demo>" "$<TARGET_FILE:run_oauth_resource_demo>"
      "$<TARGET_FILE:run_oauth_resource_client_demo>")
  set_tests_properties(demo_http_and_sse PROPERTIES TIMEOUT 35)
endif()

# 综合智能体工具 Demo：一个 stdio MCP 服务端包含 10 组、23 个工具，无需命令行参数。
add_executable(run_agent_tools_demo examples/run_agent_tools_demo.cpp)
target_link_libraries(run_agent_tools_demo PRIVATE mymcp)
if(Python3_Interpreter_FOUND)
  add_test(NAME agent_tools_demo
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/agent_tools_demo_smoke.py"
      "$<TARGET_FILE:run_agent_tools_demo>")
  set_tests_properties(agent_tools_demo PROPERTIES TIMEOUT 90)
endif()
