/* Host tests for the platform-free core. Links the real shared/core sources directly —
   tf_core.c and tf_persist.c include nothing but their own headers and <string.h>, so no
   stubbing is involved and what runs here is what ships.

   Build (MSVC):  cl /nologo /W4 /Ishared/core tests/test_core.c shared/core/tf_core.c
                      shared/core/tf_persist.c /Fe:tests/test_core.exe
   Build (gcc):   cc -Wall -Wextra -Ishared/core -o tests/test_core tests/test_core.c
                      shared/core/tf_core.c shared/core/tf_persist.c
   Run from the repository root. */

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "tf_core.h"
#include "tf_persist.h"

static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void section(const char *name) { printf("%s\n", name); }

/* ---------- in-memory slots, standing in for files / NVS ---------- */

static uint8_t  slot_buf[TF_SAVE_SLOTS][1024];
static size_t   slot_len[TF_SAVE_SLOTS];
static bool     slot_fail_write;

static bool mem_read(int slot, void *buf, size_t buf_sz, size_t *out_len)
{
    if (slot_len[slot] == 0 || slot_len[slot] > buf_sz) return false;
    memcpy(buf, slot_buf[slot], slot_len[slot]);
    if (out_len) *out_len = slot_len[slot];
    return true;
}

static bool mem_write(int slot, const void *buf, size_t buf_sz)
{
    if (slot_fail_write) return false;
    memcpy(slot_buf[slot], buf, buf_sz);
    slot_len[slot] = buf_sz;
    return true;
}

static void mem_wipe(void)
{
    memset(slot_buf, 0, sizeof slot_buf);
    memset(slot_len, 0, sizeof slot_len);
    slot_fail_write = false;
}

/* ---------- B4: tf_core_send must not burn an item it does not use ---------- */

static void test_send_consumes_only_the_item_that_sets_the_tier(void)
{
    section("B4 send: only the tier-setting item is consumed");
    tf_save_t s;

    tf_core_init(&s);
    s.items[TF_ITEM_SANDWICH] = 1;
    s.items[TF_ITEM_TENT] = 1;
    s.items[TF_ITEM_CHARM] = 1;
    CHECK(tf_core_send(&s, 0) == TF_OK, "send with charm+tent should succeed");
    CHECK(s.trip_tier == 2, "tier=%d, want 2 (charm wins)", s.trip_tier);
    CHECK(s.items[TF_ITEM_CHARM] == 0, "charm not consumed: %d", s.items[TF_ITEM_CHARM]);
    CHECK(s.items[TF_ITEM_TENT] == 1, "tent wasted: %d left, want 1", s.items[TF_ITEM_TENT]);
    CHECK(s.items[TF_ITEM_SANDWICH] == 0, "sandwich not consumed");

    tf_core_init(&s);
    s.items[TF_ITEM_SANDWICH] = 1;
    s.items[TF_ITEM_TENT] = 1;
    CHECK(tf_core_send(&s, 0) == TF_OK, "send with tent should succeed");
    CHECK(s.trip_tier == 1, "tier=%d, want 1", s.trip_tier);
    CHECK(s.items[TF_ITEM_TENT] == 0, "tent not consumed");

    tf_core_init(&s);
    s.items[TF_ITEM_SANDWICH] = 1;
    s.items[TF_ITEM_CHARM] = 1;
    CHECK(tf_core_send(&s, 0) == TF_OK, "send with charm should succeed");
    CHECK(s.trip_tier == 2, "tier=%d, want 2", s.trip_tier);

    tf_core_init(&s);
    s.items[TF_ITEM_SANDWICH] = 1;
    CHECK(tf_core_send(&s, 0) == TF_OK, "bare send should succeed");
    CHECK(s.trip_tier == 0, "tier=%d, want 0", s.trip_tier);

    /* A rejected send must not eat anything. tf_core_init() hands out one sandwich, so
       take it away to reach the TF_ERR_NO_SANDWICH path. */
    tf_core_init(&s);
    s.items[TF_ITEM_SANDWICH] = 0;
    s.items[TF_ITEM_TENT] = 1;
    s.items[TF_ITEM_CHARM] = 1;
    CHECK(tf_core_send(&s, 0) == TF_ERR_NO_SANDWICH, "send without a sandwich should fail");
    CHECK(s.items[TF_ITEM_TENT] == 1 && s.items[TF_ITEM_CHARM] == 1,
          "failed send consumed an item");
    CHECK(tf_core_state(&s) == TF_STATE_HOME, "failed send changed state");

    /* Nor may one from the wrong state. */
    tf_core_init(&s);
    s.items[TF_ITEM_SANDWICH] = 2;
    s.items[TF_ITEM_CHARM] = 1;
    tf_core_send(&s, 0);
    CHECK(tf_core_send(&s, 1000) == TF_ERR_NOT_HOME, "second send while out should fail");
    CHECK(s.items[TF_ITEM_SANDWICH] == 1, "sandwiches=%d, want 1", s.items[TF_ITEM_SANDWICH]);
}

