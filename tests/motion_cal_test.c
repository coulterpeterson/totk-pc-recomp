// runtime/motion_cal.c against a simulated handheld: the play pose and a tilt of the top edge away and back, through every
// way a driver can turn its axes (the 24 rotations of the sensor frame), both landscape rotations and a flipped gyro.
// A calibration passes when the corrected reading, turned by the screen's rotation, gives the true screen axes.
//   cc -O1 -I runtime tests/motion_cal_test.c runtime/motion_cal.c -lm -o /tmp/mct && /tmp/mct
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "motion_cal.h"

#define PI 3.14159265358979323846
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

// The sensor's axes: raw[i] = sign * conforming axis.
typedef struct { int m[3]; } Hal;

static void screen_to_conforming(int rot, const double s[3], double c[3]) {   // inverse of device_sample's turn
    switch (rot) {
    case 1: c[0] = s[1], c[1] = -s[0]; break;
    case 2: c[0] = -s[0], c[1] = -s[1]; break;
    case 3: c[0] = -s[1], c[1] = s[0]; break;
    default: c[0] = s[0], c[1] = s[1]; break;
    }
    c[2] = s[2];
}

static void conforming_to_screen(int rot, const float c[3], double s[3]) {   // input.c device_sample
    double x = c[0], y = c[1];
    switch (rot) {
    case 1: s[0] = -y, s[1] = x; break;
    case 2: s[0] = -x, s[1] = -y; break;
    case 3: s[0] = y, s[1] = -x; break;
    default: s[0] = x, s[1] = y; break;
    }
    s[2] = c[2];
}

static void to_raw(const Hal *h, const double c[3], float raw[3], double scale) {
    for (int i = 0; i < 3; i++) {
        int m = h->m[i], k = abs(m) - 1;
        raw[i] = (float)((m < 0 ? -1 : 1) * c[k] * scale);
    }
}

static double hand = 0.005;   // the gyro's noise during a run, rad/s
static double noise(double amp) { return amp * ((double)rand() / RAND_MAX * 2 - 1); }

// lean: degrees back from upright. tilt: degrees away and back over 2 s; scale: the accelerometer's error (the AYN Thor
// reads about 1.17 G). Returns the final state.
static int run(const Hal *h, int rot, int gyro_flip, double lean, double tilt, double scale, MotionCal *out, double bias) {
    MotionCalRun r;
    motion_cal_start(&r, rot);
    double dt = 0.004, t = 0, l0 = lean * PI / 180, amp = tilt * PI / 180;
    for (; r.state == MCAL_STILL || r.state == MCAL_TILT; t += dt) {
        double hold = 3.2, move = 2.0;
        double phi = 0, dphi = 0;
        if (t > hold && t < hold + move) {
            double p = (t - hold) / move;
            phi = amp * sin(PI * p), dphi = amp * PI / move * cos(PI * p);
        }
        double rs[3] = {noise(0.01), cos(l0 + phi) + noise(0.005), sin(l0 + phi) + noise(0.005)};
        for (int k = 0; k < 3; k++) rs[k] *= 9.80665;
        double ws[3] = {-dphi + noise(hand), noise(hand), noise(hand)}, rc[3], wc[3];
        screen_to_conforming(rot, rs, rc);
        screen_to_conforming(rot, ws, wc);
        float a[3], g[3];
        to_raw(h, rc, a, scale);
        to_raw(h, wc, g, gyro_flip ? -1 : 1);
        for (int k = 0; k < 3; k++) g[k] += (float)bias;
        motion_cal_feed(&r, a, g, dt);
        if (t > 60) break;
    }
    if (out) *out = r.cal;
    return r.state;
}

// The corrected axes against the truth, for a vector about each screen axis.
static int recovers(const Hal *h, int rot, int gyro_flip, const MotionCal *c, double bias) {
    for (int k = 0; k < 3; k++) {
        double s[3] = {0, 0, 0}, co[3], out[3];
        s[k] = 1;
        screen_to_conforming(rot, s, co);
        float a[3], g[3];
        to_raw(h, co, a, 1);
        to_raw(h, co, g, gyro_flip ? -1 : 1);
        for (int i = 0; i < 3; i++) g[i] += (float)bias;
        motion_cal_apply(c, a, g);
        conforming_to_screen(rot, a, out);
        for (int i = 0; i < 3; i++)
            if (fabs(out[i] - s[i]) > 1e-3) return 0;
        conforming_to_screen(rot, g, out);
        for (int i = 0; i < 3; i++)
            if (fabs(out[i] - s[i]) > 0.02) return 0;
    }
    return 1;
}

