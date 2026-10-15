#include "tf_port.h"
#include <SDL.h>
#include <stdint.h>
#include <time.h>

static int s_speed = 1;
static int64_t s_accum = 0;      /* elapsed game time, already scaled */
static int64_t s_offset = 0;     /* added by tf_port_clock_floor */
static uint64_t s_last = 0;

static int64_t scaled_now(void)
{
    uint64_t t = SDL_GetTicks64();
    s_accum += (int64_t)(t - s_last) * s_speed;
    s_last = t;
    return s_accum;
}

void tf_port_sim_init(void)
{
    s_last = SDL_GetTicks64();
}

int64_t tf_port_now_ms(void)
{
    return scaled_now() + s_offset;
}

/* The board reads this off the PCF85063A. The PC has no RTC to disagree with the game
   clock, so it reports the host wall clock — the one number the simulator cannot fast
   forward with the speed button. */
int64_t tf_port_wall_ms(void)
{
    return (int64_t)time(NULL) * 1000;
}

void tf_port_wall_set(int64_t ms)
{
    (void)ms;   /* setting the host clock is not the simulator's to do */
}

void tf_port_clock_floor(int64_t ms)
{
    int64_t now = scaled_now();
    if (now < ms) s_offset += ms - now;
}

void tf_port_speed_cycle(void)
{
    s_speed = s_speed == 1 ? 30 : (s_speed == 30 ? 300 : 1);
}

int tf_port_speed(void) { return s_speed; }
