#include "Eye_Motion.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "Eye_Textures.h"

// Timing parameters (ms)
#define FIX_MIN_MS        250     // fixation duration between saccades
#define FIX_MAX_MS        2200
#define BLINK_CLOSE_MS    75
#define BLINK_HOLD_MS     35
#define BLINK_OPEN_MS     160
#define BLINK_GAP_MIN_MS  1800
#define BLINK_GAP_MAX_MS  6500
#define PUPIL_TAU_S       0.9f    // time constant of pupil size changes

#define GAZE_MAX_Y        (EYE_GAZE_MAX * 0.75f)   // eyes move less vertically than horizontally

static SemaphoreHandle_t s_lock;

static struct {
    float x, y;                          // gaze without drift
    float from_x, from_y, to_x, to_y;
    int64_t sacc_start, sacc_end, fix_end;
    int64_t blink_start, next_blink;
    float pupil, pupil_target;
    int64_t next_pupil, last;
    bool in_saccade, blinking;
} m;

static float frand(void) { return (esp_random() >> 8) * (1.0f / 16777216.0f); }

static int64_t ms_to_us(float ms) { return (int64_t)(ms * 1000.0f); }

static void clamp_gaze(float *x, float *y)
{
    float e = (*x / EYE_GAZE_MAX) * (*x / EYE_GAZE_MAX) + (*y / GAZE_MAX_Y) * (*y / GAZE_MAX_Y);
    if (e > 1) {
        float s = 1 / sqrtf(e);
        *x *= s;
        *y *= s;
    }
}

static void start_blink(int64_t now)
{
    m.blinking = true;
    m.blink_start = now;
}

static void start_saccade(int64_t now)
{
    float tx, ty, r = frand();
    if (r < 0.35f) {            // microsaccade / small correction
        float amp = 1.5f + 4 * frand(), ang = frand() * 6.2832f;
        tx = m.x + amp * cosf(ang);
        ty = m.y + amp * sinf(ang);
    } else if (r < 0.55f) {     // back to roughly straight ahead
        tx = (frand() - 0.5f) * 16;
        ty = (frand() - 0.5f) * 12;
    } else {                    // look somewhere new
        tx = (frand() * 2 - 1) * EYE_GAZE_MAX;
        ty = (frand() * 2 - 1) * GAZE_MAX_Y;
    }
    clamp_gaze(&tx, &ty);

    float amp = hypotf(tx - m.x, ty - m.y);
    m.from_x = m.x; m.from_y = m.y;
    m.to_x = tx;    m.to_y = ty;
    m.sacc_start = now;
    m.sacc_end = now + ms_to_us(20 + 0.9f * amp);   // bigger jumps take longer (main sequence)
    m.in_saccade = true;

    // Large gaze shifts are often accompanied by a blink
    if (amp > 40 && !m.blinking && frand() < 0.25f) start_blink(now);
}

void eye_motion_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    int64_t now = esp_timer_get_time();
    m.fix_end = now + ms_to_us(500);
    m.next_blink = now + ms_to_us(1500);
    m.pupil = m.pupil_target = 0.4f;
    m.next_pupil = now;
    m.last = now;
}

void eye_motion_get(eye_pose_t *pose)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    float dt = (now - m.last) * 1e-6f;
    m.last = now;

    // Gaze: fixation, then a fast saccade to the next target
    if (m.in_saccade) {
        float t = (now - m.sacc_start) / (float)(m.sacc_end - m.sacc_start);
        if (t >= 1) {
            t = 1;
            m.in_saccade = false;
            m.fix_end = now + ms_to_us(FIX_MIN_MS + (FIX_MAX_MS - FIX_MIN_MS) * frand() * frand());
        }
        // Quick acceleration, longer deceleration
        float e = 0.5f * t * t * (3 - 2 * t) + 0.5f * (1 - powf(1 - t, 3));
        m.x = m.from_x + (m.to_x - m.from_x) * e;
        m.y = m.from_y + (m.to_y - m.from_y) * e;
    } else if (now >= m.fix_end) {
        start_saccade(now);
    }

    // Blink: lids close fast, hold briefly, open slower
    float blink = 0;
    if (!m.blinking && now >= m.next_blink) start_blink(now);
    if (m.blinking) {
        float t = (now - m.blink_start) / 1000.0f;
        if (t < BLINK_CLOSE_MS) {
            float u = t / BLINK_CLOSE_MS;
            blink = u * u;
        } else if (t < BLINK_CLOSE_MS + BLINK_HOLD_MS) {
            blink = 1;
        } else if (t < BLINK_CLOSE_MS + BLINK_HOLD_MS + BLINK_OPEN_MS) {
            float u = (t - BLINK_CLOSE_MS - BLINK_HOLD_MS) / BLINK_OPEN_MS;
            blink = (1 - u) * (1 - u);
        } else {
            m.blinking = false;
            m.next_blink = now + (frand() < 0.12f ? ms_to_us(90)    // occasional double blink
                                                  : ms_to_us(BLINK_GAP_MIN_MS + (BLINK_GAP_MAX_MS - BLINK_GAP_MIN_MS) * frand()));
        }
    }

    // Pupil: slow changes in size plus constant small oscillation (hippus)
    if (now >= m.next_pupil) {
        m.pupil_target = 0.30f + 0.22f * frand();
        m.next_pupil = now + ms_to_us(2000 + 4000 * frand());
    }
    m.pupil += (m.pupil_target - m.pupil) * (1 - expf(-dt / PUPIL_TAU_S));
    float ts = now * 1e-6f;
    float hippus = 0.012f * sinf(ts * 1.9f) + 0.008f * sinf(ts * 4.3f + 1.3f);

    // Fixational drift
    float drift_x = 0.6f * sinf(ts * 1.1f) + 0.4f * sinf(ts * 2.7f + 0.5f);
    float drift_y = 0.5f * sinf(ts * 0.9f + 2.0f) + 0.3f * sinf(ts * 2.3f);

    pose->gaze_x = m.x + drift_x;
    pose->gaze_y = m.y + drift_y;
    pose->pupil = m.pupil + hippus;
    pose->blink = blink;
    xSemaphoreGive(s_lock);
}
