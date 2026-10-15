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

static const char *TAG = "tf_snd";

static QueueHandle_t            s_q;
static esp_codec_dev_handle_t   s_dev;
static uint8_t                 *s_chunk;

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

static void snd_task(void *arg)
{
    (void)arg;
    int id;
    for (;;) {
        if (xQueueReceive(s_q, &id, portMAX_DELAY) != pdTRUE) continue;

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

    s_dev = bsp_audio_codec_speaker_init();
    if (!s_dev) {
        ESP_LOGE(TAG, "codec speaker init failed");
        return;
    }

    const esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .sample_rate     = SFX_RATE,
    };
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed");
        s_dev = NULL;
        return;
    }
    esp_codec_dev_set_out_vol(s_dev, 78);
    esp_codec_dev_set_out_mute(s_dev, false);

    /* Internal RAM: this buffer is filled while SPIFFS runs a flash operation,
       and the cache is disabled for the duration of those. */
    s_chunk = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_q = xQueueCreate(8, sizeof(int));
    if (!s_chunk || !s_q) {
        ESP_LOGE(TAG, "audio buffers unavailable");
        return;
    }
    xTaskCreate(snd_task, "tf_snd", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "sound ready: %d Hz mono s16 from " SFX_DIR);
}

void tf_port_sfx(int id)
{
    if (!s_q || id < 0 || id >= TF_SFX_COUNT) return;
    if (xQueueSend(s_q, &id, 0) != pdTRUE)
        ESP_LOGD(TAG, "queue full, dropping %s", tf_sfx_file(id));
}
