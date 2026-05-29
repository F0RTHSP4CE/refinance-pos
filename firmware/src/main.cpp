#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include "Config.h"
#include "LoggerSerial.h"
#include <Wire.h>
#include "NfcReader.h"
#include "Display.h"
#include "RelayLock.h"
#include "HttpClient.h"
#include "StateMachine.h"
#include "StatusLed.h"
#include "WebGame.h"

LoggerSerial logger;
Display display(logger);
RelayLock lockCtrl;
ApiClient api(logger);
NfcReader *nfcReader; // created after Wire init
PosContext posCtx;
PosState state = PosState::BOOT;
unsigned long lastHeartbeat = 0;
StatusLed statusLed(STATUS_LED_STRIP_PIN, STATUS_LED_STRIP_COUNT, STATUS_LED_BRIGHTNESS);
bool requestInProgress = false;
WebServer webServer(80);
unsigned long manualUnlockUntil = 0;
bool lastManualUnlockActive = false;
bool mdnsStarted = false;

WebGame webGame;

String requestStatusLine1;
String requestStatusLine2Shown;
String requestStatusLine2Pending;
bool requestStatusLine2HasPending = false;

static const char *WEB_AUTH_HEADERS[] = {"X-POS-Secret", "x-pos-secret"};

void syncStatusLed();
void pumpRequestStatusDisplay();
bool isManualUnlockActive();
void requestManualUnlock(uint32_t holdMs = DOOR_HOLD_MS);
void setupWebServer();
void setupMdns();
bool hasValidPosSecret();
String formatAmountCompact(double value);
bool ensureWifi(uint32_t timeoutMs = 12000);

void setRequestInProgress(bool inProgress)
{
    if (requestInProgress == inProgress)
        return;

    requestInProgress = inProgress;
    requestStatusLine2Pending = "";
    requestStatusLine2Shown = "";
    requestStatusLine2HasPending = false;
}

void syncStatusLed()
{
    statusLed.sync(lockCtrl.isActive(), requestInProgress,
                   state == PosState::IDLE,
                   posCtx.chargeAttempted && !posCtx.chargeSucceeded);
}

void beginRequestStatusDisplay(const String &line1)
{
    requestStatusLine1 = line1;
    requestStatusLine2Shown = "";
    requestStatusLine2Pending = "";
    requestStatusLine2HasPending = false;
    display.showMessage(line1, "");
}

void pumpRequestStatusDisplay()
{
    if (!requestInProgress || !requestStatusLine2HasPending)
        return;

    if (requestStatusLine2Shown != requestStatusLine2Pending)
    {
        display.printLine(requestStatusLine1, 0);
        display.printLine(requestStatusLine2Pending, 1);
        requestStatusLine2Shown = requestStatusLine2Pending;
    }

    requestStatusLine2HasPending = false;
}

void displayStatusCallback(const String &line1, const String &line2, void *context)
{
    (void)context;

    if (!requestInProgress)
    {
        display.showMessage(line1, line2);
        syncStatusLed();
        yield();
        return;
    }

    // On retry, release any explicit LED hold so sync resumes orange
    if (line2.indexOf(F("retry")) >= 0)
    {
        statusLed.invalidate();
    }

    if (requestStatusLine1 != line1)
    {
        requestStatusLine1 = line1;
        display.printLine(requestStatusLine1, 0);
    }

    if (requestStatusLine2Pending != line2)
    {
        requestStatusLine2Pending = line2;
        requestStatusLine2HasPending = true;
    }

    pumpRequestStatusDisplay();
    syncStatusLed();
    yield();
}

bool isManualUnlockActive()
{
    return (long)(manualUnlockUntil - millis()) > 0;
}

void requestManualUnlock(uint32_t holdMs)
{
    unsigned long now = millis();
    unsigned long requestedUntil = now + holdMs;
    if ((long)(requestedUntil - manualUnlockUntil) > 0)
    {
        manualUnlockUntil = requestedUntil;
    }

    logger.warn("Manual web unlock requested");
    display.showMessage("WEB OPEN", "door unlocked");
}

