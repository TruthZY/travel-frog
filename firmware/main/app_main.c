#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "tf_core.h"
#include "tf_port.h"
#include "tf_ui.h"

extern void tf_sleep_note_wake(void);

static const char *TAG = "app";

static tf_save_t s_save;

/* Let idle time become light sleep instead of the CPU spinning at 240 MHz; sdkconfig.defaults
   explains why that is safe with this PSRAM setup. DFS holds the frequency at max while a
   task is actually running, so rendering is unaffected — only the gaps between frames shrink. */
static void power_management_init(void)
{
    const esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        .light_sleep_enable = true,
    };
    const esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "power management not applied: %s", esp_err_to_name(err));
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    power_management_init();

    /* Light the panel before anything slow: after a deep-sleep touch wake somebody is
       standing at the desk, and the display stack takes most of a second. */
    const gpio_config_t bl = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << BSP_LCD_BACKLIGHT };
    gpio_config(&bl);
    gpio_set_level(BSP_LCD_BACKLIGHT, 1);

    /* Has to run while the reset cause is still readable, and it decides what sleep mode
       the next blank will use. */
    tf_sleep_note_wake();

    /* A deep-sleep wake is a cold boot, so without this every trip would replay the same
       "random" card from the top of the sequence. */
    tf_core_seed(tf_port_random_seed());

    /* Load once: both the fresh-install path below and tf_ui want the same save, and a
       second tf_port_load() is another two NVS reads for nothing. */
    const bool have_save = tf_port_load(&s_save);

    lv_display_t *disp = bsp_display_start();
    ESP_ERROR_CHECK(disp == NULL ? ESP_FAIL : ESP_OK);
    bsp_display_backlight_on();
    tf_port_audio_init();

    if (!have_save) {
        tf_core_init(&s_save);
        tf_port_store(&s_save);
    }

    bsp_display_lock(-1);
    tf_ui_create(lv_screen_active(), &s_save);
    bsp_display_unlock();
}
