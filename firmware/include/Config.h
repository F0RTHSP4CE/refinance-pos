#pragma once
#include <Arduino.h>

// Pin assignments
// D1 Mini (ESP8266) typical available GPIOs: D1=GPIO5 (SCL), D2=GPIO4 (SDA), D5=14, D6=12, D7=13, D8=15
// Choose a safe GPIO for relay (not pulled low/high at boot). Using D5 (GPIO14).
static const uint8_t RELAY_PIN = 14;

// Timing
// Duration the relay stays energized/open after successful transaction (ms)
static const uint32_t DOOR_HOLD_MS = 4000;

// Secret Network / API values are declared in secrets.h (not committed)
#include "secrets.h" // defines WIFI_SSID, WIFI_PASS, API_BASE, POS_ENTITY_ID, CARD_HASH_SALT

// I2C pin defaults (D1 Mini): SDA=D2(GPIO4) SCL=D1(GPIO5)
static const uint8_t I2C_SDA = 4;
static const uint8_t I2C_SCL = 5;

// PN532 wiring (D1 Mini). Adjust to your wiring.
static const uint8_t PN532_IRQ_PIN = 12;   // D6 -> IRQ
static const uint8_t PN532_RESET_PIN = 13; // D7 -> RESET
#ifndef PN532_IRQ
#define PN532_IRQ PN532_IRQ_PIN
#endif
#ifndef PN532_RESET
#define PN532_RESET PN532_RESET_PIN
#endif

// Diagnostic: enable to brute-force scan several pin pairs at boot to find devices
// #define ENABLE_I2C_BRUTE_SCAN

// PN532 I2C address (default 0x48 or 0x24 depending on board variant) - library handles
// LCD
static const uint8_t LCD_I2C_ADDR = 0x27;
static const uint8_t LCD_COLS = 16;
static const uint8_t LCD_ROWS = 2;

// POS identity (shown on display). Keep <=16 chars for first line clarity.
#ifndef POS_NAME
#define POS_NAME "F0 FRIDGE"
#endif

// Security / Hashing: CARD_HASH_SALT defined in secrets.h

// Pricing (can be overridden via build_flags e.g. -DPOS_PRICE=7.50 -DPOS_CURRENCY=\"USD\")
#ifndef POS_PRICE
#define POS_PRICE 5.0
#endif
#ifndef POS_CURRENCY
#define POS_CURRENCY "GEL"
#endif
