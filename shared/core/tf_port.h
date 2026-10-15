#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "tf_core.h"

/* Implemented once per target: sim (SDL/file) and firmware (esp_timer/NVS). */

/* Monotonic game clock in ms; honors the debug time-scale on the simulator. */
int64_t tf_port_now_ms(void);
/* Push the clock forward so it never reads below `ms` again — used after a boot
   where the wall clock restarted (no RTC / dead backup cap) but the save says
   time already passed. Keeps a reboot from pausing or rewinding a trip. */
void    tf_port_clock_floor(int64_t ms);
/* Cycle debug time scale 1x -> 30x -> 300x. Firmware: no-op. */
void    tf_port_speed_cycle(void);
int     tf_port_speed(void);

/* Entropy for tf_core_seed(). Firmware: the S3's hardware RNG. Simulator: there is none,
   so it mixes what a PC has. Only needs to differ between runs, not be unpredictable. */
uint32_t tf_port_random_seed(void);

/* Sound effects. Names live in tf_sfx.c so both targets look for the same files;
   the audio data is 22050 Hz / mono / 16-bit, which is what the board's I2S is
   fixed to (BSP_I2S_DUPLEX_MONO_CFG(22050)) and what SDL plays with no conversion. */
enum { TF_SFX_CLICK, TF_SFX_HARVEST, TF_SFX_CROAK, TF_SFX_SAD, TF_SFX_REWARD, TF_SFX_COUNT };
const char *tf_sfx_file(int id);
/* Open the audio device. Call before any tf_port_sfx(); without it sound is off. */
void    tf_port_audio_init(void);
void    tf_port_sfx(int id);

bool    tf_port_load(tf_save_t *s);
bool    tf_port_store(const tf_save_t *s);

/* Read the wall clock. Always succeeds: with no usable RTC the firmware falls back to
   2020-01-01, so this never returns a negative value. */
int64_t tf_port_wall_ms(void);
/* Move the wall clock to `ms` and persist it to the hardware RTC. Only needed when the
   time came from somewhere outside the board (NTP in M6) — an RTC that merely drifted
   backwards across a power cut cannot be corrected without this. */
void    tf_port_wall_set(int64_t ms);

/* ---------- battery ----------
   State of charge and charge state come from the BQ27220 gauge, whose getters swallow
   I2C errors, so "gauge present" has to be part of the answer: false means the UI shows
   nothing at all rather than a plausible-looking zero. */
typedef struct {
    int percent;
    bool charging;   /* current flowing into the cell */
    bool full;       /* charging, and the gauge has detected full charge */
} tf_batt_t;

bool    tf_port_battery(tf_batt_t *out);

/* ---------- sleep ----------
   Every deadline in the save is an absolute timestamp, so a sleeping board computes
   nothing: the trip and the clover field keep running on the PCF85063A and are
   settled by tf_core_tick() the next time the UI comes up. */
typedef enum {
    TF_WAKE_POWER_ON = 0,   /* cold boot, PWR key, replug */
    TF_WAKE_TOUCH,          /* the player came back and tapped the glass */
    TF_WAKE_ALARM,          /* the RTC rang for a deadline: nobody is here yet */
} tf_wake_t;

/* Which of the two wake paths brought the chip out of deep sleep; TF_WAKE_POWER_ON on a
   board that never slept. Read at boot by the firmware's sleep driver, which keeps its
   tier counters from it. */
tf_wake_t tf_port_wake_cause(void);
/* Blank the panel and sleep. The firmware's deep-sleep path never returns (a wake is a
   reboot); where the touch pin cannot be trusted the firmware returns like the simulator
   does, and the caller is then expected to keep the veil up until the next tap. */
void      tf_port_sleep(void);
/* Put back the hardware tf_port_sleep() turned off. Only the fallback path that returns
   instead of rebooting needs this — a deep-sleep wake runs app_main from the top, which
   lights the panel itself. Targets with nothing to restore implement it as a no-op. */
void      tf_port_wake_restore(void);
/* Pulse the light to catch the eye from across the room. There is no separately
   wired status LED on this board — LED1/LED2 hang off the charger — so the panel
   backlight is the only thing that can blink. */
void      tf_port_blink(int times);
/* Ring for the light show at the absolute wall-clock time `at_ms` and then power down
   without waiting for a finger. The board has no GPIO status LED, so the only way to
   tell the player "the frog is home" from across the room is to wake on the RTC's own
   alarm, blink and go straight back to sleep — the game state is not touched by that
   wake. Returns false when the chip cannot promise such a wake, in which case the
   caller should simply sleep without the reminder. */
bool      tf_port_sleep_until(int64_t at_ms);
