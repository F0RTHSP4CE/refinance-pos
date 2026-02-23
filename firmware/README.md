# XIAO ESP32-S3 POS Fridge Terminal

Modular embedded project for an NFC-based point-of-sale fridge lock using:
- Seeed XIAO ESP32-S3 (Arduino framework)
- PN532 NFC reader (I2C)
- 1602 LCD (I2C)
- Relay driving a solenoid lock
- REST API for authentication, transactions, and balance retrieval

## Flow
1. User taps NFC card
2. Device sends card UID as `value` to `USBUTLER_API_URL/api/public/users/by-identifier`
3. If a user is found, device calls `REFINANCE_API_URL/pos/charge` with `x-pos-secret` and body: `entity_name`, `amount`, `currency`, `to_entity_id`
4. On success: energizes relay (opens door) for the configured hold duration
5. Returns to idle


## Configuration
Configuration steps:
1. Copy `include/secrets.example.h` to `include/secrets.h` and fill in:
  - WIFI_SSID / WIFI_PASS
  - REFINANCE_API_URL (REFINANCE backend base URL, no trailing slash)
  - USBUTLER_API_URL (USBUTLER backend base URL, no trailing slash)
  - POS_SECRET (used as `x-pos-secret` header for `/pos/charge`)
2. Adjust `include/Config.h` for:
  - RELAY_PIN
  - I2C pins
  - Timing constants
  - POS_PRICE / POS_CURRENCY (default price & currency charged)
  - POS_ENTITY_ID (`to_entity_id` for `/pos/charge`)
   (Can also be overridden per build using PlatformIO build_flags)

## Building / Uploading
Use PlatformIO:
- Select `env:xiao_esp32s3` (Seeed XIAO ESP32-S3)
- Upload & open serial monitor @115200 baud

### ESP32-S3 notes
- Default XIAO ESP32-S3 pins are set in `include/Config.h` and can be overridden via `build_flags`.
- Verify wiring for relay and PN532 IRQ/RESET before first power-on.

## Future Improvements
- Add retry / exponential backoff for network calls
- Offline queue for transactions if WiFi down
- Over-the-air (OTA) updates
- Config menu via serial / captive portal
- Door sensor input to detect closure
- Watchdog & brownout handling
- Unit tests for API parser using `env:native`

## License
MIT
