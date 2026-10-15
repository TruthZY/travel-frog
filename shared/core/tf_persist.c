#include "tf_persist.h"
#include <string.h>

#define FRAME_MAGIC 0x47465254u   /* 'T''R''F''G' */
#define FRAME_VER   1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t body_len;
    uint32_t seq;
    int64_t  saved_at_ms;
    uint32_t crc;
    tf_save_t body;
} frame_t;

static uint32_t crc32_run(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc;
}

/* CRC covers the whole frame except the crc field itself; the frame is always
   zeroed before filling so padding bytes stay deterministic. */
static uint32_t frame_crc(const frame_t *f)
{
    const uint8_t *b = (const uint8_t *)f;
    size_t head = offsetof(frame_t, crc);
    return crc32_run(crc32_run(0xFFFFFFFFu, b, head),
                     b + head + sizeof(uint32_t), sizeof(frame_t) - head - sizeof(uint32_t)) ^ 0xFFFFFFFFu;
}

static bool frame_valid(const frame_t *f)
{
    if (f->magic != FRAME_MAGIC || f->version != FRAME_VER || f->body_len != sizeof(tf_save_t))
        return false;
    if (f->crc != frame_crc(f))
        return false;
    const tf_save_t *g = &f->body;
    /* trip_tier selects the card pool in tf_core_tick(): a negative tier empties the pool
       and the reward draw then evaluates rnd() % 0, so a torn or edited frame has to be
       rejected here rather than trusted. The rest of the blob degrades harmlessly. */
    return g->state >= TF_STATE_HOME && g->state <= TF_STATE_ANGRY
           && g->clovers >= 0
           && g->trip_tier >= 0 && g->trip_tier <= 2
           && g->last_reward_card >= -1 && g->last_reward_card < TF_NUM_CARDS;
}

size_t tf_persist_frame_size(void)
{
    return sizeof(frame_t);
}

bool tf_persist_load(tf_save_t *game, tf_save_meta_t *meta, tf_slot_read_t read)
{
    bool found = false;
    frame_t best;
    memset(&best, 0, sizeof(best));

    for (int slot = 0; slot < TF_SAVE_SLOTS; slot++) {
        frame_t f;
        size_t len = 0;
        memset(&f, 0, sizeof(f));
        if (!read(slot, &f, sizeof(f), &len) || len != sizeof(f)) continue;
        if (!frame_valid(&f)) continue;
        if (!found || f.seq > best.seq) { best = f; found = true; }
    }
    if (!found) return false;

    *game = best.body;
    meta->seq = best.seq;
    meta->saved_at_ms = best.saved_at_ms;
    return true;
}

bool tf_persist_store(const tf_save_t *game, tf_save_meta_t *meta, tf_slot_write_t write)
{
    frame_t f;
    memset(&f, 0, sizeof(f));
    f.magic = FRAME_MAGIC;
    f.version = FRAME_VER;
    f.body_len = sizeof(tf_save_t);
    f.seq = meta->seq + 1;
    f.saved_at_ms = meta->saved_at_ms;
    f.body = *game;
    f.crc = frame_crc(&f);

    if (!write((int)(f.seq % TF_SAVE_SLOTS), &f, sizeof(f))) return false;
    meta->seq = f.seq;
    return true;
}
