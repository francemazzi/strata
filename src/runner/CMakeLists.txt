set(STRATA_RUNNER_SRCS
  main.cpp
  strataremoteclient.cpp
  strataremoteprotocol.cpp
)

add_executable(strata-runner ${STRATA_RUNNER_SRCS})
target_include_directories(strata-runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(strata-runner PRIVATE
  ${QT_VERSION_BASE}::Core
  ${QT_VERSION_BASE}::Network
  Qt6Keychain::Qt6Keychain
)
set_target_properties(strata-runner PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
install(TARGETS strata-runner RUNTIME DESTINATION ${QGIS_BIN_DIR})
add_dependencies(qgis strata-runner)

if(ENABLE_TESTS)
  add_executable(test_strata_remote_protocol
    test_strataremoteprotocol.cpp
    strataremoteprotocol.cpp
  )
  target_include_directories(test_strata_remote_protocol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(test_strata_remote_protocol PRIVATE ${QT_VERSION_BASE}::Core ${QT_VERSION_BASE}::Test)
  add_test(NAME test_strata_remote_protocol COMMAND test_strata_remote_protocol)
endif()
