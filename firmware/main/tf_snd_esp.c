#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "tf_port.h"

#define SFX_DIR     "/spiffs"
#define CHUNK_BYTES 2048                 /* 46 ms of audio per I2S write */
#define SFX_RATE    22050
#define SND_VOL     78

/* Control words share the sfx queue so that only snd_task ever touches s_dev: closing the
   codec from the LVGL task could otherwise land in the middle of a write. */
#define SND_QUIESCE (-1)
#define SND_RESUME  (-2)

static const char *TAG = "tf_snd";

static QueueHandle_t            s_q;
static esp_codec_dev_handle_t   s_dev;
static uint8_t                 *s_chunk;
static bool                     s_quiet;   /* touched by snd_task alone, so no lock */

/* Walk the RIFF chunks instead of trusting a 44-byte header: lv_font_conv-style
   tools and Python's wave module both insert optional chunks. */
static bool wav_data_span(FILE *f, uint32_t *bytes)
{
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
        return false;

    long off = 12;
    uint8_t ch[8];
    while (fseek(f, off, SEEK_SET) == 0 && fread(ch, 1, 8, f) == 8) {
        uint32_t sz = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8)
                    | ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (memcmp(ch, "data", 4) == 0) {
            *bytes = sz;
            return true;
        }
        off += 8 + sz + (sz & 1);        /* chunks are word aligned */
    }
    return false;
}

static bool codec_open(void)
{
    /* Not const: esp_codec_dev_open() takes a mutable sample-info pointer. */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .sample_rate     = SFX_RATE,
    };
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed");
        return false;
    }
    esp_codec_dev_set_out_vol(s_dev, SND_VOL);
    esp_codec_dev_set_out_mute(s_dev, false);
    return true;
}

/* Closing drops the I2S clocks and the ES8311's MCLK with them, which is worth doing
   before a sleep that returns rather than rebooting. */
static void codec_close(void)
{
    esp_codec_dev_set_out_mute(s_dev, true);
    esp_codec_dev_close(s_dev);
}

static void snd_task(void *arg)
{
    (void)arg;
    int id;
    for (;;) {
        if (xQueueReceive(s_q, &id, portMAX_DELAY) != pdTRUE) continue;
        /* Idempotent: a wake that follows an interrupted fade never closed anything, and
           reopening an already-open codec is not something the driver promises to forgive. */
        if (id == SND_QUIESCE) { if (!s_quiet) { codec_close(); s_quiet = true; }  continue; }
        if (id == SND_RESUME)  { if (s_quiet)  { codec_open();  s_quiet = false; } continue; }

        char path[64];
        snprintf(path, sizeof path, SFX_DIR "/%s.wav", tf_sfx_file(id));
        FILE *f = fopen(path, "rb");
        if (!f) {
            ESP_LOGW(TAG, "missing %s (is the storage partition flashed?)", path);
            continue;
        }

        uint32_t left;
        if (!wav_data_span(f, &left)) {
            ESP_LOGW(TAG, "bad wav: %s", path);
            fclose(f);
            continue;
        }

        while (left) {
            size_t n = left > CHUNK_BYTES ? CHUNK_BYTES : left;
            if (fread(s_chunk, 1, n, f) != n) break;
            esp_codec_dev_write(s_dev, s_chunk, (int)n);
            left -= n;
        }
        fclose(f);
    }
}

void tf_port_audio_init(void)
{
    /* A missing/unformatted image must not stop the game from booting. */
    esp_err_t err = bsp_spiffs_mount();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "spiffs mount failed: %s — sound disabled", esp_err_to_name(err));
        return;
    }

    /* Internal RAM: this buffer is filled while SPIFFS runs a flash operation,
       and the cache is disabled for the duration of those. Taken before the codec is
       opened, so running out of memory here cannot leave the I2S clocks running. */
    s_chunk = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_q = xQueueCreate(8, sizeof(int));
    if (!s_chunk || !s_q) {
        ESP_LOGE(TAG, "audio buffers unavailable");
        if (s_chunk) { heap_caps_free(s_chunk); s_chunk = NULL; }
        if (s_q) { vQueueDelete(s_q); s_q = NULL; }
        return;
    }

    s_dev = bsp_audio_codec_speaker_init();
    if (!s_dev) {
        ESP_LOGE(TAG, "codec speaker init failed");
        return;
    }
    if (!codec_open()) {
        s_dev = NULL;
        return;
    }
    if (xTaskCreate(snd_task, "tf_snd", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "audio task not created");
        codec_close();
        s_dev = NULL;
        return;
    }
    ESP_LOGI(TAG, "sound ready: %d Hz mono s16 from " SFX_DIR);
}

/* Queued rather than called directly, so s_dev stays owned by snd_task. Both are no-ops
   when sound never came up. The wait is bounded but not zero: a queue full of pending
   effects must not silently drop the quiesce, or tier 1 keeps the codec clocking. */
void tf_snd_quiesce(void)
{
    if (!s_q || !s_dev) return;
    const int cmd = SND_QUIESCE;
    xQueueSend(s_q, &cmd, pdMS_TO_TICKS(100));
}

void tf_snd_resume(void)
{
    if (!s_q || !s_dev) return;
    const int cmd = SND_RESUME;
    xQueueSend(s_q, &cmd, 0);
}

void tf_port_sfx(int id)
{
    if (!s_q || id < 0 || id >= TF_SFX_COUNT) return;
    if (xQueueSend(s_q, &id, 0) != pdTRUE)
        ESP_LOGD(TAG, "queue full, dropping %s", tf_sfx_file(id));
}
