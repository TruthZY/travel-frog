#include "tf_ui.h"
#include "tf_port.h"
#include "tf_fonts.h"
#include "tf_assets.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef LV_INT_TO_PTR
#define LV_INT_TO_PTR(n) ((void *)(intptr_t)(n))
#endif
#ifndef LV_PTR_TO_INT
#define LV_PTR_TO_INT(p) ((int)(intptr_t)(p))
#endif

/* ---------- palette (sampled from the pixel art) ---------- */
#define COL_SKY     0xBFE3FF
#define COL_GRASS   0x7CB84E
#define COL_PANEL   0xFFF6E3
#define COL_ROW     0xF3E7CD
#define COL_WOOD    0x8B5E3C
#define COL_INK     0x4A3220
#define COL_PLATE   0xF0DDB4
#define COL_PLATE_P 0xD6BC8B
#define COL_GREEN   0x4D8A3C
#define COL_GREEN_P 0x3C6E2F
#define COL_GREY    0x8A7A66
#define COL_GREY_P  0x6E6152
#define COL_FROG    0x66C04F
#define COL_CLOVER  0x2E9B5C
#define COL_BATT_HI  0x3EA03A            /* three bars: enough charge */
#define COL_BATT_MID 0xE8A800            /* two bars, or the bolt while charging */
#define COL_BATT_LO  0xD6453C            /* one bar: find a cable */

#define UI_D 360                    /* round panel diameter */
#define BAR_W 176                   /* widest pill that still fits the circle at y=25 */
#define DOCK_SZ 56

/* Idle time before the panel goes dark. Being away is the long wait of this game, so
   it blanks sooner than the garden view the player might still be looking at. */
#define IDLE_MS 20000
#define SLEEP_FADE_MS 400

enum { DOCK_SHOP = 0, DOCK_GO, DOCK_CARD, DOCK_SPD, DOCK_NUM };

static const lv_point_t CLOVER_XY[TF_NUM_CLOVER] = {{46,150},{280,150},{22,206},{303,206}};
/* dock button centres along the bottom arc */
static const lv_point_t DOCK_XY[DOCK_NUM] = {{84,272},{146,300},{214,300},{276,272}};

#ifdef TF_HAS_ASSETS
/* dock icons, in dock button order */
static const int DOCK_ICON[DOCK_NUM] = {
    TF_ASSET_ICON_SHOP, TF_ASSET_ICON_GO, TF_ASSET_ICON_CARD, TF_ASSET_ICON_SPD
};
#endif

typedef struct {
    tf_save_t *save;
    lv_obj_t *frog_box;
    lv_obj_t *frog_img[2];          /* [0] home, [1] angry (NULL when no assets) */
    lv_obj_t *clover_lbl;
    lv_obj_t *trip_lbl;
    lv_obj_t *batt_card;
    lv_obj_t *batt_bolt;
    lv_obj_t *batt_bar[3];
    lv_obj_t *dock_lbl[DOCK_NUM];
    lv_obj_t *clover_btn[TF_NUM_CLOVER];
    bool clover_vis[TF_NUM_CLOVER];
    lv_obj_t *bubble;
    lv_obj_t *bubble_lbl;
    lv_timer_t *bubble_hide;
    lv_obj_t *modal_dim;
    lv_obj_t *modal_title;
    lv_obj_t *modal_body;
    lv_obj_t *black;
} ui_t;

static ui_t U;
static int s_sleep;                     /* 0 awake, 1 fading out, 2 veiled */
static int64_t s_last_act_ms;
static uint32_t s_batt_sig = ~0u;       /* last painted battery look, ~0 = never painted */
#ifdef TF_DEBUG_IDLE
static int64_t s_boot_ms;               /* temporary M3b rig: watch the countdown on UART */
#endif

static void refresh(void);

/* ---------- helpers ---------- */

