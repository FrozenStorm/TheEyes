#pragma once

typedef struct {
    float gaze_x, gaze_y;   // iris centre offset from the screen centre (px), +y = down
    float pupil;            // pupil radius as a fraction of the iris radius
    float blink;            // 0 = open, 1 = closed
} eye_pose_t;

void eye_motion_init(void);

// Advances the gaze / blink / pupil model to the current time. Thread-safe, so
// both eye render tasks can call it and get a consistent pose.
void eye_motion_get(eye_pose_t *pose);
