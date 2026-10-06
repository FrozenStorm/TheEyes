#include "Eye_Textures.h"
#include <math.h>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG_TEX = "EyeTex";

typedef struct { float r, g, b; } rgb_t;

typedef struct { rgb_t inner, mid, outer, limbal, ruff; } iris_palette_t;

#if CONFIG_EYES_IRIS_BLUE
static const iris_palette_t PAL = {
    {0.62f, 0.60f, 0.42f}, {0.36f, 0.52f, 0.68f}, {0.26f, 0.40f, 0.58f}, {0.05f, 0.08f, 0.14f}, {0.20f, 0.16f, 0.10f}
};
#elif CONFIG_EYES_IRIS_BROWN
static const iris_palette_t PAL = {
    {0.58f, 0.36f, 0.14f}, {0.42f, 0.24f, 0.09f}, {0.30f, 0.17f, 0.07f}, {0.07f, 0.04f, 0.02f}, {0.16f, 0.09f, 0.04f}
};
#else // hazel / green
static const iris_palette_t PAL = {
    {0.66f, 0.46f, 0.16f}, {0.42f, 0.46f, 0.22f}, {0.30f, 0.40f, 0.22f}, {0.05f, 0.07f, 0.04f}, {0.20f, 0.12f, 0.05f}
};
#endif

