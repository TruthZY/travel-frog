#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tf_core.h"

/* Saves must survive an instant power cut: the PWR key is a hardware latch
   (U6 ECJ23001 -> PWR_EN) that gives the firmware no warning at all.
   A frame is written to one of TF_SAVE_SLOTS slots in round-robin, so a torn
   write always leaves the previous valid frame behind. */

#define TF_SAVE_SLOTS 2

typedef struct {
    int64_t  saved_at_ms;   /* game clock at write time, used as clock floor on load */
    uint32_t seq;           /* bumped on every store; 0 when nothing was saved yet */
} tf_save_meta_t;

/* Slot callbacks move one raw frame between storage and `buf` (>= tf_persist_frame_size()). */
typedef bool (*tf_slot_read_t)(int slot, void *buf, size_t buf_sz, size_t *out_len);
typedef bool (*tf_slot_write_t)(int slot, const void *buf, size_t buf_sz);

size_t tf_persist_frame_size(void);

/* Newest valid frame, or false when every slot is missing/corrupt. */
bool tf_persist_load(tf_save_t *game, tf_save_meta_t *meta, tf_slot_read_t read);

/* Writes `game` and updates meta (seq). */
bool tf_persist_store(const tf_save_t *game, tf_save_meta_t *meta, tf_slot_write_t write);
