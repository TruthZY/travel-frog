/* M3b sleep. The board is a pet that sits on a desk, so "asleep" has to mean the panel
   is dark and the draw is gone, while the trip keeps running: every deadline in the save is
   an absolute timestamp on the PCF85063A, so nothing is computed while sleeping and nothing
   is settled on a timer. The next tap reboots the chip and tf_core_tick() brings the frog
   home.

   There is deliberately no "the frog is home" reminder. A tap is the only thing that can
   *show* the result, and waking the chip to pulse the backlight only reaches someone who is
   already looking at the desk — which is not worth an alarm path plus its own wake case.
   The trip settles the next time the player touches the glass.

   From the Rev1.1 schematic: the CST816S INT is GPIO4 (BSP_LCD_TOUCH_INT) and the
   PCF85063A INT is GPIO6, both through 0R links. GPIO0-21 are RTC IOs on the S3, so both can
   wake deep sleep through EXT1 — a digital-only pin could not. LED1/LED2 hang off the charger
   circuit rather than a GPIO, so the only light that can be blinked is the backlight on GPIO5.

   Those INT links cannot be proven from software, so behaviour decides. Two tiers:
   0 = deep sleep woken by the touch pin, 1 = panel dark but the chip keeps polling touch
   over I2C. Tier 0 costs the battery nothing and tier 1 costs nothing but battery, so the
   degradation is safe in both directions — and it has to be reversible, because tier 1
   can never gather the evidence that would vindicate tier 0. It re-probes on a countdown
   instead of latching; see degrade_tier(). */

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
#define C2_TF        0x08                    /* timer flag, the other thing that asserts INT */
#define TMODE        0x11                    /* PCF85063A Timer_mode */
#define ALEN_REG     0x0B                    /* the five alarm fields, second..weekday */
#define ALEN         0x80                    /* set in a field = that field never matches */
#define BLINK_ON_MS  90
#define BLINK_OFF_MS 110

static const char *TAG = "tf_sleep";
static bool s_noted;

/* ---------- the RTC, which tf_port_esp.c opened for the clock ---------- */

extern pcf85063a_dev_t *tf_rtc_handle(void);
extern void tf_snd_quiesce(void);
extern void tf_snd_resume(void);

/* The INT pin is open-drain and stays asserted for as long as any flag is set, so a stale
   AF or TF holds it low forever — and ext1_arm() then reads a working wake link as a broken
   one and drops the board to tier 1 for good. Measured on hardware: ctrl2=0xc0 (AIE|AF) held
   GPIO6 low, and the same pad read high the instant those flags were cleared. Clearing AF
   alone is not enough; the timer flag and the timer itself assert the same pin.

   The alarm registers are masked too, and that is the half that cannot be done by clearing
   flags: PCF85063A raises AF whenever the clock matches the alarm fields, whether or not
   AIE is on, so an alarm left over from an old test keeps re-flagging every day at that
   time of day, and every one of those matches pulls INT low again behind our back. Masking
   every field leaves AF as a flag that is only ever set by something we asked for. */
