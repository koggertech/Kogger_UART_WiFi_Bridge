cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.16.0")
# Test builds override the version (tools/sbp_update.py needs two different ones).
if(DEFINED ENV{WB_VERSION})
    set(PROJECT_VER "$ENV{WB_VERSION}")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(headunit_wifi_bridge)
