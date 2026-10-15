#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bq27220.h"
#include "tf_port.h"

/* The gauge shares the RTC's I2C bus but is reached through Espressif's legacy i2c_bus
   driver, and bq27220_create() can spend seconds unsealing the device and writing the
   CEDV profile — far too slow for the LVGL task behind tf_port_battery(). So the reads
   live in a low-priority task that publishes a one-word snapshot and the getter never
   blocks. Presence is decided by create(), which checks the device number: every getter
   in the driver casts its error away and would report a silent gauge as 0%. */

#define TAG       "tf_batt"
#define POLL_MS   2000
#define RETRY_MS  30000

/* One aligned 32-bit store is atomic on Xtensa, so the snapshot needs no lock. */
#define SNAP_READY (1u << 0)
#define SNAP_CHG   (1u << 8)
#define SNAP_FULL  (1u << 9)

static volatile uint32_t s_snap;
static bool s_task_up;

/* Vendor profile for the board's 500 mAh cell, from Waveshare's 08_bq27220_test. */
static const parameter_cedv_t CEDV = {
    .full_charge_cap = 500,
    .design_cap = 500,
    .reserve_cap = 0,
    .near_full = 200,
    .self_discharge_rate = 20,
    .EDV0 = 3490,
    .EDV1 = 3511,
    .EDV2 = 3535,
    .EMF = 3670,
    .C0 = 115,
    .R0 = 968,
    .T0 = 4547,
    .R1 = 4764,
    .TC = 11,
    .C1 = 0,
    .DOD0 = 4147,
    .DOD10 = 4002,
    .DOD20 = 3969,
    .DOD30 = 3938,
    .DOD40 = 3880,
    .DOD50 = 3824,
    .DOD60 = 3794,
    .DOD70 = 3753,
    .DOD80 = 3677,
    .DOD90 = 3574,
    .DOD100 = 3490,
};

static const gauging_config_t GAUGING = {
    .CCT = 1,
    .CSYNC = 0,
    .EDV_CMP = 0,
    .SC = 1,
    .FIXED_EDV0 = 0,
    .FCC_LIM = 1,
    .FC_FOR_VDQ = 1,
    .IGNORE_SD = 1,
    .SME0 = 0,
};

static void poll(bq27220_handle_t h)
{
    /* StateOfCharge is documented as % x 2 on this family, but the driver returns the raw
       register, so the ratio of the two mAh counters is the only unambiguous percent. */
    const uint16_t rc = bq27220_get_remaining_capacity(h);
    const uint16_t fcc = bq27220_get_full_charge_capacity(h);
    const int16_t current = bq27220_get_current(h);
    battery_status_t st = {};
    bq27220_get_battery_status(h, &st);

    if (fcc == 0) return;         /* gauge has not settled yet: keep the last snapshot */

    int percent = (int)((uint32_t)rc * 100 / fcc);
    if (percent > 100) percent = 100;

    const bool charging = current > 0;
    uint32_t s = SNAP_READY | ((uint32_t)percent << 1);
    if (charging) s |= SNAP_CHG;
    if (charging && st.FC) s |= SNAP_FULL;
    s_snap = s;
}

static void batt_task(void *arg)
{
    (void)arg;
    bq27220_handle_t h = NULL;
    for (;;) {
        if (!h) {
            h = bq27220_create(&(bq27220_config_t){
                .i2c_bus = bsp_i2c_bus_get_handle(),
                .cfg = &GAUGING,
                .cedv = &CEDV,
            });
            if (!h) ESP_LOGW(TAG, "gauge absent, retrying in %d s", RETRY_MS / 1000);
        }
        if (h) poll(h);
        vTaskDelay(pdMS_TO_TICKS(h ? POLL_MS : RETRY_MS));
    }
}

bool tf_port_battery(tf_batt_t *out)
{
    if (!s_task_up) {
        s_task_up = true;
        if (xTaskCreate(batt_task, "tf_batt", 4096, NULL, 1, NULL) != pdPASS)
            ESP_LOGE(TAG, "poll task not created");
    }
    const uint32_t s = s_snap;
    if (!(s & SNAP_READY)) return false;
    out->percent = (int)((s >> 1) & 0x7F);
    out->charging = (s & SNAP_CHG) != 0;
    out->full = (s & SNAP_FULL) != 0;
    return true;
}
