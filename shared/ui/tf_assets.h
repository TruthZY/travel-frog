#pragma once

/* One asset table, two backends: the simulator returns "A:..." PNG paths decoded
   at runtime, the firmware returns &lv_image_dsc_t built by tools/gen_fw_assets.py.
   Both are what lv_image_set_src() takes, so tf_ui.c stays target-blind.
   Keep in sync with ASSETS in tools/gen_fw_assets.py. */

enum {
    TF_ASSET_BG,
    TF_ASSET_FROG_HOME,
    TF_ASSET_FROG_ANGRY,
    TF_ASSET_CLOVER,
    TF_ASSET_ICON_SHOP,
    TF_ASSET_ICON_GO,
    TF_ASSET_ICON_CARD,
    TF_ASSET_ICON_SPD,
    TF_ASSET_NUM
};

const void * tf_asset_src(int which);
