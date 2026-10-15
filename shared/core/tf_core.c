#include "tf_core.h"
#include <string.h>

#define TRIP_BASE_MS      60000
#define CLOVER_REGROW_MS  20000
#define ANGRY_MS          30000
#define GIFT_CHANCE_PCT   50

static const char *CARD_NAMES[TF_NUM_CARDS] = {
    "溪边",     "落日",     "蘑菇",     "大河",
    "灯塔",     "温泉",     "古寺",     "海边",
    "雾峰",     "星空营地", "乌龟岛",   "彩虹",
};
static const uint8_t CARD_TIER[TF_NUM_CARDS] = {
    0, 0, 0, 0,
    1, 1, 1, 1,
    2, 2, 2, 2,
};

static const int ITEM_PRICES[TF_ITEM_COUNT] = { 10, 25, 20 };

/* xorshift32: cheap, and plenty for shuffling a 12-card pool. The fixed initial state is
   only a placeholder for the moment before tf_core_seed() runs. */
static uint32_t rng_s = 0x9e3779b9u;

void tf_core_seed(uint32_t seed)
{
    /* 0 is xorshift's one fixed point — it would return 0 forever and the frog would hand
       out the same card on every trip. */
    rng_s = seed ? seed : 0x9e3779b9u;
}

static uint32_t rnd(void)
{
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 17;
    rng_s ^= rng_s << 5;
    return rng_s;
}

void tf_core_init(tf_save_t *s)
{
    memset(s, 0, sizeof(*s));
    s->state = TF_STATE_HOME;
    s->clovers = 5;
    s->items[TF_ITEM_SANDWICH] = 1;
    s->last_reward_card = -1;
    /* clover_ready_ms all 0 => ready immediately */
}

tf_state_t tf_core_state(const tf_save_t *s) { return (tf_state_t)s->state; }

uint32_t tf_core_tick(tf_save_t *s, int64_t now_ms)
{
    uint32_t ev = TF_EV_NONE;

    switch (tf_core_state(s)) {
    case TF_STATE_OUT:
        if (now_ms >= s->trip_end_ms) {
            int tier = s->trip_tier;

            int pool[TF_NUM_CARDS];
            int n = 0;
            for (int i = 0; i < TF_NUM_CARDS; i++)
                if (CARD_TIER[i] <= tier)
                    pool[n++] = i;
            int pick = pool[rnd() % (unsigned)n];
            s->cards[pick]++;
            s->last_reward_card = pick;
            s->last_reward_clovers = 0;

            if ((int)(rnd() % 100) < GIFT_CHANCE_PCT) {
                int g = 10 + (int)(rnd() % 21);
                s->clovers += g;
                s->last_reward_clovers = g;
                ev |= TF_EV_GIFT_CLOVERS;
            }

            s->state = TF_STATE_HOME;
            s->trip_tier = 0;
            ev |= TF_EV_TRIP_RETURN | TF_EV_NEW_CARD;
        }
        break;

    case TF_STATE_ANGRY:
        if (now_ms >= s->angry_end_ms) {
            s->state = TF_STATE_HOME;
            ev |= TF_EV_ANGRY_END;
        }
        break;

    case TF_STATE_HOME:
    default:
        break;
    }

    return ev;
}

tf_err_t tf_core_send(tf_save_t *s, int64_t now_ms)
{
    if (tf_core_state(s) != TF_STATE_HOME)
        return TF_ERR_NOT_HOME;
    if (s->items[TF_ITEM_SANDWICH] < 1)
        return TF_ERR_NO_SANDWICH;

    s->items[TF_ITEM_SANDWICH]--;
    /* Only one item can set the trip tier, so only consume the best one in the bag.
       Taking a tent as well as a charm charged the player for an item that did nothing. */
    int tier = 0;
    if (s->items[TF_ITEM_CHARM] > 0) {
        s->items[TF_ITEM_CHARM]--;
        tier = 2;
    } else if (s->items[TF_ITEM_TENT] > 0) {
        s->items[TF_ITEM_TENT]--;
        tier = 1;
    }

    int len = TRIP_BASE_MS;
    if (tier >= 1) len *= 2;
    if (tier >= 2) len = len + len / 2;

    s->trip_tier = tier;
    s->trip_end_ms = now_ms + len;
    s->state = TF_STATE_OUT;
    return TF_OK;
}

tf_err_t tf_core_recall(tf_save_t *s, int64_t now_ms)
{
    if (tf_core_state(s) != TF_STATE_OUT)
        return TF_ERR_NOT_OUT;
    s->state = TF_STATE_ANGRY;
    s->angry_end_ms = now_ms + ANGRY_MS;
    s->trip_tier = 0;
    return TF_OK;
}

bool tf_core_harvest(tf_save_t *s, int slot, int64_t now_ms)
{
    if (slot < 0 || slot >= TF_NUM_CLOVER)
        return false;
    if (tf_core_state(s) == TF_STATE_OUT)
        return false;
    if (!tf_core_clover_ready(s, slot, now_ms))
        return false;

    s->clovers += 1;
    s->clover_ready_ms[slot] = now_ms + CLOVER_REGROW_MS;
    return true;
}

tf_err_t tf_core_buy(tf_save_t *s, tf_item_t item)
{
    if (item >= TF_ITEM_COUNT)
        return TF_ERR_NOT_ENOUGH_CLOVERS;
    int price = ITEM_PRICES[item];
    if (s->clovers < price)
        return TF_ERR_NOT_ENOUGH_CLOVERS;
    s->clovers -= price;
    s->items[item]++;
    return TF_OK;
}

int64_t tf_core_trip_remaining_ms(const tf_save_t *s, int64_t now_ms)
{
    if (tf_core_state(s) != TF_STATE_OUT)
        return 0;
    int64_t left = s->trip_end_ms - now_ms;
    return left < 0 ? 0 : left;
}

bool tf_core_clover_ready(const tf_save_t *s, int slot, int64_t now_ms)
{
    if (slot < 0 || slot >= TF_NUM_CLOVER)
        return false;
    return now_ms >= s->clover_ready_ms[slot];
}

int tf_core_card_count(const tf_save_t *s)
{
    int n = 0;
    for (int i = 0; i < TF_NUM_CARDS; i++)
        if (s->cards[i] > 0)
            n++;
    return n;
}

const char *tf_core_card_name(int idx)
{
    if (idx < 0 || idx >= TF_NUM_CARDS)
        return "?";
    return CARD_NAMES[idx];
}

int tf_core_item_price(tf_item_t item)
{
    if (item >= TF_ITEM_COUNT)
        return 0;
    return ITEM_PRICES[item];
}
