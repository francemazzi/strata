set(CPACK_GENERATOR)
set(CPACK_OUTPUT_CONFIG_FILE "${CMAKE_BINARY_DIR}/BundleConfig.cmake")

add_custom_target(bundle
                  COMMAND ${CMAKE_CPACK_COMMAND} "--config" "${CMAKE_BINARY_DIR}/BundleConfig.cmake" "--verbose"
                  COMMENT "Running CPACK. Please wait..."
                  DEPENDS qgis)

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND QGIS_MAC_BUNDLE AND WITH_BINDINGS)
  # vcpkg deliberately does not install pg_config or gdal-config. The Python
  # bootstrap receives this prefix to create build-only shims from the same
  # include and library directories used by the desktop build.
  set(STRATA_PYTHON_RUNTIME_PREFIX "")
  if(WITH_VCPKG AND DEFINED TARGET_SYSROOT)
    set(STRATA_PYTHON_RUNTIME_PREFIX "${TARGET_SYSROOT}")
  endif()
  # Python GDAL bindings must have exactly the same release as the native
  # library selected by CMake. Passing it through avoids a stale package pin
  # when the vcpkg baseline changes GDAL.
  set(STRATA_PYTHON_RUNTIME_GDAL_VERSION "${GDAL_VERSION}")

  add_custom_target(strata_python_runtime_deps
                    COMMAND ${CMAKE_COMMAND} -E env
                      "STRATA_BUILD_DIR=${CMAKE_BINARY_DIR}"
                      "STRATA_PYTHON_EXECUTABLE=${Python_EXECUTABLE}"
                      "STRATA_RUNTIME_PREFIX=${STRATA_PYTHON_RUNTIME_PREFIX}"
                      "STRATA_GDAL_VERSION=${STRATA_PYTHON_RUNTIME_GDAL_VERSION}"
                      /bin/bash "${CMAKE_SOURCE_DIR}/scripts/bootstrap-strata-python-deps.sh"
                    COMMENT "Preparing bundled Strata Python runtime dependencies"
                    DEPENDS qgis)
  add_dependencies(bundle strata_python_runtime_deps)

  if(TARGET test_app_qgisapppython)
    add_dependencies(test_app_qgisapppython strata_python_runtime_deps)
  endif()
endif()

if(WIN32 AND NOT UNIX)
  set (CREATE_NSIS FALSE CACHE BOOL "Create an installer using NSIS")
endif()
set (CREATE_ZIP FALSE CACHE BOOL "Create a ZIP package")
set (STRATA_WINDOWS_CODE_SIGN FALSE CACHE BOOL "Sign Windows packages using Azure Artifact Signing")

# Do not warn about runtime libs when building using VS Express
if(NOT DEFINED CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS ON)
endif()

if(QGIS_INSTALL_SYS_LIBS)
  include(InstallRequiredSystemLibraries)
endif()

set(CPACK_PACKAGE_NAME "Strata")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Strata - The AI-native GIS")
set(CPACK_PACKAGE_VENDOR "Francesco Mazzi")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/COPYING")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Strata ${COMPLETE_VERSION}")
set(CPACK_PACKAGE_EXECUTABLES "${QGIS_APP_NAME}" "Strata")
set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_SOURCE_DIR}/README.md")

if(WIN32 AND STRATA_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  # Keep the existing installation directory/registry identity for upgrades.
  # QGIS version constants must continue to describe the QGIS ABI.
  set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "Strata ${COMPLETE_VERSION}")
  set(CPACK_PACKAGE_VERSION "${STRATA_VERSION}")
  set(CPACK_PACKAGE_FILE_NAME "Strata-${STRATA_VERSION}-win64")
endif()

