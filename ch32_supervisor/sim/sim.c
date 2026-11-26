// sim.c -- runs the supervisor state machine on a PC against a simple RP
// model. Build and run: make && ./sim   (-v prints state changes)

#include <stdio.h>
#include <string.h>
#include "../supervisor.h"

// ---------------------------------------------------------------- RP model

typedef enum {
    RP_DOWN,              // no 3V3
    RP_BOOTING,           // loading OLED registers
    RP_WAIT_ACK,          // ready line high, waiting for the mirror ack
    RP_RELEASE_READY,     // got the ack, drop the ready line
    RP_RUN,
    RP_SHUTDOWN           // display-off sequence, then the double edge
} rp_state_t;

#define RP_BOOT_MS            300
#define RP_SHUTDOWN_MS        200
#define RP_PULSE_HALF_MS       30   // half-period of the shutdown double pulse
#define RP_PRESS_HISTORY        8

typedef struct {
    rp_state_t state;
    uint8_t    ready_line_out;
    uint32_t   state_entered_at;
    uint8_t    mirror_previous;
    int        press_count;
    uint32_t   press_times[RP_PRESS_HISTORY];
    int        press_index;
    int        got_ack;
} rp_model_t;

static void rp_init(rp_model_t *rp) { memset(rp, 0, sizeof(*rp)); rp->state = RP_DOWN; }

static void rp_tick(rp_model_t *rp, uint8_t rail_3v3_on, uint8_t mirror_in, uint32_t now)
{
    if (!rail_3v3_on) { rp_init(rp); return; }
    if (rp->state == RP_DOWN) {
        rp->state = RP_BOOTING;
        rp->state_entered_at = now;
    }

    uint8_t mirror_rising_edge = (mirror_in && !rp->mirror_previous);
    rp->mirror_previous = mirror_in;

    switch (rp->state) {
    case RP_BOOTING:
        if (now - rp->state_entered_at >= RP_BOOT_MS) {
            rp->ready_line_out = 1;          // "OLED ready"
            rp->state = RP_WAIT_ACK;
            rp->state_entered_at = now;
        }
        break;

    case RP_WAIT_ACK:
        if (mirror_rising_edge) {
            rp->got_ack = 1;
            rp->state = RP_RELEASE_READY;
            rp->state_entered_at = now;
        }
        break;

    case RP_RELEASE_READY:
        rp->ready_line_out = 0;
        rp->state = RP_RUN;
        rp->state_entered_at = now;
        break;

    case RP_RUN:
        if (mirror_rising_edge) {
            rp->press_count++;
            rp->press_times[rp->press_index % RP_PRESS_HISTORY] = now;
            rp->press_index++;
            if (rp->press_index >= 5) {
                uint32_t fifth_last_press = rp->press_times[(rp->press_index - 5) % RP_PRESS_HISTORY];
                if (now - fifth_last_press < 2000) {     // five quick presses = power off
                    rp->state = RP_SHUTDOWN;
                    rp->state_entered_at = now;
                }
            }
        }
        break;

    case RP_SHUTDOWN: {
        // "cut power" = two rising edges: low, high, low, high (held)
        uint32_t elapsed = now - rp->state_entered_at;
        if      (elapsed < RP_SHUTDOWN_MS)                      rp->ready_line_out = 0;
        else if (elapsed < RP_SHUTDOWN_MS + RP_PULSE_HALF_MS)   rp->ready_line_out = 1;
        else if (elapsed < RP_SHUTDOWN_MS + 2*RP_PULSE_HALF_MS) rp->ready_line_out = 0;
        else                                                    rp->ready_line_out = 1;
        break;
    }

    default:
        break;
    }
}

// ------------------------------------------------------------- test harness

static supervisor_t sup;
static rp_model_t   rp;
static uint32_t     now_ms;
static uint8_t      button_down;
static int          failures;
static int          verbose;

// Tests needing a misbehaving RP disable the model and drive PC1 by hand.
static int     rp_model_enabled;
static uint8_t manual_ready_line;

static const char *state_name(supervisor_state_t state)
{
    switch (state) {
    case SUP_OFF:       return "OFF";
    case SUP_WAIT_OLED: return "WAIT_OLED";
    case SUP_VBAT_ACK:  return "VBAT_ACK";
    case SUP_ON:        return "ON";
    case SUP_SHUTDOWN:  return "SHUTDOWN";
    }
    return "?";
}

static void run_for(uint32_t ms)
{
    uint32_t end = now_ms + ms;
    while (now_ms < end) {
        supervisor_inputs_t inputs;
        inputs.button_pressed  = button_down;
        inputs.ready_line_high = rp_model_enabled ? rp.ready_line_out : manual_ready_line;
        inputs.now_ms          = now_ms;

        supervisor_state_t before = sup.state;
        supervisor_tick(&sup, &inputs);
        if (rp_model_enabled) rp_tick(&rp, sup.out.rail_3v3_enable, sup.out.mirror_line, now_ms);

        if (verbose && sup.state != before)
            printf("      %6ums  %-9s -> %-9s  rt=%d ap=%d\n", now_ms, state_name(before),
                   state_name(sup.state), sup.out.rail_3v3_enable, sup.out.vbat_enable);

        now_ms += SUP_TICK_MS;
    }
}

