#pragma once
#include <stdint.h>

// Screen and eye geometry (pixels)
#define EYE_SCREEN       240
#define EYE_GAZE_MAX     56                                  // max offset of the iris centre from the screen centre
#define EYE_BALL_R       170                                 // eyeball radius, used for iris foreshortening
#define EYE_IRIS_R       80                                  // iris radius on screen
#define EYE_SCLERA_SIZE  (EYE_SCREEN + 2 * (EYE_GAZE_MAX + 4)) // sclera texture is larger than the screen and slides with the gaze

// Iris texture in polar form: ring 0 = pupil edge, last ring = limbus
#define EYE_IRIS_ANGLES  512
#define EYE_IRIS_RINGS   64

// Polar lookup table around the iris centre: distance 0..EYE_LIMBUS_D is iris,
// EYE_LIMBUS_D..254 is the soft limbus fading into the sclera, 255 is outside.
#define EYE_LIMBUS_D     230
#define EYE_POLAR_OUT    255
#define EYE_POLAR_HALF   ((EYE_IRIS_R * EYE_POLAR_OUT + EYE_LIMBUS_D - 1) / EYE_LIMBUS_D)
#define EYE_POLAR_SIZE   (2 * EYE_POLAR_HALF)

typedef struct {
    uint16_t *iris;         // [EYE_IRIS_RINGS][EYE_IRIS_ANGLES] RGB565
    uint16_t *sclera[2];    // [EYE_SCLERA_SIZE][EYE_SCLERA_SIZE] RGB565, one per eye
    uint16_t *polar_angle;  // [EYE_POLAR_SIZE][EYE_POLAR_SIZE] 0..EYE_IRIS_ANGLES-1
    uint8_t  *polar_dist;   // [EYE_POLAR_SIZE][EYE_POLAR_SIZE] see EYE_LIMBUS_D
} eye_textures_t;

// Procedurally generates all textures (takes about a second at boot)
void eye_textures_generate(eye_textures_t *tex);

uint16_t eye_rgb565(float r, float g, float b);
