// lomenett_logic.h -- hardware-free logic shared by the sketch and sim/sim.c:
// wheel decoding, the five-press gesture, the battery gauge, orientation.

#ifndef LOMENETT_LOGIC_H
#define LOMENETT_LOGIC_H

#include <stdint.h>

// ------------------------------------------------------------------ wheel
// Quadrature table indexed by (previous AB << 2) | current AB. Zero entries
// are illegal two-bit jumps (contact bounce), so bounce is ignored.
static const int8_t QUADRATURE_STEP_TABLE[16] = { 0,-1, 1, 0,  1, 0, 0,-1,
                                                 -1, 0, 0, 1,  0, 1,-1, 0 };

// Feed the new AB reading; returns -1, 0 or +1 detents. Four quarter-steps
// make one detent.
static inline int wheel_decode_step(uint8_t ab_now, uint8_t *ab_previous, int8_t *quarter_steps)
{
    int8_t step = QUADRATURE_STEP_TABLE[((*ab_previous & 3) << 2) | (ab_now & 3)];
    *ab_previous = ab_now;
    if (!step) return 0;

    *quarter_steps += step;
    if (*quarter_steps >= 4)  { *quarter_steps -= 4; return  1; }
    if (*quarter_steps <= -4) { *quarter_steps += 4; return -1; }
    return 0;
}

// ---------------------------------------------------------------- gesture
// Record a press; true when the last `presses_needed` presses all fell
// within `window_ms`. press_times must hold presses_needed entries.
static inline int press_gesture_detected(uint32_t *press_times, uint8_t *press_count,
                                         uint8_t presses_needed, uint32_t window_ms, uint32_t now)
{
    press_times[*press_count % presses_needed] = now;
    (*press_count)++;
    if (*press_count < presses_needed) return 0;
    uint32_t oldest_press = press_times[(*press_count - presses_needed) % presses_needed];
    return (now - oldest_press) < window_ms;
}

// ---------------------------------------------------------------- battery
// Terminal voltage reads high while charging and low under load. The gauge
// filters it, estimates open-circuit voltage (OCV) with learned offsets, and
// only moves the shown percent when it really changes. Offsets are learned
// from the voltage drop after an unplug (lowest point within a window,
// because a small pouch cell keeps sagging for a few seconds).

typedef struct {
    float    filtered_volts;              // 0 = no reading yet
    float    battery_offset_volts;        // added to loaded voltage to estimate OCV
    float    usb_offset_volts;            // subtracted from charging voltage
    int      shown_percent;               // -1 = none yet
    int      on_usb;                      // -1 = unknown
    uint32_t source_changed_at;
    float    volts_before_unplug;
    float    lowest_volts_since_unplug;
    int      learning;                    // inside the post-unplug window
    int      offsets_learned;             // set when new offsets adopted; caller clears
    int      last_battery_percent;        // -1 = never ran on battery
} gauge_t;

// The board draws ~2x the charge current, so ~2/3 of the unplug step is load sag.
#define GAUGE_DEFAULT_BATTERY_OFFSET 0.15f
#define GAUGE_DEFAULT_USB_OFFSET     0.08f
#define GAUGE_BATTERY_SHARE_OF_STEP  0.65f
#define GAUGE_LEARN_WINDOW_MS        5000

static inline void gauge_init(gauge_t *gauge)
{
    gauge->filtered_volts = 0;
    gauge->battery_offset_volts = GAUGE_DEFAULT_BATTERY_OFFSET;
    gauge->usb_offset_volts = GAUGE_DEFAULT_USB_OFFSET;
    gauge->shown_percent = -1;
    gauge->on_usb = -1;
    gauge->source_changed_at = 0;
    gauge->volts_before_unplug = 0;
    gauge->lowest_volts_since_unplug = 0;
    gauge->learning = 0;
    gauge->offsets_learned = 0;
    gauge->last_battery_percent = -1;
}

// Generic LiPo curve: open-circuit voltage -> percent.
static inline int gauge_percent_from_ocv(float volts)
{
    static const struct { float volts; int percent; } curve[] = {
        {3.30f,0},{3.50f,10},{3.60f,25},{3.70f,45},{3.80f,60},
        {3.90f,75},{4.00f,85},{4.10f,95},{4.20f,100}};
    if (volts <= curve[0].volts) return 0;
    for (int i = 1; i < 9; i++)
        if (volts < curve[i].volts)
            return curve[i-1].percent + (int)((volts - curve[i-1].volts) / (curve[i].volts - curve[i-1].volts)
                                              * (curve[i].percent - curve[i-1].percent) + 0.5f);
    return 100;
}

static inline float gauge_estimate_ocv(const gauge_t *gauge)
{
    return gauge->on_usb ? gauge->filtered_volts - gauge->usb_offset_volts
                         : gauge->filtered_volts + gauge->battery_offset_volts;
}

