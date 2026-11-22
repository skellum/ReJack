cmake_minimum_required(VERSION 3.18)
project(rejack C)

set(CMAKE_C_STANDARD 11)
add_compile_definitions(_DEFAULT_SOURCE)
add_compile_options(-Wall -Wextra)

# --- Core logic: no baresip dependency, always built and tested ------------------
add_library(rejack_core STATIC src/phone.c src/tones.c src/tjdev.c src/e911.c src/linemon.c src/dtmfdet.c)
set_target_properties(rejack_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(rejack_core PUBLIC m)

enable_testing()
add_executable(test_phone test/test_phone.c)
target_link_libraries(test_phone rejack_core)
add_test(NAME phone COMMAND test_phone)
add_executable(dtmfscan test/dtmfscan.c)
target_link_libraries(dtmfscan rejack_core)

# --- baresip module (needs libre + libbaresip v4.12 installed, see README) -------
find_package(PkgConfig REQUIRED)
pkg_check_modules(RE IMPORTED_TARGET libre)
pkg_check_modules(BARESIP IMPORTED_TARGET libbaresip)

if(RE_FOUND AND BARESIP_FOUND)
  message(STATUS "libre ${RE_VERSION}, baresip ${BARESIP_VERSION}: building rejack.so")
  add_library(rejack MODULE src/rejack.c)
  set_target_properties(rejack PROPERTIES PREFIX "")          # rejack.so, as baresip expects
  target_link_libraries(rejack PRIVATE rejack_core PkgConfig::RE PkgConfig::BARESIP)

  # Install next to baresip's own modules unless overridden.
  pkg_get_variable(BARESIP_PREFIX libbaresip prefix)
  set(REJACK_MODULE_DIR "${BARESIP_PREFIX}/lib/baresip/modules" CACHE PATH "baresip module dir")
  install(TARGETS rejack LIBRARY DESTINATION "${REJACK_MODULE_DIR}")
else()
  message(WARNING "libre/libbaresip not found via pkg-config: only core + tests are built. "
                  "Set PKG_CONFIG_PATH to the baresip install prefix (see README).")
endif()