static void rtc_quiesce(void)
{
    pcf85063a_dev_t *rtc = tf_rtc_handle();
    if (!rtc) return;
    uint8_t c2 = 0;
    if (pcf85063a_read_register(rtc, CTRL2, &c2, 1) == ESP_OK) {
        uint8_t buf[2] = { CTRL2, (uint8_t)((c2 | AF | C2_TF) & ~AIE) };
        pcf85063a_write_register(rtc, buf, 2);
    }
    uint8_t stop[2] = { TMODE, 0x00 };
    pcf85063a_write_register(rtc, stop, 2);
    uint8_t mask[6] = { ALEN_REG, ALEN, ALEN, ALEN, ALEN, ALEN };
    pcf85063a_write_register(rtc, mask, sizeof mask);
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
   would prove tier 0 works. Meanwhile a wet finger or an ESD kick on the 0R link looks
   exactly like a broken INT — and so did the stale RTC alarm flag that held GPIO6 low, which
   is what pinned this board to tier 1 to begin with. So a degraded tier counts sleeps down
   and then tries deep sleep again rather than staying hot forever. */
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

    pwr_set(K_SLEPT, 0);

    /* An EXT1 wake can only be a finger now. rtc_quiesce() runs before every sleep and
       masks all five alarm fields plus stops the timer, so the RTC has nothing left that
       could assert the INT line it shares with the touch controller. */
    if (src == ESP_SLEEP_WAKEUP_EXT1) {
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
    ESP_LOGI(TAG, "woke by %s, tier=%u",
             src == ESP_SLEEP_WAKEUP_EXT1 ? "EXT1" : "power-on",
             (unsigned)pwr_get(K_TIER, 0));
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

/* ---------- sleep side ---------- */

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

/* Both INT lines as RTC inputs with their pull-ups, so their levels survive deep sleep.
   Returns false when a line cannot carry an active-low wake, which is the only thing
   software can actually measure about them.

   The RTC's pad is checked first, on purpose: rtc_gpio_init() takes a pin away from the
   digital mux, and the touch driver's wake interrupt lives on the digital side. Measuring
   GPIO4 before GPIO6 and then bailing out on GPIO6 left GPIO4 orphaned — tier 1 was then
   "panel dark, chip awake, taps ignored", and only the power key could bring it back. */
static bool ext1_arm(uint64_t mask)
{
    static const int pins[] = { RTC_INT, TOUCH_INT };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        const int pin = pins[i];
        bool broken = false;
        if (!(mask & (1ULL << pin))) continue;
        if (rtc_gpio_init(pin) != ESP_OK) {
            ESP_LOGW(TAG, "GPIO%d is not usable as an RTC input", pin);
            broken = true;
        } else {
            ESP_ERROR_CHECK(rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY));
            ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(pin));
            ESP_ERROR_CHECK(rtc_gpio_pullup_en(pin));
            if (rtc_gpio_get_level(pin) != 1) {
                ESP_LOGW(TAG, "GPIO%d idles low, ANY_LOW would wake us at once", pin);
                broken = true;
            }
        }
        if (!broken) continue;
        /* Only the touch pad needs handing back; if it was the RTC we never touched it. */
        if (pin == TOUCH_INT) ext1_disarm(mask);
        return false;
    }
    if (esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW) != ESP_OK) {
        ext1_disarm(mask);
        return false;
    }
    return true;
}

/* Nothing may be drawn after this: on the board the chip is simply gone. */
static void to_sleep(void)
{
    pwr_set(K_SLEPT, (uint32_t)(tf_port_now_ms() / 1000));
    /* The veil already covers the UI; killing the lamp is what the player actually sees. */
    bsp_display_backlight_off();
    /* A deep sleep takes the I2S clocks down with it, but the tier-1 fallback below does
       not, so drop the codec here either way — everything left clocking is pure heat. */
    tf_snd_quiesce();

    tier_count_down();

    if (pwr_get(K_TIER, 0) == 0 && ext1_arm(WAKE_MASK)) {
        esp_deep_sleep_start();
        ESP_LOGE(TAG, "deep sleep did not take");
        ext1_disarm(WAKE_MASK);
        return;
    }
    if (pwr_get(K_TIER, 0) == 0) degrade_tier("GPIO cannot carry an EXT1 wake");
    /* Tier 1: nothing more to do. on_tick stays parked behind the veil and the indev keeps
       reading over I2C, so wake_cb fires on the next tap. */
}

/* Only the tier-1 fallback reaches this: a deep-sleep wake is a reboot, and app_main lights
   the panel from the top. Without it the first tap after a tier-1 sleep brings the whole UI
   back onto a panel nobody can see, and the only cure is the power key. */
void tf_port_wake_restore(void)
{
    bsp_display_backlight_on();
    tf_snd_resume();
}

void tf_port_sleep(void)
{
    if (!s_noted) tf_sleep_note_wake();
    /* Before sleeping, not after: a flag left over from whatever once armed the alarm holds
       INT low, and ext1_arm() reads that as a broken wake link and drops us out of tier 0 —
       which is exactly how this board got stuck behind a dark panel with the chip at full
       speed. */
    rtc_quiesce();
    to_sleep();
}
