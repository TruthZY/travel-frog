idf_component_register(
    SRCS
        app_main.c
        tf_port_esp.c
        tf_sleep_esp.c
        tf_snd_esp.c
        tf_assets_gen.c
        ../../shared/core/tf_core.c
        ../../shared/core/tf_persist.c
        ../../shared/core/tf_sfx.c
        ../../shared/ui/tf_ui.c
        ../../shared/ui/tf_font_cn_16.c
        ../../shared/ui/tf_font_cn_20.c
    INCLUDE_DIRS
        .
        ../../shared/core
        ../../shared/ui
    REQUIRES
        nvs_flash
        driver
        waveshare__esp32_s3_touch_lcd_1_85B
        waveshare__pcf85063a
        bsp_extra
)

# lv_font_conv emits `#include "lvgl/lvgl.h"` unless this is defined;
# the IDF component layout only exposes "lvgl.h".
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)

# Pre-decoded pixel art from tools/gen_fw_assets.py instead of runtime PNG decoding.
target_compile_definitions(${COMPONENT_LIB} PRIVATE TF_HAS_ASSETS)

# Sound effects go to the 2MB "storage" partition (mounted at /spiffs by the BSP),
# so the app image stays free of audio and idf.py flash writes them in one go.
spiffs_create_partition_image(storage ../../assets/audio FLASH_IN_PROJECT)

# Temporary M3b bring-up rig: prints the idle countdown so the sleep path can be
# watched over UART without touching the board. Remove with the debug block in tf_ui.c.
target_compile_definitions(${COMPONENT_LIB} PRIVATE TF_DEBUG_IDLE)
