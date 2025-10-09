#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "Config.h"
#include "LoggerSerial.h"
#include <Wire.h>
#include "NfcReader.h"
#include "Display.h"
#include "RelayLock.h"
#include "HttpClient.h"
#include "StateMachine.h"
#include "Sha256.h"

LoggerSerial logger;
Display display(logger);
RelayLock lockCtrl;
ApiClient api(logger);
NfcReader *nfcReader; // created after Wire init
PosContext posCtx;
PosState state = PosState::BOOT;
unsigned long lastHeartbeat = 0;

// Using default global Wire instance instead of a separate TwoWire

String sha256Hex(const String &input) { return computeSha256Hex(input); }

bool ensureWifi(uint32_t timeoutMs = 12000)
{
    if (WiFi.status() == WL_CONNECTED)
        return true;
    logger.warn("WiFi reconnect...");
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs)
    {
        delay(300);
        Serial.print('~');
    }
    if (WiFi.status() == WL_CONNECTED)
    {
        logger.info(String("WiFi IP ") + WiFi.localIP().toString());
        display.showMessage("WiFi OK", WiFi.localIP().toString());
        delay(300);
        return true;
    }
    display.showMessage("WiFi FAIL");
    return false;
}

void setState(PosState s)
{
    if (state == s)
        return; // no change
    state = s;
    posCtx.stateSince = millis();
    if (state == PosState::IDLE)
    {
        // Initialize idle screen once when entering IDLE
        display.showIdle(POS_NAME, posCtx.amount, posCtx.currency);
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    for (int i = 0; i < 20 && !Serial; ++i)
    {
        delay(50);
    }
    Serial.println();
    logger.begin(Serial);
    Serial.println(F("[BOOT] ReFinance POS starting"));
    logger.info("Serial initialized");
    // Initialize I2C before any peripheral using it (start at 100k for stability)
    auto beginAndScan = [&](uint8_t sda, uint8_t scl, uint32_t freq)
    {
        // Re-init not needed on ESP8266; just begin with desired pins
        pinMode(sda, INPUT_PULLUP);
        pinMode(scl, INPUT_PULLUP);
        delay(2);
        Wire.begin(sda, scl); // ESP8266 path only
        Wire.setClock(freq);
        Serial.print(F("[I2C] Init SDA="));
        Serial.print(sda);
        Serial.print(F(" SCL="));
        Serial.print(scl);
        Serial.print(F(" F="));
        Serial.println(freq);
        // Check bus idle (both high)
        int sdaLevel = digitalRead(sda);
        int sclLevel = digitalRead(scl);
        Serial.print(F("[I2C] Levels SDA="));
        Serial.print(sdaLevel);
        Serial.print(F(" SCL="));
        Serial.println(sclLevel);
        uint8_t found = 0;
        for (uint8_t addr = 1; addr < 127; addr++)
        {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0)
            {
                Serial.print(F("[I2C] Found device @ 0x"));
                Serial.println(addr, HEX);
                found++;
                delay(2);
            }
        }
        Serial.print(F("[I2C] Devices found: "));
        Serial.println(found);
        return found;
    };

    uint8_t activeSDA = I2C_SDA;
    uint8_t activeSCL = I2C_SCL;
    uint8_t devices = 0;

    devices = beginAndScan(activeSDA, activeSCL, 100000);

    if (devices == 0)
    {
        logger.warn("No I2C devices detected. Check wiring & pull-ups.");
    }
    else
    {
        logger.info("I2C scan complete");
    }

    display.begin();
    lockCtrl.begin();
    nfcReader = new NfcReader(Wire, logger);
    if (!nfcReader->begin())
    {
        display.showMessage("NFC FAIL");
    }
    else
    {
        logger.info("PN532 init OK");
    }
    ensureWifi();
    // DNS test for API host
    IPAddress testIp;
    if (WiFi.hostByName(String(API_BASE).substring(String(API_BASE).indexOf("//") + 2).c_str(), testIp))
    {
        logger.info(String("DNS OK ") + testIp.toString());
    }
    else
    {
        logger.error("DNS lookup failed at setup");
    }
    api.begin();
    setState(PosState::IDLE);
    logger.info("Setup complete, entering IDLE");
}

void loopIdle()
{
    String uid;
    if (nfcReader->readCardUID(uid))
    {
        posCtx.cardUID = uid;
        posCtx.cardHash = sha256Hex(uid + CARD_HASH_SALT);
        logger.info("Card UID: " + uid);
        setState(PosState::CARD_DETECTED);
    }
}

void loopCardDetected()
{
    // Single POS charge call replaces token->me->transaction sequence
    if (!ensureWifi())
        return;
    display.showMessage("PROCESSING...");
    auto r = api.posCharge(posCtx.cardHash, posCtx.amount, posCtx.currency, POS_ENTITY_ID);
    if (!r.ok || !r.success)
    {
        display.showMessage(String("PAYMENT FAILED"), String("HTTP ") + r.httpCode);
        setState(PosState::ERROR_STATE);
        display.blink(3, 250);
        return;
    }
    posCtx.meId = r.entityId; // treat returned entity as payer
    posCtx.payerName = r.entityName;
    posCtx.balancePrefetched = true;
    posCtx.balanceCompleted = r.balanceCompleted;
    posCtx.balanceDraft = r.balanceDraft;
    display.showMessage(String("@") + posCtx.payerName, ("paid ") + String(posCtx.amount, 2) + " " + posCtx.currency);
    setState(PosState::TRANSACTION_OK);
}

void loopTransactionOk()
{
    if (millis() - posCtx.stateSince < DOOR_HOLD_MS)
    {
        lockCtrl.open();
        if (millis() - posCtx.stateSince > 2000)
        { // fetch balance once
            if (posCtx.balancePrefetched)
            {
                display.showMessage(String("Balance:"), String(posCtx.balanceCompleted) + " " + posCtx.currency);
                setState(PosState::SHOW_BALANCE);
            }
        }
    }
    else
    {
        lockCtrl.close();
        setState(PosState::IDLE);
    }
}

void loopShowBalance()
{
    if (millis() - posCtx.stateSince > DOOR_HOLD_MS)
    {
        lockCtrl.close();
        setState(PosState::IDLE);
    }
}

void loopError()
{
    if (millis() - posCtx.stateSince > 3000)
    {
        setState(PosState::IDLE);
    }
}

void loop()
{
    // Heartbeat every 5s while idle or waiting
    if (millis() - lastHeartbeat > 5000)
    {
        lastHeartbeat = millis();
        Serial.print('#');
        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.print('R'); // reconnect indicator
        }
    }
    switch (state)
    {
    case PosState::IDLE:
        loopIdle();
        break;
    case PosState::CARD_DETECTED:
        loopCardDetected();
        break;
    case PosState::TRANSACTION_OK:
        loopTransactionOk();
        break;
    case PosState::SHOW_BALANCE:
        loopShowBalance();
        break;
    case PosState::ERROR_STATE:
        loopError();
        break;
    default:
        break;
    }
    // Animate any active display effects (e.g., idle price blink)
    display.tick();
}
