#include "RealEyes.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "LCD_Driver.h"
#include "Eye_Textures.h"
#include "Eye_Motion.h"

static const char *TAG_EYES = "RealEyes";

#define C                 (EYE_SCREEN / 2)
#define STRIP_H           20                      // lines per DMA transfer
#define N_STRIPS          (EYE_SCREEN / STRIP_H)

// Lid geometry: edge height above/below the screen centre at the middle column
#define LID_UPPER_OPEN    92
#define LID_LOWER_OPEN    86
#define LID_CURVE         0.62f                   // how far the lid edge drops towards the sides
#define LID_TRACK_UP      0.75f                   // upper lid follows vertical gaze
#define LID_TRACK_LOW     0.35f
#define LID_LUT_N         64
#define UPPER_SHADOW_PX   14
#define LOWER_SHADOW_PX   5

// Corneal reflection: fixed on screen, because the light source does not move with the eye
#define HL_X              (C - 30)
#define HL_Y              (C - 32)
#define HL_BOX            36

#define VIGNETTE_N        ((2 * C * C >> 6) + 1)
#define FEATHER_N         (EYE_POLAR_OUT - EYE_LIMBUS_D)

typedef struct {
    int icx, icy;                     // iris centre on screen
    int scl_ox, scl_oy;               // sclera texture offset
    int32_t m00, m01, m11;            // iris foreshortening (symmetric 2x2, Q12)
    uint8_t pupil_lut[256];           // polar distance -> iris ring, 0xFF = pupil
    int16_t lid_up[EYE_SCREEN];       // first visible eyeball row per column
    int16_t lid_low[EYE_SCREEN];      // last visible eyeball row per column
} frame_t;

typedef struct {
    int index;
    esp_lcd_panel_handle_t panel;
    uint16_t *buf[2];
    frame_t frame;
} eye_ctx_t;

static eye_textures_t s_tex;
static eye_ctx_t s_eyes[2];
static uint16_t s_lid_up_lut[LID_LUT_N], s_lid_low_lut[LID_LUT_N];
static uint8_t s_shadow_up[UPPER_SHADOW_PX], s_shadow_low[LOWER_SHADOW_PX];
static uint8_t s_vignette[VIGNETTE_N];
static uint8_t s_feather[FEATHER_N];
static uint8_t s_highlight[2 * HL_BOX][2 * HL_BOX];
static uint16_t s_pupil_color;

/**********************
 * RGB565 helpers (alpha 0..32)
 **********************/
static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint32_t alpha)
{
    uint32_t b = (bg | ((uint32_t)bg << 16)) & 0x07E0F81F;
    uint32_t f = (fg | ((uint32_t)fg << 16)) & 0x07E0F81F;
    uint32_t r = ((((f - b) * alpha) >> 5) + b) & 0x07E0F81F;
    return (uint16_t)((r >> 16) | r);
}

static inline uint16_t shade565(uint16_t c, uint32_t s)
{
    uint32_t x = (c | ((uint32_t)c << 16)) & 0x07E0F81F;
    x = ((x * s) >> 5) & 0x07E0F81F;
    return (uint16_t)((x >> 16) | x);
}

static float smooth01(float t)
{
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t * t * (3 - 2 * t);
}

static uint16_t lerp565(float r0, float g0, float b0, float r1, float g1, float b1, float t)
{
    return eye_rgb565(r0 + (r1 - r0) * t, g0 + (g1 - g0) * t, b0 + (b1 - b0) * t);
}

/**********************
 * Static lookup tables
 **********************/
