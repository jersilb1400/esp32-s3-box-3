# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Extra defaults for this fork (ESP-BOX-3 full kit). IDF merges these in order after
# sdkconfig (if any); see https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/kconfig.html
set(_ESP_BOX3_XIAOZHI_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.esp-box3-xiaozhi")
if(EXISTS "${_ESP_BOX3_XIAOZHI_DEFAULTS}")
    set(SDKCONFIG_DEFAULTS
        "${CMAKE_SOURCE_DIR}/sdkconfig.defaults"
        "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.esp32s3"
        "${_ESP_BOX3_XIAOZHI_DEFAULTS}"
    )
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Jarvis HUD: emit RGB565 face blob at CMake configure time. add_custom_command(OUTPUT ...)
# cannot live in main/CMakeLists.txt because ESP-IDF re-parses that file in script mode during
# component dependency scanning ("add_custom_command command is not scriptable").
set(_JARVIS_FACE_TOOL "${CMAKE_SOURCE_DIR}/jarvis_hud/tools/gen_jarvis_artist_face_asset.py")
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_JARVIS_FACE_TOOL}")
execute_process(
    COMMAND python3 "${_JARVIS_FACE_TOOL}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE _jarvis_face_gen_rc
)
if(NOT _jarvis_face_gen_rc EQUAL "0")
    message(FATAL_ERROR "jarvis_hud face generator failed: python3 \"${_JARVIS_FACE_TOOL}\" (exit ${_jarvis_face_gen_rc})")
endif()

# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.2.6")
project(xiaozhi)
