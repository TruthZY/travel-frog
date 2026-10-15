/* M3b/M3c sleep. The board is a pet that sits on a desk, so "asleep" has to mean the panel
   is dark and the draw is gone, while the trip keeps running: every deadline in the save is
   an absolute timestamp on the PCF85063A, so nothing is computed while sleeping and nothing
   is settled on a timer. The next tap reboots the chip and tf_core_tick() brings the frog
   home.

   A tap is the only thing that can *show* the result, which is no use to a player who is in
   another room, so the alarm half of this file wakes the chip at the trip's end, flashes the
   panel and goes straight back to sleep without ever drawing or settling anything.

   From the Rev1.1 schematic: the CST816S INT is GPIO4 (BSP_LCD_TOUCH_INT) and the
   PCF85063A INT is GPIO6, both through 0R links. GPIO0-21 are RTC IOs on the S3, so both can
   wake deep sleep through EXT1 — a digital-only pin could not. LED1/LED2 hang off the charger
   circuit rather than a GPIO, so the only light that can be blinked is the backlight on GPIO5.

   Those INT links cannot be proven from software, so behaviour decides. Two tiers:
   0 = deep sleep woken by the touch pin, 1 = panel dark but the chip keeps polling touch
   over I2C. Tier 0 costs the battery nothing and tier 1 costs nothing but battery, so the
   degradation is safe in both directions — and it has to be reversible, because tier 1
   can never gather the evidence that would vindicate tier 0. It re-probes on a countdown
   instead of latching; see degrade_tier(). The alarm reminder only exists in tier 0. */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs.h"
#include "bsp/esp-bsp.h"
#include "pcf85063a.h"
#include "tf_core.h"
#include "tf_port.h"

#define TOUCH_INT    BSP_LCD_TOUCH_INT
#define RTC_INT      GPIO_NUM_6
#define WAKE_MASK    ((1ULL << TOUCH_INT) | (1ULL << RTC_INT))

#define NVS_PWR      "pwr"
#define K_TIER       "tier"    /* u32: 0 = deep sleep on INT, 1 = dark panel, chip awake */
#define K_FAST       "fast"    /* u32: EXT1 wakes that landed right after falling asleep */
#define K_SLEPT      "slept"   /* u32: game clock in seconds when we went to sleep, 0 = awake */
#define K_RETRY      "retry"   /* u32: sleeps left before a degraded tier re-probes tier 0 */

#define FAST_WAKE_S  2                       /* an EXT1 wake this soon after sleeping is bogus */
#define POKED_S      (5 * 60)                /* a power cycle that cut a short sleep: they poked it */
#define GIVE_UP      3
#define TIER_PROBE_SLEEPS  20                /* how long a degraded tier is believed for */
#define CTRL2        0x01                    /* PCF85063A Control_2 */
#define AF           0x40                    /* alarm flag, held until cleared */
#define AIE          0x80                    /* alarm interrupt enable */
#define BLINK_ON_MS  90
#define BLINK_OFF_MS 110

static const char *TAG = "tf_sleep";
static tf_wake_t s_cause = TF_WAKE_POWER_ON;
static bool s_noted;

/* ---------- the RTC, which tf_port_esp.c opened for the clock ---------- */

extern bool tf_rtc_available(void);
extern pcf85063a_dev_t *tf_rtc_handle(void);

static void rtc_clear_alarm(void)
{
    pcf85063a_dev_t *rtc = tf_rtc_handle();
    uint8_t buf[2];
    if (!rtc || pcf85063a_read_register(rtc, CTRL2, &buf[1], 1) != ESP_OK) return;
    buf[0] = CTRL2;
    buf[1] |= AF;          /* writing 1 clears the flag */
    pcf85063a_write_register(rtc, buf, 2);
}

static bool rtc_alarm_pending(void)
{
    pcf85063a_dev_t *rtc = tf_rtc_handle();
    uint8_t v = 0;
    if (!rtc || pcf85063a_read_register(rtc, CTRL2, &v, 1) != ESP_OK) return false;
    return (v & AF) != 0;
}

/* One-shot: the driver's own setter leaves day and weekday masked, so the alarm can only
   ever fire on the next matching time of day. */
static bool rtc_arm_alarm(int64_t wall_ms)
{
    pcf85063a_dev_t *rtc = tf_rtc_handle();
    if (!rtc) return false;
    const int64_t sec = wall_ms / 1000;
    const pcf85063a_datetime_t t = {
        .hour = (uint8_t)((sec / 3600) % 24),
        .min  = (uint8_t)((sec / 60) % 60),
        .sec  = (uint8_t)(sec % 60),
    };
    return pcf85063a_set_alarm(rtc, t) == ESP_OK && pcf85063a_enable_alarm(rtc) == ESP_OK;
}

