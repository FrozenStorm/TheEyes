#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool face;          // a face is in view
    float x, y;         // where to look, in eye gaze pixels (same space as eye_pose_t gaze)
    float size;         // face width relative to the camera image, 0..1
    int64_t time_us;    // esp_timer time the packet was received
} gaze_target_t;

// Starts Wi-Fi (station mode, no connection) and listens for ESP-NOW packets from the camera
void GazeLink_Init(void);

// Latest target from the camera. Returns false if nothing was received yet.
bool gaze_link_get(gaze_target_t *out);
