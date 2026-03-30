cmake_minimum_required(VERSION 3.16)

# ESP-ADF components (audio_pipeline, audio_stream, etc.)
if(DEFINED ENV{ADF_PATH})
    set(EXTRA_COMPONENT_DIRS $ENV{ADF_PATH}/components)
    message(STATUS "ESP-ADF found: $ENV{ADF_PATH}")
else()
    message(WARNING "ADF_PATH not set — building without ESP-ADF components")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(MOSA_MIC_PROJ)