static bool rtc_arm_clock(void)
{
    pcf85063a_dev_t *rtc = tf_rtc_handle();
    pcf85063a_datetime_t t;
    return rtc && pcf85063a_get_time_date(rtc, &t) == ESP_OK;
}

/* Seconds from the RTC's own time-of-day to the next arrival of `want`. */
static int64_t seconds_until(int hour, int min, int sec)
{
    pcf85063a_dev_t *rtc = tf_rtc_handle();
    pcf85063a_datetime_t t;
    if (!rtc || pcf85063a_get_time_date(rtc, &t) != ESP_OK) return -1;
    int64_t delta = (hour * 3600 + min * 60 + sec) - (t.hour * 3600 + t.min * 60 + t.sec);
    if (delta < 0) delta += 86400;
    return delta;
}

/* ---------- NVS tier bookkeeping ---------- */

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

/* Degrading is a hypothesis about the INT lines, not a verdict, and it cannot be confirmed
   from inside tier 1: the chip never deep sleeps there, so it never gets the EXT1 wake that
   would prove tier 0 works. Meanwhile a wet finger, an ESD kick on the 0R link or a single
   failed I2C read in rtc_alarm_pending() all look exactly like a broken INT — so a degraded
   tier counts sleeps down and then tries deep sleep again rather than staying hot forever. */
static void degrade_tier(const char *why)
{
    pwr_set(K_TIER, 1);
    pwr_set(K_FAST, 0);
    pwr_set(K_RETRY, TIER_PROBE_SLEEPS);
    ESP_LOGW(TAG, "%s -> dark panel, chip stays awake (re-probing in %d sleeps)",
             why, TIER_PROBE_SLEEPS);
}

/* Called on the way into every sleep, so a board that only sleeps once a day still gets
   its re-probe. A missing K_RETRY means an older build degraded us: arm it on sight. */
static void tier_count_down(void)
{
    if (pwr_get(K_TIER, 0) != 1) return;
    uint32_t left = pwr_get(K_RETRY, 0);
    if (left == 0) left = TIER_PROBE_SLEEPS;
    if (--left) {
        pwr_set(K_RETRY, left);
        return;
    }
    pwr_set(K_RETRY, 0);
    pwr_set(K_TIER, 0);
    pwr_set(K_FAST, 0);
    ESP_LOGI(TAG, "tier 1 got nothing conclusive for %d sleeps, re-probing deep sleep",
             TIER_PROBE_SLEEPS);
}

/* ---------- wake side ---------- */

void tf_sleep_note_wake(void)
{
    const esp_sleep_source_t src = esp_sleep_get_wakeup_cause();
    const uint32_t slept = pwr_get(K_SLEPT, 0);
    const int64_t delta = slept ? tf_port_now_ms() / 1000 - (int64_t)slept : -1;
    s_noted = true;

    /* The RTC alarm pin is open-drain and holds the line until AF is cleared, so an EXT1
       wake can be told apart from a finger by reading the flag over I2C. */
    const bool rang = src == ESP_SLEEP_WAKEUP_EXT1 && rtc_alarm_pending();

    s_cause = rang ? TF_WAKE_ALARM
            : src == ESP_SLEEP_WAKEUP_EXT1 ? TF_WAKE_TOUCH : TF_WAKE_POWER_ON;
    pwr_set(K_SLEPT, 0);

    if (rang) {
        rtc_clear_alarm();
    } else if (src == ESP_SLEEP_WAKEUP_EXT1) {
        /* The line idles low or chatters: ANY_LOW would reboot us forever. */
        const uint32_t fast = (delta >= 0 && delta <= FAST_WAKE_S) ? pwr_get(K_FAST, 0) + 1 : 0;
        pwr_set(K_FAST, fast);
        if (fast >= GIVE_UP) degrade_tier("EXT1 wakes the instant we sleep");
    } else {
        /* Woken by the PWR key or a replug *out of a sleep that had barely started*: most
           likely the player tapped a dark panel, got nothing and reached for the button.
           A negative delta means the wall clock restarted, so no conclusion.
           Only tier 0 can supply that evidence — in tier 1 the chip never slept, so a power
           cycle is just a power cycle and counting it would push the re-probe out forever. */
        const uint32_t fast = (pwr_get(K_TIER, 0) == 0 && delta >= 0 && delta < POKED_S)
                              ? pwr_get(K_FAST, 0) + 1 : 0;
        pwr_set(K_FAST, fast);
        if (fast >= GIVE_UP) degrade_tier("short sleeps ended by a power cycle: INT cannot wake");
    }
    ESP_LOGI(TAG, "wake cause=%d tier=%u", (int)s_cause, (unsigned)pwr_get(K_TIER, 0));
}

