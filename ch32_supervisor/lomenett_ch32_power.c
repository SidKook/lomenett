// lomenett_ch32_power.c -- CH32V003 power supervisor for Lomenett V3.6.
// State machine is in supervisor.h (tested by sim/).
//
// Pins:
//   PA2  select button, active low
//   PA1  button mirror / ack out -> RP GP11
//   PC1  ready in <- RP GP18
//   PC2  AP3015A SHDN (VBAT, OLED panel)
//   PC4  RT9013 EN (3V3, RP + OLED logic)
//
// PC4 is never driven low: RT_EN1 may be solder-bridged high for debugging,
// and driving against it killed a CH32. On = drive high, off = pull-down.
// "Bridged" (EN reads high while off) means: boot as if powered, never sleep.
//
// Standby (wakes on button) kills SWIO, so it is skipped when bridged, in the
// first 10 s after reset, if the button was held at power-on, or when not idle.

#include "ch32fun.h"
#include "supervisor.h"

#define PIN_BUTTON       PA2
#define PIN_MIRROR_OUT   PA1
#define PIN_READY_IN     PC1
#define PIN_VBAT_ENABLE  PC2
#define PIN_3V3_ENABLE   PC4

#define BRIDGE_DETECT_TICKS 5   // consecutive reads needed to decide bridged / not

int main(void)
{
    SystemInit();
    funGpioInitAll();

    funPinMode(PIN_BUTTON, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(PIN_BUTTON, FUN_HIGH);           // pull-up

    funPinMode(PIN_MIRROR_OUT, GPIO_CFGLR_OUT_2Mhz_PP);
    funDigitalWrite(PIN_MIRROR_OUT, FUN_LOW);

    funPinMode(PIN_READY_IN, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(PIN_READY_IN, FUN_LOW);          // pull-down

    funPinMode(PIN_VBAT_ENABLE, GPIO_CFGLR_OUT_2Mhz_PP);
    funDigitalWrite(PIN_VBAT_ENABLE, FUN_LOW);

    funPinMode(PIN_3V3_ENABLE, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(PIN_3V3_ENABLE, FUN_LOW);        // pull-down = rail off
    Delay_Ms(2);
    int rail_bridged = (funDigitalRead(PIN_3V3_ENABLE) == FUN_HIGH);
    int enable_read_high_ticks = 0, enable_read_low_ticks = 0;

    int sleep_disabled = (funDigitalRead(PIN_BUTTON) == FUN_LOW);   // held at power-on

    // Wake on falling edge of PA2 (EXTI2 event), deep sleep = standby.
    RCC->APB1PCENR |= RCC_APB1Periph_PWR;
    RCC->APB2PCENR |= RCC_AFIOEN;
    AFIO->EXTICR   &= ~(uint32_t)(0b11 << (2*2));    // EXTI2 <- port A
    EXTI->EVENR    |= EXTI_Line2;
    EXTI->FTENR    |= EXTI_Line2;
    PWR->CTLR      |= PWR_CTLR_PDDS;

    supervisor_t supervisor;
    supervisor_init(&supervisor, rail_bridged);

    uint32_t now_ms = 0;
    while (1) {
        supervisor_inputs_t inputs;
        inputs.button_pressed  = (funDigitalRead(PIN_BUTTON) == FUN_LOW);
        inputs.ready_line_high = (funDigitalRead(PIN_READY_IN) == FUN_HIGH);
        inputs.now_ms          = now_ms;

        supervisor_tick(&supervisor, &inputs);

        funDigitalWrite(PIN_MIRROR_OUT,  supervisor.out.mirror_line ? FUN_HIGH : FUN_LOW);
        funDigitalWrite(PIN_VBAT_ENABLE, supervisor.out.vbat_enable ? FUN_HIGH : FUN_LOW);
        if (supervisor.out.rail_3v3_enable) {
            funPinMode(PIN_3V3_ENABLE, GPIO_CFGLR_OUT_2Mhz_PP);
            funDigitalWrite(PIN_3V3_ENABLE, FUN_HIGH);
            enable_read_high_ticks = enable_read_low_ticks = 0;
        } else {
            funPinMode(PIN_3V3_ENABLE, GPIO_CFGLR_IN_PUPD);
            funDigitalWrite(PIN_3V3_ENABLE, FUN_LOW);
            // Re-detect the bridge from settled readings while off.
            if (funDigitalRead(PIN_3V3_ENABLE) == FUN_HIGH) {
                enable_read_low_ticks = 0;
                if (enable_read_high_ticks < BRIDGE_DETECT_TICKS) enable_read_high_ticks++;
            } else {
                enable_read_high_ticks = 0;
                if (enable_read_low_ticks < BRIDGE_DETECT_TICKS) enable_read_low_ticks++;
            }
            if (enable_read_high_ticks >= BRIDGE_DETECT_TICKS) rail_bridged = 1;
            if (enable_read_low_ticks  >= BRIDGE_DETECT_TICKS) rail_bridged = 0;
        }

        if (!rail_bridged && enable_read_low_ticks >= BRIDGE_DETECT_TICKS &&
            !sleep_disabled && supervisor_may_sleep(&supervisor, now_ms)) {
            // Outputs are all low in SUP_OFF and hold through standby.
            supervisor_prepare_for_sleep(&supervisor);
            PFIC->SCTLR |= (1 << 2);                 // SLEEPDEEP
            __WFE();                                 // ch32fun flushes stale events first
            PFIC->SCTLR &= ~(1 << 2);
            SystemInit();
            supervisor_woke_from_sleep(&supervisor, now_ms);
            continue;
        }

        Delay_Ms(SUP_TICK_MS);
        now_ms += SUP_TICK_MS;
    }
}
