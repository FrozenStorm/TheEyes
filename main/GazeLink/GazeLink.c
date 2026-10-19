#include "GazeLink.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "gaze_msg.h"
#include "Eye_Textures.h"

static const char *TAG_LINK = "GazeLink";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static gaze_target_t s_target;
static bool s_valid;

#if CONFIG_GAZE_LINK_INVERT_X
#define SIGN_X (-1.0f)
#else
#define SIGN_X (1.0f)
#endif
#if CONFIG_GAZE_LINK_INVERT_Y
#define SIGN_Y (-1.0f)
#else
#define SIGN_Y (1.0f)
#endif

// Runs in the Wi-Fi task
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len != sizeof(gaze_msg_t)) return;
    gaze_msg_t msg;
    memcpy(&msg, data, sizeof(msg));
    if (msg.magic != GAZE_MSG_MAGIC || msg.version != GAZE_MSG_VERSION) return;

    // Camera coordinates -> eye gaze pixels. The camera sits between the eyes, so the
    // full image width maps to the full gaze range at 100 % gain.
    gaze_target_t t = {
        .face = msg.faces > 0,
        .x = SIGN_X * msg.x / 1000.0f * EYE_GAZE_MAX * CONFIG_GAZE_LINK_GAIN_X / 100.0f,
        .y = SIGN_Y * msg.y / 1000.0f * EYE_GAZE_MAX * CONFIG_GAZE_LINK_GAIN_Y / 100.0f,
        .size = msg.size / 1000.0f,
        .time_us = esp_timer_get_time(),
    };

    taskENTER_CRITICAL(&s_mux);
    s_target = t;
    s_valid = true;
    taskEXIT_CRITICAL(&s_mux);
}

bool gaze_link_get(gaze_target_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_target;
    bool valid = s_valid;
    taskEXIT_CRITICAL(&s_mux);
    return valid;
}

void GazeLink_Init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_GAZE_LINK_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ESP_LOGI(TAG_LINK, "Listening for ESP-NOW gaze packets on channel %d", CONFIG_GAZE_LINK_CHANNEL);
}