static void init_luts(void)
{
    const float skin[3] = {0.78f, 0.57f, 0.47f};
    const float lash[3] = {0.10f, 0.07f, 0.06f};
    const float rim[3]  = {0.80f, 0.52f, 0.50f};   // moist lid margin

    // Upper lid, by distance from its edge: margin, lash line, then skin getting lighter
    for (int e = 0; e < LID_LUT_N; e++) {
        if (e < 1) {
            s_lid_up_lut[e] = lerp565(rim[0], rim[1], rim[2], lash[0], lash[1], lash[2], 0.5f);
        } else if (e < 6) {
            s_lid_up_lut[e] = eye_rgb565(lash[0], lash[1], lash[2]);
        } else {
            float t = smooth01((e - 6) / 10.0f), s = 0.85f + 0.15f * (e - 6) / (float)(LID_LUT_N - 6);
            s_lid_up_lut[e] = lerp565(lash[0], lash[1], lash[2], skin[0] * s, skin[1] * s, skin[2] * s, t);
        }
    }
    // Lower lid: waterline, thin lash line, skin
    for (int e = 0; e < LID_LUT_N; e++) {
        if (e < 2) {
            s_lid_low_lut[e] = eye_rgb565(rim[0], rim[1], rim[2]);
        } else if (e < 4) {
            s_lid_low_lut[e] = eye_rgb565(0.18f, 0.12f, 0.10f);
        } else {
            float t = smooth01((e - 4) / 7.0f);
            s_lid_low_lut[e] = lerp565(0.18f, 0.12f, 0.10f, skin[0] * 0.92f, skin[1] * 0.92f, skin[2] * 0.92f, t);
        }
    }

    // Shadow cast by the lids onto the eyeball
    for (int s = 0; s < UPPER_SHADOW_PX; s++) {
        s_shadow_up[s] = (uint8_t)(32 * (0.45f + 0.55f * smooth01(s / (float)UPPER_SHADOW_PX)) + 0.5f);
    }
    for (int s = 0; s < LOWER_SHADOW_PX; s++) {
        s_shadow_low[s] = (uint8_t)(32 * (0.70f + 0.30f * smooth01(s / (float)LOWER_SHADOW_PX)) + 0.5f);
    }

    // Eye socket vignette, indexed by r^2 >> 6
    for (int i = 0; i < VIGNETTE_N; i++) {
        float r = sqrtf(i * 64.0f);
        s_vignette[i] = (uint8_t)(32 * (1 - 0.5f * smooth01((r - 92) / 28.0f)) + 0.5f);
    }

    // Soft limbus: iris fades into the sclera
    for (int i = 0; i < FEATHER_N; i++) {
        s_feather[i] = (uint8_t)(32 * smooth01((i + 0.5f) / FEATHER_N) + 0.5f);
    }

    // Corneal highlight: bright window reflection plus a faint glossy glow
    for (int y = 0; y < 2 * HL_BOX; y++) {
        for (int x = 0; x < 2 * HL_BOX; x++) {
            float dx = x - HL_BOX + 0.5f, dy = y - HL_BOX + 0.5f;
            float e = sqrtf((dx / 10.0f) * (dx / 10.0f) + (dy / 7.0f) * (dy / 7.0f));
            float a = 0.9f * (1 - smooth01((e - 0.6f) / 0.4f));
            a += 0.18f * expf(-(dx * dx + dy * dy) / 400.0f);
            if (a > 1) a = 1;
            s_highlight[y][x] = (uint8_t)(32 * a + 0.5f);
        }
    }

    s_pupil_color = eye_rgb565(0.03f, 0.025f, 0.025f);
}

/**********************
 * Per-frame setup
 **********************/
static void frame_setup(frame_t *f, const eye_pose_t *p)
{
    float gx = p->gaze_x, gy = p->gaze_y;
    if (gx > EYE_GAZE_MAX) gx = EYE_GAZE_MAX;
    if (gx < -EYE_GAZE_MAX) gx = -EYE_GAZE_MAX;
    if (gy > EYE_GAZE_MAX) gy = EYE_GAZE_MAX;
    if (gy < -EYE_GAZE_MAX) gy = -EYE_GAZE_MAX;

    f->icx = C + (int)lroundf(gx);
    f->icy = C + (int)lroundf(gy);
    f->scl_ox = EYE_SCLERA_SIZE / 2 - C - (int)lroundf(gx);
    f->scl_oy = EYE_SCLERA_SIZE / 2 - C - (int)lroundf(gy);

    // The iris sits on a sphere: looking sideways squashes it along the gaze direction
    float g = sqrtf(gx * gx + gy * gy);
    float s = g / EYE_BALL_R;
    float k = 1 / sqrtf(1 - s * s) - 1;
    float ux = g > 0.01f ? gx / g : 0, uy = g > 0.01f ? gy / g : 0;
    f->m00 = (int32_t)(4096 * (1 + k * ux * ux));
    f->m01 = (int32_t)(4096 * (k * ux * uy));
    f->m11 = (int32_t)(4096 * (1 + k * uy * uy));

    int pd = (int)(p->pupil * EYE_LIMBUS_D);
    for (int d = 0; d < 256; d++) {
        if (d < pd) {
            f->pupil_lut[d] = 0xFF;
        } else {
            int ring = (d - pd) * EYE_IRIS_RINGS / (EYE_LIMBUS_D - pd);
            f->pupil_lut[d] = ring >= EYE_IRIS_RINGS ? EYE_IRIS_RINGS - 1 : ring;
        }
    }

    // Lids follow the vertical gaze; on a blink the upper lid sweeps down to meet the lower one
    float up_h = LID_UPPER_OPEN - gy * LID_TRACK_UP;
    float low_h = (LID_LOWER_OPEN + gy * LID_TRACK_LOW) * (1 - 0.12f * p->blink);
    up_h += (-(low_h + 3) - up_h) * p->blink;
    for (int x = 0; x < EYE_SCREEN; x++) {
        float q = (x - C) / (float)C;
        float shape = 1 - LID_CURVE * q * q;
        f->lid_up[x] = (int16_t)lroundf(C - up_h * shape);
        f->lid_low[x] = (int16_t)lroundf(C + low_h * shape);
    }
}

/**********************
 * Rendering
 **********************/
