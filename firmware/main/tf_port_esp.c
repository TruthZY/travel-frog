#include <stdio.h>
#include <string.h>
#include "tf_port.h"
#include "tf_persist.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "pcf85063a.h"

/* The PWR key is a hardware latch: cutting power gives this task no warning, so
   every write goes through tf_persist (CRC + two rotating NVS slots) and the game
   clock is anchored on the PCF85063A rather than esp_timer. A trip therefore keeps
   running while the board is off, and a torn write can never eat the older save. */

#define NVS_NS     "game"
#define NVS_KEY    "save%d"
#define RTC_DAY_MS 86400000LL

/* Waveshare wires PCF85063A to the shared BSP I2C bus. */
#define RTC_BUS_ADDR PCF85063A_ADDRESS

static const char *TAG = "tf_port";
static tf_save_meta_t s_meta;

static pcf85063a_dev_t s_rtc;
static bool s_rtc_ready;
static int64_t s_base_ms;      /* game clock when the clock was anchored */
static int64_t s_base_us;

/* ---------- civil calendar <-> epoch (Howard Hinnant's algorithms) ---------- */

static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t days, int *y, unsigned *m, unsigned *d, unsigned *dotw)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    unsigned month = mp + (mp < 10 ? 3 : -9);
    *d = doy - (153 * mp + 2) / 5 + 1;
    *y = (int)(yoe + era * 400) + (month <= 2);
    *m = month;
    *dotw = (unsigned)((days - 3) % 7 + 7) % 7;   /* 1970-01-01 was a Thursday */
}

static int64_t rtc_read_ms(void)
{
    pcf85063a_datetime_t t;
    if (!s_rtc_ready) return -1;
    if (pcf85063a_get_time_date(&s_rtc, &t) != ESP_OK) return -1;
    if (t.year < 2020 || t.year > 2099 || t.month < 1 || t.month > 12 ||
        t.day < 1 || t.day > 31 || t.hour > 23 || t.min > 59 || t.sec > 59)
        return -1;                                    /* backup cap died, or never set */
    return days_from_civil(t.year, t.month, t.day) * RTC_DAY_MS
           + (t.hour * 3600 + t.min * 60 + t.sec) * 1000LL;
}

static void rtc_write_ms(int64_t ms)
{
    if (!s_rtc_ready) return;
    int y;
    unsigned m, d, w;
    civil_from_days(ms / RTC_DAY_MS, &y, &m, &d, &w);
    const int64_t sec = ms / 1000;
    const pcf85063a_datetime_t t = {
        .year = (uint16_t)y, .month = (uint8_t)m, .day = (uint8_t)d, .dotw = (uint8_t)w,
        .hour = (uint8_t)((sec / 3600) % 24), .min = (uint8_t)((sec / 60) % 60),
        .sec = (uint8_t)(sec % 60),
    };
    if (pcf85063a_set_time_date(&s_rtc, t) != ESP_OK)
        ESP_LOGW(TAG, "rtc set failed");
}

/* ---------- clock ---------- */

static void clock_init(void)
{
    static bool done;
    if (done) return;
    done = true;

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    s_rtc_ready = (bus && pcf85063a_init(&s_rtc, bus, RTC_BUS_ADDR) == ESP_OK);
    if (!s_rtc_ready) ESP_LOGW(TAG, "rtc unavailable, using boot-relative clock");

    int64_t ms = s_rtc_ready ? rtc_read_ms() : -1;
    if (ms < 0) {
        /* No usable wall clock: start from 2020-01-01 so absolute deadlines in the
           save stay in the past, then push the RTC there for the next boot. */
        ms = 1577836800000LL;
        rtc_write_ms(ms);
    }
    s_base_ms = ms;
    s_base_us = esp_timer_get_time();
    ESP_LOGI(TAG, "clock at %lld ms (rtc %s)", (long long)ms, s_rtc_ready ? "ok" : "off");
}

int64_t tf_port_now_ms(void)
{
    clock_init();
    return s_base_ms + (esp_timer_get_time() - s_base_us) / 1000;
}

void tf_port_clock_floor(int64_t ms)
{
    clock_init();
    const int64_t now = tf_port_now_ms();
    if (now >= ms) return;
    s_base_ms += ms - now;
    ESP_LOGI(TAG, "clock ahead by %lld ms, resyncing rtc", (long long)(ms - now));
    rtc_write_ms(tf_port_now_ms());
}

void tf_port_speed_cycle(void) { /* real time on device */ }
int  tf_port_speed(void) { return 1; }

/* ---------- storage ---------- */

static bool slot_read(int slot, void *buf, size_t buf_sz, size_t *out_len)
{
    char key[12];
    snprintf(key, sizeof key, NVS_KEY, slot);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = buf_sz;
    const esp_err_t err = nvs_get_blob(h, key, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) return false;
    if (out_len) *out_len = len;
    return true;
}

static bool slot_write(int slot, const void *buf, size_t buf_sz)
{
    char key[12];
    snprintf(key, sizeof key, NVS_KEY, slot);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, key, buf, buf_sz);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool tf_port_load(tf_save_t *s)
{
    if (!tf_persist_load(s, &s_meta, slot_read)) {
        ESP_LOGW(TAG, "no valid save frame, starting a new frog");
        return false;
    }
    /* The save knows more about the time than a reset RTC: never run the game
       clock backwards, or a trip that was due lands in the future again. */
    tf_port_clock_floor(s_meta.saved_at_ms);
    ESP_LOGI(TAG, "save seq=%u loaded, %d clovers", s_meta.seq, (int)s->clovers);
    return true;
}

bool tf_port_store(const tf_save_t *s)
{
    s_meta.saved_at_ms = tf_port_now_ms();
    if (tf_persist_store(s, &s_meta, slot_write)) return true;
    ESP_LOGE(TAG, "save failed");
    return false;
}