/**********************
 * Colour helpers
 **********************/
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline float smoothstepf(float e0, float e1, float x)
{
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline rgb_t mix(rgb_t a, rgb_t b, float t)
{
    return (rgb_t){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

static inline rgb_t scale(rgb_t a, float s) { return (rgb_t){a.r * s, a.g * s, a.b * s}; }

uint16_t eye_rgb565(float r, float g, float b)
{
    int ri = (int)(clampf(r, 0, 1) * 31.0f + 0.5f);
    int gi = (int)(clampf(g, 0, 1) * 63.0f + 0.5f);
    int bi = (int)(clampf(b, 0, 1) * 31.0f + 0.5f);
    return (uint16_t)((ri << 11) | (gi << 5) | bi);
}

static inline uint16_t to565(rgb_t c) { return eye_rgb565(c.r, c.g, c.b); }

static inline rgb_t from565(uint16_t c)
{
    return (rgb_t){(c >> 11) / 31.0f, ((c >> 5) & 63) / 63.0f, (c & 31) / 31.0f};
}

/**********************
 * Noise
 **********************/
static uint32_t hash3(int x, int y, uint32_t seed)
{
    uint32_t h = seed * 0x9E3779B9u ^ (uint32_t)x * 0x85EBCA6Bu ^ (uint32_t)y * 0xC2B2AE35u;
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

static inline float lattice(int x, int y, uint32_t seed)
{
    return (hash3(x, y, seed) & 0xFFFFFF) * (1.0f / 16777215.0f);
}

// Value noise in [0,1]; wraps in x every `period` units when period > 0
static float vnoise(float x, float y, int period, uint32_t seed)
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    int x0 = xi, x1 = xi + 1;
    if (period > 0) {
        x0 = ((x0 % period) + period) % period;
        x1 = ((x1 % period) + period) % period;
    }
    float u = fx * fx * (3 - 2 * fx), v = fy * fy * (3 - 2 * fy);
    float a = lattice(x0, yi, seed), b = lattice(x1, yi, seed);
    float c = lattice(x0, yi + 1, seed), d = lattice(x1, yi + 1, seed);
    float top = a + (b - a) * u, bottom = c + (d - c) * u;
    return top + (bottom - top) * v;
}

static float fbm(float x, float y, int period, int octaves, uint32_t seed)
{
    float sum = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < octaves; o++) {
        sum += amp * vnoise(x, y, period, seed + o * 101);
        norm += amp;
        x *= 2; y *= 2; period *= 2; amp *= 0.5f;
    }
    return sum / norm;
}

// Deterministic RNG for the vein layout
static uint32_t s_rng;
static float rnd(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (s_rng >> 8) * (1.0f / 16777216.0f);
}

/**********************
 * Iris
 **********************/
static void gen_iris(uint16_t *iris, uint32_t seed)
{
    for (int r = 0; r < EYE_IRIS_RINGS; r++) {
        float v = r / (float)(EYE_IRIS_RINGS - 1);      // 0 = pupil edge, 1 = limbus
        for (int a = 0; a < EYE_IRIS_ANGLES; a++) {
            float u = a / (float)EYE_IRIS_ANGLES;        // 0..1 around the iris

            float collar = 0.34f + 0.10f * (fbm(u * 10, 0.5f, 10, 2, seed + 1) - 0.5f);
            rgb_t c = v < collar ? mix(PAL.inner, PAL.mid, smoothstepf(0, collar, v))
                                 : mix(PAL.mid, PAL.outer, smoothstepf(collar, 1, v));

            // Radial stroma fibres: high frequency around, stretched along the radius
            float fib = fbm(u * 128, v * 2.5f, 128, 3, seed + 2);
            float bundle = fbm(u * 28, v * 4, 28, 3, seed + 3);
            c = scale(c, 0.62f + 0.55f * fib);
            c = scale(c, 0.80f + 0.40f * bundle);

            // Bright, wavy collarette ring
            float dc = (v - collar) / 0.035f;
            c = scale(c, 1 + 0.30f * expf(-dc * dc));

            // Crypts: dark lacunae between collarette and outer iris
            float crypt = fbm(u * 18, v * 5, 18, 2, seed + 4);
            float zone = smoothstepf(collar, collar + 0.08f, v) * (1 - smoothstepf(0.70f, 0.85f, v));
            c = scale(c, 1 - 0.55f * zone * smoothstepf(0.60f, 0.75f, crypt));

            // Concentric contraction furrows in the outer iris
            float furrow = sinf((v * 9 + 0.6f * fbm(u * 8, v * 2, 8, 2, seed + 5)) * 6.2832f);
            c = scale(c, 1 - 0.07f * smoothstepf(0.5f, 0.9f, v) * (furrow * 0.5f + 0.5f));

            // Dark limbal ring and pupillary ruff
            c = mix(c, PAL.limbal, 0.9f * smoothstepf(0.78f, 1.0f, v));
            c = mix(c, PAL.ruff, 1 - smoothstepf(0.0f, 0.07f, v));

            iris[r * EYE_IRIS_ANGLES + a] = to565(c);
        }
    }
}

/**********************
 * Sclera
 **********************/
static void plot(uint16_t *img, float x, float y, rgb_t col, float alpha)
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    float w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    for (int k = 0; k < 4; k++) {
        int px = xi + (k & 1), py = yi + (k >> 1);
        if (px < 0 || py < 0 || px >= EYE_SCLERA_SIZE || py >= EYE_SCLERA_SIZE) continue;
        uint16_t *p = &img[py * EYE_SCLERA_SIZE + px];
        *p = to565(mix(from565(*p), col, alpha * w[k]));
    }
}

// Meandering blood vessel from the eyeball edge towards the iris, with occasional branches
static void draw_vein(uint16_t *img, float x, float y, float dir, float len, float alpha, float width, int depth)
{
    const float S2 = EYE_SCLERA_SIZE / 2.0f;
    const rgb_t col = {0.66f, 0.13f, 0.12f};
    for (float t = 0; t < len; t += 0.5f) {
        float k = 1 - t / len;
        float dx = x - S2, dy = y - S2;
        float d = sqrtf(dx * dx + dy * dy) / S2;
        if (d < 0.40f) break;                                   // vessels vanish under the limbus
        float a = alpha * k * smoothstepf(0.40f, 0.55f, d);
        float wd = width * (0.4f + 0.6f * k);
        plot(img, x, y, col, a);
        if (wd > 1.0f) {
            plot(img, x + cosf(dir + 1.5708f) * 0.7f, y + sinf(dir + 1.5708f) * 0.7f, col, a * (wd - 1.0f));
        }
        dir += (rnd() - 0.5f) * 0.35f;
        x += cosf(dir) * 0.5f;
        y += sinf(dir) * 0.5f;
        if (depth < 2 && rnd() < 0.012f) {
            float turn = (rnd() < 0.5f ? -1.0f : 1.0f) * (0.4f + 0.6f * rnd());
            draw_vein(img, x, y, dir + turn, (len - t) * (0.3f + 0.4f * rnd()), a * 0.8f, wd * 0.7f, depth + 1);
        }
    }
}

static void gen_sclera(uint16_t *img, uint32_t seed)
{
    const float S2 = EYE_SCLERA_SIZE / 2.0f;
    const rgb_t white = {0.94f, 0.91f, 0.87f};
    const rgb_t grey_blue = {0.82f, 0.85f, 0.90f};
    const rgb_t pink = {0.88f, 0.62f, 0.58f};

    for (int y = 0; y < EYE_SCLERA_SIZE; y++) {
        for (int x = 0; x < EYE_SCLERA_SIZE; x++) {
            float dx = x - S2, dy = y - S2;
            float d = sqrtf(dx * dx + dy * dy) / S2;    // 0 = under the iris, 1 = edge of the visible eyeball
            rgb_t c = scale(white, 0.93f + 0.10f * fbm(x / 45.0f, y / 45.0f, 0, 4, seed));
            c = mix(c, grey_blue, 0.30f * (1 - smoothstepf(0.35f, 0.60f, d)));
            c = mix(c, pink, 0.45f * smoothstepf(0.45f, 1.0f, d) * (0.6f + 0.8f * fbm(x / 25.0f, y / 25.0f, 0, 2, seed + 7)));
            c = scale(c, 1 - 0.50f * smoothstepf(0.30f, 1.1f, d));   // eyeball curvature
            img[y * EYE_SCLERA_SIZE + x] = to565(c);
        }
    }

    s_rng = seed;
    for (int i = 0; i < 28; i++) {
        // Most vessels enter from the nasal and temporal sides, where the sclera is exposed
        float ang = (rnd() < 0.5f ? 0.0f : 3.14159f) + (rnd() + rnd() + rnd() - 1.5f) * 0.9f;
        float r = S2 * (0.95f + 0.05f * rnd());
        float dir = ang + 3.14159f + (rnd() - 0.5f) * 0.6f;
        draw_vein(img, S2 + cosf(ang) * r, S2 + sinf(ang) * r, dir,
                  S2 * (0.35f + 0.4f * rnd()), 0.25f + 0.35f * rnd(), 0.8f + 1.4f * rnd(), 0);
    }
}

/**********************
 * Polar lookup
 **********************/
static void gen_polar(uint16_t *angle, uint8_t *dist)
{
    for (int ty = 0; ty < EYE_POLAR_SIZE; ty++) {
        for (int tx = 0; tx < EYE_POLAR_SIZE; tx++) {
            float fx = tx - EYE_POLAR_HALF + 0.5f, fy = ty - EYE_POLAR_HALF + 0.5f;
            float d = sqrtf(fx * fx + fy * fy) * EYE_LIMBUS_D / EYE_IRIS_R;
            float a = atan2f(fy, fx) / 6.2831853f;
            if (a < 0) a += 1.0f;
            int i = ty * EYE_POLAR_SIZE + tx;
            dist[i] = d >= EYE_POLAR_OUT ? EYE_POLAR_OUT : (uint8_t)d;
            angle[i] = (uint16_t)((int)(a * EYE_IRIS_ANGLES) & (EYE_IRIS_ANGLES - 1));
        }
    }
}

static void *alloc_prefer_internal(size_t size)
{
    void *p = heap_caps_malloc_prefer(size, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, MALLOC_CAP_SPIRAM);
    assert(p);
    return p;
}

static void *alloc_psram(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    assert(p);
    return p;
}

void eye_textures_generate(eye_textures_t *tex)
{
    int64_t t0 = esp_timer_get_time();
    const size_t polar_n = EYE_POLAR_SIZE * EYE_POLAR_SIZE;

    tex->iris = alloc_prefer_internal(EYE_IRIS_RINGS * EYE_IRIS_ANGLES * sizeof(uint16_t));
    tex->polar_angle = alloc_psram(polar_n * sizeof(uint16_t));
    tex->polar_dist = alloc_psram(polar_n);
    for (int i = 0; i < 2; i++) {
        tex->sclera[i] = alloc_psram(EYE_SCLERA_SIZE * EYE_SCLERA_SIZE * sizeof(uint16_t));
    }

    gen_iris(tex->iris, 1234);
    gen_polar(tex->polar_angle, tex->polar_dist);
    gen_sclera(tex->sclera[0], 777);
    gen_sclera(tex->sclera[1], 4242);

    ESP_LOGI(TAG_TEX, "Textures generated in %d ms", (int)((esp_timer_get_time() - t0) / 1000));
}
