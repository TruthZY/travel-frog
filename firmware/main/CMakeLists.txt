idf_component_register(
    SRCS
        app_main.c
        tf_port_esp.c
        tf_assets_gen.c
        ../../shared/core/tf_core.c
        ../../shared/core/tf_persist.c
        ../../shared/ui/tf_ui.c
        ../../shared/ui/tf_font_cn_16.c
        ../../shared/ui/tf_font_cn_20.c
    INCLUDE_DIRS
        .
        ../../shared/core
        ../../shared/ui
    REQUIRES
        nvs_flash
        waveshare__esp32_s3_touch_lcd_1_85B
        waveshare__pcf85063a
        bsp_extra
)

# lv_font_conv emits `#include "lvgl/lvgl.h"` unless this is defined;
# the IDF component layout only exposes "lvgl.h".
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)

# Pre-decoded pixel art from tools/gen_fw_assets.py instead of runtime PNG decoding.
target_compile_definitions(${COMPONENT_LIB} PRIVATE TF_HAS_ASSETS)
