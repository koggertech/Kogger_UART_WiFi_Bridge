# Standalone bring-up tool, separate from firmware/ (the shipping bridge). Same board, same ESP-IDF.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gps_sniffer)
