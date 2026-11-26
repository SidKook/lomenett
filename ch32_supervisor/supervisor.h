// supervisor.h -- CH32 power supervisor state machine. Pure logic (no
// hardware), so the firmware and sim/sim.c share it.

#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#include <stdint.h>

#define SUP_TICK_MS                    5
#define SUP_DEBOUNCE_MS               30
#define SUP_GESTURE_MAX_GAP_MS      2000   // max gap between power-on presses
#define SUP_GESTURE_PRESS_COUNT        5
#define SUP_ACK_PULSE_MS             100
#define SUP_VBAT_SETTLE_MS           100
#define SUP_VBAT_OFF_BEFORE_3V3_MS    50
#define SUP_FAILSAFE_HOLD_MS        8000   // hold button this long to force power off
#define SUP_SHUTDOWN_DOUBLE_EDGE_MS  150   // two ready edges this close = "cut power"
#define SUP_BOOT_GRACE_MS          10000   // no sleep this soon after reset (debug window)

typedef enum {
    SUP_OFF,             // rails down, watching for the power-on gesture
    SUP_WAIT_OLED,       // 3V3 up, waiting for the RP's "OLED ready" edge
    SUP_VBAT_ACK,        // VBAT up, sending the ack pulse
    SUP_ON,              // fully powered
    SUP_SHUTDOWN         // dropping rails in order
} supervisor_state_t;

typedef struct {
    uint8_t  button_pressed;     // select button is physically down
    uint8_t  ready_line_high;    // RP is driving PC1 high
    uint32_t now_ms;
} supervisor_inputs_t;

typedef struct {
    uint8_t mirror_line;         // PA1 -> RP GP11: debounced button level OR ack pulse
    uint8_t vbat_enable;         // PC2 -> AP3015 SHDN (VBAT, OLED panel)
    uint8_t rail_3v3_enable;     // PC4 -> RT9013 EN (3V3, RP + OLED logic)
} supervisor_outputs_t;

typedef struct {
    supervisor_state_t   state;
    supervisor_outputs_t out;

    uint8_t  button_raw;
    uint8_t  button_debounced;
    uint8_t  ready_line_previous;
    uint32_t button_raw_changed_at;
    uint32_t button_pressed_at;
    uint32_t state_entered_at;
    uint32_t ack_pulse_started_at;
    uint32_t last_gesture_press_at;
    uint8_t  gesture_press_count;
    uint8_t  ack_pulse_active;
    uint8_t  vbat_ack_sent;
    uint8_t  ready_edge_pending;      // one ready edge seen while ON, waiting for a second
    uint32_t ready_edge_at;
} supervisor_t;

// start_on: 3V3 is already live (RT_EN bridged on the bench), skip the gesture.
static void supervisor_init(supervisor_t *sup, uint8_t start_on)
{
    uint8_t *bytes = (uint8_t*)sup;
    for (uint8_t i = 0; i < sizeof(*sup); i++) bytes[i] = 0;

    if (start_on) {
        sup->state = SUP_WAIT_OLED;
        sup->out.rail_3v3_enable = 1;
    } else {
        sup->state = SUP_OFF;
    }
}

static void supervisor_enter_state(supervisor_t *sup, supervisor_state_t new_state, uint32_t now)
{
    sup->state = new_state;
    sup->state_entered_at = now;
}

static void supervisor_start_ack_pulse(supervisor_t *sup, uint32_t now)
{
    sup->ack_pulse_active = 1;
    sup->ack_pulse_started_at = now;
}