static lv_obj_t *mk_base(lv_obj_t *parent, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* cream card with a wooden outline — shared look for the bar, bubble and modal */
static lv_obj_t *mk_card(lv_obj_t *parent, int w, int h, int radius)
{
    lv_obj_t *o = mk_base(parent, w, h, COL_PANEL, radius);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(COL_WOOD), 0);
    lv_obj_set_style_shadow_width(o, 12, 0);
    lv_obj_set_style_shadow_ofs_y(o, 3, 0);
    lv_obj_set_style_shadow_color(o, lv_color_hex(0x2A1808), 0);
    lv_obj_set_style_shadow_opa(o, LV_OPA_30, 0);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

#ifdef TF_HAS_ASSETS
static lv_obj_t *mk_asset_img(lv_obj_t *parent, int which)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, tf_asset_src(which));
    lv_obj_center(img);
    return img;
}
#endif

static void anim_start(lv_obj_t *o, lv_anim_exec_xcb_t exec, int32_t from, int32_t to,
                       uint32_t ms, uint32_t repeat, lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_repeat_count(&a, repeat);
    if (repeat == LV_ANIM_REPEAT_INFINITE) lv_anim_set_playback_duration(&a, ms);
    lv_anim_start(&a);
}

/* lv_anim exec callbacks take void * so the pointer type matches lv_anim_exec_xcb_t */
static void set_translate_y(void *o, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)o, v, 0); }
static void set_scale(void *o, int32_t v)       { lv_obj_set_style_transform_scale((lv_obj_t *)o, v, 0); }
static void set_bg_opa(void *o, int32_t v)      { lv_obj_set_style_bg_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

static void bubble_hide_cb(lv_timer_t *t)
{
    (void)t;
    U.bubble_hide = NULL;
    lv_obj_add_flag(U.bubble, LV_OBJ_FLAG_HIDDEN);
}

static void show_bubble(const char *txt, uint32_t ms)
{
    lv_label_set_text(U.bubble_lbl, txt);
    lv_obj_update_layout(U.bubble);
    lv_obj_clear_flag(U.bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(U.bubble, LV_ALIGN_TOP_MID, 0, 86);
    lv_obj_update_layout(U.bubble);
    anim_start(U.bubble, set_translate_y, -10, 0, 180, 1, lv_anim_path_ease_out);
    if (U.bubble_hide) lv_timer_delete(U.bubble_hide);
    U.bubble_hide = lv_timer_create(bubble_hide_cb, ms, NULL);
    lv_timer_set_repeat_count(U.bubble_hide, 1);
}

/* ---------- generic modal ---------- */

static lv_obj_t *mk_text_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud,
                             uint32_t col, uint32_t col_pressed)
{
    lv_obj_t *b = mk_base(parent, LV_SIZE_CONTENT, LV_SIZE_CONTENT, col, LV_RADIUS_CIRCLE);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_pad_ver(b, 5, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(col_pressed), LV_STATE_PRESSED);
    mk_label(b, txt, 0xFFFFFF);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void modal_close_cb(lv_event_t *e)
{
    (void)e;
    tf_port_sfx(TF_SFX_CLICK);
    lv_obj_add_flag(U.modal_dim, LV_OBJ_FLAG_HIDDEN);
    refresh();
}

static void modal_open(const char *title)
{
    lv_obj_t *old;
    while ((old = lv_obj_get_child(U.modal_body, 0)) != NULL)
        lv_obj_delete(old);
    lv_label_set_text(U.modal_title, title);
    lv_obj_clear_flag(U.modal_dim, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- shop ---------- */

static const char *ITEM_NAME[TF_ITEM_COUNT] = {"便当", "护身符", "帐篷"};
static lv_obj_t *shop_status_lbl;

static void shop_buy_cb(lv_event_t *e)
{
    tf_item_t it = (tf_item_t)LV_PTR_TO_INT(lv_obj_get_user_data(lv_obj_get_parent(lv_event_get_target_obj(e))));
    char buf[80];
    if (tf_core_buy(U.save, it) == TF_OK) {
        tf_port_sfx(TF_SFX_CLICK);
        snprintf(buf, sizeof buf, "买下了%s，还剩 %d 三叶草", ITEM_NAME[it], (int)U.save->clovers);
        tf_port_store(U.save);
    } else {
        snprintf(buf, sizeof buf, "三叶草不够，先去采一些");
    }
    lv_label_set_text(shop_status_lbl, buf);
    refresh();
}

static void open_shop(void)
{
    modal_open("小卖部");
    char buf[64];
    for (int i = 0; i < TF_ITEM_COUNT; i++) {
        lv_obj_t *row = mk_base(U.modal_body, 224, 40, COL_ROW, 10);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0xD8C49A), 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_user_data(row, LV_INT_TO_PTR(i));

        snprintf(buf, sizeof buf, "%s · %d叶", ITEM_NAME[i], tf_core_item_price((tf_item_t)i));
        mk_label(row, buf, COL_INK);
        snprintf(buf, sizeof buf, "已有%d", (int)U.save->items[i]);
        mk_label(row, buf, 0x8A7355);
        mk_text_btn(row, "购买", shop_buy_cb, NULL, COL_GREEN, COL_GREEN_P);
    }
    snprintf(buf, sizeof buf, "三叶草 %d · 便当 %d", (int)U.save->clovers,
             (int)U.save->items[TF_ITEM_SANDWICH]);
    shop_status_lbl = mk_label(U.modal_body, buf, COL_INK);
}

/* ---------- gallery ---------- */

static void open_gallery(void)
{
    modal_open("明信片");
    int owned = 0;
    for (int i = 0; i < TF_NUM_CARDS; i++)
        if (U.save->cards[i] > 0) owned++;

    lv_obj_t *grid = mk_base(U.modal_body, 224, LV_SIZE_CONTENT, 0x000000, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grid, 5, 0);

    char buf[40];
    for (int i = 0; i < TF_NUM_CARDS; i++) {
        bool got = U.save->cards[i] > 0;
        lv_obj_t *cell = mk_base(grid, 108, 26, got ? COL_ROW : 0xEFE6D4, 8);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_border_color(cell, lv_color_hex(got ? 0xD8C49A : 0xE2D8C2), 0);
        if (got) snprintf(buf, sizeof buf, "%s ×%d", tf_core_card_name(i), (int)U.save->cards[i]);
        else     snprintf(buf, sizeof buf, "· · ·");
        lv_obj_center(mk_label(cell, buf, got ? COL_INK : 0xB3A48C));
    }

    snprintf(buf, sizeof buf, "已收集 %d / %d", owned, TF_NUM_CARDS);
    mk_label(U.modal_body, buf, COL_INK);
}

/* ---------- actions ---------- */

static void trip_btn_cb(lv_event_t *e)
{
    (void)e;
    int64_t now = tf_port_now_ms();
    switch (tf_core_state(U.save)) {
    case TF_STATE_HOME:
        if (tf_core_send(U.save, now) == TF_OK) {
            tf_port_sfx(TF_SFX_CROAK);
            show_bubble("出发啦！", 2500);
        } else {
            tf_port_sfx(TF_SFX_SAD);
            show_bubble("没有便当，出不了门", 2500);
        }
        break;
    case TF_STATE_OUT:
        tf_core_recall(U.save, now);
        tf_port_sfx(TF_SFX_SAD);
        show_bubble("半路召回，什么礼物都不会有", 3000);
        break;
    case TF_STATE_ANGRY:
        show_bubble("别理我，我想静一会儿", 2000);
        break;
    }
    tf_port_store(U.save);
    refresh();
}

static void clover_cb(lv_event_t *e)
{
    int slot = LV_PTR_TO_INT(lv_event_get_user_data(e));
    if (tf_core_harvest(U.save, slot, tf_port_now_ms())) {
        tf_port_sfx(TF_SFX_HARVEST);
        tf_port_store(U.save);
        refresh();
    }
}

static void bag_cb(lv_event_t *e)  { (void)e; tf_port_sfx(TF_SFX_CLICK); open_shop(); }
static void book_cb(lv_event_t *e) { (void)e; tf_port_sfx(TF_SFX_CLICK); open_gallery(); }

static void spd_cb(lv_event_t *e)
{
    (void)e;
    char buf[48];
    tf_port_speed_cycle();
    snprintf(buf, sizeof buf, "模拟速度 x%d", (int)tf_port_speed());
    show_bubble(buf, 1500);
    refresh();
}

/* ---------- sleep ---------- */

/* The two breathing loops are the only things that keep asking for a redraw while the
   player is away, so sleep stops them and wake restarts them. */
static void idle_anims_start(void)
{
    anim_start(U.frog_box, set_translate_y, 0, 4, 900, LV_ANIM_REPEAT_INFINITE,
               lv_anim_path_ease_in_out);
    for (int i = 0; i < TF_NUM_CLOVER; i++)
        anim_start(U.clover_btn[i], set_bg_opa, LV_OPA_20, LV_OPA_80, 650,
                   LV_ANIM_REPEAT_INFINITE, lv_anim_path_ease_in_out);
}

static void idle_anims_stop(void)
{
    lv_anim_delete(U.frog_box, set_translate_y);
    for (int i = 0; i < TF_NUM_CLOVER; i++)
        lv_anim_delete(U.clover_btn[i], set_bg_opa);
}

/* The veil is opaque and covers the whole panel, but LVGL 9.3 has no occlusion culling
   (lv_obj_area_is_visible() only tests HIDDEN), so animations underneath it would keep
   pushing full frames to a panel nobody can see — which is what makes the polled sleep
   fallback run hot. Killing invalidation stops it at the source. Touch keeps working:
   the indev hit-tests the object tree by coordinates, never the framebuffer. */
static void drawing_gate(bool closed)
{
    lv_display_t *d = lv_obj_get_display(U.black);
    if (!d) return;
    lv_display_enable_invalidation(d, !closed);
    lv_timer_t *refr = lv_display_get_refr_timer(d);
    if (!refr) return;
    if (closed) lv_timer_pause(refr);
    else        lv_timer_resume(refr);
}

/* Sampling the devices at the same rate LVGL reads them means a tap too short for the
   indev to notice also doesn't count as activity — which is the behaviour we want. */
static void activity_cb(lv_timer_t *t)
{
    (void)t;
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            s_last_act_ms = tf_port_now_ms();
            return;
        }
        indev = lv_indev_get_next(indev);
    }
}

static void sleep_fade_done(lv_anim_t *a)
{
    (void)a;
    s_sleep = 2;
    idle_anims_stop();
    drawing_gate(true);
    tf_port_store(U.save);   /* a deep-sleep wake is a reboot: whatever is unsaved is lost */
    /* A trip that will end while the panel is dark gets an alarm, so the board can flash
       the light for a player who is nowhere near it. The wake that ring never draws UI. */
    const int64_t due = tf_core_trip_remaining_ms(U.save, tf_port_now_ms());
    if (tf_core_state(U.save) == TF_STATE_OUT && due > 0 &&
        tf_port_sleep_until(tf_port_wall_ms() + due))
        return;
    tf_port_sleep();
    /* Only the simulator and the polled fallback come back here; the veil stays up and
       wake_cb handles the tap. */
}

static void sleep_now(void)
{
    lv_obj_t *old;
    s_sleep = 1;
    while ((old = lv_obj_get_child(U.modal_body, 0)) != NULL)
        lv_obj_delete(old);
    lv_obj_add_flag(U.modal_dim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(U.bubble, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(U.black, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(U.black, LV_OPA_TRANSP, 0);
    lv_obj_update_layout(U.black);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, U.black);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a, set_bg_opa);
    lv_anim_set_duration(&a, SLEEP_FADE_MS);
    lv_anim_set_completed_cb(&a, sleep_fade_done);
    lv_anim_start(&a);
}

static void wake_cb(lv_event_t *e)
{
    (void)e;
    if (!s_sleep) return;
    const bool was_veiled = (s_sleep == 2);   /* a tap during the fade never slept for real */
    s_sleep = 0;
    lv_anim_delete(U.black, set_bg_opa);   /* a tap during the fade cancels the sleep */
    if (was_veiled) {
        tf_port_wake_restore();   /* lamp and codec first, so the frame lands on a lit panel */
        drawing_gate(false);
        idle_anims_start();
        lv_obj_invalidate(lv_screen_active());
    }
    s_last_act_ms = tf_port_now_ms();
    lv_obj_add_flag(U.black, LV_OBJ_FLAG_HIDDEN);
    refresh();   /* on_tick then settles the trip and blinks if the frog came home */
}

/* ---------- tick ---------- */

static void on_tick(lv_timer_t *t)
{
    (void)t;
    if (s_sleep) return;               /* nothing behind the veil is worth drawing */
    int64_t now = tf_port_now_ms();
    uint32_t ev = tf_core_tick(U.save, now);
    if (ev & TF_EV_TRIP_RETURN) {
        char buf[128];
        tf_port_blink(3);               /* the player may be across the room */
        tf_port_sfx(TF_SFX_REWARD);
        if (U.save->last_reward_card >= 0 && U.save->last_reward_clovers > 0)
            snprintf(buf, sizeof buf, "回来啦！明信片「%s」+%d 三叶草",
                     tf_core_card_name(U.save->last_reward_card), (int)U.save->last_reward_clovers);
        else if (U.save->last_reward_card >= 0)
            snprintf(buf, sizeof buf, "回来啦！明信片「%s」", tf_core_card_name(U.save->last_reward_card));
        else
            snprintf(buf, sizeof buf, "回来啦！");
        show_bubble(buf, 4000);
        tf_port_store(U.save);
    }
    if (ev & TF_EV_ANGRY_END)
        tf_port_store(U.save);
    refresh();

#ifdef TF_DEBUG_IDLE
    {
        static int last_s = -1;
        const int s = (int)((now - s_boot_ms) / 1000);
        if (s != last_s) {
            last_s = s;
            printf("idle %ds / %llds -> %s\n", s, (long long)(IDLE_MS / 1000),
                   s_sleep ? "VEILED" : "awake");
            fflush(stdout);
        }
    }
#endif
    if (now - s_last_act_ms >= IDLE_MS) sleep_now();
}

/* ---------- layout ---------- */

static void build_dock_btn(lv_obj_t *parent, int i, const char *caption, lv_event_cb_t cb)
{
    lv_obj_t *b = mk_base(parent, DOCK_SZ, DOCK_SZ, COL_PLATE, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(b, DOCK_XY[i].x - DOCK_SZ / 2, DOCK_XY[i].y - DOCK_SZ / 2);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_PLATE_P), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(COL_WOOD), 0);
    lv_obj_set_style_shadow_width(b, 8, 0);
    lv_obj_set_style_shadow_ofs_y(b, 2, 0);
    lv_obj_set_style_shadow_color(b, lv_color_hex(0x2A1808), 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_40, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

#ifdef TF_HAS_ASSETS
    lv_obj_t *img = lv_image_create(b);
    lv_image_set_src(img, tf_asset_src(DOCK_ICON[i]));
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 3);
#else
    (void)caption;
#endif
    U.dock_lbl[i] = mk_label(b, caption, COL_INK);
    lv_obj_align(U.dock_lbl[i], LV_ALIGN_BOTTOM_MID, 0, -2);
}

void tf_ui_create(lv_obj_t *screen, tf_save_t *save)
{
    memset(&U, 0, sizeof U);
    U.save = save;

    lv_obj_t *scr = screen ? screen : lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_SKY), 0);
    lv_obj_set_style_text_font(scr, TF_FONT_BODY, 0);   /* inherited by every child */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* background */
