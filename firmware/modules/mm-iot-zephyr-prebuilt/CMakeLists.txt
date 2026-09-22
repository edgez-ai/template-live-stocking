# Binary-only Morse Micro module for the KeepTeen nRF54L15/MM6108 profile.
set(MORSE_BINARY_ARCHIVE "${CMAKE_CURRENT_LIST_DIR}/zephyr/blobs/lib/mm6108/arm-cortex-m33f/libmorse.a")
if(CONFIG_WIFI_MORSE_SM)
  if(NOT EXISTS "${MORSE_BINARY_ARCHIVE}")
    message(FATAL_ERROR "Missing packaged Morse archive: ${MORSE_BINARY_ARCHIVE}")
  endif()
  add_library(libmorse STATIC IMPORTED GLOBAL)
  set_target_properties(libmorse PROPERTIES
    IMPORTED_LOCATION "${MORSE_BINARY_ARCHIVE}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/include")
  target_compile_definitions(libmorse INTERFACE MMOSAL_NOASSERT MMOSAL_NO_DEBUGLOG)
  zephyr_include_directories("${CMAKE_CURRENT_LIST_DIR}/include")
  zephyr_compile_definitions(MMOSAL_NOASSERT MMOSAL_NO_DEBUGLOG)
endif()
