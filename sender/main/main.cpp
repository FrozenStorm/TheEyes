// Face tracker for the XIAO ESP32S3 Sense: detects faces with esp-dl and sends the
// look-at point to the eye board via ESP-NOW broadcast (see common/gaze_msg.h).
#include <cmath>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "gaze_msg.h"
#if !CONFIG_SENDER_TEST_PATTERN
#include "esp_camera.h"
#include "human_face_detect.hpp"
#endif

static const char *TAG = "FaceSender";

#ifdef CONFIG_SENDER_HMIRROR
static constexpr int HMIRROR = 1;
#else
static constexpr int HMIRROR = 0;
#endif
#ifdef CONFIG_SENDER_VFLIP
static constexpr int VFLIP = 1;
#else
static constexpr int VFLIP = 0;
#endif

static const uint8_t BROADCAST[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t s_seq;

static void espnow_init(void)
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
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_SENDER_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST, ESP_NOW_ETH_ALEN);
    peer.channel = CONFIG_SENDER_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_LOGI(TAG, "ESP-NOW broadcasting on channel %d", CONFIG_SENDER_CHANNEL);
}

static int16_t to_norm(float v)   // 0..1 -> -1000..1000
{
    float n = (v * 2.0f - 1.0f) * 1000.0f;
    return (int16_t)(n < -1000 ? -1000 : (n > 1000 ? 1000 : n));
}

static void send_target(int faces, float x01, float y01, float size01, float score)
{
    gaze_msg_t msg = {};
    msg.magic = GAZE_MSG_MAGIC;
    msg.version = GAZE_MSG_VERSION;
    msg.seq = s_seq++;
    msg.faces = (uint8_t)(faces > 255 ? 255 : faces);
    if (faces > 0) {
        msg.x = to_norm(x01);
        msg.y = to_norm(y01);
        msg.size = (uint16_t)(size01 * 1000.0f);
        msg.score = (uint8_t)(score * 100.0f);
    }
    esp_err_t err = esp_now_send(BROADCAST, (const uint8_t *)&msg, sizeof(msg));
    if (err != ESP_OK) ESP_LOGW(TAG, "esp_now_send: %s", esp_err_to_name(err));
}

#if CONFIG_SENDER_TEST_PATTERN

// No camera: a slowly wandering "face" that disappears for 4 s every 20 s, to test the eye board
static void run(void)
{
    ESP_LOGW(TAG, "Test pattern mode - camera not used");
    while (true) {
        float t = esp_timer_get_time() * 1e-6f;
        bool visible = fmodf(t, 20.0f) < 16.0f;
        float x = 0.5f + 0.35f * sinf(t * 0.5f);
        float y = 0.5f + 0.25f * sinf(t * 0.8f);
        send_target(visible ? 1 : 0, x, y, 0.25f, 0.9f);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

#else

static bool camera_init(void)
{
    // XIAO ESP32S3 Sense camera pins
    camera_config_t cfg = {};
    cfg.pin_pwdn = -1;
    cfg.pin_reset = -1;
    cfg.pin_xclk = 10;
    cfg.pin_sccb_sda = 40;
    cfg.pin_sccb_scl = 39;
    cfg.pin_d7 = 48;
    cfg.pin_d6 = 11;
    cfg.pin_d5 = 12;
    cfg.pin_d4 = 14;
    cfg.pin_d3 = 16;
    cfg.pin_d2 = 18;
    cfg.pin_d1 = 17;
    cfg.pin_d0 = 15;
    cfg.pin_vsync = 38;
    cfg.pin_href = 47;
    cfg.pin_pclk = 13;
    cfg.xclk_freq_hz = 20000000;
    cfg.ledc_timer = LEDC_TIMER_0;
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.pixel_format = PIXFORMAT_RGB565;
    cfg.frame_size = FRAMESIZE_QVGA;      // 320x240 is plenty for face detection
    cfg.jpeg_quality = 12;
    cfg.fb_count = 2;
    cfg.fb_location = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        return false;
    }
    sensor_t *s = esp_camera_sensor_get();
    s->set_hmirror(s, HMIRROR);
    s->set_vflip(s, VFLIP);
    ESP_LOGI(TAG, "Camera ready (sensor PID 0x%x)", s->id.PID);
    return true;
}

static void run(void)
{
    if (!camera_init()) {
        while (true) vTaskDelay(portMAX_DELAY);
    }
    HumanFaceDetect *detect = new HumanFaceDetect();

    float prev_x = 0.5f, prev_y = 0.5f;
    bool have_prev = false;
    int frames = 0;
    int64_t t_log = esp_timer_get_time();

    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "Frame capture failed");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        const float w = fb->width, h = fb->height;
        dl::image::img_t img = {
            .data = fb->buf,
            .width = (uint16_t)fb->width,
            .height = (uint16_t)fb->height,
            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE,
        };
        std::list<dl::detect::result_t> &faces = detect->run(img);
        esp_camera_fb_return(fb);

        // Pick a face: stay with the one closest to the previous target so the eyes do not
        // jump between people, unless another face is clearly bigger (closer).
        const dl::detect::result_t *best = nullptr;
        float best_rank = -1e9f;
        for (const auto &f : faces) {
            float fw = (f.box[2] - f.box[0]) / w;
            float cx = (f.box[0] + f.box[2]) * 0.5f / w, cy = (f.box[1] + f.box[3]) * 0.5f / h;
            float dist = have_prev ? hypotf(cx - prev_x, cy - prev_y) : 0;
            float rank = fw - 0.8f * dist;
            if (rank > best_rank) {
                best_rank = rank;
                best = &f;
            }
        }

        if (best) {
            float fw = (best->box[2] - best->box[0]) / w;
            float x, y;
            const std::vector<int> &kp = best->keypoint;
            if (kp.size() >= 10) {
                // 5 keypoints (eyes, nose, mouth corners). In an upright face the two highest
                // ones are the eyes - look between them.
                int a = -1, b = -1;
                for (int i = 0; i < 5; i++) {
                    if (a < 0 || kp[2 * i + 1] < kp[2 * a + 1]) {
                        b = a;
                        a = i;
                    } else if (b < 0 || kp[2 * i + 1] < kp[2 * b + 1]) {
                        b = i;
                    }
                }
                x = (kp[2 * a] + kp[2 * b]) * 0.5f / w;
                y = (kp[2 * a + 1] + kp[2 * b + 1]) * 0.5f / h;
            } else {
                x = (best->box[0] + best->box[2]) * 0.5f / w;
                y = (best->box[1] + 0.4f * (best->box[3] - best->box[1])) / h;
            }
            prev_x = x;
            prev_y = y;
            have_prev = true;
            send_target((int)faces.size(), x, y, fw, best->score);
        } else {
            have_prev = false;
            send_target(0, 0, 0, 0, 0);
        }

        frames++;
        int64_t now = esp_timer_get_time();
        if (now - t_log >= 5000000) {
            if (best) {
                ESP_LOGI(TAG, "%.1f fps, %d face(s), target x=%.2f y=%.2f", frames * 1e6f / (now - t_log),
                         (int)faces.size(), prev_x, prev_y);
            } else {
                ESP_LOGI(TAG, "%.1f fps, no face", frames * 1e6f / (now - t_log));
            }
            frames = 0;
            t_log = now;
        }
    }
}

#endif

extern "C" void app_main(void)
{
    espnow_init();
    run();
}
