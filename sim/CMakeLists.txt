cmake_minimum_required(VERSION 3.16)
project(travel_frog_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)

# Offline builds: unpack the two release archives under build/_deps_manual/ and they are
# used instead of the network. FetchContent resolves SOURCE_DIR at configure time, so the
# choice has to be made here rather than delegated to it.
set(TF_DEPS_MANUAL ${CMAKE_CURRENT_SOURCE_DIR}/build/_deps_manual)
if(EXISTS ${TF_DEPS_MANUAL}/lvgl-release-v9.3 AND EXISTS ${TF_DEPS_MANUAL}/SDL2-2.32.8)
    FetchContent_Declare(lvgl SOURCE_DIR ${TF_DEPS_MANUAL}/lvgl-release-v9.3)
    FetchContent_Declare(sdl2 SOURCE_DIR ${TF_DEPS_MANUAL}/SDL2-2.32.8)
else()
    FetchContent_Declare(lvgl
        GIT_REPOSITORY https://github.com/lvgl/lvgl.git
        GIT_TAG        v9.3.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_Declare(sdl2
        URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.8/SDL2-devel-2.32.8-VC.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
endif()

# v9.3 renamed these from LV_CONF_BUILD_DISABLE_{DEMOS,EXAMPLES}; under the old names both
# stay ON, which wastes a lot of compile time and fails outright on lv_example_scroll_8.c
# (undeclared 'total').
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "")
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "")
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE STRING "" FORCE)
FetchContent_MakeAvailable(lvgl sdl2)

target_include_directories(lvgl PUBLIC ${sdl2_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/shim ${CMAKE_CURRENT_SOURCE_DIR})

# LVGL adds dllimport for MSVC consumers; we link it statically, so drop that define.
get_property(lvgl_if TARGET lvgl PROPERTY INTERFACE_COMPILE_DEFINITIONS)
list(FILTER lvgl_if EXCLUDE REGEX "LV_ATTRIBUTE_EXTERN_DATA.*")
set_property(TARGET lvgl PROPERTY INTERFACE_COMPILE_DEFINITIONS ${lvgl_if})

add_executable(travel_frog_sim
    main.c
    tf_port_sim.c
    tf_batt_sim.c
    tf_assets_sim.c
    tf_snd_sdl.c
    sim_clock.c
    ../shared/core/tf_core.c
    ../shared/core/tf_persist.c
    ../shared/core/tf_sfx.c
    ../shared/ui/tf_ui.c
    ../shared/ui/tf_font_cn_16.c
    ../shared/ui/tf_font_cn_20.c
)

target_include_directories(travel_frog_sim PRIVATE
    ../shared/core
    ../shared/ui
    ${sdl2_SOURCE_DIR}/include
)
target_link_directories(travel_frog_sim PRIVATE ${sdl2_SOURCE_DIR}/lib/x64)
target_link_libraries(travel_frog_sim PRIVATE lvgl SDL2 SDL2main)
target_compile_definitions(travel_frog_sim PRIVATE TF_HAS_ASSETS
    "TF_ASSET_DIR=\"E:/HardWork/travel-frog/assets/ui/\""
    "TF_AUDIO_DIR=\"E:/HardWork/travel-frog/assets/audio/\"")
if(MSVC)
    target_compile_options(travel_frog_sim PRIVATE /utf-8)
endif()

add_custom_command(TARGET travel_frog_sim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll
        $<TARGET_FILE_DIR:travel_frog_sim>
)
