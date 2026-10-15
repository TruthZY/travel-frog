/* M3b sleep. The board is a pet that sits on a desk, so "asleep" has to mean the panel is
   dark and the draw is gone, while the trip keeps running: every deadline in the save is an
   absolute timestamp on the PCF85063A, so nothing is computed while sleeping and nothing is
   settled on a timer. The next tap reboots the chip and tf_core_tick() brings the frog home.

   From the Rev1.1 schematic: the CST816S INT is GPIO4 (BSP_LCD_TOUCH_INT). GPIO0-21 are RTC
   IOs on the S3, so GPIO4 can wake deep sleep through EXT1 — a digital-only pin could not.
   LED1/LED2 hang off the charger circuit rather than a GPIO, so the only light that can be
   blinked is the backlight on GPIO5.

   That INT reaches the SoC through a 0R link, so software cannot prove it is wired. Two
   tiers, chosen by observed behaviour: 0 = deep sleep woken by the touch pin,
   1 = panel dark but the chip keeps polling touch over I2C. Tier 0 costs the battery nothing
   and tier 1 costs nothing but battery, so the degradation is safe in both directions. */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs.h"
#include "bsp/esp-bsp.h"
#include "tf_port.h"

#define TOUCH_INT    BSP_LCD_TOUCH_INT
#define NVS_PWR      "pwr"
#define K_TIER       "tier"    /* u32: 0 = deep sleep on INT, 1 = dark panel, chip awake */
#define K_FAST       "fast"    /* u32: EXT1 wakes that landed right after falling asleep */
#define K_SLEPT      "slept"   /* u32: game clock in seconds when we went to sleep, 0 = awake */

#define FAST_WAKE_S  2                       /* an EXT1 wake this soon after sleeping is bogus */
#define POKED_S      (5 * 60)                /* a power cycle that cut a short sleep: they poked it */
#define GIVE_UP      3

static const char *TAG = "tf_sleep";
static tf_wake_t s_cause = TF_WAKE_POWER_ON;
static bool s_noted;

static uint32_t pwr_get(const char *key, uint32_t missing)
{
    nvs_handle_t h;
    if (nvs_open(NVS_PWR, NVS_READONLY, &h) != ESP_OK) return missing;
    uint32_t v = missing;
    if (nvs_get_u32(h, key, &v) != ESP_OK) v = missing;
    nvs_close(h);
    return v;
}

static void pwr_set(const char *key, uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_PWR, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u32(h, key, v) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void give_up_deep_sleep(const char *why)
{
    pwr_set(K_TIER, 1);
    pwr_set(K_FAST, 0);
    ESP_LOGW(TAG, "%s -> dark panel, chip stays awake", why);
}

/* ---------- wake side ---------- */

void tf_sleep_note_wake(void)
{
    const esp_sleep_source_t src = esp_sleep_get_wakeup_cause();
    const uint32_t slept = pwr_get(K_SLEPT, 0);
    const int64_t delta = slept ? tf_port_now_ms() / 1000 - (int64_t)slept : -1;
    s_noted = true;

    if (src == ESP_SLEEP_WAKEUP_EXT1) {
        s_cause = TF_WAKE_TOUCH;
        /* The line idles low or chatters: ANY_LOW would reboot us forever. */
        const uint32_t fast = (delta >= 0 && delta <= FAST_WAKE_S) ? pwr_get(K_FAST, 0) + 1 : 0;
        pwr_set(K_FAST, fast);
        if (fast >= GIVE_UP) give_up_deep_sleep("EXT1 wakes the instant we sleep");
    } else {
        /* Woken by the PWR key or a replug *out of a sleep that had barely started*: most
           likely the player tapped a dark panel, got nothing and reached for the button.
           A negative delta means the wall clock restarted, so no conclusion. */
        s_cause = TF_WAKE_POWER_ON;
        const uint32_t fast = (delta >= 0 && delta < POKED_S) ? pwr_get(K_FAST, 0) + 1 : 0;
        pwr_set(K_FAST, fast);
        if (fast >= GIVE_UP) give_up_deep_sleep("short sleeps ended by a power cycle: INT cannot wake");
    }
    pwr_set(K_SLEPT, 0);
    ESP_LOGI(TAG, "wake cause=%d tier=%u", (int)src, (unsigned)pwr_get(K_TIER, 0));
}

tf_wake_t tf_port_wake_cause(void)
{
    if (!s_noted) tf_sleep_note_wake();
    return s_cause;
}

/* ---------- sleep side ---------- */

/* GPIO4 as an RTC input with its pull-up, so the level survives deep sleep. Returns false
   when the line cannot carry an active-low touch wake, which is the only thing software can
   actually measure about it. */
static bool ext1_arm(void)
{
    if (rtc_gpio_init(TOUCH_INT) != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d is not usable as an RTC input", TOUCH_INT);
        return false;
    }
    ESP_ERROR_CHECK(rtc_gpio_set_direction(TOUCH_INT, RTC_GPIO_MODE_INPUT_ONLY));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(TOUCH_INT));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(TOUCH_INT));
    if (rtc_gpio_get_level(TOUCH_INT) != 1) {
        ESP_LOGW(TAG, "touch INT idles low, ANY_LOW would wake us at once");
        return false;
    }
    return esp_sleep_enable_ext1_wakeup(1ULL << TOUCH_INT, ESP_EXT1_WAKEUP_ANY_LOW) == ESP_OK;
}

/* Hand the pad back to the digital GPIO mux so the touch driver keeps working after a sleep
   that returned instead of a reboot. */
static void ext1_disarm(void)
{
    rtc_gpio_deinit(TOUCH_INT);
    gpio_set_direction(TOUCH_INT, GPIO_MODE_INPUT);
    gpio_pullup_en(TOUCH_INT);
}

void tf_port_sleep(void)
{
    if (!s_noted) tf_sleep_note_wake();
    pwr_set(K_SLEPT, (uint32_t)(tf_port_now_ms() / 1000));

    /* The veil already covers the UI; killing the lamp is what the player actually sees.
       With the chip off, R22 holds the backlight gate down, so a reboot wakes dark too. */
    bsp_display_backlight_off();

    if (pwr_get(K_TIER, 0) == 0 && ext1_arm()) {
        ESP_LOGI(TAG, "deep sleep, touch on GPIO%d", TOUCH_INT);
        esp_deep_sleep_start();
        ESP_LOGE(TAG, "deep sleep did not take");
        ext1_disarm();
        return;
    }

    if (pwr_get(K_TIER, 0) == 0) pwr_set(K_TIER, 1);
    /* Tier 1: nothing to do. on_tick stays parked behind the veil and the indev keeps
       reading over I2C, so wake_cb fires on the next tap. */
}

void tf_port_blink(int times)
{
    if (times < 1) return;
    for (int i = 0; i < times; i++) {
        bsp_display_brightness_set(0);
        vTaskDelay(pdMS_TO_TICKS(90));
        bsp_display_brightness_set(100);
        vTaskDelay(pdMS_TO_TICKS(110));
    }
}