bool hasValidPosSecret()
{
    String provided;

    for (size_t i = 0; i < (sizeof(WEB_AUTH_HEADERS) / sizeof(WEB_AUTH_HEADERS[0])); ++i)
    {
        if (webServer.hasHeader(WEB_AUTH_HEADERS[i]))
        {
            provided = webServer.header(WEB_AUTH_HEADERS[i]);
            break;
        }
    }

    if (provided.length() == 0 && webServer.hasArg("secret"))
    {
        provided = webServer.arg("secret");
    }

    provided.trim();
    return provided.length() > 0 && provided == POS_SECRET;
}

void setupWebServer()
{
    webServer.collectHeaders(WEB_AUTH_HEADERS, sizeof(WEB_AUTH_HEADERS) / sizeof(WEB_AUTH_HEADERS[0]));
    webGame.generateChallenge();

    webServer.on("/", HTTP_GET, []()
                 {
                webGame.generateChallenge();
                webGame.sendPage(webServer, POS_NAME, isManualUnlockActive() || lockCtrl.isActive()); });

    webServer.on("/solve", HTTP_POST, []()
                 {
                if (webGame.verifySolution(webServer))
        {
            requestManualUnlock();
                        webGame.generateChallenge();
                        webServer.send(200, "text/plain", "Unlocked. Lock opened.");
            return;
        }

                webGame.generateChallenge();
                webServer.send(403, "text/plain", "Game check failed. Try a new run."); });

    webServer.on("/open", HTTP_POST, []()
                 { webServer.send(403, "text/plain", "Direct unlock disabled. Solve puzzle at /"); });

    webServer.on("/remote-charge", HTTP_POST, []()
                 {
        if (!hasValidPosSecret())
        {
            webServer.send(403, "application/json", "{\"ok\":false,\"error\":\"forbidden\"}");
            return;
        }

        if (!webServer.hasArg("entity_name"))
        {
            webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing entity_name\"}");
            return;
        }

        String entityName = webServer.arg("entity_name");
        entityName.trim();
        if (entityName.length() == 0)
        {
            webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"empty entity_name\"}");
            return;
        }

        double amount = POS_PRICE;
        if (webServer.hasArg("amount"))
        {
            amount = webServer.arg("amount").toDouble();
            if (!(amount > 0.0))
            {
                webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid amount\"}");
                return;
            }
        }

        String currency = POS_CURRENCY;
        if (webServer.hasArg("currency"))
        {
            currency = webServer.arg("currency");
            currency.trim();
            if (currency.length() == 0)
            {
                webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"empty currency\"}");
                return;
            }
        }

        if (!ensureWifi())
        {
            webServer.send(503, "application/json", "{\"ok\":false,\"error\":\"wifi unavailable\"}");
            return;
        }

        // Charge must succeed before the relay is opened.
        setRequestInProgress(true);
        beginRequestStatusDisplay("remote charge");
        auto charge = api.chargeEntityByName(entityName,
                                             amount,
                                             currency,
                                             POS_ENTITY_ID,
                                             displayStatusCallback,
                                             nullptr);
        setRequestInProgress(false);

        if (!charge.ok || !charge.success)
        {
            String reason = (charge.error.length() > 0) ? charge.error : (String("HTTP ") + charge.httpCode);
            logger.warn(String("Remote charge failed for ") + entityName + ": " + reason);
            display.showMessage(charge.errorCode == 10001 ? "X UNPAID INVOICE" : "CHARGE FAILED", reason);
            webServer.send(200,
                           "application/json",
                           String("{\"ok\":false,\"unlocked\":false,\"charged\":false,\"amount\":") +
                               formatAmountCompact(amount) +
                               ",\"currency\":\"" + currency +
                               "\",\"error\":\"" + reason + "\"}");
            return;
        }

        requestManualUnlock();
        String chargedName = charge.entityName.length() ? charge.entityName : entityName;
        display.showMessage(String("@") + chargedName,
                            String("-") + formatAmountCompact(amount) + " " + currency);

        webServer.send(200,
                       "application/json",
                       String("{\"ok\":true,\"unlocked\":true,\"charged\":true,\"entity_name\":\"") +
                           chargedName +
                           "\",\"amount\":" + formatAmountCompact(amount) +
                           ",\"currency\":\"" + currency +
                           "\",\"balance_completed\":" + String(charge.balanceCompleted) +
                           ",\"balance_draft\":" + String(charge.balanceDraft) + "}"); });

    webServer.onNotFound([]()
                         { webServer.send(404, "text/plain", "Not found"); });

    webServer.begin();
    logger.info("HTTP server started on port 80");
}

