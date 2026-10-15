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

/* ---------- sleep ----------
   Every deadline in the save is an absolute timestamp, so a sleeping board computes
   nothing: the trip and the clover field keep running on the PCF85063A and are
   settled by tf_core_tick() the next time the UI comes up. */
typedef enum {
    TF_WAKE_POWER_ON = 0,   /* cold boot, PWR key, replug */
    TF_WAKE_TOUCH,          /* the player came back and tapped the glass */
} tf_wake_t;

/* Which of the two wake paths brought the chip out of deep sleep; TF_WAKE_POWER_ON on a
   board that never slept. Read at boot by the firmware's sleep driver, which keeps its
   tier counters from it. */
tf_wake_t tf_port_wake_cause(void);
/* Blank the panel and sleep. The firmware's deep-sleep path never returns (a wake is a
   reboot); where the touch pin cannot be trusted the firmware returns like the simulator
   does, and the caller is then expected to keep the veil up until the next tap. */
void      tf_port_sleep(void);
/* Pulse the light to catch the eye from across the room. There is no separately
   wired status LED on this board — LED1/LED2 hang off the charger — so the panel
   backlight is the only thing that can blink. */
void      tf_port_blink(int times);
