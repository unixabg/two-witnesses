# the CSI sensing module (docs/MODULES.md, module 1): its sources built when CONFIG_TW_MOD_CSI is on.
# Requirements must not depend on the configuration (ESP-IDF settles them before it reads it):
# only the sources do
set(srcs)
if(CONFIG_TW_MOD_CSI)
    set(srcs "csi_sense.c" "tw_mod_csi.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../firmware/tw_node/main"
                       PRIV_REQUIRES nvs_flash log)