// Feed one reading (a few Hz); returns the percent to display.
// On battery the percent only falls. On USB it holds the last battery percent
// (the charger pins the terminal near 4.2 V), unless the board never ran on
// battery since boot, in which case it estimates from the charging voltage.
static inline int gauge_update(gauge_t *gauge, float volts, int on_usb, uint32_t now)
{
    if (on_usb != gauge->on_usb) {
        if (gauge->on_usb == 1 && !on_usb && gauge->filtered_volts > 0) {   // just unplugged
            gauge->volts_before_unplug = gauge->filtered_volts;
            gauge->lowest_volts_since_unplug = volts;
            gauge->learning = 1;
        } else {
            gauge->learning = 0;
        }
        gauge->on_usb = on_usb;
        gauge->source_changed_at = now;
        gauge->filtered_volts = volts;                       // restart the filter
        if (!gauge->learning) gauge->shown_percent = -1;     // else hold until learned
        if (on_usb && gauge->last_battery_percent >= 0) gauge->shown_percent = gauge->last_battery_percent;
    } else if (gauge->filtered_volts <= 0) {
        gauge->filtered_volts = volts;
    } else {
        gauge->filtered_volts += 0.15f * (volts - gauge->filtered_volts);   // ~1.5 s at 4 Hz
    }

    if (gauge->learning) {
        if (gauge->filtered_volts < gauge->lowest_volts_since_unplug)
            gauge->lowest_volts_since_unplug = gauge->filtered_volts;
        if ((uint32_t)(now - gauge->source_changed_at) >= GAUGE_LEARN_WINDOW_MS) {
            float unplug_step = gauge->volts_before_unplug - gauge->lowest_volts_since_unplug;
            if (unplug_step > 0.03f && unplug_step < 0.60f) {
                gauge->battery_offset_volts = unplug_step * GAUGE_BATTERY_SHARE_OF_STEP;
                gauge->usb_offset_volts = unplug_step * (1.0f - GAUGE_BATTERY_SHARE_OF_STEP);
                gauge->offsets_learned = 1;
            }
            gauge->learning = 0;
            gauge->shown_percent = -1;                       // one clean jump to the new value
        } else {
            return gauge->shown_percent < 0 ? gauge_percent_from_ocv(gauge_estimate_ocv(gauge))
                                            : gauge->shown_percent;
        }
    }

    if (on_usb && gauge->last_battery_percent >= 0) return gauge->shown_percent;

    int percent = gauge_percent_from_ocv(gauge_estimate_ocv(gauge));
    if (on_usb && gauge->filtered_volts >= 4.17f) percent = 100;   // charge terminated

    if (gauge->shown_percent < 0) {
        gauge->shown_percent = percent;
    } else if (!on_usb) {
        if (percent <= gauge->shown_percent - 2) gauge->shown_percent = percent;
    } else if (percent >= gauge->shown_percent + 2 || percent <= gauge->shown_percent - 6) {
        gauge->shown_percent = percent;
    }
    if (!on_usb) gauge->last_battery_percent = gauge->shown_percent;
    return gauge->shown_percent;
}

// ------------------------------------------------------------ orientation
// Which edge is down, from the accelerometer's in-plane axes (milli-g).
// Sector: 0 = +X down, 1 = +Y down, 2 = -X down, 3 = -Y down. Returns
// current_sector when lying flat or ambiguous, so moving it doesn't flip.

#define ORIENT_MIN_GRAVITY_MG    500   // in-plane gravity needed to trust it
#define ORIENT_AXIS_MARGIN_MG    200   // one axis must beat the other by this

static inline int orient_sector_from_accel(int accel_x_mg, int accel_y_mg, int current_sector)
{
    int abs_x = accel_x_mg < 0 ? -accel_x_mg : accel_x_mg;
    int abs_y = accel_y_mg < 0 ? -accel_y_mg : accel_y_mg;
    if (abs_x < ORIENT_MIN_GRAVITY_MG && abs_y < ORIENT_MIN_GRAVITY_MG) return current_sector;   // flat
    int difference = abs_x - abs_y;
    if (difference < ORIENT_AXIS_MARGIN_MG && difference > -ORIENT_AXIS_MARGIN_MG) return current_sector;
    if (abs_x > abs_y) return accel_x_mg > 0 ? 0 : 2;
    return accel_y_mg > 0 ? 1 : 3;
}

// Debounced orientation: a new sector must be seen `polls_required` times in a row.
typedef struct { int sector; int candidate_sector; uint8_t candidate_polls; } orient_t;

static inline void orient_init(orient_t *orient, int sector)
{
    orient->sector = sector;
    orient->candidate_sector = sector;
    orient->candidate_polls = 0;
}

static inline int orient_update(orient_t *orient, int accel_x_mg, int accel_y_mg, uint8_t polls_required)
{
    int seen = orient_sector_from_accel(accel_x_mg, accel_y_mg, orient->sector);
    if (seen == orient->sector) {
        orient->candidate_sector = seen;
        orient->candidate_polls = 0;
    } else if (seen != orient->candidate_sector) {
        orient->candidate_sector = seen;
        orient->candidate_polls = 1;
    } else if (++orient->candidate_polls >= polls_required) {
        orient->sector = seen;
        orient->candidate_polls = 0;
    }
    return orient->sector;
}

#endif // LOMENETT_LOGIC_H
