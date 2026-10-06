#pragma once

// Generates the eye textures and starts one render task per display (core 0 and 1).
// Draws directly to the panels, so LVGL must not be running at the same time.
void RealEyes_Start(void);
