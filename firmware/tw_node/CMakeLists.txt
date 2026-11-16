# tw_node: two-witnesses many-board firmware (ESP32-S3, ESP-IDF v5.5.6)
cmake_minimum_required(VERSION 3.16)
# modules (docs/MODULES.md): each modules/<name>/tw_mod_<name>/ is an ESP-IDF component, found here;
# its Kconfig decides whether it is built (make menuconfig). None yet: the core alone
file(GLOB TW_MODULE_COMPONENTS LIST_DIRECTORIES true "${CMAKE_CURRENT_LIST_DIR}/../../modules/*/tw_mod_*")
set(EXTRA_COMPONENT_DIRS ${TW_MODULE_COMPONENTS})
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tw_node)
