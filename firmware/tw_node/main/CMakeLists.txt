# the modules this configuration has (docs/MODULES.md): each modules/<name>/ whose Kconfig option
# CONFIG_TW_MOD_<NAME> is on. Their table, tw_modules.c, is written into the build folder
set(tw_mod_requires)
set(tw_mod_decls "")
set(tw_mod_entries "")
set(tw_mod_count 0)
file(GLOB tw_mod_ymls "${CMAKE_CURRENT_LIST_DIR}/../../../modules/*/module.yml")
list(SORT tw_mod_ymls)
foreach(yml ${tw_mod_ymls})
    get_filename_component(dir "${yml}" DIRECTORY)
    get_filename_component(name "${dir}" NAME)
    string(TOUPPER "${name}" up)
    # every module found is required (ESP-IDF settles requirements before it reads the
    # configuration); one turned off builds nothing
    list(APPEND tw_mod_requires "tw_mod_${name}")
    if(CONFIG_TW_MOD_${up})
        string(APPEND tw_mod_decls "extern const tw_module_t tw_mod_${name};\n")
        string(APPEND tw_mod_entries "    &tw_mod_${name},\n")
        math(EXPR tw_mod_count "${tw_mod_count} + 1")
    endif()
endforeach()
if(tw_mod_count EQUAL 0)
    set(tw_mod_entries "    NULL,   /* none: the core alone */\n")
endif()
set(tw_mod_c "${CMAKE_CURRENT_BINARY_DIR}/tw_modules.c")
set(tw_mod_text "/* tw_modules.c: written by main/CMakeLists.txt from the configuration (docs/MODULES.md) */\n#include \"tw_module.h\"\n\n${tw_mod_decls}\nconst tw_module_t *const tw_modules[] = {\n${tw_mod_entries}};\nconst int tw_modules_n = ${tw_mod_count};\n")
if(EXISTS "${tw_mod_c}")
    file(READ "${tw_mod_c}" tw_mod_old)
else()
    set(tw_mod_old "")
endif()
if(NOT tw_mod_old STREQUAL tw_mod_text)
    file(WRITE "${tw_mod_c}" "${tw_mod_text}")
endif()

idf_component_register(SRCS "main.c" "tw_proto.c" "tw_led.c" "${tw_mod_c}"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_timer nvs_flash esp_driver_usb_serial_jtag esp_psram esp_driver_rmt
                                app_update esp_partition esp_app_format mbedtls ${tw_mod_requires})