void setupMdns()
{
    if (mdnsStarted)
    {
        MDNS.end();
        mdnsStarted = false;
    }

    if (!MDNS.begin(POS_MDNS_HOSTNAME))
    {
        logger.warn(String("mDNS start failed for ") + POS_MDNS_HOSTNAME + "." + POS_MDNS_DOMAIN_SUFFIX);
        return;
    }

    MDNS.setInstanceName(POS_NAME);
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;

    String localName = String(POS_MDNS_HOSTNAME) + "." + POS_MDNS_DOMAIN_SUFFIX;
    logger.info(String("mDNS ready: http://") + localName + "/");
}

// Using default global Wire instance instead of a separate TwoWire

String formatAmountCompact(double value)
{
    long rounded = lround(value);
    if (fabs(value - (double)rounded) < 0.0001)
        return String(rounded);
    return String(value, 2);
}

bool ensureWifi(uint32_t timeoutMs)
{
    if (WiFi.status() == WL_CONNECTED)
        return true;
    logger.warn("WiFi reconnect...");
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.setSleep(false);
    // Ensure DHCP is used for IP + DNS configuration.
    WiFi.config(IPAddress(0, 0, 0, 0),
                IPAddress(0, 0, 0, 0),
                IPAddress(0, 0, 0, 0),
                IPAddress(0, 0, 0, 0),
                IPAddress(0, 0, 0, 0));
    WiFi.disconnect(true);
    delay(100);
    if (!WiFi.setHostname(POS_DHCP_HOSTNAME))
    {
        logger.warn(String("Failed to set DHCP hostname: ") + POS_DHCP_HOSTNAME);
    }
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs)
    {
        pumpRequestStatusDisplay();
        syncStatusLed();
        delay(300);
        Serial.print('~');
    }
    if (WiFi.status() == WL_CONNECTED)
    {
        unsigned long dnsStart = millis();
        while (WiFi.dnsIP(0) == IPAddress((uint32_t)0) && millis() - dnsStart < 2000)
        {
            delay(50);
            yield();
        }

        logger.info(String("WiFi IP ") + WiFi.localIP().toString());
        logger.info(String("DHCP host ") + POS_DHCP_HOSTNAME);
        logger.info(String("DNS0 ") + WiFi.dnsIP(0).toString());
        logger.info(String("DNS1 ") + WiFi.dnsIP(1).toString());
        setupMdns();
        String localName = String(POS_MDNS_HOSTNAME) + "." + POS_MDNS_DOMAIN_SUFFIX;
        display.showMessage("WiFi OK", localName);
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
        statusLed.invalidate();
        display.showIdle(POS_NAME, posCtx.amount, posCtx.currency);
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    randomSeed((uint32_t)micros());
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
        pinMode(sda, INPUT_PULLUP);
        pinMode(scl, INPUT_PULLUP);
        delay(2);
        Wire.begin(sda, scl);
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
    statusLed.begin();
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
    WiFi.setSleep(false);
    logger.info("WiFi sleep disabled");
    setupWebServer();
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
        logger.info("Card UID: " + uid);
        setState(PosState::CARD_DETECTED);
    }
}

void loopCardDetected()
{
    // Step 1: verify card is known (cache or USBUTLER) before unlocking.
    setRequestInProgress(true);
    beginRequestStatusDisplay("net check");
    displayStatusCallback("net check", "[wifi...]", nullptr);
    if (!ensureWifi())
    {
        setRequestInProgress(false);
        display.showMessage("WiFi FAIL", "try again");
        setState(PosState::ERROR_STATE);
        statusLed.blinkError();
        return;
    }
    displayStatusCallback("card lookup", "[starting...]", nullptr);
    auto lookup = api.lookupEntityByCardUID(posCtx.cardUID,
                                            displayStatusCallback,
                                            nullptr);
    setRequestInProgress(false);
    if (!lookup.ok || !lookup.success)
    {
        String reason = (lookup.error.length() > 0) ? lookup.error : (String("HTTP ") + lookup.httpCode);
        display.showMessage(String("CARD UNKNOWN"), reason);
        setState(PosState::ERROR_STATE);
        statusLed.blinkError();
        display.blink(3, 250);
        return;
    }

    statusLed.setColor(0, 255, 0); // card known → green

    posCtx.meId = "";
    posCtx.payerName = lookup.entityName;
    posCtx.balancePrefetched = false;
    posCtx.balanceCompleted = 0;
    posCtx.balanceDraft = 0;
    posCtx.chargeAttempted = false;
    posCtx.chargeSucceeded = false;
    posCtx.chargeError = "";

    display.showMessage(String("@") + posCtx.payerName, "OPENING...");
    setState(PosState::TRANSACTION_OK);
}

