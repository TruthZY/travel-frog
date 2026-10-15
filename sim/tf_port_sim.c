#include "tf_port.h"
#include "tf_persist.h"
#include "lvgl.h"
#include <stdio.h>
#include <time.h>

static const char *SLOT_FILE[TF_SAVE_SLOTS] = { "travel_frog_save.bin", "travel_frog_save.bak" };
static tf_save_meta_t s_meta;

void tf_port_sim_init(void);

static bool slot_read(int slot, void *buf, size_t buf_sz, size_t *out_len)
{
    FILE *f = fopen(SLOT_FILE[slot], "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, buf_sz, f);
    fclose(f);
    if (out_len) *out_len = n;
    return n > 0;
}

static bool slot_write(int slot, const void *buf, size_t buf_sz)
{
    FILE *f = fopen(SLOT_FILE[slot], "wb");
    if (!f) return false;
    size_t n = fwrite(buf, 1, buf_sz, f);
    fclose(f);
    return n == buf_sz;
}

bool tf_port_load(tf_save_t *s)
{
    if (!tf_persist_load(s, &s_meta, slot_read)) return false;
    tf_port_clock_floor(s_meta.saved_at_ms);
    return true;
}

bool tf_port_store(const tf_save_t *s)
{
    s_meta.saved_at_ms = tf_port_now_ms();
    return tf_persist_store(s, &s_meta, slot_write);
}

uint32_t tf_port_random_seed(void)
{
    /* A PC has no hardware RNG. Seconds alone would make two runs in the same second draw
       identical cards, so fold in the LVGL tick and an ASLR'd stack address as well. */
    uint32_t mix = 0;
    mix ^= (uint32_t)time(NULL);
    mix ^= (uint32_t)lv_tick_get();
    mix ^= (uint32_t)(uintptr_t)&mix;
    return mix;
}

/* ---------- sleep ----------
   A PC cannot switch itself off, so the simulator sleeps the way the polled firmware
   fallback does: tf_port_sleep() returns at once and tf_ui keeps a black veil over the
   window until the next tap. That is exactly the state machine the board runs through,
   which is why the sleep path is testable here. The veil also stops tf_ui from drawing
   at all, so use the x300 speed button to fast-forward a trip while it is up. */

void tf_port_sleep(void) { }

/* Nothing here is ever powered down: the window has no backlight and SDL owns the audio. */
void tf_port_wake_restore(void) { }

static lv_obj_t *s_flash;

static void flash_done(lv_anim_t *a)
{
    (void)a;
    if (s_flash) { lv_obj_delete(s_flash); s_flash = NULL; }
}

/* lv_anim exec callbacks take void */
static void set_flash_opa(void *o, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

void tf_port_blink(int times)
{
    if (times < 1) return;
    /* On the board this pulses the backlight; here the panel is the whole window, so a
       white veil on top of everything is the closest thing to "the screen flashed". */
    if (s_flash) { lv_obj_delete(s_flash); s_flash = NULL; }

    s_flash = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_flash);
    lv_obj_set_size(s_flash, 360, 360);
    lv_obj_set_style_bg_color(s_flash, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_flash, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_flash, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(s_flash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(s_flash);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_flash);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_80);
    lv_anim_set_exec_cb(&a, set_flash_opa);
    lv_anim_set_duration(&a, 110);
    lv_anim_set_playback_duration(&a, 160);
    lv_anim_set_repeat_count(&a, times);
    lv_anim_set_completed_cb(&a, flash_done);
    lv_anim_start(&a);
}