static void supervisor_tick(supervisor_t *sup, const supervisor_inputs_t *in)
{
    uint32_t now = in->now_ms;

    // Debounce the button and detect a clean press.
    if (in->button_pressed != sup->button_raw) {
        sup->button_raw = in->button_pressed;
        sup->button_raw_changed_at = now;
    }
    uint8_t button_press_edge = 0;
    if ((now - sup->button_raw_changed_at) >= SUP_DEBOUNCE_MS &&
        sup->button_debounced != sup->button_raw) {
        sup->button_debounced = sup->button_raw;
        if (sup->button_debounced) {
            button_press_edge = 1;
            sup->button_pressed_at = now;
        }
    }

    // Ready line (PC1) is edge-triggered only: a line left high must not be
    // re-read as the next message. One edge at boot = "OLED ready"; two edges
    // within SUP_SHUTDOWN_DOUBLE_EDGE_MS while ON = "cut power"; one lone edge
    // while ON = RP rebooted (e.g. USB reflash), so just ack it.
    uint8_t ready_rising_edge = (in->ready_line_high && !sup->ready_line_previous);
    sup->ready_line_previous = in->ready_line_high;

    if (sup->ack_pulse_active && (now - sup->ack_pulse_started_at) >= SUP_ACK_PULSE_MS)
        sup->ack_pulse_active = 0;

    // Failsafe: long hold forces power off even if the RP is hung.
    if (sup->state != SUP_OFF && sup->state != SUP_SHUTDOWN &&
        sup->button_debounced && (now - sup->button_pressed_at) >= SUP_FAILSAFE_HOLD_MS) {
        sup->out.vbat_enable = 0;
        supervisor_enter_state(sup, SUP_SHUTDOWN, now);
        return;
    }

    switch (sup->state) {

    case SUP_OFF:
        if (button_press_edge) {
            if (sup->gesture_press_count && (now - sup->last_gesture_press_at) <= SUP_GESTURE_MAX_GAP_MS)
                sup->gesture_press_count++;
            else
                sup->gesture_press_count = 1;
            sup->last_gesture_press_at = now;

            if (sup->gesture_press_count >= SUP_GESTURE_PRESS_COUNT) {
                sup->gesture_press_count = 0;
                sup->out.rail_3v3_enable = 1;          // RP starts booting
                supervisor_enter_state(sup, SUP_WAIT_OLED, now);
            }
        }
        break;

    case SUP_WAIT_OLED:
        if (ready_rising_edge) {
            sup->out.vbat_enable = 1;
            sup->vbat_ack_sent = 0;
            supervisor_enter_state(sup, SUP_VBAT_ACK, now);
        }
        break;

    case SUP_VBAT_ACK:
        if (!sup->vbat_ack_sent) {
            if ((now - sup->state_entered_at) >= SUP_VBAT_SETTLE_MS) {
                supervisor_start_ack_pulse(sup, now);
                sup->vbat_ack_sent = 1;
            }
        } else if (!sup->ack_pulse_active) {
            supervisor_enter_state(sup, SUP_ON, now);
        }
        break;

    case SUP_ON:
        if (ready_rising_edge) {
            if (sup->ready_edge_pending && (now - sup->ready_edge_at) <= SUP_SHUTDOWN_DOUBLE_EDGE_MS) {
                sup->ready_edge_pending = 0;           // second edge: shutdown request
                sup->out.vbat_enable = 0;              // panel power off first
                supervisor_enter_state(sup, SUP_SHUTDOWN, now);
            } else {
                sup->ready_edge_pending = 1;
                sup->ready_edge_at = now;
            }
        } else if (sup->ready_edge_pending && (now - sup->ready_edge_at) > SUP_SHUTDOWN_DOUBLE_EDGE_MS) {
            sup->ready_edge_pending = 0;               // lone edge: RP rebooted, ack it
            supervisor_start_ack_pulse(sup, now);
        }
        break;

    case SUP_SHUTDOWN:
        if ((now - sup->state_entered_at) >= SUP_VBAT_OFF_BEFORE_3V3_MS) {
            sup->out.rail_3v3_enable = 0;
            sup->ack_pulse_active = 0;
            sup->gesture_press_count = 0;
            sup->ready_edge_pending = 0;
            supervisor_enter_state(sup, SUP_OFF, now);
        }
        break;
    }

    // Mirror line = button level while the RP is powered (so it can time
    // holds), plus ack pulses.
    uint8_t rp_powered = (sup->state != SUP_OFF && sup->state != SUP_SHUTDOWN);
    sup->out.mirror_line = sup->ack_pulse_active || (rp_powered && sup->button_debounced);
}

// Standby disables SWIO, so a chip that sleeps right after reset can't be
// reflashed. Only sleep when off, idle, and past the boot grace period.
static uint8_t supervisor_may_sleep(const supervisor_t *sup, uint32_t now)
{
    if (now < SUP_BOOT_GRACE_MS)                          return 0;
    if (sup->state != SUP_OFF)                            return 0;
    if (sup->button_raw || sup->button_debounced)         return 0;
    if (sup->gesture_press_count &&
        (now - sup->last_gesture_press_at) <= SUP_GESTURE_MAX_GAP_MS) return 0;
    return 1;
}

// The tick clock stops in standby, so drop any stale gesture count.
static void supervisor_prepare_for_sleep(supervisor_t *sup)
{
    sup->gesture_press_count = 0;
}

// The wake press may be over before clocks are back: count it as press one
// and mark the button down so the debouncer only sees the release.
static void supervisor_woke_from_sleep(supervisor_t *sup, uint32_t now)
{
    sup->button_raw = 1;
    sup->button_debounced = 1;
    sup->button_raw_changed_at = now;
    sup->button_pressed_at = now;
    sup->gesture_press_count = 1;
    sup->last_gesture_press_at = now;
}

#endif // SUPERVISOR_H