#ifdef TF_HAS_ASSETS
    mk_asset_img(scr, TF_ASSET_BG);
#else
    lv_obj_t *grass = mk_base(scr, UI_D, 190, COL_GRASS, 0);
    lv_obj_set_pos(grass, 0, 170);
    lv_obj_t *house = mk_base(scr, 90, 60, 0xC98A5A, 6);
    lv_obj_set_pos(house, 135, 64);
    lv_obj_t *roof = mk_base(scr, 70, 34, 0xA4552F, 16);
    lv_obj_set_pos(roof, 145, 34);
#endif

    /* frog */
    U.frog_box = mk_base(scr, 160, 160, 0x000000, 0);
    lv_obj_set_style_bg_opa(U.frog_box, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(U.frog_box, 100, 112);
#ifdef TF_HAS_ASSETS
    U.frog_img[0] = mk_asset_img(U.frog_box, TF_ASSET_FROG_HOME);
    U.frog_img[1] = mk_asset_img(U.frog_box, TF_ASSET_FROG_ANGRY);
#else
    U.frog_img[0] = mk_base(U.frog_box, 70, 70, COL_FROG, LV_RADIUS_CIRCLE);
    lv_obj_align(U.frog_img[0], LV_ALIGN_CENTER, 0, 8);
#endif

    /* clovers: a pulsing light disc makes them readable on the grass */
    for (int i = 0; i < TF_NUM_CLOVER; i++) {
        lv_obj_t *c = mk_base(scr, 36, 36, 0xFFF2A0, LV_RADIUS_CIRCLE);
        U.clover_btn[i] = c;
        lv_obj_set_style_bg_opa(c, LV_OPA_40, 0);
        lv_obj_set_style_border_width(c, 2, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(0xFFFAE0), 0);
        lv_obj_set_style_border_opa(c, LV_OPA_70, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
#ifdef TF_HAS_ASSETS
        mk_asset_img(c, TF_ASSET_CLOVER);
#else
        lv_obj_center(mk_base(c, 26, 26, COL_CLOVER, LV_RADIUS_CIRCLE));
#endif
        lv_obj_add_event_cb(c, clover_cb, LV_EVENT_CLICKED, LV_INT_TO_PTR(i));
        lv_obj_set_pos(c, CLOVER_XY[i].x, CLOVER_XY[i].y);
        U.clover_vis[i] = true;   /* created visible, so refresh() only has to hide changes */
    }
    idle_anims_start();

    /* status bar: one pill, narrow enough to stay inside the round panel */
    lv_obj_t *bar = mk_card(scr, BAR_W, 30, LV_RADIUS_CIRCLE);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 25);
    lv_obj_set_style_bg_opa(bar, LV_OPA_90, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 12, 0);
    U.clover_lbl = mk_label(bar, "三叶草 0", COL_INK);
    U.trip_lbl = mk_label(bar, "在家", COL_INK);

    /* battery: a second, smaller pill centred under the status bar. At y=59..77 the round
       panel is ~295 px wide, so this ~60 px pill has plenty of clearance. */
    U.batt_card = mk_card(scr, LV_SIZE_CONTENT, 18, LV_RADIUS_CIRCLE);
    lv_obj_align(U.batt_card, LV_ALIGN_TOP_MID, 0, 59);
    lv_obj_set_style_bg_opa(U.batt_card, LV_OPA_90, 0);
    lv_obj_set_style_pad_hor(U.batt_card, 5, 0);
    lv_obj_set_flex_flow(U.batt_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(U.batt_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(U.batt_card, 3, 0);
    U.batt_bolt = mk_label(U.batt_card, LV_SYMBOL_CHARGE, COL_BATT_MID);
    lv_obj_set_style_text_font(U.batt_bolt, &lv_font_montserrat_14, 0);
    for (int i = 0; i < 3; i++)
        U.batt_bar[i] = mk_base(U.batt_card, 9, 12, COL_GREY, 2);
    lv_obj_add_flag(U.batt_card, LV_OBJ_FLAG_HIDDEN);

    /* dock */
    build_dock_btn(scr, DOCK_SHOP, "商店", bag_cb);
    build_dock_btn(scr, DOCK_GO, "出发", trip_btn_cb);
    build_dock_btn(scr, DOCK_CARD, "图鉴", book_cb);
    build_dock_btn(scr, DOCK_SPD, "速度", spd_cb);

    /* speech bubble */
    U.bubble = mk_card(scr, LV_SIZE_CONTENT, LV_SIZE_CONTENT, 12);
    lv_obj_set_style_pad_hor(U.bubble, 12, 0);
    lv_obj_set_style_pad_ver(U.bubble, 6, 0);
    lv_obj_add_flag(U.bubble, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t *tail = mk_base(U.bubble, 12, 12, COL_PANEL, 2);
    lv_obj_set_style_border_width(tail, 2, 0);
    lv_obj_set_style_border_color(tail, lv_color_hex(COL_WOOD), 0);
    lv_obj_set_style_transform_rotation(tail, 450, 0);
    lv_obj_align(tail, LV_ALIGN_BOTTOM_MID, 0, 4);
    U.bubble_lbl = mk_label(U.bubble, "", COL_INK);
    lv_obj_set_width(U.bubble_lbl, LV_SIZE_CONTENT);
    lv_label_set_long_mode(U.bubble_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(U.bubble_lbl, 248, 0);
    lv_obj_set_style_text_align(U.bubble_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(U.bubble, LV_OBJ_FLAG_HIDDEN);

    /* modal */
    U.modal_dim = mk_base(scr, UI_D, UI_D, 0x1A1008, 0);
    lv_obj_set_style_bg_opa(U.modal_dim, LV_OPA_60, 0);
    lv_obj_center(U.modal_dim);
    lv_obj_add_flag(U.modal_dim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(U.modal_dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.modal_dim, modal_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = mk_card(U.modal_dim, 252, LV_SIZE_CONTENT, 16);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(panel);

    U.modal_title = mk_label(panel, "", COL_INK);
    lv_obj_set_style_text_font(U.modal_title, TF_FONT_TITLE, 0);
    lv_obj_set_style_pad_bottom(U.modal_title, 4, 0);

    lv_obj_t *sep = mk_base(panel, 200, 2, COL_WOOD, 1);
    lv_obj_set_style_opa(sep, LV_OPA_30, 0);
    lv_obj_set_style_pad_bottom(sep, 6, 0);

    U.modal_body = mk_base(panel, 224, LV_SIZE_CONTENT, 0x000000, 0);
    lv_obj_set_style_bg_opa(U.modal_body, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(U.modal_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(U.modal_body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(U.modal_body, 6, 0);
    lv_obj_set_style_pad_ver(U.modal_body, 4, 0);

    mk_text_btn(panel, "关闭", modal_close_cb, NULL, COL_GREY, COL_GREY_P);

    /* Sleep veil. It is the topmost object and the only clickable one while dark, so the
       tap that wakes the board is consumed by the wake itself, not by a button. */
    U.black = mk_base(scr, UI_D, UI_D, 0x000000, 0);
    lv_obj_center(U.black);
    lv_obj_add_flag(U.black, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(U.black, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.black, wake_cb, LV_EVENT_PRESSED, NULL);

    s_last_act_ms = tf_port_now_ms();
#ifdef TF_DEBUG_IDLE
    s_boot_ms = s_last_act_ms;
#endif
    lv_timer_create(on_tick, 250, NULL);
    lv_timer_create(activity_cb, 30, NULL);
    refresh();
}

/* Three bars green, two yellow, one red — the count and the colour both carry the level,
   so the pill reads at a glance from across a room. The bolt only appears on external
   power: yellow while charging, green once the gauge reports full. */
static void refresh_battery(void)
{
    tf_batt_t b = { .percent = 0, .charging = false, .full = false };
    const bool present = tf_port_battery(&b);
    const int bars = !present ? 0 : b.percent >= 60 ? 3 : b.percent >= 25 ? 2 : 1;
    const uint32_t sig = ((uint32_t)present << 3) | ((uint32_t)bars << 8) |
                         ((uint32_t)b.charging << 1) | ((uint32_t)b.full << 2);
    if (sig == s_batt_sig) return;          /* the gauge only moves every couple of seconds */
    s_batt_sig = sig;

    set_hidden(U.batt_card, !present);      /* no gauge is better answered with silence */
    if (!present) return;

    const uint32_t band = bars == 3 ? COL_BATT_HI : bars == 2 ? COL_BATT_MID : COL_BATT_LO;
    for (int i = 0; i < 3; i++) {
        const bool lit = i < bars;
        lv_obj_set_style_bg_color(U.batt_bar[i], lv_color_hex(lit ? band : COL_GREY), 0);
        lv_obj_set_style_bg_opa(U.batt_bar[i], lit ? LV_OPA_COVER : LV_OPA_20, 0);
    }
    set_hidden(U.batt_bolt, !b.charging);
    if (b.charging)
        lv_obj_set_style_text_color(U.batt_bolt, lv_color_hex(b.full ? COL_BATT_HI : COL_BATT_MID), 0);
}

static void refresh(void)
{
    int64_t now = tf_port_now_ms();
    tf_state_t st = tf_core_state(U.save);
    char buf[64];

    refresh_battery();
    snprintf(buf, sizeof buf, "三叶草 %d", (int)U.save->clovers);
    lv_label_set_text(U.clover_lbl, buf);

    switch (st) {
    case TF_STATE_HOME:
        lv_label_set_text(U.trip_lbl, "在家");
        lv_label_set_text(U.dock_lbl[DOCK_GO], "出发");
        break;
    case TF_STATE_OUT: {
        int64_t left = tf_core_trip_remaining_ms(U.save, now);
        snprintf(buf, sizeof buf, "旅行 %d:%02d", (int)(left / 60000), (int)(left / 1000 % 60));
        lv_label_set_text(U.trip_lbl, buf);
        lv_label_set_text(U.dock_lbl[DOCK_GO], "召回");
        break;
    }
    case TF_STATE_ANGRY:
        lv_label_set_text(U.trip_lbl, "生气中");
        lv_label_set_text(U.dock_lbl[DOCK_GO], "哄它");
        break;
    }

    set_hidden(U.frog_box, st == TF_STATE_OUT);
    set_hidden(U.frog_img[0], st == TF_STATE_ANGRY);
    set_hidden(U.frog_img[1], st != TF_STATE_ANGRY);

    for (int i = 0; i < TF_NUM_CLOVER; i++) {
        bool vis = st != TF_STATE_OUT && tf_core_clover_ready(U.save, i, now);
        lv_obj_t *c = U.clover_btn[i];
        if (vis == U.clover_vis[i])
            continue;
        U.clover_vis[i] = vis;
        if (vis) {
            set_hidden(c, false);
            lv_obj_set_style_transform_scale(c, 0, 0);
            anim_start(c, set_scale, 0, LV_SCALE_NONE, 220, 1, lv_anim_path_ease_out);
        } else {
            set_hidden(c, true);
        }
    }
}
