// Calibration of the device's own motion sensors (input.c device_sample, the menu's Calibrate motion): which sensor axis
// is which, for a driver that does not follow Android's convention, and the gyro's resting error. motion_cal.c; no SDL.
#pragma once
#include <stdint.h>

// A calibration: corrected axis i = sign(map[i]) * raw axis |map[i]| - 1, in Android's axes for the device's natural
// orientation; the gyro's raw reading less bias, times gyro_sign. Identity: {1, 2, 3}, 1.
typedef struct {
    int map[3];
    int gyro_sign;
    float bias[3];
} MotionCal;

enum {
    MCAL_IDLE,       // not running
    MCAL_STILL,      // waiting for 3 s of holding still
    MCAL_TILT,       // waiting for the top edge to tilt away and back
    MCAL_OK,         // finished; the calibration is in the run
    MCAL_ERR_POSE,   // not held upright enough to tell the axes apart
    MCAL_ERR_TILT,   // no clear tilt in time
    MCAL_ERR_AXIS,   // the tilt was not about one sensor axis
    MCAL_ERR_GYRO,   // the gyro did not see the tilt the accelerometer saw
};

typedef struct {
    int state, rotation;
    MotionCal cal;
    double still_s, t, peak, since_peak;
    double a_sum[3], g_sum[3], g2_sum[3], smooth[3], r0[3], r1[3], integ[3], integ_peak[3], bias[3];
    int n, smooth_on, turn_bias_ok;
} MotionCalRun;

// rotation: the screen's from the natural orientation, quarter turns (Surface.ROTATION_*).
void motion_cal_start(MotionCalRun *r, int rotation);
// One raw sample, accelerometer in m/s^2 and gyro in rad/s, dt seconds after the last. Returns the state.
int motion_cal_feed(MotionCalRun *r, const float a[3], const float g[3], double dt);
int motion_cal_still_pct(const MotionCalRun *r);   // how far along the hold is
// The sensors' raw reading into corrected axes.
void motion_cal_apply(const MotionCal *c, float a[3], float g[3]);
int motion_cal_is_standard(const MotionCal *c);   // the axes need no correction
void motion_cal_identity(MotionCal *c);
// The setting's text: "1 2 3 1 0.00100 -0.00100 0.00000" (map, gyro sign, bias). parse: 1 when valid.
void motion_cal_format(const MotionCal *c, char *out, unsigned cap);
int motion_cal_parse(const char *s, MotionCal *c);