/* ---------- B3: the reward draw must actually depend on the seed ---------- */

/* Runs one full trip and returns the card it produced. */
static int one_trip(uint32_t seed)
{
    tf_save_t s;
    tf_core_seed(seed);
    tf_core_init(&s);
    s.items[TF_ITEM_SANDWICH] = 1;
    if (tf_core_send(&s, 0) != TF_OK) return -999;
    tf_core_tick(&s, s.trip_end_ms);
    return s.last_reward_card;
}

static void test_seeding(void)
{
    section("B3 seed: the reward draw varies with the seed and is stable within one");

    /* Same seed => same sequence. */
    CHECK(one_trip(12345) == one_trip(12345), "same seed gave different cards");

    /* Different seeds => different sequences. Compare a run of draws, not a single one:
       any individual draw can coincide by chance. */
    int a[40], b[40];
    tf_save_t s1, s2;
    tf_core_seed(0x11111111u);
    tf_core_init(&s1);
    tf_core_seed(0x22222222u);
    tf_core_init(&s2);
    int same = 0;
    for (int i = 0; i < 40; i++) {
        s1.items[TF_ITEM_SANDWICH] = 1;
        s2.items[TF_ITEM_SANDWICH] = 1;
        tf_core_send(&s1, i * 100000);
        tf_core_send(&s2, i * 100000);
        tf_core_tick(&s1, s1.trip_end_ms);
        tf_core_tick(&s2, s2.trip_end_ms);
        a[i] = s1.last_reward_card;
        b[i] = s2.last_reward_card;
        if (a[i] == b[i]) same++;
    }
    CHECK(same < 40, "two different seeds produced an identical 40-trip sequence");
    printf("      (seed A vs seed B agreed on %d/40 draws; a fixed state gives 40)\n", same);

    /* Every card index drawn must be inside the pool the tier allows. */
    for (int i = 0; i < 40; i++) {
        CHECK(a[i] >= 0 && a[i] < TF_NUM_CARDS, "card %d out of range", a[i]);
        CHECK(b[i] >= 0 && b[i] < TF_NUM_CARDS, "card %d out of range", b[i]);
    }

    /* A zero seed is the one value that would wedge xorshift at 0 forever. */
    CHECK(one_trip(0) >= 0, "seed 0 produced no card (xorshift fell into its fixed point)");
    CHECK(one_trip(0) == one_trip(0), "seed 0 is not deterministic");
}

/* ---------- B5: frame_valid ---------- */

/* The frame layout is private to tf_persist.c, so rather than duplicate the struct the
   offsets are confirmed against a real frame: crc32 over the frame with the 4-byte crc
   field skipped must equal the u32 sitting at CRC_OFF. */
#define CRC_OFF   24
#define BODY_OFF  32

static uint32_t crc32_run(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc;
}

