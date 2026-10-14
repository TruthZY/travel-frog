#include "nvs_flash.h"
#include "esp_err.h"
#include "bsp/esp-bsp.h"
#include "tf_core.h"
#include "tf_port.h"
#include "tf_ui.h"

static tf_save_t s_save;

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    lv_display_t *disp = bsp_display_start();
    ESP_ERROR_CHECK(disp == NULL ? ESP_FAIL : ESP_OK);
    bsp_display_backlight_on();

    if (!tf_port_load(&s_save)) {
        tf_core_init(&s_save);
        tf_port_store(&s_save);
    }

    bsp_display_lock(-1);
    tf_ui_create(lv_screen_active(), &s_save);
    bsp_display_unlock();
}
