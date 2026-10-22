cmake_minimum_required(VERSION 3.16)

# The wired-link protocol is shared verbatim with the Windows host.
set(EXTRA_COMPONENT_DIRS "")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hikari_bridge)
