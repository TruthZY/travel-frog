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
