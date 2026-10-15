#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TF_NUM_CARDS   12
#define TF_NUM_CLOVER  4

#define TF_EV_NONE          0u
#define TF_EV_TRIP_RETURN   (1u << 0)  /* frog auto-returned with rewards */
#define TF_EV_ANGRY_END     (1u << 1)
#define TF_EV_NEW_CARD      (1u << 2)
#define TF_EV_GIFT_CLOVERS  (1u << 3)

typedef enum { TF_STATE_HOME = 0, TF_STATE_OUT, TF_STATE_ANGRY } tf_state_t;
typedef enum { TF_ITEM_SANDWICH, TF_ITEM_CHARM, TF_ITEM_TENT, TF_ITEM_COUNT } tf_item_t;
typedef enum {
    TF_OK = 0,
    TF_ERR_NOT_HOME,
    TF_ERR_NO_SANDWICH,
    TF_ERR_NOT_ENOUGH_CLOVERS,
    TF_ERR_NOT_OUT,
} tf_err_t;

/* Plain serializable blob: sim stores a file, firmware stores an NVS blob. */
typedef struct {
    int32_t state;
    int32_t clovers;
    int32_t items[TF_ITEM_COUNT];
    int64_t trip_end_ms;
    int64_t angry_end_ms;
    int32_t trip_tier;               /* 0 near, 1 mid, 2 far (decides card pool) */
    int32_t cards[TF_NUM_CARDS];
    int64_t clover_ready_ms[TF_NUM_CLOVER];
    int32_t last_reward_card;        /* -1 when none */
    int32_t last_reward_clovers;
} tf_save_t;

void     tf_core_init(tf_save_t *s);
/* Seed the reward draw from `seed`. Call once at startup with tf_port_random_seed():
   without it every board plays the identical sequence, and because a deep-sleep wake is a
   reboot it would restart from the top on every trip. A zero seed is ignored. */
void     tf_core_seed(uint32_t seed);
uint32_t tf_core_tick(tf_save_t *s, int64_t now_ms);

tf_err_t tf_core_send(tf_save_t *s, int64_t now_ms);
tf_err_t tf_core_recall(tf_save_t *s, int64_t now_ms);
bool     tf_core_harvest(tf_save_t *s, int slot, int64_t now_ms);
tf_err_t tf_core_buy(tf_save_t *s, tf_item_t item);

tf_state_t tf_core_state(const tf_save_t *s);
int64_t    tf_core_trip_remaining_ms(const tf_save_t *s, int64_t now_ms);
bool       tf_core_clover_ready(const tf_save_t *s, int slot, int64_t now_ms);
int        tf_core_card_count(const tf_save_t *s);
const char *tf_core_card_name(int idx);
int        tf_core_item_price(tf_item_t item);
