cmake_minimum_required(VERSION 3.16)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/.mesh-sdk/bt")
# Build the controller and its declared dependencies, excluding unused SDK
# components (notably the S3 RGB LCD driver that crashes the pinned compiler).
set(COMPONENTS src)
# PlatformIO needs its generated language-probe component for SCons flags.
if(EXISTS "$ENV{IDF_PATH}/components/__pio_env/CMakeLists.txt")
    list(APPEND COMPONENTS __pio_env)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pump_muti)
