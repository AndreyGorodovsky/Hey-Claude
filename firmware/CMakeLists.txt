# Top-level build file for the device firmware. ESP-IDF projects are built
# with CMake; `idf.py build` runs it.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the ESP-IDF components that this project's components actually
# list in their REQUIRES / PRIV_REQUIRES, rather than all of ESP-IDF.
# A consequence: a feature whose component nothing requires is silently left
# out, even if sdkconfig.defaults enables it (the core dump needed this).
idf_build_set_property(MINIMAL_BUILD ON)
project(hey_claude)
