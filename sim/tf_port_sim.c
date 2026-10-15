#include "tf_port.h"
#include "tf_persist.h"
#include <stdio.h>

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