void loopTransactionOk()
{
    // Keep door open immediately once a known card is validated.
    lockCtrl.open();

    // Step 2: after door opened, attempt charge request once.
    if (!posCtx.chargeAttempted)
    {
        posCtx.chargeAttempted = true;
        setRequestInProgress(true);
        beginRequestStatusDisplay("charge");
        auto charge = api.chargeEntityByName(posCtx.payerName,
                                             posCtx.amount,
                                             posCtx.currency,
                                             POS_ENTITY_ID,
                                             displayStatusCallback,
                                             nullptr);
        setRequestInProgress(false);

        if (charge.ok && charge.success)
        {
            posCtx.chargeSucceeded = true;
            posCtx.meId = charge.entityId;
            posCtx.balancePrefetched = charge.balanceAvailable;
            posCtx.balanceCompleted = charge.balanceCompleted;
            posCtx.balanceDraft = charge.balanceDraft;
            display.showMessage(String("@") + posCtx.payerName,
                                String("-") + formatAmountCompact(posCtx.amount) + " " + posCtx.currency);
            statusLed.setColor(0, 255, 0); // charge ok → green
        }
        else
        {
            posCtx.chargeSucceeded = false;
            posCtx.chargeError = (charge.error.length() > 0) ? charge.error : (String("HTTP ") + charge.httpCode);
            logger.warn(String("Charge failed for known card: ") + posCtx.chargeError);
            display.showMessage(String("OPEN @") + posCtx.payerName, charge.errorCode == 10001 ? "X UNPAID INVOICE" : "CHARGE FAILED");
            statusLed.blinkError();

            // Rapidly toggle relay 5 times while blinking red
            const int buzzCount = 5;
            const uint32_t buzzOnMs = 150;
            const uint32_t buzzOffMs = 150;
            for (int i = 0; i < buzzCount; i++)
            {
                lockCtrl.open();
                syncStatusLed();
                delay(buzzOnMs);
                lockCtrl.close();
                syncStatusLed();
                delay(buzzOffMs);
            }
            lockCtrl.close();
            setState(PosState::ERROR_STATE);
            return;
        }
    }

    if (millis() - posCtx.stateSince < DOOR_HOLD_MS)
    {
        if (millis() - posCtx.stateSince > (DOOR_HOLD_MS - 1000))
        {
            if (posCtx.chargeSucceeded && posCtx.balancePrefetched)
            {
                display.showMessage(String("Balance:"), String(posCtx.balanceCompleted) + " " + posCtx.currency);
                setState(PosState::SHOW_BALANCE);
            }
        }
    }
    else
    {
        if (!isManualUnlockActive())
        {
            lockCtrl.close();
        }
        setState(PosState::IDLE);
    }
}

void loopShowBalance()
{
    if (millis() - posCtx.stateSince > DOOR_HOLD_MS)
    {
        if (!isManualUnlockActive())
        {
            lockCtrl.close();
        }
        setState(PosState::IDLE);
    }
}

void loopError()
{
    if (millis() - posCtx.stateSince > 5000)
    {
        setState(PosState::IDLE);
    }
}

void loop()
{
    webServer.handleClient();

    bool manualUnlockActive = isManualUnlockActive();

    if (lastManualUnlockActive && !manualUnlockActive)
    {
        if (state == PosState::IDLE)
        {
            display.showIdle(POS_NAME, posCtx.amount, posCtx.currency);
        }
    }

    lastManualUnlockActive = manualUnlockActive;

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

    if (manualUnlockActive)
    {
        lockCtrl.open();
    }
    else if (state != PosState::TRANSACTION_OK && state != PosState::SHOW_BALANCE && lockCtrl.isActive())
    {
        lockCtrl.close();
    }

    syncStatusLed();
    pumpRequestStatusDisplay();
    // Animate any active display effects (e.g., idle price blink)
    display.tick();
}
