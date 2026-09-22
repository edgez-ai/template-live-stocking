# The Seeed PlatformIO package installs its board definitions into Zephyr.
# Do not add the platform board tree as another BOARD_ROOT: Zephyr 4.2.1
# already contains xiao_nrf54l15 and rejects duplicate board definitions.

if(NOT DEFINED DTC_OVERLAY_FILE)
	set(DTC_OVERLAY_FILE
		"${CMAKE_CURRENT_LIST_DIR}/boards/xiao_nrf54l15_nrf54l15_cpuapp.overlay"
	)
endif()

if(DEFINED ENV{MORSE_ZEPHYR_MODULE})
	set(MORSE_ZEPHYR_MODULE "$ENV{MORSE_ZEPHYR_MODULE}")
	list(APPEND ZEPHYR_EXTRA_MODULES "${MORSE_ZEPHYR_MODULE}")
else()
	set(MORSE_ZEPHYR_MODULE "${CMAKE_CURRENT_LIST_DIR}/../modules/mm-iot-zephyr")
	if(EXISTS "${MORSE_ZEPHYR_MODULE}/CMakeLists.txt")
		list(APPEND ZEPHYR_EXTRA_MODULES "${MORSE_ZEPHYR_MODULE}")
	endif()
endif()

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(live-stocking-nrf54)

zephyr_include_directories("${CMAKE_CURRENT_LIST_DIR}")
zephyr_include_directories(
	"${CMAKE_CURRENT_LIST_DIR}/../nrf54/include"
	"${CMAKE_CURRENT_LIST_DIR}/../nrf54/src"
	"${CMAKE_CURRENT_LIST_DIR}/../nrf54/src/mesh"
	"${CMAKE_CURRENT_LIST_DIR}/../nrf54/src/mesh/generated"
	"${CMAKE_CURRENT_LIST_DIR}/../nrf54/src/nanopb"
)
target_compile_definitions(app PRIVATE PB_ENABLE_MALLOC=1)

if(CONFIG_WIFI_MORSE_SM AND TARGET libmorse)
	get_target_property(LIBMORSE_IMPORTED libmorse IMPORTED)
	if(NOT LIBMORSE_IMPORTED)
		set(MORSE_MORSELIB_DRIVER_SOURCE
		"${MORSE_ZEPHYR_MODULE}/submodules/mm-iot-sdk/framework/morselib/src/driver/driver.c"
		)
		set(MORSE_MORSELIB_DRIVER_WRAPPER
		"${CMAKE_CURRENT_BINARY_DIR}/morse_platformio_wrappers/morse_morselib_driver_wrapper.c"
		)
		get_target_property(LIBMORSE_SOURCES libmorse SOURCES)
		if(LIBMORSE_SOURCES)
			list(REMOVE_ITEM LIBMORSE_SOURCES "${MORSE_MORSELIB_DRIVER_SOURCE}")
			set_target_properties(libmorse PROPERTIES SOURCES "${LIBMORSE_SOURCES}")
			configure_file(
			"${CMAKE_CURRENT_LIST_DIR}/morse_morselib_driver_wrapper.c.in"
			"${MORSE_MORSELIB_DRIVER_WRAPPER}"
			@ONLY
			)
			target_sources(libmorse PRIVATE "${MORSE_MORSELIB_DRIVER_WRAPPER}")
		endif()
	endif()
endif()

if(CONFIG_WIFI_MORSE OR CONFIG_WIFI_MORSE_SM)
	get_target_property(LIBMORSE_IMPORTED libmorse IMPORTED)
	if(LIBMORSE_IMPORTED)
		get_target_property(MORSE_ARCHIVE libmorse IMPORTED_LOCATION)
		# The prebuilt driver allocates only 400 words for its health thread.
		# Full newlib logging overflows that stack during the first health check.
		zephyr_ld_options("-Wl,--wrap=mmosal_task_create")
		target_compile_definitions(app PRIVATE EDGEZ_MORSE_HEALTH_STACK_WRAPPER=1)
		target_link_libraries(app PRIVATE
			"-Wl,--push-state,--whole-archive,${MORSE_ARCHIVE},--pop-state")
		target_compile_definitions(app PRIVATE MMOSAL_NOASSERT MMOSAL_NO_DEBUGLOG)
		zephyr_include_directories("${MORSE_ZEPHYR_MODULE}/include")
	else()
		target_link_libraries(app PRIVATE libmorse)
		zephyr_include_directories(
			"${MORSE_ZEPHYR_MODULE}/components/morse_sm/hostap"
			"${MORSE_ZEPHYR_MODULE}/submodules/mm-iot-sdk/framework/morselib/include"
		)
	endif()
endif()

file(GLOB app_sources ../nrf54/src/*.c ../nrf54/src/nanopb/*.c)
list(FILTER app_sources EXCLUDE REGEX "/meshtastic_phone_api\\.c$")
target_sources(app PRIVATE ${app_sources})

if(CONFIG_WIFI_MORSE_SM)
	get_target_property(LIBMORSE_IMPORTED libmorse IMPORTED)
	if(LIBMORSE_IMPORTED)
		set(MORSE_FIRMWARE_ROOT "${MORSE_ZEPHYR_MODULE}/zephyr/blobs/firmware")
	else()
		set(MORSE_FIRMWARE_ROOT
			"${MORSE_ZEPHYR_MODULE}/submodules/mm-iot-sdk/framework/morsefirmware")
	endif()
	if(CONFIG_DT_HAS_MORSE_MM8108_ENABLED)
		set(MORSE_HALOW_FIRMWARE_PATH "${MORSE_FIRMWARE_ROOT}/mm8108b2-rl.mbin")
		if(LIBMORSE_IMPORTED)
			set(MORSE_HALOW_BCF_PATH "${MORSE_FIRMWARE_ROOT}/${CONFIG_WIFI_MORSE_BCF}")
		else()
			set(MORSE_HALOW_BCF_PATH
				"${MORSE_FIRMWARE_ROOT}/mm8108/bcfs/${CONFIG_WIFI_MORSE_BCF}")
		endif()
	else()
		set(MORSE_HALOW_FIRMWARE_PATH "${MORSE_FIRMWARE_ROOT}/mm6108.mbin")
		if(LIBMORSE_IMPORTED)
			set(MORSE_HALOW_BCF_PATH "${MORSE_FIRMWARE_ROOT}/${CONFIG_WIFI_MORSE_BCF}")
		else()
			set(MORSE_HALOW_BCF_PATH
				"${MORSE_FIRMWARE_ROOT}/mm6108/bcfs/${CONFIG_WIFI_MORSE_BCF}")
		endif()
	endif()
	if(EXISTS "${MORSE_HALOW_FIRMWARE_PATH}" AND EXISTS "${MORSE_HALOW_BCF_PATH}")
		target_compile_definitions(app PRIVATE
			MORSE_HALOW_FIRMWARE_PATH="${MORSE_HALOW_FIRMWARE_PATH}"
			MORSE_HALOW_BCF_PATH="${MORSE_HALOW_BCF_PATH}"
		)
	else()
		message(FATAL_ERROR
			"Morse firmware or BCF is missing for the selected chip: "
			"${MORSE_HALOW_FIRMWARE_PATH}; ${MORSE_HALOW_BCF_PATH}"
		)
	endif()
endif()