static void press(void)          { button_down = 1; run_for(60); button_down = 0; run_for(60); }
static void hold(uint32_t ms)    { button_down = 1; run_for(ms); button_down = 0; run_for(60); }

static void check(const char *description, int passed)
{
    printf("    %-52s %s\n", description, passed ? "ok" : "FAILED");
    if (!passed) failures++;
}

static void begin_test(const char *name, int rail_bridged)
{
    printf("\n%s\n", name);
    now_ms = 0; button_down = 0; rp_model_enabled = 1; manual_ready_line = 0;
    supervisor_init(&sup, rail_bridged);
    rp_init(&rp);
}

// ------------------------------------------------------------------- tests

static void test_bench_boot(void)
{
    begin_test("bench boot, RT_EN bridged", 1);
    run_for(1000);
    check("supervisor reaches ON", sup.state == SUP_ON);
    check("VBAT enabled", sup.out.vbat_enable == 1);
    check("RP got its ack", rp.got_ack == 1);
    check("PC1 released after handshake", rp.ready_line_out == 0);
}

static void test_cold_boot(void)
{
    begin_test("cold boot, gesture power-on", 0);
    run_for(200);
    check("starts off", sup.state == SUP_OFF && sup.out.rail_3v3_enable == 0);

    for (int i = 0; i < 4; i++) press();
    check("four presses is not enough", sup.out.rail_3v3_enable == 0);

    press();
    check("fifth press turns 3V3 on", sup.out.rail_3v3_enable == 1);

    run_for(1000);
    check("handshake completes to ON", sup.state == SUP_ON);
    check("VBAT enabled", sup.out.vbat_enable == 1);
}

// Regression: a level-triggered ready line read the stuck boot pulse as "shut down".
static void test_ready_stuck_high(void)
{
    begin_test("PC1 left high after boot must not shut down", 1);
    rp_model_enabled = 0;

    manual_ready_line = 1;
    run_for(1000);
    check("handshake still completed", sup.state == SUP_ON);
    check("VBAT came up", sup.out.vbat_enable == 1);

    run_for(3000);
    check("still ON after 3s of PC1 stuck high", sup.state == SUP_ON);
    check("power still up", sup.out.rail_3v3_enable == 1 && sup.out.vbat_enable == 1);

    manual_ready_line = 0; run_for(50);
    manual_ready_line = 1; run_for(30); manual_ready_line = 0; run_for(30); manual_ready_line = 1; run_for(50);
    check("a fresh double edge does trigger shutdown", sup.state != SUP_ON);
}

static void test_shutdown(void)
{
    begin_test("shutdown via five presses", 1);
    run_for(1000);
    check("up and running", sup.state == SUP_ON);

    for (int i = 0; i < 5; i++) press();
    check("RP started its shutdown", rp.state == RP_SHUTDOWN);

    run_for(100);
    while (sup.state == SUP_ON && now_ms < 5000) run_for(SUP_TICK_MS);
    check("VBAT dropped first", sup.out.vbat_enable == 0 && sup.out.rail_3v3_enable == 1);

    run_for(SUP_VBAT_OFF_BEFORE_3V3_MS + 20);
    check("then logic rail dropped", sup.out.rail_3v3_enable == 0);
    check("back to OFF", sup.state == SUP_OFF);
}

static void test_failsafe(void)
{
    begin_test("failsafe long press", 1);
    run_for(1000);
    check("up and running", sup.state == SUP_ON);

    hold(SUP_FAILSAFE_HOLD_MS + 200);
    run_for(SUP_VBAT_OFF_BEFORE_3V3_MS + 50);
    check("power cut by long press", sup.out.rail_3v3_enable == 0 && sup.out.vbat_enable == 0);
    check("back to OFF", sup.state == SUP_OFF);
}

static void test_mirror_without_oled(void)
{
    begin_test("buttons mirror while OLED handshake is pending", 0);
    rp_model_enabled = 0;              // RP hangs during boot
    manual_ready_line = 0;

    for (int i = 0; i < 5; i++) press();
    check("3V3 on, waiting on the RP", sup.state == SUP_WAIT_OLED);

    int mirror_seen = 0;
    button_down = 1; run_for(60);
    for (int i = 0; i < 20; i++) { if (sup.out.mirror_line) mirror_seen = 1; run_for(SUP_TICK_MS); }
    button_down = 0; run_for(120);
    check("press still mirrored to the RP", mirror_seen == 1);
    check("no VBAT without the handshake", sup.out.vbat_enable == 0);
}

static void test_gesture_timeout(void)
{
    begin_test("slow presses do not power on", 0);
    for (int i = 0; i < 4; i++) press();
    run_for(SUP_GESTURE_MAX_GAP_MS + 200);
    press();
    check("still off", sup.state == SUP_OFF && sup.out.rail_3v3_enable == 0);
    press(); press(); press();
    check("still off after 3 more", sup.out.rail_3v3_enable == 0);
    press();
    check("fifth in the new run powers on", sup.out.rail_3v3_enable == 1);
}

