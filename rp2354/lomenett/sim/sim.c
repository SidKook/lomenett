// sim.c -- PC tests for lomenett_logic.h (wheel, gesture, gauge, orientation).
// Build and run: make && ./sim

#include <stdio.h>
#include <string.h>
#include "../lomenett_logic.h"

static int failures;

static void check(const char *description, int passed)
{
    printf("    %-52s %s\n", description, passed ? "ok" : "FAILED");
    if (!passed) failures++;
}

// One detent = a full gray-code cycle, resting at 11.
static const uint8_t CLOCKWISE_CYCLE[4]        = { 0b01, 0b00, 0b10, 0b11 };
static const uint8_t COUNTERCLOCKWISE_CYCLE[4] = { 0b10, 0b00, 0b01, 0b11 };

static long turn_wheel(const uint8_t *cycle, int clicks, uint8_t *ab_previous, int8_t *quarter_steps)
{
    long position = 0;
    for (int click = 0; click < clicks; click++)
        for (int i = 0; i < 4; i++)
            position += wheel_decode_step(cycle[i], ab_previous, quarter_steps);
    return position;
}

static void test_wheel_direction(void)
{
    printf("\nwheel decoding\n");
    uint8_t ab_previous = 0b11; int8_t quarter_steps = 0;

    long position = turn_wheel(CLOCKWISE_CYCLE, 10, &ab_previous, &quarter_steps);
    check("ten clicks clockwise gives +10", position == 10);

    position = turn_wheel(COUNTERCLOCKWISE_CYCLE, 10, &ab_previous, &quarter_steps);
    check("ten clicks back gives -10", position == -10);

    position = turn_wheel(CLOCKWISE_CYCLE, 3, &ab_previous, &quarter_steps)
             + turn_wheel(COUNTERCLOCKWISE_CYCLE, 3, &ab_previous, &quarter_steps);
    check("three each way nets zero", position == 0);
}

static void test_wheel_partial(void)
{
    printf("\npartial turns and bounce\n");
    uint8_t ab_previous = 0b11; int8_t quarter_steps = 0;

    long position = 0;   // half a click and back
    position += wheel_decode_step(0b01, &ab_previous, &quarter_steps);
    position += wheel_decode_step(0b00, &ab_previous, &quarter_steps);
    position += wheel_decode_step(0b01, &ab_previous, &quarter_steps);
    position += wheel_decode_step(0b11, &ab_previous, &quarter_steps);
    check("rocking between detents counts nothing", position == 0);

    ab_previous = 0b11; quarter_steps = 0; position = 0;
    const uint8_t bouncy[] = { 0b01, 0b11, 0b01, 0b11, 0b01, 0b00, 0b10, 0b11 };
    for (unsigned i = 0; i < sizeof(bouncy); i++)
        position += wheel_decode_step(bouncy[i], &ab_previous, &quarter_steps);
    check("a bouncing contact still counts one click", position == 1);

    ab_previous = 0b11; quarter_steps = 0;
    position = wheel_decode_step(0b00, &ab_previous, &quarter_steps);   // 11 -> 00 is illegal
    check("an impossible jump is ignored", position == 0);
}

static void test_gesture(void)
{
    printf("\nfive press power-off gesture\n");
    uint32_t press_times[5]; uint8_t press_count = 0; int fired = 0;
    memset(press_times, 0, sizeof(press_times));

    for (int i = 0; i < 4; i++)
        fired |= press_gesture_detected(press_times, &press_count, 5, 2000, 100 * i);
    check("four quick presses do nothing", !fired);

    fired = press_gesture_detected(press_times, &press_count, 5, 2000, 400);
    check("the fifth fires it", fired);

    press_count = 0; memset(press_times, 0, sizeof(press_times)); fired = 0;
    for (int i = 0; i < 8; i++)
        fired |= press_gesture_detected(press_times, &press_count, 5, 2000, 3000 * (i + 1));
    check("presses 3s apart never fire", !fired);

    press_count = 0; memset(press_times, 0, sizeof(press_times)); fired = 0;
    uint32_t now = 0;
    for (int i = 0; i < 4; i++) { now += 3000; press_gesture_detected(press_times, &press_count, 5, 2000, now); }
    for (int i = 0; i < 5; i++) { now += 100;  fired |= press_gesture_detected(press_times, &press_count, 5, 2000, now); }
    check("a quick run after slow ones still fires", fired);
}

// Deterministic +/-15 mV noise, like one raw ADC sample.
static float adc_noise(int i) { return ((i * 7) % 5 - 2) * 0.0075f; }

