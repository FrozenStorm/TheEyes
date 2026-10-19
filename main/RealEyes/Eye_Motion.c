#include "Eye_Motion.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "sdkconfig.h"
#include "Eye_Textures.h"
#if CONFIG_GAZE_LINK_ENABLED
#include "GazeLink.h"
#endif

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

// Attention on a face reported by the camera (GazeLink)
#define FACE_TIMEOUT_MS   400     // camera data older than this is stale
#define FACE_HOLD_MS      1500    // keep looking at the last position after the face is lost
#define FACE_LOOK_P       0.80f   // share of saccades that go to the face while attending
#define FACE_MICRO_P      0.92f   // ... microsaccades up to this, the rest glance away
#define FACE_JUMP_PX      14      // face moved this far from the fixation point -> corrective saccade
#define PURSUIT_TAU_S     0.12f   // smaller face movements are followed smoothly

static SemaphoreHandle_t s_lock;

static struct {
    float x, y;                          // gaze without drift
    float from_x, from_y, to_x, to_y;
    int64_t sacc_start, sacc_end, fix_end;
    int64_t blink_start, next_blink;
    float pupil, pupil_target;
    int64_t next_pupil, last;
    bool in_saccade, blinking;
    bool glance;                         // current saccade is a short glance away from the face
    // Face attention
    float face_x, face_y, face_size;
    float off_x, off_y;                  // fixation point within the face (eyes / mouth)
    int64_t face_seen;                   // last time a fresh face position arrived, 0 = never
    bool face_fresh, attending, on_face;
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

static void begin_saccade(int64_t now, float tx, float ty)
{
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

static void microsaccade(int64_t now)
{
    float amp = 1.5f + 4 * frand(), ang = frand() * 6.2832f;
    begin_saccade(now, m.x + amp * cosf(ang), m.y + amp * sinf(ang));
}

// Jump to the face. Like people do, fixate the eyes most of the time and sometimes the mouth.
static void look_at_face(int64_t now, bool centre)
{
    float spread = 1.0f + 8.0f * m.face_size;
    m.off_x = centre ? 0 : (frand() - 0.5f) * 2 * spread;
    m.off_y = centre ? 0 : (frand() < 0.25f ? 1.6f * spread : (frand() - 0.5f) * spread);
    m.on_face = true;
    m.glance = false;
    begin_saccade(now, m.face_x + m.off_x, m.face_y + m.off_y);
}

static void start_saccade(int64_t now)
{
    float r = frand();
    if (m.attending) {
        if (r < FACE_LOOK_P) {
            look_at_face(now, false);
        } else if (r < FACE_MICRO_P) {
            microsaccade(now);
        } else {                // brief glance away, then back to the face
            m.on_face = false;
            m.glance = true;
            begin_saccade(now, (frand() * 2 - 1) * EYE_GAZE_MAX, (frand() * 2 - 1) * GAZE_MAX_Y);
        }
        return;
    }

    m.on_face = false;
    m.glance = false;
    if (r < 0.35f) {            // microsaccade / small correction
        microsaccade(now);
    } else if (r < 0.55f) {     // back to roughly straight ahead
        begin_saccade(now, (frand() - 0.5f) * 16, (frand() - 0.5f) * 12);
    } else {                    // look somewhere new
        begin_saccade(now, (frand() * 2 - 1) * EYE_GAZE_MAX, (frand() * 2 - 1) * GAZE_MAX_Y);
    }
}

#if CONFIG_GAZE_LINK_ENABLED
static void update_face(int64_t now)
{
    gaze_target_t t;
    m.face_fresh = gaze_link_get(&t) && t.face && now - t.time_us < ms_to_us(FACE_TIMEOUT_MS);
    if (m.face_fresh) {
        bool appeared = m.face_seen == 0 || now - m.face_seen > ms_to_us(FACE_HOLD_MS);
        m.face_x = t.x;
        m.face_y = t.y;
        m.face_size = t.size;
        m.face_seen = now;
        if (appeared) {         // someone showed up: look right away, often with a blink, pupils widen a little
            m.attending = true;
            look_at_face(now, true);
            if (!m.blinking && frand() < 0.4f) start_blink(now);
            m.pupil_target = fminf(m.pupil_target + 0.06f, 0.55f);
        }
    }
    m.attending = m.face_seen != 0 && now - m.face_seen < ms_to_us(FACE_HOLD_MS);
}

// While fixating the face, follow small movements smoothly and catch up on big ones with a saccade
static void follow_face(int64_t now, float dt)
{
    if (!m.attending || !m.on_face || !m.face_fresh || m.in_saccade) return;
    float tx = m.face_x + m.off_x, ty = m.face_y + m.off_y;
    clamp_gaze(&tx, &ty);
    if (hypotf(tx - m.x, ty - m.y) > FACE_JUMP_PX) {
        begin_saccade(now, tx, ty);
    } else {
        float k = 1 - expf(-dt / PURSUIT_TAU_S);
        m.x += (tx - m.x) * k;
        m.y += (ty - m.y) * k;
    }
}
#endif

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

#if CONFIG_GAZE_LINK_ENABLED
    update_face(now);
    follow_face(now, dt);
#endif

    // Gaze: fixation, then a fast saccade to the next target
    if (m.in_saccade) {
        float t = (now - m.sacc_start) / (float)(m.sacc_end - m.sacc_start);
        if (t >= 1) {
            t = 1;
            m.in_saccade = false;
            m.fix_end = now + (m.glance ? ms_to_us(200 + 400 * frand())
                                        : ms_to_us(FIX_MIN_MS + (FIX_MAX_MS - FIX_MIN_MS) * frand() * frand()));
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
