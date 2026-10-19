#pragma once
// ESP-NOW packet from the face-tracking camera (sender/) to the eye board (main/GazeLink).
// Shared by both projects - bump GAZE_MSG_VERSION when the layout changes.
#include <stdint.h>

#define GAZE_MSG_MAGIC    0xE7
#define GAZE_MSG_VERSION  1

typedef struct __attribute__((packed)) {
    uint8_t  magic;      // GAZE_MSG_MAGIC
    uint8_t  version;    // GAZE_MSG_VERSION
    uint8_t  seq;        // incremented per packet
    uint8_t  faces;      // number of faces detected, 0 = nobody in view
    int16_t  x, y;       // look-at point (between the eyes) in camera image coordinates,
                         // -1000..1000, x to the right, y down, as the camera sees it
    uint16_t size;       // face width relative to the image width, 0..1000
    uint8_t  score;      // detection confidence, 0..100
} gaze_msg_t;