static void test_gauge(void)
{
    printf("\nbattery gauge\n");
    gauge_t gauge; gauge_init(&gauge);
    uint32_t now = 0; int percent, first_percent, changed;

    first_percent = gauge_update(&gauge, 3.71f, 1, now); changed = 0;
    for (int i = 1; i < 200; i++) {
        now += 250;
        percent = gauge_update(&gauge, 3.71f + adc_noise(i), 1, now);
        changed |= (percent != first_percent);
    }
    check("noisy but steady reading never changes the percent", !changed);

    // Unplug: 3.71 V sags to 3.46 V over a few seconds.
    int before_unplug = gauge_update(&gauge, 3.71f, 1, now);
    int display_changes = 0, last_percent = before_unplug;
    for (int i = 0; i < 24; i++) {
        now += 250;
        float sag = 0.25f * (1.0f - 1.0f / (1.0f + i * 0.5f));
        percent = gauge_update(&gauge, 3.71f - sag + adc_noise(i), 0, now);
        if (percent != last_percent) display_changes++;
        last_percent = percent;
    }
    float total_offset = gauge.battery_offset_volts + gauge.usb_offset_volts;
    check("the display moves once across the unplug, not in steps", display_changes <= 1);
    check("learned offsets sum to the step", total_offset > 0.20f && total_offset < 0.30f);
    check("learned flag raised for the caller", gauge.offsets_learned);
    int expected_percent = gauge_percent_from_ocv(gauge.filtered_volts + gauge.battery_offset_volts);
    check("battery percent matches the corrected estimate",
          percent >= expected_percent - 2 && percent <= expected_percent + 2);

    gauge.offsets_learned = 0;
    for (int i = 0; i < 40; i++) { now += 250; percent = gauge_update(&gauge, 3.46f + 0.25f + adc_noise(i), 1, now); }
    check("USB and battery estimates agree after learning", percent >= last_percent - 3 && percent <= last_percent + 3);

    for (int i = 0; i < 8; i++)  { now += 250; percent = gauge_update(&gauge, 3.46f, 0, now); }
    for (int i = 0; i < 24; i++) { now += 250; percent = gauge_update(&gauge, 3.46f, 0, now); }
    last_percent = percent; int rose = 0;
    for (int i = 0; i < 400; i++) {
        now += 250;
        percent = gauge_update(&gauge, 3.46f - i * 0.0005f + adc_noise(i), 0, now);
        if (percent > last_percent) rose = 1;
        last_percent = percent;
    }
    check("on battery the percent never rises", !rose);
    check("and it does fall as the cell drains", percent < 10);

    int fell = 0; last_percent = -1;
    for (int i = 0; i < 400; i++) {
        now += 250;
        percent = gauge_update(&gauge, 3.45f + i * 0.001f + adc_noise(i), 1, now);
        if (last_percent >= 0 && percent < last_percent) fell = 1;
        last_percent = percent;
    }
    check("on USB the percent never dips while charging", !fell);
    int held_percent = gauge.last_battery_percent;
    for (int i = 0; i < 60; i++) { now += 250; percent = gauge_update(&gauge, 4.19f + adc_noise(i), 1, now); }
    check("on USB the charger's 4.2 V doesn't read as 100", percent == held_percent && percent < 100);

    // Worn cell: 3.45 V on battery, pinned at 4.2 V once plugged in.
    gauge_init(&gauge); now = 0;
    for (int i = 0; i < 40; i++) { now += 250; percent = gauge_update(&gauge, 3.45f + adc_noise(i), 0, now); }
    int battery_percent = percent;
    for (int i = 0; i < 40; i++) { now += 250; percent = gauge_update(&gauge, 4.20f + adc_noise(i), 1, now); }
    check("plugged in keeps the last battery percent", percent == battery_percent);

    gauge_init(&gauge); now = 0;
    for (int i = 0; i < 60; i++) { now += 250; percent = gauge_update(&gauge, 4.19f + adc_noise(i), 1, now); }
    check("booted on USB with no battery reading falls back to the voltage", percent == 100);

    gauge_init(&gauge);
    check("0 V reads 0 percent", gauge_update(&gauge, 0.0f, 0, 0) == 0);
}

static void test_orient(void)
{
    printf("\norientation\n");
    check("+X down is sector 0",  orient_sector_from_accel( 1000,   50, 9) == 0);
    check("+Y down is sector 1",  orient_sector_from_accel(   50, 1000, 9) == 1);
    check("-X down is sector 2",  orient_sector_from_accel(-1000,   50, 9) == 2);
    check("-Y down is sector 3",  orient_sector_from_accel(   50,-1000, 9) == 3);
    check("lying flat keeps the current sector", orient_sector_from_accel(40, -60, 3) == 3);
    check("a 45 degree tilt keeps the current sector", orient_sector_from_accel(700, 720, 1) == 1);

    orient_t orient; orient_init(&orient, 0);
    int sector = 0;
    for (int i = 0; i < 2; i++) sector = orient_update(&orient, 50, 1000, 3);
    check("two polls of a new sector do not switch yet", sector == 0);
    sector = orient_update(&orient, 50, 1000, 3);
    check("the third poll switches", sector == 1);
    sector = orient_update(&orient, -1000, 0, 3);
    sector = orient_update(&orient, 50, 1000, 3);
    sector = orient_update(&orient, -1000, 0, 3);
    check("a glimpse of another sector while waving does nothing", sector == 1);
    for (int i = 0; i < 5; i++) sector = orient_update(&orient, 0, 0, 3);
    check("setting it down flat keeps the last orientation", sector == 1);
}

int main(void)
{
    printf("Lomenett RP-side logic simulation\n");
    test_wheel_direction();
    test_wheel_partial();
    test_gesture();
    test_gauge();
    test_orient();
    printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures != 0;
}
