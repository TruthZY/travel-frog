#include "nvs_flash.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "tf_core.h"
#include "tf_port.h"
#include "tf_ui.h"

extern void tf_sleep_note_wake(void);

static tf_save_t s_save;

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Light the panel before anything slow: after a deep-sleep touch wake somebody is
       standing at the desk, and the display stack takes most of a second. */
    const gpio_config_t bl = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << BSP_LCD_BACKLIGHT };
    gpio_config(&bl);
    gpio_set_level(BSP_LCD_BACKLIGHT, 1);

    /* Has to run while the reset cause is still readable, and it decides what sleep mode
       the next blank will use. */
    tf_sleep_note_wake();

    lv_display_t *disp = bsp_display_start();
    ESP_ERROR_CHECK(disp == NULL ? ESP_FAIL : ESP_OK);
    bsp_display_backlight_on();
    tf_port_audio_init();

    if (!tf_port_load(&s_save)) {
        tf_core_init(&s_save);
        tf_port_store(&s_save);
    }

    bsp_display_lock(-1);
    tf_ui_create(lv_screen_active(), &s_save);
    bsp_display_unlock();
}