tf_wake_t tf_port_wake_cause(void)
{
    if (!s_noted) tf_sleep_note_wake();
    return s_cause;
}

/* ---------- light show ---------- */

void tf_port_blink(int times)
{
    if (times < 1) return;
    for (int i = 0; i < times; i++) {
        bsp_display_brightness_set(0);
        vTaskDelay(pdMS_TO_TICKS(BLINK_ON_MS));
        bsp_display_brightness_set(100);
        vTaskDelay(pdMS_TO_TICKS(BLINK_OFF_MS));
    }
}

/* A cold boot out of deep sleep gets this far before LVGL exists, so it must not fall
   through into the normal UI: that would light the panel for a player who never asked,
   and the whole point is a flash you can notice from the next room. */
void tf_sleep_announce_arrival(const tf_save_t *game)
{
    tf_port_blink(6);
    /* The alarm is a time-of-day match, so it can only ever fire once; re-arming it would
       blink again tomorrow at the same clock time, for a frog that is long home. */
    (void)game;
    esp_deep_sleep_start();
}

/* ---------- sleep side ---------- */

/* Both INT lines as RTC inputs with their pull-ups, so their levels survive deep sleep.
   Returns false when a line cannot carry an active-low wake, which is the only thing
   software can actually measure about them. */
static bool ext1_arm(uint64_t mask)
{
    static const int pins[] = { TOUCH_INT, RTC_INT };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if (!(mask & (1ULL << pins[i]))) continue;
        if (rtc_gpio_init(pins[i]) != ESP_OK) {
            ESP_LOGW(TAG, "GPIO%d is not usable as an RTC input", pins[i]);
            return false;
        }
        ESP_ERROR_CHECK(rtc_gpio_set_direction(pins[i], RTC_GPIO_MODE_INPUT_ONLY));
        ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(pins[i]));
        ESP_ERROR_CHECK(rtc_gpio_pullup_en(pins[i]));
        if (rtc_gpio_get_level(pins[i]) != 1) {
            ESP_LOGW(TAG, "GPIO%d idles low, ANY_LOW would wake us at once", pins[i]);
            return false;
        }
    }
    return esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW) == ESP_OK;
}

/* Hand the pads back to the digital GPIO mux so the drivers keep working after a sleep
   that returned instead of a reboot. */
static void ext1_disarm(uint64_t mask)
{
    static const int pins[] = { TOUCH_INT, RTC_INT };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if (!(mask & (1ULL << pins[i]))) continue;
        rtc_gpio_deinit(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        gpio_pullup_en(pins[i]);
    }
}

/* Nothing may be drawn after this: on the board the chip is simply gone. */
static void to_sleep(void)
{
    pwr_set(K_SLEPT, (uint32_t)(tf_port_now_ms() / 1000));
    /* The veil already covers the UI; killing the lamp is what the player actually sees. */
    bsp_display_backlight_off();

    tier_count_down();

    if (pwr_get(K_TIER, 0) == 0 && ext1_arm(WAKE_MASK)) {
        esp_deep_sleep_start();
        ESP_LOGE(TAG, "deep sleep did not take");
        ext1_disarm(WAKE_MASK);
        return;
    }
    if (pwr_get(K_TIER, 0) == 0) degrade_tier("GPIO cannot carry an EXT1 wake");
    /* Tier 1: nothing to do. on_tick stays parked behind the veil and the indev keeps
       reading over I2C, so wake_cb fires on the next tap. */
}

void tf_port_sleep(void)
{
    if (!s_noted) tf_sleep_note_wake();
    rtc_clear_alarm();
    to_sleep();
}

bool tf_port_sleep_until(int64_t at_ms)
{
    if (!s_noted) tf_sleep_note_wake();
    if (pwr_get(K_TIER, 0) != 0 || !tf_rtc_available() || !rtc_arm_clock()) return false;
    if (!rtc_arm_alarm(at_ms)) {
        ESP_LOGW(TAG, "alarm at %lld ms did not take", (long long)at_ms);
        return false;
    }
    /* The alarm is a time-of-day match, so it really fires this far ahead — worth logging,
       because a wall clock that was never set makes it land somewhere arbitrary. */
    ESP_LOGI(TAG, "sleeping until %lld ms, alarm in %lld s",
             (long long)at_ms,
             (long long)seconds_until((int)((at_ms / 1000 / 3600) % 24),
                                      (int)((at_ms / 1000 / 60) % 60), (int)((at_ms / 1000) % 60)));
    to_sleep();
    return true;   /* only reached when the deep sleep refused */
}
