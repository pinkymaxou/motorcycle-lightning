/* Single source of truth for the PCB wiring (schematic V1.0). */
#pragma once

/* Opto-isolated inputs, ACTIVE-LOW (10k pull-ups to 3V3 on the PCB). */
#define PIN_IN_LEFT    32
#define PIN_IN_RIGHT   33
#define PIN_IN_BRAKE   36   /* input-only pin */
#define PIN_IN_AUX     18

/* WS2812B data outputs (through 5V level shifters). */
#define PIN_STRIP_1    26   /* main strip */
#define PIN_STRIP_2    21   /* secondary strip, reserved */

/* On-module peripherals (M5Stamp Pico). */
#define PIN_BUTTON     39   /* input-only, pressed = low */
#define PIN_STATUS_LED 27   /* SK6812, 1 pixel */
