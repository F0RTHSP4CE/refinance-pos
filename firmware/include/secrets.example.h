#pragma once
// Copy this file to secrets.h and edit values. Do NOT commit secrets.h

// WiFi credentials
static const char *WIFI_SSID = "YOUR_WIFI_SSID";
static const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";

// REFINANCE API base (no trailing slash)
static const char *API_BASE = "https://api.example.com";

// POS entity id in REFINANCE backend representing this terminal/fridge
static const char *POS_ENTITY_ID = "REFINANCE_POS_ENTITY_ID";

// Salt used to hash NFC card UID before sending to REFINANCE token endpoint
static const char *CARD_HASH_SALT = "CHANGE_ME_SALT";
