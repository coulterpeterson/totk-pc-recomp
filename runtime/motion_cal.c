// Calibration of the device's own motion sensors (motion_cal.h). Two steps, which need no reference but the player's
// pose. Held still as for play, gravity gives the direction that is up the screen and out of it, leaning back by some
// angle, and the gyro's reading is its resting error. Then the top edge tilts away: gravity as the sensors see it turns
// about the screen's x axis, so r0 x r1 is that axis (rotating the top edge away takes the reading from up-the-screen
// toward out-of-it, the sense of y to z about +x). A sensor is mounted square to the body, so that axis is close to one
// sensor axis, and of the four ways to lay the other two in the plane, one has the pose leaning back by 5-75 degrees.
// The gyro has to turn the other way, as a body turning by -x sees a fixed vector turn by +x: its integral over the
// tilt gives the gyro's sign and catches a gyro on other axes than the accelerometer's.
//
// The steps cannot tell a tilt toward the player on an ordinary device from a tilt away on one driver's layout (in the
// screen's axes, x reversed and y and z swapped): both turn the same readings. An ordinary device is by far the likelier, so a run that fits it with the
// tilt reversed asks for the tilt again, and that one layout cannot be calibrated.
//
// A resting error is measured only when the hold was steadier than hands are: the mean of a hand's slow drift, kept for
// good, would turn into a steady drift of the aim.
#include "motion_cal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define STILL_S 3.0
#define STILL_RATE 0.2      // rad/s: more is moving
#define REST_MIN 7.0        // m/s^2: a reading this far off 1 G is no rest
#define REST_MAX 14.0
#define TILT_MIN 25.0       // degrees before a tilt counts as one
#define TILT_TIMEOUT_S 15.0
#define PEAK_HOLD_S 2.5     // a tilt held this long past its peak is over
#define LEAN_MIN 5.0        // degrees of lean back from upright that tell the axes apart: the four candidates are 90
#define LEAN_MAX 75.0       // degrees apart, so at most one falls in here
#define AXIS_MIN 0.8        // the tilt's axis along one sensor axis, as a cosine
#define BIAS_STEADY 0.008   // rad/s: the gyro's spread during a hold that says something about its resting error
#define PI 3.14159265358979323846

static double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const double a[3], const double b[3], double o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}
static double unit3(double v[3]) {
    double n = sqrt(dot3(v, v));
    if (n > 0)
        for (int i = 0; i < 3; i++) v[i] /= n;
    return n;
}

void motion_cal_identity(MotionCal *c) { *c = (MotionCal){{1, 2, 3}, 1, {0, 0, 0}}; }

int motion_cal_is_standard(const MotionCal *c) {
    return c->map[0] == 1 && c->map[1] == 2 && c->map[2] == 3 && c->gyro_sign == 1;
}

void motion_cal_start(MotionCalRun *r, int rotation) {
    memset(r, 0, sizeof *r);
    r->state = MCAL_STILL;
    r->rotation = rotation & 3;
}

// A signed axis as map[] holds it: e (a unit vector along axis k, sign s) gives s * (k + 1).
static int axis_code(const double e[3]) {
    for (int k = 0; k < 3; k++)
        if (fabs(e[k]) > 0.5) return (e[k] > 0 ? 1 : -1) * (k + 1);
    return 0;
}

static double lean_of(const double r0[3], const double y[3], const double z[3]) {
    return atan2(dot3(r0, z), dot3(r0, y)) * 180 / PI;
}
static int lean_ok(double lean) { return lean >= LEAN_MIN && lean <= LEAN_MAX; }

// The screen's x and y on a device that follows Android's axes (device_sample's turn by the rotation).
static void standard_screen(int rotation, double x[3], double y[3]) {
    static const double axes[4][2][3] = {{{1, 0, 0}, {0, 1, 0}}, {{0, -1, 0}, {1, 0, 0}}, {{-1, 0, 0}, {0, -1, 0}}, {{0, 1, 0}, {-1, 0, 0}}};
    memcpy(x, axes[rotation][0], sizeof axes[0][0]);
    memcpy(y, axes[rotation][1], sizeof axes[0][1]);
}

static int solve(MotionCalRun *r) {
    double x[3];
    cross3(r->r0, r->r1, x);
    unit3(x);
    int k = 0;
    for (int i = 1; i < 3; i++)
        if (fabs(x[i]) > fabs(x[k])) k = i;
    if (fabs(x[k]) < AXIS_MIN) return MCAL_ERR_AXIS;
    double sx[3] = {0, 0, 0}, sy[3], sz[3];
    sx[k] = x[k] > 0 ? 1 : -1;
    double std_x[3], std_y[3], std_z[3] = {0, 0, 1};
    standard_screen(r->rotation, std_x, std_y);
    if (dot3(sx, std_x) < -0.5 && lean_ok(lean_of(r->r0, std_y, std_z))) return MCAL_ERR_TILT;   // tilted toward the player
    // The four candidates for the screen's y: the two other axes, either way
    int found = 0, a = (k + 1) % 3;
    for (int c = 0; c < 4 && !found; c++) {
        double y[3] = {0, 0, 0}, z[3];
        y[c < 2 ? a : (k + 2) % 3] = c & 1 ? -1 : 1;
        cross3(sx, y, z);
        if (lean_ok(lean_of(r->r0, y, z))) {
            memcpy(sy, y, sizeof y), memcpy(sz, z, sizeof z);
            found = 1;
        }
    }
    if (!found) return MCAL_ERR_POSE;
    // The gyro over the tilt: about -x, by the angle gravity turned
    double along = dot3(r->integ_peak, sx), all = sqrt(dot3(r->integ_peak, r->integ_peak));
    if (fabs(along) < 0.4 * r->peak * PI / 180 || fabs(along) < 0.6 * all) return MCAL_ERR_GYRO;
    r->cal.gyro_sign = along < 0 ? 1 : -1;
    // Screen axes into the natural orientation's, the inverse of device_sample's turn by the screen's rotation
    double cx[3], cy[3];
    for (int i = 0; i < 3; i++) {
        switch (r->rotation) {
        case 1: cx[i] = sy[i], cy[i] = -sx[i]; break;
        case 2: cx[i] = -sx[i], cy[i] = -sy[i]; break;
        case 3: cx[i] = -sy[i], cy[i] = sx[i]; break;
        default: cx[i] = sx[i], cy[i] = sy[i]; break;
        }
    }
    r->cal.map[0] = axis_code(cx), r->cal.map[1] = axis_code(cy), r->cal.map[2] = axis_code(sz);
    for (int i = 0; i < 3; i++) r->cal.bias[i] = r->turn_bias_ok ? (float)r->bias[i] : 0;
    return MCAL_OK;
}

