#include "tf_assets.h"
#include <stddef.h>

/* Simulator backend: LVGL decodes these PNGs at runtime through
   LV_USE_LODEPNG + LV_USE_FS_STDIO (drive letter 'A:').
   lv_image_set_src keeps the pointer, so every path must outlive the widget. */

#define ASSET(f) "A:" TF_ASSET_DIR f

const void * tf_asset_src(int which)
{
    static const void *const s_src[TF_ASSET_NUM] = {
        [TF_ASSET_BG]         = ASSET("garden_bg.png"),
        [TF_ASSET_FROG_HOME]  = ASSET("frog_home.png"),
        [TF_ASSET_FROG_ANGRY] = ASSET("frog_angry.png"),
        [TF_ASSET_CLOVER]     = ASSET("clover.png"),
        [TF_ASSET_ICON_SHOP]  = ASSET("icon_shop.png"),
        [TF_ASSET_ICON_GO]    = ASSET("icon_go.png"),
        [TF_ASSET_ICON_CARD]  = ASSET("icon_card.png"),
        [TF_ASSET_ICON_SPD]   = ASSET("icon_spd.png"),
    };
    return (which >= 0 && which < TF_ASSET_NUM) ? s_src[which] : NULL;
}