if(CREATE_NSIS)
  list(APPEND CPACK_GENERATOR "NSIS")
  # The win_build/sidebar.bmp referenced by upstream QGIS does not exist in
  # this fork — leave CPACK_PACKAGE_ICON unset so NSIS uses its default header.
  set(CPACK_NSIS_INSTALLED_ICON_NAME "\\\\${QGIS_APP_NAME}.exe")
  set(CPACK_NSIS_DISPLAY_NAME "Strata ${CPACK_PACKAGE_VERSION}")
  set(CPACK_NSIS_PACKAGE_NAME "Strata")
  set(CPACK_NSIS_HELP_LINK "https:\\\\\\\\github.com\\\\francemazzi\\\\strata")
  set(CPACK_NSIS_URL_INFO_ABOUT "https:\\\\\\\\github.com\\\\francemazzi\\\\strata")
  set(CPACK_NSIS_CONTACT "francemazzi@gmail.com")
  set(CPACK_NSIS_MODIFY_PATH ON)
endif()

if(CREATE_ZIP)
  list(APPEND CPACK_GENERATOR "ZIP")
endif()

if(WIN32 AND STRATA_WINDOWS_CODE_SIGN)
  if(CREATE_NSIS)
    if(NOT EXISTS "$ENV{STRATA_NSIS_EXECUTABLE}")
      message(FATAL_ERROR "Signed NSIS plugin toolchain is required for release packaging")
    endif()
    set(CPACK_NSIS_EXECUTABLE "$ENV{STRATA_NSIS_EXECUTABLE}")
    set(CPACK_NSIS_DEFINES
      "!uninstfinalize '\"powershell.exe\" -NoProfile -ExecutionPolicy Bypass -File \"${CMAKE_SOURCE_DIR}/scripts/ci/sign-windows-artifacts.ps1\" -Mode staging -Path \"%1\"' = 0")
  endif()
  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/StrataWindowsCodeSignPreBuild.cmake.in"
    "${CMAKE_BINARY_DIR}/StrataWindowsCodeSignPreBuild.cmake"
    @ONLY
  )
  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/StrataWindowsCodeSignPostBuild.cmake.in"
    "${CMAKE_BINARY_DIR}/StrataWindowsCodeSignPostBuild.cmake"
    @ONLY
  )
  list(APPEND CPACK_PRE_BUILD_SCRIPTS "${CMAKE_BINARY_DIR}/StrataWindowsCodeSignPreBuild.cmake")
  list(APPEND CPACK_POST_BUILD_SCRIPTS "${CMAKE_BINARY_DIR}/StrataWindowsCodeSignPostBuild.cmake")
  message(STATUS "   + Windows code signing              YES")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND QGIS_MAC_BUNDLE)
  set(CREATE_DMG FALSE CACHE BOOL "Create a dmg bundle")
  set(PYMACDEPLOYQT_EXECUTABLE "${CMAKE_SOURCE_DIR}/platform/macos/pymacdeployqt.py")

  configure_file("${CMAKE_SOURCE_DIR}/platform/macos/Info.plist.in" "${CMAKE_BINARY_DIR}/platform//macos/Info.plist" @ONLY)
  install(FILES "${CMAKE_BINARY_DIR}/platform/macos/Info.plist" DESTINATION "${APP_CONTENTS_DIR}")
  install(FILES "${CMAKE_SOURCE_DIR}/images/icons/mac/strata.icns" DESTINATION "${APP_RESOURCES_DIR}")

  set(CPACK_DMG_VOLUME_NAME "Strata")
  set(CPACK_DMG_FORMAT "UDBZ")
  list(APPEND CPACK_GENERATOR "External")
  message(STATUS "   + macdeployqt/DMG                      YES ")
  configure_file(${CMAKE_SOURCE_DIR}/platform/macos/CPackMacDeployQt.cmake.in "${CMAKE_BINARY_DIR}/CPackExternal.cmake" @ONLY)
  set(CPACK_EXTERNAL_PACKAGE_SCRIPT "${CMAKE_BINARY_DIR}/CPackExternal.cmake")
  set(CPACK_EXTERNAL_ENABLE_STAGING ON)
  set(CPACK_PACKAGING_INSTALL_PREFIX "/${QGIS_APP_NAME}.app")
endif()

include(CPack)
