#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <time.h>
#include <esp_system.h>
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
#include "PosCoinStore.h"

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
PosCoinStore posCoinStore;
bool posCoinStoreReady = false;
unsigned long rechargeModeUntil = 0;
bool rechargeLedHeld = false;
bool lastRechargeModeActive = false;
String lastRechargeTagUid;
unsigned long lastRechargeTagSeenAt = 0;
String lastConsumedCoinTagUid;
uint8_t lastConsumedCoinUuid[16];
bool lastConsumedCoinValid = false;
unsigned long lastConsumedCoinSeenAt = 0;
bool ntpConfigured = false;

WebGame webGame;

String requestStatusLine1;
String requestStatusLine2Shown;
String requestStatusLine2Pending;
bool requestStatusLine2HasPending = false;

static const char *WEB_AUTH_HEADERS[] = {"X-POS-Secret", "x-pos-secret"};
static const char *RECHARGE_AUTH_HEADERS[] = {"X-Recharge-Secret", "x-recharge-secret", "X-POS-Recharge-Secret", "x-pos-recharge-secret"};
static const char *ALL_WEB_AUTH_HEADERS[] = {
    "X-POS-Secret",
    "x-pos-secret",
    "X-Recharge-Secret",
    "x-recharge-secret",
    "X-POS-Recharge-Secret",
    "x-pos-recharge-secret",
};

void syncStatusLed();
void pumpRequestStatusDisplay();
bool isManualUnlockActive();
bool isRechargeModeActive();
void requestManualUnlock(uint32_t holdMs = DOOR_HOLD_MS);
void requestPosCoinUnlock(uint32_t holdMs = DOOR_HOLD_MS);
void setState(PosState s);
void setupWebServer();
void setupMdns();
bool hasValidPosSecret();
bool hasValidRechargeSecret();
String formatAmountCompact(double value);
bool ensureWifi(uint32_t timeoutMs = 12000);
void configureTimeSync();
String currentRechargeTimestamp();
void handleRechargeTap();
String uuidToHex(const uint8_t uuid[16]);
String maskedUuid(const uint8_t uuid[16]);
String htmlEscape(const String &value);
String rechargePage(bool authenticated, const String &message = "");
void startRechargeMode();
bool isRecentlyConsumedCoin(const String &tagUid, const uint8_t uuid[16]);
bool isRecentlyConsumedCoinTag(const String &tagUid);
void rememberConsumedCoin(const String &tagUid, const uint8_t uuid[16]);
void noteNoCoinPresent();

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
    if (isRechargeModeActive())
    {
        if (!rechargeLedHeld)
        {
            statusLed.setColor(0, 0, 255);
            rechargeLedHeld = true;
        }
        return;
    }

    if (rechargeLedHeld)
    {
        statusLed.invalidate();
        rechargeLedHeld = false;
    }

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