int main(void) {
    static const int perm[6][3] = {{1, 2, 3}, {2, 3, 1}, {3, 1, 2}, {2, 1, 3}, {1, 3, 2}, {3, 2, 1}};
    int rotations = 0, refused = 0;
    for (int p = 0; p < 6; p++)
        for (int s = 0; s < 8; s++) {
            Hal h;
            int odd = p >= 3, neg = 0;
            for (int i = 0; i < 3; i++) {
                int minus = s >> i & 1;
                neg += minus;
                h.m[i] = (minus ? -1 : 1) * perm[p][i];
            }
            if ((odd + neg) & 1) continue;   // a mirror image: no real sensor frame
            rotations++;
            for (int rot = 1; rot <= 3; rot += 2)
                for (int flip = 0; flip < 2; flip++) {
                    MotionCal c;
                    int st = run(&h, rot, flip, 29, 40, 1.17, &c, 0.02);
                    // The one layout at each rotation a tilt can't tell from an ordinary device tilted the wrong way
                    // (motion_cal.c): in the screen's axes, x reversed and y and z swapped
                    if (st == MCAL_ERR_TILT && h.m[1] == -2 && h.m[0] == (rot == 1 ? 3 : -3) && h.m[2] == (rot == 1 ? 1 : -1)) {
                        refused++;
                        continue;
                    }
                    CHECK(st == MCAL_OK, "hal %d %d %d rot %d flip %d: state %d", h.m[0], h.m[1], h.m[2], rot, flip, st);
                    if (st != MCAL_OK) continue;
                    CHECK(recovers(&h, rot, flip, &c, 0.02), "hal %d %d %d rot %d flip %d: map %d %d %d sign %d", h.m[0],
                          h.m[1], h.m[2], rot, flip, c.map[0], c.map[1], c.map[2], c.gyro_sign);
                    char text[96];
                    MotionCal back;
                    motion_cal_format(&c, text, sizeof text);
                    CHECK(motion_cal_parse(text, &back) && !memcmp(back.map, c.map, sizeof c.map), "round trip of %s", text);
                }
        }
    CHECK(rotations == 24, "%d rotations of the sensor frame", rotations);
    CHECK(refused == 4, "%d runs refused as a tilt the wrong way", refused);
    Hal std = {{1, 2, 3}};
    MotionCal c;
    CHECK(run(&std, 1, 0, 29, 40, 1.17, &c, 0) == MCAL_OK && motion_cal_is_standard(&c), "a standard device needs no correction");
    for (double lean = 10; lean <= 70; lean += 10)
        CHECK(run(&std, 1, 0, lean, 40, 1, &c, 0) == MCAL_OK && motion_cal_is_standard(&c), "lean %.0f", lean);
    CHECK(run(&std, 1, 0, 29, 10, 1, NULL, 0) == MCAL_ERR_TILT, "too small a tilt");
    CHECK(run(&std, 1, 0, 40, -35, 1, NULL, 0) == MCAL_ERR_TILT, "a tilt toward the player");
    CHECK(run(&std, 3, 0, 40, -35, 1, NULL, 0) == MCAL_ERR_TILT, "a tilt toward the player, turned the other way");
    // A steady gyro's resting error is kept; a hand's is not
    CHECK(run(&std, 1, 0, 29, 40, 1, &c, 0.02) == MCAL_OK && fabs(c.bias[0] - 0.02) < 0.002, "resting error %.4f", c.bias[0]);
    hand = 0.05;
    CHECK(run(&std, 1, 0, 29, 40, 1, &c, 0.02) == MCAL_OK && c.bias[0] == 0, "a hand's drift taken for error: %.4f", c.bias[0]);
    hand = 0.005;
    // The AYN Thor's own numbers, recorded by hand with dumpsys sensorservice: rotation 1, held at (9.60, -1.21, 6.30),
    // the tilt's far end at (1.43, -0.94, 10.55), the gyro at about (0.15, 1.0, 0.03) on the way there.
    {
        MotionCalRun r;
        motion_cal_start(&r, 1);
        float still_a[3] = {9.60f, -1.21f, 6.30f}, far_a[3] = {1.43f, -0.94f, 10.55f}, rest_g[3] = {0, 0, 0}, turn_g[3] = {0.15f, 1.0f, 0.03f};
        for (int i = 0; i < 800; i++) motion_cal_feed(&r, still_a, rest_g, 0.004);
        for (int i = 0; i < 250; i++) {
            float p = (float)i / 249, a[3];
            for (int k = 0; k < 3; k++) a[k] = still_a[k] + (far_a[k] - still_a[k]) * p;
            motion_cal_feed(&r, a, turn_g, 0.004);
        }
        for (int i = 0; i < 250 && r.state == MCAL_TILT; i++) {
            float p = (float)i / 249, a[3], back[3] = {-turn_g[0], -turn_g[1], -turn_g[2]};
            for (int k = 0; k < 3; k++) a[k] = far_a[k] + (still_a[k] - far_a[k]) * p;
            motion_cal_feed(&r, a, back, 0.004);
        }
        CHECK(r.state == MCAL_OK && motion_cal_is_standard(&r.cal), "the Thor's recording: state %d, map %d %d %d, sign %d",
              r.state, r.cal.map[0], r.cal.map[1], r.cal.map[2], r.cal.gyro_sign);
    }
    CHECK(run(&std, 1, 0, 85, 40, 1, NULL, 0) == MCAL_ERR_POSE, "lying nearly flat");
    CHECK(!motion_cal_parse("1 1 3 1 0 0 0", &c) && !motion_cal_parse("1 2 3 2 0 0 0", &c) && !motion_cal_parse("junk", &c),
          "a bad setting is refused");
    printf("motion_cal: %d of %d checks passed, over %d rotations of the sensor frame\n", checks - fails, checks, rotations);
    return fails != 0;
}
