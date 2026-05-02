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
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.2.6")
project(xiaozhi)
