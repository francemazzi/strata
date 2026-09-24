set(QGIS_STRATA_AI_DIR ${QGIS_DATA_DIR}/python/strata_ai)

set(PY_STRATA_AI_FILES
  __init__.py
  run_python.py
)

add_custom_target(pystrataai ALL)

add_custom_command(TARGET pystrataai
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PYTHON_OUTPUT_DIRECTORY}/strata_ai
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

foreach(pyfile ${PY_STRATA_AI_FILES})
  add_custom_command(TARGET pystrataai
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${pyfile} ${PYTHON_OUTPUT_DIRECTORY}/strata_ai
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
endforeach(pyfile)
PY_COMPILE(pystrataai "${PYTHON_OUTPUT_DIRECTORY}/strata_ai")

install(FILES ${PY_STRATA_AI_FILES} DESTINATION "${QGIS_STRATA_AI_DIR}")
