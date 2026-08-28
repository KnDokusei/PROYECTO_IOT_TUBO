cmake_minimum_required(VERSION 3.16)

# Components shared with the other Kundt modules live one level up, so the same
# WiFi/NVS code can be reused when E2, E3 and EC are migrated.
set(EXTRA_COMPONENT_DIRS "../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(e1_mic)