// USB reflash reboots the RP while ON: its lone boot edge must not cut power.
static void test_rp_reboot(void)
{
    begin_test("RP reboots underneath a running supervisor (reflash)", 0);
    for (int i = 0; i < 5; i++) press();
    run_for(1000);
    check("up and running", sup.state == SUP_ON);

    rp_init(&rp);
    run_for(RP_BOOT_MS + 400);
    check("supervisor stayed ON", sup.state == SUP_ON);
    check("rails untouched", sup.out.rail_3v3_enable == 1 && sup.out.vbat_enable == 1);
    check("RP's handshake was acked", rp.got_ack == 1);
    check("RP is running again", rp.state == RP_RUN);

    for (int i = 0; i < 5; i++) press();
    run_for(1000);
    check("five presses still shut it down", sup.state == SUP_OFF && sup.out.rail_3v3_enable == 0);
}

static void test_mirror_level(void)
{
    begin_test("button mirrored as a level", 0);
    button_down = 1; run_for(100);
    check("held while OFF: mirror stays low", sup.out.mirror_line == 0);
    button_down = 0; run_for(100);
    for (int i = 0; i < 5; i++) press();
    run_for(1000);
    check("up and running", sup.state == SUP_ON);
    button_down = 1; run_for(100);
    check("held while ON: mirror high after debounce", sup.out.mirror_line == 1);
    run_for(2000);
    check("still high two seconds in (a level, not a pulse)", sup.out.mirror_line == 1);
    button_down = 0; run_for(100);
    check("released: mirror low", sup.out.mirror_line == 0);
}

// The tick clock stops in sleep, so "sleeping" here is just a may_sleep check.
static void test_sleep_gate(void)
{
    begin_test("standby only when idle, never in the boot grace", 0);
    run_for(1000);
    check("idle but inside boot grace: awake", !supervisor_may_sleep(&sup, now_ms));
    run_for(SUP_BOOT_GRACE_MS);
    check("idle after grace: may sleep", supervisor_may_sleep(&sup, now_ms));

    button_down = 1; run_for(10);
    check("button down: awake", !supervisor_may_sleep(&sup, now_ms));
    button_down = 0; run_for(60);

    supervisor_prepare_for_sleep(&sup);
    supervisor_woke_from_sleep(&sup, now_ms); run_for(5);   // tap already over at wake
    check("lone wake press: stays up for more", !supervisor_may_sleep(&sup, now_ms));
    run_for(SUP_GESTURE_MAX_GAP_MS + 50);
    check("nothing followed: back to sleep", supervisor_may_sleep(&sup, now_ms));
    check("still off", sup.out.rail_3v3_enable == 0);

    supervisor_prepare_for_sleep(&sup);
    button_down = 1; supervisor_woke_from_sleep(&sup, now_ms); run_for(60);   // still held at wake
    button_down = 0; run_for(60);
    for (int i = 0; i < 3; i++) press();
    check("wake + 3: not yet, and not sleepy",
          sup.out.rail_3v3_enable == 0 && !supervisor_may_sleep(&sup, now_ms));
    press();
    check("wake + 4 more powers on", sup.out.rail_3v3_enable == 1);
    check("powered: never sleeps", !supervisor_may_sleep(&sup, now_ms));
    run_for(1000);
    check("ON: never sleeps", sup.state == SUP_ON && !supervisor_may_sleep(&sup, now_ms));
}

// Presses before a sleep must not count toward the five after waking.
static void test_sleep_clears_gesture(void)
{
    begin_test("sleep forgets a half-finished gesture", 0);
    run_for(SUP_BOOT_GRACE_MS + 100);
    press(); press();
    run_for(SUP_GESTURE_MAX_GAP_MS + 50);
    check("gap expired: may sleep", supervisor_may_sleep(&sup, now_ms));
    supervisor_prepare_for_sleep(&sup);
    supervisor_woke_from_sleep(&sup, now_ms); run_for(60);
    for (int i = 0; i < 3; i++) press();
    check("wake + 3 is not five", sup.out.rail_3v3_enable == 0);
    press();
    check("wake + 4 powers on", sup.out.rail_3v3_enable == 1);
}

int main(int argc, char **argv)
{
    verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

    printf("Lomenett power supervisor simulation\n");
    printf("tick %dms, gesture %d presses within %dms, failsafe %dms\n",
           SUP_TICK_MS, SUP_GESTURE_PRESS_COUNT, SUP_GESTURE_MAX_GAP_MS, SUP_FAILSAFE_HOLD_MS);

    test_bench_boot();
    test_cold_boot();
    test_ready_stuck_high();
    test_rp_reboot();
    test_mirror_level();
    test_shutdown();
    test_failsafe();
    test_mirror_without_oled();
    test_gesture_timeout();
    test_sleep_gate();
    test_sleep_clears_gesture();

    printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures != 0;
}
