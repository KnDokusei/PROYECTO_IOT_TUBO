cmake_minimum_required(VERSION 3.16)

# Shared components live one level up and are reused by all four modules.
set(EXTRA_COMPONENT_DIRS "../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ec_cameras)