int motion_cal_feed(MotionCalRun *r, const float a[3], const float g[3], double dt) {
    double len = sqrt((double)a[0] * a[0] + (double)a[1] * a[1] + (double)a[2] * a[2]);
    double rate = sqrt((double)g[0] * g[0] + (double)g[1] * g[1] + (double)g[2] * g[2]);
    if (r->state == MCAL_STILL) {
        if (rate > STILL_RATE || len < REST_MIN || len > REST_MAX) {   // moved: the 3 s start over
            r->still_s = 0, r->n = 0;
            memset(r->a_sum, 0, sizeof r->a_sum), memset(r->g_sum, 0, sizeof r->g_sum), memset(r->g2_sum, 0, sizeof r->g2_sum);
            return r->state;
        }
        for (int i = 0; i < 3; i++) r->a_sum[i] += a[i], r->g_sum[i] += g[i], r->g2_sum[i] += (double)g[i] * g[i];
        r->n++;
        r->still_s += dt;
        if (r->still_s >= STILL_S) {
            double spread = 0;
            for (int i = 0; i < 3; i++) {
                r->r0[i] = r->a_sum[i] / r->n, r->bias[i] = r->g_sum[i] / r->n;
                double v = r->g2_sum[i] / r->n - r->bias[i] * r->bias[i];
                if (v > spread) spread = v;
            }
            unit3(r->r0);
            r->turn_bias_ok = sqrt(spread) < BIAS_STEADY;
            r->state = MCAL_TILT;
        }
        return r->state;
    }
    if (r->state != MCAL_TILT) return r->state;
    r->t += dt;
    double k = dt / (dt + 0.05);   // 50 ms: the hand's acceleration, not the turn
    for (int i = 0; i < 3; i++) {
        r->smooth[i] = r->smooth_on ? r->smooth[i] + (a[i] - r->smooth[i]) * k : a[i];
        r->integ[i] += (g[i] - r->bias[i]) * dt;
    }
    r->smooth_on = 1;
    double v[3] = {r->smooth[0], r->smooth[1], r->smooth[2]};
    unit3(v);
    double d = dot3(v, r->r0), angle = acos(d > 1 ? 1 : d < -1 ? -1 : d) * 180 / PI;
    if (angle > r->peak) {
        r->peak = angle, r->since_peak = 0;
        memcpy(r->r1, v, sizeof v), memcpy(r->integ_peak, r->integ, sizeof r->integ);
    } else {
        r->since_peak += dt;
    }
    if (r->peak >= TILT_MIN && (angle < r->peak * 0.6 || r->since_peak > PEAK_HOLD_S)) r->state = solve(r);
    else if (r->t > TILT_TIMEOUT_S) r->state = MCAL_ERR_TILT;
    return r->state;
}

int motion_cal_still_pct(const MotionCalRun *r) { return r->still_s >= STILL_S ? 100 : (int)(r->still_s * 100 / STILL_S); }

void motion_cal_apply(const MotionCal *c, float a[3], float g[3]) {
    float ra[3], rg[3];
    memcpy(ra, a, sizeof ra), memcpy(rg, g, sizeof rg);
    for (int i = 0; i < 3; i++) {
        int m = c->map[i], k = (m < 0 ? -m : m) - 1;
        float s = m < 0 ? -1.0f : 1.0f;
        a[i] = s * ra[k];
        g[i] = s * (float)c->gyro_sign * (rg[k] - c->bias[k]);
    }
}

void motion_cal_format(const MotionCal *c, char *out, unsigned cap) {
    snprintf(out, cap, "%d %d %d %d %.5f %.5f %.5f", c->map[0], c->map[1], c->map[2], c->gyro_sign, c->bias[0], c->bias[1],
             c->bias[2]);
}

int motion_cal_parse(const char *s, MotionCal *c) {
    MotionCal t;
    if (!s || sscanf(s, "%d %d %d %d %f %f %f", &t.map[0], &t.map[1], &t.map[2], &t.gyro_sign, &t.bias[0], &t.bias[1],
                     &t.bias[2]) != 7) return 0;
    int seen = 0;
    for (int i = 0; i < 3; i++) {
        int k = t.map[i] < 0 ? -t.map[i] : t.map[i];
        if (k < 1 || k > 3) return 0;
        seen |= 1 << (k - 1);
    }
    if (seen != 7 || (t.gyro_sign != 1 && t.gyro_sign != -1)) return 0;
    *c = t;
    return 1;
}
