cmake_minimum_required(VERSION 3.22)
project(StrataClaudeTransportTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network Test)
enable_testing()
get_filename_component(STRATA_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_library(strata_claude_transport STATIC
  "${STRATA_SOURCE}/src/app/ai/qgsaiclaudecodeauth.cpp"
  "${STRATA_SOURCE}/src/app/ai/qgsaiclaudecoderuntime.cpp"
  "${STRATA_SOURCE}/src/app/ai/qgsaiclaudemcpbridge.cpp")
target_include_directories(strata_claude_transport PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/support"
  "${STRATA_SOURCE}/src/app"
  "${STRATA_SOURCE}/src/app/ai"
  "${STRATA_SOURCE}/tests/src/app")
target_compile_definitions(strata_claude_transport PUBLIC STRATA_CLAUDE_TRANSPORT_TESTS QT_USE_QSTRINGBUILDER)
target_link_libraries(strata_claude_transport PUBLIC Qt6::Core Qt6::Network Qt6::Test)
if(MSVC)
  target_compile_options(strata_claude_transport PUBLIC /utf-8)
endif()
foreach(name IN ITEMS claudecoderuntime claudemcpbridge)
  add_executable(test_${name} "${STRATA_SOURCE}/tests/src/app/testqgsai${name}.cpp")
  target_link_libraries(test_${name} PRIVATE strata_claude_transport)
  add_test(NAME ${name} COMMAND test_${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 90)
endforeach()
