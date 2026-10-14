#include <SDL.h>
#include <stdbool.h>
#include "lvgl.h"
#include "tf_core.h"
#include "tf_port.h"

extern void tf_port_sim_init(void);

int main(void)
{
    lv_init();
    lv_display_t *disp = lv_sdl_window_create(360, 360);
    if (!disp) return 1;
    lv_sdl_mouse_create();
    tf_port_sim_init();

    static tf_save_t save;
    if (!tf_port_load(&save)) {
        tf_core_init(&save);
        tf_port_store(&save);
    }

    tf_ui_create(lv_screen_active(), &save);

    uint64_t last = SDL_GetTicks64();
    while (SDL_WasInit(SDL_INIT_VIDEO)) {
        uint32_t wait = lv_timer_handler();
        SDL_Delay(wait > 20 ? 20 : (wait ? wait : 1));
        uint64_t now = SDL_GetTicks64();
        lv_tick_inc((uint32_t)(now - last));
        last = now;
    }
    return 0;
}