bool isRechargeModeActive()
{
    return (long)(rechargeModeUntil - millis()) > 0;
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

void requestPosCoinUnlock(uint32_t holdMs)
{
    unsigned long now = millis();
    unsigned long requestedUntil = now + holdMs;
    if ((long)(requestedUntil - manualUnlockUntil) > 0)
    {
        manualUnlockUntil = requestedUntil;
    }

    logger.warn("Single-use POS coin unlock");
    display.showMessage("POS COIN", "door unlocked");
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

bool hasValidRechargeSecret()
{
    String provided;

    for (size_t i = 0; i < (sizeof(RECHARGE_AUTH_HEADERS) / sizeof(RECHARGE_AUTH_HEADERS[0])); ++i)
    {
        if (webServer.hasHeader(RECHARGE_AUTH_HEADERS[i]))
        {
            provided = webServer.header(RECHARGE_AUTH_HEADERS[i]);
            break;
        }
    }

    if (provided.length() == 0 && webServer.hasArg("recharge_secret"))
    {
        provided = webServer.arg("recharge_secret");
    }

    if (provided.length() == 0 && webServer.hasArg("secret"))
    {
        provided = webServer.arg("secret");
    }

    provided.trim();
    return provided.length() > 0 && provided == POS_RECHARGE_SECRET;
}

void startRechargeMode()
{
    rechargeModeUntil = millis() + POS_COIN_RECHARGE_MODE_MS;
    lastRechargeTagUid = "";
    lastRechargeTagSeenAt = 0;
    rechargeLedHeld = false;
    display.showMessage("RECHARGE", "tap coins");
    syncStatusLed();
    logger.warn("POS coin recharge mode started");
}

bool isRecentlyConsumedCoin(const String &tagUid, const uint8_t uuid[16])
{
    if (!isRecentlyConsumedCoinTag(tagUid))
        return false;

    if (memcmp(uuid, lastConsumedCoinUuid, sizeof(lastConsumedCoinUuid)) != 0)
        return false;

    return true;
}

bool isRecentlyConsumedCoinTag(const String &tagUid)
{
    if (!lastConsumedCoinValid || tagUid != lastConsumedCoinTagUid)
        return false;

    lastConsumedCoinSeenAt = millis();
    return true;
}

void rememberConsumedCoin(const String &tagUid, const uint8_t uuid[16])
{
    lastConsumedCoinTagUid = tagUid;
    memcpy(lastConsumedCoinUuid, uuid, sizeof(lastConsumedCoinUuid));
    lastConsumedCoinSeenAt = millis();
    lastConsumedCoinValid = true;
}

void noteNoCoinPresent()
{
    if (lastConsumedCoinValid && millis() - lastConsumedCoinSeenAt > 700)
    {
        lastConsumedCoinValid = false;
        lastConsumedCoinTagUid = "";
    }
}

String uuidToHex(const uint8_t uuid[16])
{
    static const char *hex = "0123456789ABCDEF";
    String out;
    out.reserve(32);
    for (uint8_t i = 0; i < 16; ++i)
    {
        out += hex[(uuid[i] >> 4) & 0x0F];
        out += hex[uuid[i] & 0x0F];
    }
    return out;
}

String maskedUuid(const uint8_t uuid[16])
{
    String hex = uuidToHex(uuid);
    return String("...") + hex.substring(hex.length() - 8);
}

String htmlEscape(const String &value)
{
    String out;
    out.reserve(value.length());
    for (size_t i = 0; i < value.length(); ++i)
    {
        char c = value[i];
        if (c == '&')
            out += F("&amp;");
        else if (c == '<')
            out += F("&lt;");
        else if (c == '>')
            out += F("&gt;");
        else if (c == '"')
            out += F("&quot;");
        else
            out += c;
    }
    return out;
}

String rechargePage(bool authenticated, const String &message)
{
    String html;
    html.reserve(4096);
    html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
    html += F("<title>Recharge POS Coins</title><style>");
    html += F("body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}main{max-width:760px;margin:0 auto;padding:24px}");
    html += F("form{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:18px 0}input{font:inherit;padding:10px;background:#222;color:#eee;border:1px solid #555;border-radius:4px}");
    html += F("button{font:inherit;padding:10px 14px;background:#0b6bff;color:white;border:0;border-radius:4px}table{width:100%;border-collapse:collapse;margin-top:14px}");
    html += F("th,td{text-align:left;border-bottom:1px solid #333;padding:8px}.msg{padding:10px 0;color:#8fd694}.warn{color:#ffb86b}");
    html += F("</style></head><body><main><h1>Recharge POS Coins</h1>");
    if (message.length() > 0)
        html += String(F("<p class='msg'>")) + htmlEscape(message) + F("</p>");
    if (isRechargeModeActive())
    {
        unsigned long remaining = (rechargeModeUntil - millis()) / 1000;
        html += String(F("<p class='warn'>Recharge mode active: ")) + remaining + F("s remaining.</p>");
    }
    html += F("<form method='post' action='/recharge'><input type='password' name='recharge_secret' placeholder='Recharge password' autocomplete='current-password'>");
    html += F("<button type='submit'>Activate</button></form>");

    if (authenticated)
    {
        html += String(F("<p>Active tokens: ")) + posCoinStore.count() + F("</p>");
        html += F("<table><thead><tr><th>Token</th><th>Recharge date</th></tr></thead><tbody>");
        for (uint8_t i = 0; i < posCoinStore.count(); ++i)
        {
            const PosCoinRecord &record = posCoinStore.record(i);
            html += F("<tr><td>");
            html += htmlEscape(maskedUuid(record.uuid));
            html += F("</td><td>");
            html += htmlEscape(String(record.generatedAt));
            html += F("</td></tr>");
        }
        html += F("</tbody></table>");
    }
    else
    {
        html += F("<p>Enter the recharge password to activate recharge mode.</p>");
    }

    html += F("</main></body></html>");
    return html;
}

void setupWebServer()
{
    webServer.collectHeaders(ALL_WEB_AUTH_HEADERS, sizeof(ALL_WEB_AUTH_HEADERS) / sizeof(ALL_WEB_AUTH_HEADERS[0]));
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

    webServer.on("/favicon.ico", HTTP_GET, []()
                 { webServer.send(204, "text/plain", ""); });

    webServer.on("/recharge", HTTP_GET, []()
                 {
        bool authenticated = hasValidRechargeSecret();
        webServer.send(200, "text/html", rechargePage(authenticated)); });

    webServer.on("/recharge", HTTP_POST, []()
                 {
        if (!hasValidRechargeSecret())
        {
            webServer.send(403, "text/html", rechargePage(false, "Invalid recharge password."));
            return;
        }
        if (!posCoinStoreReady)
        {
            webServer.send(503, "text/html", rechargePage(true, "POS coin store is unavailable."));
            return;
        }

        startRechargeMode();
        webServer.send(200, "text/html", rechargePage(true, "Recharge mode activated. Tap coins.")); });

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
            setState(PosState::ERROR_STATE);
            webServer.send(200,
                           "application/json",
                           String("{\"ok\":false,\"unlocked\":false,\"charged\":false,\"amount\":") +
                               formatAmountCompact(amount) +
                               ",\"currency\":\"" + currency + "\"" +
                               (charge.errorCode != 0 ? String(",\"error_code\":") + charge.errorCode : String()) +
                               ",\"error\":\"" + reason + "\"}");
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

void configureTimeSync()
{
    if (ntpConfigured)
        return;

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    ntpConfigured = true;
    logger.info("NTP sync configured");
}

String currentRechargeTimestamp()
{
    struct tm timeinfo;
    if (ntpConfigured && getLocalTime(&timeinfo, 50))
    {
        char buf[24];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
        return String(buf);
    }

    return String("uptime:") + String(millis() / 1000) + "s";
}

bool ensureWifi(uint32_t timeoutMs)
{
    if (WiFi.status() == WL_CONNECTED)
    {
        configureTimeSync();
        return true;
    }
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
        configureTimeSync();
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
    statusLed.releaseHold();
    rechargeLedHeld = false;
    if (state == PosState::IDLE)
    {
        display.showIdle(POS_NAME, posCtx.amount, posCtx.currency);
    }
    syncStatusLed();
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
    posCoinStoreReady = posCoinStore.begin();
    if (!posCoinStoreReady)
    {
        logger.error("POS coin token store unavailable");
    }
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
    if (isRechargeModeActive())
    {
        handleRechargeTap();
        return;
    }

    if (isManualUnlockActive())
        return;

    NfcTag tag;
    if (!nfcReader->pollTag(tag))
    {
        noteNoCoinPresent();
        return;
    }

    PosCoinPayload coin;
    String coinTagUid;
    PosCoinReadStatus coinStatus = posCoinStoreReady ? nfcReader->readPosCoinDetailedForTag(tag, coin, coinTagUid) : PosCoinReadStatus::NoTag;
    if (coinStatus == PosCoinReadStatus::Ok)
    {
        logger.info(String("POS coin tag UID: ") + coinTagUid);
        if (posCoinStore.consume(coin.uuid))
        {
            rememberConsumedCoin(coinTagUid, coin.uuid);
            requestPosCoinUnlock();
        }
        else if (isRecentlyConsumedCoin(coinTagUid, coin.uuid))
        {
            return;
        }
        else
        {
            logger.warn(String("POS coin not active: ") + maskedUuid(coin.uuid));
            display.showMessage("COIN USED", "or unknown");
            statusLed.blinkError();
            display.blink(3, 200);
            setState(PosState::ERROR_STATE);
        }
        return;
    }
    if (coinStatus == PosCoinReadStatus::ReadFailed)
    {
        if (isRecentlyConsumedCoinTag(coinTagUid))
            return;

        logger.warn(String("POS coin read failed for ") + coinTagUid + ": " + (int)coinStatus);
        display.showMessage("COIN READ FAIL", "try recharge");
        statusLed.blinkError();
        display.blink(3, 200);
        setState(PosState::ERROR_STATE);
        return;
    }
    if (coinStatus == PosCoinReadStatus::BadMagic)
    {
        if (isRecentlyConsumedCoinTag(coinTagUid))
            return;

        logger.warn(String("NTAG has no POS coin payload: ") + coinTagUid);
        display.showMessage("NOT POS COIN", "recharge first");
        statusLed.blinkError();
        display.blink(3, 200);
        setState(PosState::ERROR_STATE);
        return;
    }

    posCtx.cardUID = tag.uidHex;
    logger.info("Card UID: " + tag.uidHex);
    setState(PosState::CARD_DETECTED);
}

void generateUuid(uint8_t uuid[16])
{
    for (uint8_t i = 0; i < 16; i += 4)
    {
        uint32_t r = esp_random();
        uuid[i] = (uint8_t)(r & 0xFF);
        uuid[i + 1] = (uint8_t)((r >> 8) & 0xFF);
        uuid[i + 2] = (uint8_t)((r >> 16) & 0xFF);
        uuid[i + 3] = (uint8_t)((r >> 24) & 0xFF);
    }
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
}

void handleRechargeTap()
{
    NfcTag tag;
    if (!nfcReader->pollTag(tag))
        return;

    unsigned long now = millis();
    if (tag.uidHex == lastRechargeTagUid && now - lastRechargeTagSeenAt < POS_COIN_PRESENT_GUARD_MS)
        return;

    lastRechargeTagUid = tag.uidHex;
    lastRechargeTagSeenAt = now;

    if (!posCoinStoreReady)
    {
        display.showMessage("STORE FAIL", "no recharge");
        statusLed.blinkError();
        return;
    }
    if (posCoinStore.count() >= POS_COIN_MAX_TOKENS)
    {
        display.showMessage("STORE FULL", "use coins first");
        statusLed.blinkError();
        return;
    }

    uint8_t uuid[16];
    uint8_t oldUuid[16];
    bool hadOldUuid = false;
    String writtenUid;
    generateUuid(uuid);

    PosCoinWriteStatus status = nfcReader->writePosCoinForTag(tag, uuid, writtenUid, oldUuid, &hadOldUuid);
    if (status != PosCoinWriteStatus::Ok)
    {
        logger.warn(String("POS coin write failed for ") + tag.uidHex + ": " + (int)status);
        display.showMessage("WRITE FAIL", "try again");
        statusLed.blinkError(1200, 150);
        return;
    }

    if (hadOldUuid)
    {
        posCoinStore.remove(oldUuid);
    }

    if (!posCoinStore.addOrUpdate(uuid, currentRechargeTimestamp()))
    {
        logger.error("POS coin written but token store save failed");
        display.showMessage("SAVE FAIL", "coin invalid");
        statusLed.blinkError();
        return;
    }

    logger.info(String("POS coin recharged ") + writtenUid + " " + maskedUuid(uuid));
    display.showMessage("COIN READY", maskedUuid(uuid));
    statusLed.setColor(0, 0, 255);
    rechargeLedHeld = true;
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

    // On the fast path the door is about to open → show green immediately.
    // On the slow path wait until charge succeeds; let sync() drive orange during the request.
    if (api.isChargeSuccessCached(posCtx.cardUID))
        statusLed.setColor(0, 255, 0); // card known + pre-authorized → green
    else
        statusLed.invalidate(); // let sync() show orange while charging

    posCtx.meId = "";
    posCtx.payerName = lookup.entityName;
    posCtx.balancePrefetched = false;
    posCtx.balanceCompleted = 0;
    posCtx.balanceDraft = 0;
    posCtx.chargeAttempted = false;
    posCtx.chargeSucceeded = false;
    posCtx.chargePreAuthorized = api.isChargeSuccessCached(posCtx.cardUID);
    posCtx.chargeError = "";

    if (posCtx.chargePreAuthorized)
        display.showMessage(String("@") + posCtx.payerName, "OPENING...");
    else
        display.showMessage(String("@") + posCtx.payerName, "");
    setState(PosState::TRANSACTION_OK);
}

void loopTransactionOk()
{
    // Pre-authorized path: previous charge succeeded → open door immediately while we charge.
    // Non-pre-authorized path: charge must succeed first, then open door.
    if (posCtx.chargePreAuthorized || posCtx.chargeSucceeded)
    {
        lockCtrl.open();
    }

    // Attempt charge exactly once.
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
            api.addChargeSuccessCache(posCtx.cardUID);

            if (!posCtx.chargePreAuthorized)
            {
                // First successful charge for this UID: open door now, reset hold timer.
                lockCtrl.open();
                posCtx.stateSince = millis();
            }

            display.showMessage(String("@") + posCtx.payerName,
                                String("-") + formatAmountCompact(posCtx.amount) + " " + posCtx.currency);
            statusLed.setColor(0, 255, 0); // charge ok → green
        }
        else
        {
            posCtx.chargeSucceeded = false;
            posCtx.chargeError = (charge.error.length() > 0) ? charge.error : (String("HTTP ") + charge.httpCode);
            logger.warn(String("Charge failed for known card: ") + posCtx.chargeError);
            api.removeChargeSuccessCache(posCtx.cardUID);

            if (posCtx.chargePreAuthorized)
            {
                // Door was already open; show error and buzz to indicate failure.
                display.showMessage(String("OPEN @") + posCtx.payerName, charge.errorCode == 10001 ? "X UNPAID INVOICE" : "CHARGE FAILED");
                statusLed.blinkError();

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
            }
            else
            {
                // Door was never opened; just show the error.
                display.showMessage(charge.errorCode == 10001 ? "X UNPAID INVOICE" : "CHARGE FAILED", posCtx.chargeError);
                statusLed.blinkError();
            }

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

    bool rechargeActive = isRechargeModeActive();
    if (lastRechargeModeActive && !rechargeActive)
    {
        rechargeModeUntil = 0;
        lastRechargeTagUid = "";
        lastRechargeTagSeenAt = 0;
        rechargeLedHeld = false;
        statusLed.invalidate();
        if (state == PosState::IDLE)
        {
            display.showIdle(POS_NAME, posCtx.amount, posCtx.currency);
        }
    }
    lastRechargeModeActive = rechargeActive;

    bool manualUnlockActive = isManualUnlockActive();

    if (lastManualUnlockActive && !manualUnlockActive)
    {
        statusLed.releaseHold();
        rechargeLedHeld = false;
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
