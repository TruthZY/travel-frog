#pragma once
#include "lvgl.h"
#include "tf_core.h"

/* Build the whole 360x360 round UI. screen is the active lv_display screen. */
void tf_ui_create(lv_obj_t *screen, tf_save_t *save);