static uint32_t frame_crc_at(const uint8_t *f, size_t sz)
{
    return crc32_run(crc32_run(0xFFFFFFFFu, f, CRC_OFF),
                     f + CRC_OFF + 4, sz - CRC_OFF - 4) ^ 0xFFFFFFFFu;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* Store a save, then hand back the raw frame so a test can mutate it. */
static size_t make_frame(const tf_save_t *game, uint8_t *out)
{
    mem_wipe();
    tf_save_meta_t meta = { .saved_at_ms = 1000, .seq = 0 };
    if (!tf_persist_store(game, &meta, mem_write)) return 0;
    for (int i = 0; i < TF_SAVE_SLOTS; i++)
        if (slot_len[i]) { memcpy(out, slot_buf[i], slot_len[i]); return slot_len[i]; }
    return 0;
}

/* Write a mutated frame into slot 0 with its CRC repaired, so only the semantic
   validators can reject it. */
static bool load_mutated(const uint8_t *frame, size_t sz, size_t body_field, int32_t value)
{
    uint8_t f[1024];
    memcpy(f, frame, sz);
    wr32(f + BODY_OFF + body_field, (uint32_t)value);
    wr32(f + CRC_OFF, frame_crc_at(f, sz));
    mem_wipe();
    memcpy(slot_buf[0], f, sz);
    slot_len[0] = sz;
    tf_save_t out;
    tf_save_meta_t meta;
    return tf_persist_load(&out, &meta, mem_read);
}

static void test_persist_validation(void)
{
    section("B5 frame_valid: corrupt frames are rejected, honest ones survive");

    tf_save_t good;
    tf_core_init(&good);
    good.clovers = 42;
    good.state = TF_STATE_OUT;
    good.trip_tier = 2;
    good.trip_end_ms = 123456;
    good.last_reward_card = 7;

    uint8_t frame[1024];
    const size_t sz = make_frame(&good, frame);
    CHECK(sz == tf_persist_frame_size(), "frame size %zu != %zu", sz, tf_persist_frame_size());
    CHECK(sz + 512 <= sizeof slot_buf, "test slot buffer too small");

    /* Confirm the offsets and the CRC before trusting any mutation below. */
    CHECK(rd32(frame + CRC_OFF) == frame_crc_at(frame, sz),
          "CRC_OFF %d is wrong, or the CRC algorithm differs", CRC_OFF);

    /* A clean frame loads back byte-identical. */
    mem_wipe();
    tf_save_meta_t meta = { .saved_at_ms = 1000, .seq = 0 };
    CHECK(tf_persist_store(&good, &meta, mem_write), "store failed");
    CHECK(meta.seq == 1, "seq not bumped: %u", meta.seq);
    tf_save_t back;
    tf_save_meta_t m2;
    CHECK(tf_persist_load(&back, &m2, mem_read), "clean frame did not load");
    CHECK(memcmp(&back, &good, sizeof good) == 0, "round-trip changed the save");
    CHECK(m2.saved_at_ms == 1000, "saved_at_ms lost");

    const size_t off_tier  = offsetof(tf_save_t, trip_tier);
    const size_t off_state = offsetof(tf_save_t, state);
    const size_t off_clov  = offsetof(tf_save_t, clovers);
    const size_t off_card  = offsetof(tf_save_t, last_reward_card);

    /* trip_tier selects the card pool in tf_core_tick(); a negative one empties the pool
       and the reward draw evaluates rnd() % 0. This is the crash this check exists for. */
    CHECK(!load_mutated(frame, sz, off_tier, -1), "trip_tier=-1 accepted (rnd()%%0 on return)");
    CHECK(!load_mutated(frame, sz, off_tier, -1000), "trip_tier=-1000 accepted");
    CHECK(!load_mutated(frame, sz, off_tier, 3), "trip_tier=3 accepted");
    CHECK(load_mutated(frame, sz, off_tier, 0), "trip_tier=0 rejected");
    CHECK(load_mutated(frame, sz, off_tier, 2), "trip_tier=2 rejected");

    CHECK(!load_mutated(frame, sz, off_state, -1), "state=-1 accepted");
    CHECK(!load_mutated(frame, sz, off_state, 3), "state=3 accepted");
    CHECK(load_mutated(frame, sz, off_state, TF_STATE_ANGRY), "state=ANGRY rejected");

    CHECK(!load_mutated(frame, sz, off_clov, -1), "clovers=-1 accepted");
    CHECK(load_mutated(frame, sz, off_clov, 0), "clovers=0 rejected");

    CHECK(load_mutated(frame, sz, off_card, -1), "last_reward_card=-1 rejected");
    CHECK(load_mutated(frame, sz, off_card, 0), "last_reward_card=0 rejected");
    CHECK(load_mutated(frame, sz, off_card, TF_NUM_CARDS - 1), "last card index rejected");
    CHECK(!load_mutated(frame, sz, off_card, TF_NUM_CARDS),
          "last_reward_card=TF_NUM_CARDS accepted (one past the end)");
    CHECK(!load_mutated(frame, sz, off_card, -1000000), "wildly negative card accepted");

    /* A body edit that does not repair the CRC must be caught by the CRC. */
    {
        uint8_t f[1024];
        memcpy(f, frame, sz);
        f[BODY_OFF + off_clov] ^= 0xFF;
        mem_wipe();
        memcpy(slot_buf[0], f, sz);
        slot_len[0] = sz;
        tf_save_t out; tf_save_meta_t m;
        CHECK(!tf_persist_load(&out, &m, mem_read), "bit-flip with a stale CRC was accepted");
    }
}

static void test_persist_slots(void)
{
    section("persist: slot rotation, torn writes, and a failing store");

    tf_save_t a, b;
    tf_core_init(&a);
    tf_core_init(&b);
    a.clovers = 11;
    b.clovers = 22;

    mem_wipe();
    tf_save_meta_t meta = { .saved_at_ms = 500, .seq = 0 };
    CHECK(tf_persist_store(&a, &meta, mem_write), "store 1 failed");
    const uint32_t seq1 = meta.seq;
    CHECK(tf_persist_store(&b, &meta, mem_write), "store 2 failed");
    CHECK(meta.seq == seq1 + 1, "seq did not advance");
    CHECK(slot_len[seq1 % TF_SAVE_SLOTS] && slot_len[meta.seq % TF_SAVE_SLOTS],
          "the two stores did not land in different slots");

    tf_save_t out; tf_save_meta_t m;
    CHECK(tf_persist_load(&out, &m, mem_read), "load failed");
    CHECK(out.clovers == 22, "newest frame did not win: clovers=%d", out.clovers);
    CHECK(m.seq == meta.seq, "seq not restored");

    /* Corrupting the newer slot must fall back to the older one, not fail outright. */
    slot_buf[meta.seq % TF_SAVE_SLOTS][BODY_OFF] ^= 0x5A;
    CHECK(tf_persist_load(&out, &m, mem_read), "load failed with one slot corrupt");
    CHECK(out.clovers == 11, "did not fall back to the older slot: clovers=%d", out.clovers);

    /* Both gone => report failure rather than a zeroed save. */
    mem_wipe();
    CHECK(!tf_persist_load(&out, &m, mem_read), "empty storage reported a save");

    /* A store that cannot write must not advance seq, or the caller believes it saved. */
    mem_wipe();
    tf_save_meta_t m3 = { .saved_at_ms = 1, .seq = 7 };
    slot_fail_write = true;
    CHECK(!tf_persist_store(&a, &m3, mem_write), "failing store reported success");
    CHECK(m3.seq == 7, "seq advanced on a failed store: %u", m3.seq);
}

/* ---------- regression guard for the round screen's reward flow ---------- */

static void test_trip_flow(void)
{
    section("core: trip, recall, harvest and buy boundaries");

    tf_save_t s;
    tf_core_seed(0xC0FFEEu);
    tf_core_init(&s);
    CHECK(s.last_reward_card == -1, "init should leave last_reward_card at -1");
    CHECK(s.clovers == 5, "init clovers=%d", s.clovers);

    s.items[TF_ITEM_SANDWICH] = 1;
    tf_core_send(&s, 1000);
    CHECK(tf_core_state(&s) == TF_STATE_OUT, "send did not set OUT");
    CHECK(tf_core_trip_remaining_ms(&s, 1000) > 0, "no time left on a trip just sent");
    CHECK(tf_core_trip_remaining_ms(&s, s.trip_end_ms) == 0, "time left at trip_end_ms");
    CHECK(tf_core_trip_remaining_ms(&s, s.trip_end_ms + 1) == 0,
          "remaining went negative past trip_end_ms");

    /* Recall parks the frog in ANGRY and clears the tier. */
    CHECK(tf_core_recall(&s, 2000) == TF_OK, "recall failed");
    CHECK(tf_core_state(&s) == TF_STATE_ANGRY, "recall did not set ANGRY");
    CHECK(s.trip_tier == 0, "recall left trip_tier at %d", s.trip_tier);
    CHECK(tf_core_recall(&s, 2000) == TF_ERR_NOT_OUT, "second recall should fail");

    /* Harvest bounds. */
    tf_core_init(&s);
    CHECK(tf_core_harvest(&s, -1, 0) == false, "harvest slot -1 accepted");
    CHECK(tf_core_harvest(&s, TF_NUM_CLOVER, 0) == false, "harvest slot N accepted");
    CHECK(tf_core_harvest(&s, 0, 0) == true, "harvest of a ready clover failed");
    CHECK(s.clovers == 6, "clovers=%d after harvest, want 6", s.clovers);
    CHECK(tf_core_harvest(&s, 0, 1) == false, "clover regrew instantly");

    /* Buy bounds. */
    tf_core_init(&s);
    s.clovers = 10;
    CHECK(tf_core_buy(&s, TF_ITEM_COUNT) == TF_ERR_NOT_ENOUGH_CLOVERS,
          "out-of-range item accepted");
    CHECK(tf_core_buy(&s, TF_ITEM_SANDWICH) == TF_OK, "affordable buy failed");
    CHECK(s.clovers == 0, "clovers=%d after buying a 10-cost item", s.clovers);
    CHECK(tf_core_buy(&s, TF_ITEM_SANDWICH) == TF_ERR_NOT_ENOUGH_CLOVERS,
          "bought with no clovers");

    /* Card name lookup must not read out of bounds. */
    CHECK(strcmp(tf_core_card_name(-1), "?") == 0, "card_name(-1) not guarded");
    CHECK(strcmp(tf_core_card_name(TF_NUM_CARDS), "?") == 0, "card_name(N) not guarded");
    CHECK(tf_core_card_name(0) != NULL, "card_name(0) null");
}

int main(void)
{
    test_send_consumes_only_the_item_that_sets_the_tier();
    test_seeding();
    test_persist_validation();
    test_persist_slots();
    test_trip_flow();
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