static void IRAM_ATTR render_strip(const frame_t *f, const uint16_t *sclera, int y0, uint16_t *out)
{
    const uint16_t *iris = s_tex.iris;
    const uint16_t *iris_outer = s_tex.iris + (EYE_IRIS_RINGS - 1) * EYE_IRIS_ANGLES;
    const uint16_t *pa = s_tex.polar_angle;
    const uint8_t *pdist = s_tex.polar_dist;

    for (int y = y0; y < y0 + STRIP_H; y++) {
        const int dy = y - f->icy;
        const int vy2 = (y - C) * (y - C);
        const uint16_t *srow = sclera + (y + f->scl_oy) * EYE_SCLERA_SIZE + f->scl_ox;
        const int hy = y - (HL_Y - HL_BOX);
        const bool hl_row = (unsigned)hy < 2 * HL_BOX;
        const int32_t ax = f->m01 * dy, ay = f->m11 * dy;

        for (int x = 0; x < EYE_SCREEN; x++) {
            uint16_t c;
            const int up = f->lid_up[x], low = f->lid_low[x];

            if (y < up) {
                int e = up - y - 1;
                c = s_lid_up_lut[e < LID_LUT_N ? e : LID_LUT_N - 1];
            } else if (y > low) {
                int e = y - low - 1;
                c = s_lid_low_lut[e < LID_LUT_N ? e : LID_LUT_N - 1];
            } else {
                const int dx = x - f->icx;
                const int tx = ((f->m00 * dx + ax) >> 12) + EYE_POLAR_HALF;
                const int ty = ((f->m01 * dx + ay) >> 12) + EYE_POLAR_HALF;
                int idx = 0;
                unsigned d = EYE_POLAR_OUT;
                if ((unsigned)tx < EYE_POLAR_SIZE && (unsigned)ty < EYE_POLAR_SIZE) {
                    idx = ty * EYE_POLAR_SIZE + tx;
                    d = pdist[idx];
                }

                if (d == EYE_POLAR_OUT) {
                    c = srow[x];
                } else {
                    const unsigned a = pa[idx];
                    if (d >= EYE_LIMBUS_D) {
                        c = blend565(iris_outer[a], srow[x], s_feather[d - EYE_LIMBUS_D]);
                    } else {
                        const uint8_t ring = f->pupil_lut[d];
                        c = ring == 0xFF ? s_pupil_color : iris[ring * EYE_IRIS_ANGLES + a];
                    }
                    if (hl_row) {
                        const int hx = x - (HL_X - HL_BOX);
                        if ((unsigned)hx < 2 * HL_BOX && s_highlight[hy][hx]) {
                            c = blend565(c, 0xFFFF, s_highlight[hy][hx]);
                        }
                    }
                }

                const int su = y - up, sl = low - y;
                if (su < UPPER_SHADOW_PX) c = shade565(c, s_shadow_up[su]);
                if (sl < LOWER_SHADOW_PX) c = shade565(c, s_shadow_low[sl]);
            }

            const int vx = x - C;
            const uint8_t v = s_vignette[(vx * vx + vy2) >> 6];
            if (v < 32) c = shade565(c, v);

            *out++ = __builtin_bswap16(c);   // panel expects big-endian RGB565
        }
    }
}

static void eye_task(void *arg)
{
    eye_ctx_t *ctx = arg;
    const uint16_t *sclera = s_tex.sclera[ctx->index];
    int b = 0, frames = 0;
    int64_t t_log = esp_timer_get_time();

    while (1) {
        eye_pose_t pose;
        eye_motion_get(&pose);
        frame_setup(&ctx->frame, &pose);

        for (int s = 0; s < N_STRIPS; s++) {
            int y0 = s * STRIP_H;
            render_strip(&ctx->frame, sclera, y0, ctx->buf[b]);
            // Waits for the previous strip of this panel before queueing, so the other
            // buffer is free to render into once this call returns.
            esp_lcd_panel_draw_bitmap(ctx->panel, 0, y0, EYE_SCREEN, y0 + STRIP_H, ctx->buf[b]);
            b ^= 1;
        }

        frames++;
        int64_t now = esp_timer_get_time();
        if (now - t_log >= 5000000) {
            ESP_LOGI(TAG_EYES, "eye %d: %.1f fps", ctx->index, frames * 1e6f / (now - t_log));
            frames = 0;
            t_log = now;
        }
        vTaskDelay(1);   // let the idle task run (task watchdog)
    }
}

void RealEyes_Start(void)
{
    init_luts();
    eye_textures_generate(&s_tex);
    eye_motion_init();

    esp_lcd_panel_handle_t panels[2] = {panel_handle, panel_handle2};
    for (int i = 0; i < 2; i++) {
        s_eyes[i].index = i;
        s_eyes[i].panel = panels[i];
        for (int k = 0; k < 2; k++) {
            s_eyes[i].buf[k] = heap_caps_malloc(EYE_SCREEN * STRIP_H * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            assert(s_eyes[i].buf[k]);
        }
        xTaskCreatePinnedToCore(eye_task, i ? "eye1" : "eye0", 4096, &s_eyes[i], 3, NULL, i);
    }
    ESP_LOGI(TAG_EYES, "Realistic eyes started");
}
