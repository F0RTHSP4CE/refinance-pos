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
#include <Adafruit_NeoPixel.h>

LoggerSerial logger;
Display display(logger);
RelayLock lockCtrl;
ApiClient api(logger);
NfcReader *nfcReader; // created after Wire init
PosContext posCtx;
PosState state = PosState::BOOT;
unsigned long lastHeartbeat = 0;
Adafruit_NeoPixel statusStrip(STATUS_LED_STRIP_COUNT, STATUS_LED_STRIP_PIN, NEO_GRB + NEO_KHZ800);
bool lastRelayActive = false;
uint8_t lastRedLevel = 255;
bool ledModeInitialized = false;
bool requestInProgress = false;
WebServer webServer(80);
unsigned long manualUnlockUntil = 0;
bool lastManualUnlockActive = false;
bool mdnsStarted = false;

struct InvadersChallenge
{
    uint32_t token = 0;
    unsigned long issuedAt = 0;
    unsigned long expiresAt = 0;
    bool consumed = false;
};

InvadersChallenge currentChallenge;

static const int WEB_GAME_TARGET_SCORE = 10;
static const uint32_t WEB_GAME_MIN_PLAY_MS = 7000;
static const uint32_t WEB_GAME_MAX_PLAY_MS = 180000;

String requestStatusLine1;
String requestStatusLine2Shown;
String requestStatusLine2Pending;
bool requestStatusLine2HasPending = false;

void syncStatusStripWithRelay();
void pumpRequestStatusDisplay();
bool isManualUnlockActive();
void requestManualUnlock(uint32_t holdMs = DOOR_HOLD_MS);
void setupWebServer();
void setupMdns();
void generateWebGameChallenge();
bool verifyWebGameChallenge();

void setRequestInProgress(bool inProgress)
{
    if (requestInProgress == inProgress)
        return;

    requestInProgress = inProgress;
    requestStatusLine2Pending = "";
    requestStatusLine2Shown = "";
    requestStatusLine2HasPending = false;
    ledModeInitialized = false;
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
        syncStatusStripWithRelay();
        yield();
        return;
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
    syncStatusStripWithRelay();
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

void generateWebGameChallenge()
{
    currentChallenge.token = (uint32_t)random(100000, 999999);
        currentChallenge.issuedAt = millis();
        currentChallenge.expiresAt = currentChallenge.issuedAt + WEB_GAME_MAX_PLAY_MS;
        currentChallenge.consumed = false;
}

bool verifyWebGameChallenge()
{
        if (!webServer.hasArg("token") || !webServer.hasArg("score") || !webServer.hasArg("timeMs"))
    {
        return false;
    }

        if (currentChallenge.consumed)
        {
                return false;
        }

    if ((long)(currentChallenge.expiresAt - millis()) <= 0)
    {
        return false;
    }

    uint32_t token = (uint32_t)webServer.arg("token").toInt();
    if (token != currentChallenge.token)
    {
        return false;
    }

        int score = webServer.arg("score").toInt();
        uint32_t timeMs = (uint32_t)webServer.arg("timeMs").toInt();

        if (score < WEB_GAME_TARGET_SCORE)
        {
                return false;
        }

        if (timeMs < WEB_GAME_MIN_PLAY_MS || timeMs > WEB_GAME_MAX_PLAY_MS)
        {
                return false;
        }

        currentChallenge.consumed = true;
        return true;
}

void sendWebGamePage()
{
    static const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html>
<html>
<head>
    <meta charset='utf-8'>
    <meta name='viewport' content='width=device-width,initial-scale=1'>
    <meta name='color-scheme' content='dark'>
    <title>)HTML";

    static const char PAGE_AFTER_TITLE[] PROGMEM = R"HTML(</title>
</head>
<body style='font-family:sans-serif;max-width:460px;margin:1rem auto;padding:0 1rem;background:#000;color:#fff;touch-action:manipulation;' data-name=')HTML";

    static const char PAGE_AFTER_NAME[] PROGMEM = R"HTML(' data-token=')HTML";

    static const char PAGE_AFTER_TOKEN[] PROGMEM = R"HTML(' data-target=')HTML";

    static const char PAGE_AFTER_TARGET[] PROGMEM = R"HTML('>
    <h2 id='title' style='margin:.2rem 0 0 0;'></h2>
    <p>Status: )HTML";

    static const char PAGE_REST[] PROGMEM = R"HTML(</p>
    <p><b>Goal:</b> reach target score to unlock. Use Left, Right and Space (or touch buttons).</p>
    <canvas id='game' width='320' height='420' style='width:100%;max-width:360px;display:block;border:1px solid #fff;background:#000;margin:0 auto;'></canvas>
    <div style='margin-top:.6rem;display:flex;gap:.5rem;max-width:360px;margin-left:auto;margin-right:auto;'>
        <button id='leftBtn' style='flex:1;padding:.85rem;font-size:1rem;background:#000;color:#fff;border:1px solid #fff;'>&larr;</button>
        <button id='fireBtn' style='flex:1;padding:.85rem;font-size:1rem;background:#000;color:#fff;border:1px solid #fff;'>FIRE</button>
        <button id='rightBtn' style='flex:1;padding:.85rem;font-size:1rem;background:#000;color:#fff;border:1px solid #fff;'>&rarr;</button>
    </div>
    <p id='hud' style='max-width:360px;margin:0.6rem auto 0 auto;'>Score: 0</p>
    <p id='msg' style='max-width:360px;min-height:1.5rem;margin:0.3rem auto 0 auto;'></p>
    <p style='opacity:.85;font-size:.92rem;max-width:360px;margin-left:auto;margin-right:auto;'>Avoid monsters and enemy shots. Refresh for a fresh run.</p>

<script>
(() => {
    const posName = document.body.dataset.name || 'POS';
    const token = document.body.dataset.token || '';
    const targetScore = Number(document.body.dataset.target || '12');
    const canvas = document.getElementById('game');
    const ctx = canvas.getContext('2d');
    const hud = document.getElementById('hud');
    const msg = document.getElementById('msg');
    const state = {
        startMs: Date.now(),
        over: false,
        won: false,
        score: 0,
        keys: {},
        player: {x: 150, y: 390, w: 20, h: 12, speed: 3.2},
        bullet: null,
        enemyBullets: [],
        dir: 1,
        stepDown: 10,
        enemySpeed: 0.35,
        enemies: [],
        obstacles: [
            {x: 45, y: 330, w: 46, h: 16, hp: 6},
            {x: 137, y: 330, w: 46, h: 16, hp: 6},
            {x: 229, y: 330, w: 46, h: 16, hp: 6},
        ],
        nextEnemyShotAt: 0,
        lastFireAt: 0,
    };

    const titleEl = document.getElementById('title');
    if (titleEl) titleEl.textContent = posName;
    document.title = posName;

    hud.textContent = 'Score: 0 / ' + targetScore;

    for (let r = 0; r < 4; r++) {
        for (let c = 0; c < 6; c++) {
            state.enemies.push({x: 26 + c * 45, y: 40 + r * 30, w: 22, h: 14, alive: true});
        }
    }

    function rectHit(a, b) {
        return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
    }

    function fire() {
        const now = Date.now();
        if (state.bullet || now - state.lastFireAt < 180 || state.over) return;
        state.bullet = {x: state.player.x + state.player.w / 2 - 1.5, y: state.player.y - 8, w: 3, h: 8, v: -5.2};
        state.lastFireAt = now;
    }

    function spawnEnemyShot() {
        const alive = state.enemies.filter(e => e.alive);
        if (!alive.length) return;
        const shooter = alive[(Math.random() * alive.length) | 0];
        state.enemyBullets.push({x: shooter.x + shooter.w / 2 - 1.5, y: shooter.y + shooter.h, w: 3, h: 8, v: 2.6});
    }

    function damageObstacle(x, y) {
        for (const o of state.obstacles) {
            if (o.hp <= 0) continue;
            if (x >= o.x && x <= o.x + o.w && y >= o.y && y <= o.y + o.h) {
                o.hp -= 1;
                return true;
            }
        }
        return false;
    }

    function submitWin() {
        const elapsed = Date.now() - state.startMs;
        const body = new URLSearchParams();
        body.set('token', token);
        body.set('score', String(state.score));
        body.set('timeMs', String(elapsed));

        fetch('/solve', {
            method: 'POST',
            headers: {'Content-Type': 'application/x-www-form-urlencoded'},
            body: body.toString(),
        }).then(async (res) => {
            const txt = await res.text();
            msg.textContent = txt;
        }).catch(() => {
            msg.textContent = 'Network error while submitting score.';
        });
    }

    function update() {
        if (state.over) return;

        if (state.keys['ArrowLeft'] || state.keys['a']) state.player.x -= state.player.speed;
        if (state.keys['ArrowRight'] || state.keys['d']) state.player.x += state.player.speed;
        if (state.player.x < 0) state.player.x = 0;
        if (state.player.x + state.player.w > canvas.width) state.player.x = canvas.width - state.player.w;

        let edge = false;
        for (const e of state.enemies) {
            if (!e.alive) continue;
            e.x += state.enemySpeed * state.dir;
            if (e.x <= 6 || e.x + e.w >= canvas.width - 6) edge = true;
        }
        if (edge) {
            state.dir *= -1;
            for (const e of state.enemies) {
                if (!e.alive) continue;
                e.y += state.stepDown;
                if (e.y + e.h >= state.player.y) {
                    state.over = true;
                    msg.textContent = 'Game over: monsters reached you.';
                    return;
                }
            }
        }

        const now = Date.now();
        if (now >= state.nextEnemyShotAt) {
            spawnEnemyShot();
            state.nextEnemyShotAt = now + 350 + ((Math.random() * 400) | 0);
        }

        if (state.bullet) {
            state.bullet.y += state.bullet.v;
            if (state.bullet.y + state.bullet.h < 0) {
                state.bullet = null;
            } else {
                if (damageObstacle(state.bullet.x + 1, state.bullet.y + 1)) {
                    state.bullet = null;
                } else {
                    for (const e of state.enemies) {
                        if (!e.alive || !state.bullet) continue;
                        if (rectHit(state.bullet, e)) {
                            e.alive = false;
                            state.bullet = null;
                            state.score += 1;
                            hud.textContent = 'Score: ' + state.score + ' / ' + targetScore;
                            if (state.score >= targetScore && !state.won) {
                                state.won = true;
                                state.over = true;
                                msg.textContent = 'Target score reached. Verifying with server...';
                                submitWin();
                            }
                        }
                    }
                }
            }
        }

        for (const b of state.enemyBullets) b.y += b.v;
        state.enemyBullets = state.enemyBullets.filter(b => b.y <= canvas.height + 10);

        for (const b of state.enemyBullets) {
            if (rectHit(b, state.player)) {
                state.over = true;
                msg.textContent = 'Game over: you were hit.';
                return;
            }
            if (damageObstacle(b.x + 1, b.y + 1)) {
                b.y = canvas.height + 20;
            }
        }
    }

    function draw() {
        ctx.fillStyle = '#000';
        ctx.fillRect(0, 0, canvas.width, canvas.height);

        ctx.fillStyle = '#fff';
        for (const o of state.obstacles) {
            if (o.hp <= 0) continue;
            ctx.globalAlpha = 0.25 + (o.hp / 6) * 0.75;
            ctx.fillRect(o.x, o.y, o.w, o.h);
            ctx.globalAlpha = 1;
        }

        ctx.fillStyle = '#fff';
        ctx.fillRect(state.player.x, state.player.y, state.player.w, state.player.h);
        ctx.fillRect(state.player.x + 8, state.player.y - 4, 4, 4);

        ctx.fillStyle = '#fff';
        if (state.bullet) ctx.fillRect(state.bullet.x, state.bullet.y, state.bullet.w, state.bullet.h);

        ctx.fillStyle = '#fff';
        for (const b of state.enemyBullets) ctx.fillRect(b.x, b.y, b.w, b.h);

        ctx.fillStyle = '#fff';
        for (const e of state.enemies) {
            if (!e.alive) continue;
            ctx.fillRect(e.x, e.y, e.w, e.h);
            ctx.fillRect(e.x + 4, e.y + e.h, 4, 3);
            ctx.fillRect(e.x + e.w - 8, e.y + e.h, 4, 3);
        }
    }

    function tick() {
        update();
        draw();
        requestAnimationFrame(tick);
    }

    document.addEventListener('keydown', (e) => {
        if (e.key === ' ') {
            e.preventDefault();
            fire();
            return;
        }
        state.keys[e.key] = true;
    });
    document.addEventListener('keyup', (e) => { state.keys[e.key] = false; });

    function bindHold(id, key) {
        const el = document.getElementById(id);
        if (!el) return;
        const down = (ev) => { ev.preventDefault(); state.keys[key] = true; };
        const up = (ev) => { ev.preventDefault(); state.keys[key] = false; };
        el.addEventListener('mousedown', down);
        el.addEventListener('mouseup', up);
        el.addEventListener('mouseleave', up);
        el.addEventListener('touchstart', down, {passive:false});
        el.addEventListener('touchend', up, {passive:false});
    }

    bindHold('leftBtn', 'ArrowLeft');
    bindHold('rightBtn', 'ArrowRight');
    const fireBtn = document.getElementById('fireBtn');
    fireBtn.addEventListener('click', (ev) => { ev.preventDefault(); fire(); });
    fireBtn.addEventListener('touchstart', (ev) => { ev.preventDefault(); fire(); }, {passive:false});

    tick();
})();
</script>
</body>
</html>)HTML";

    webServer.sendHeader("Cache-Control", "no-store");
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.send(200, "text/html", "");
    webServer.sendContent_P(PAGE_HEAD);
    webServer.sendContent(POS_NAME);
    webServer.sendContent_P(PAGE_AFTER_TITLE);
    webServer.sendContent(POS_NAME);
    webServer.sendContent_P(PAGE_AFTER_NAME);
    webServer.sendContent(String(currentChallenge.token));
    webServer.sendContent_P(PAGE_AFTER_TOKEN);
    webServer.sendContent(String(WEB_GAME_TARGET_SCORE));
    webServer.sendContent_P(PAGE_AFTER_TARGET);
    webServer.sendContent((isManualUnlockActive() || lockCtrl.isActive()) ? "OPEN" : "CLOSED");
    webServer.sendContent_P(PAGE_REST);
    webServer.sendContent("");
}

void setupWebServer()
{
        generateWebGameChallenge();

    webServer.on("/", HTTP_GET, []()
                 {
                generateWebGameChallenge();
                sendWebGamePage(); });

    webServer.on("/solve", HTTP_POST, []()
                 {
                if (verifyWebGameChallenge())
        {
            requestManualUnlock();
                        generateWebGameChallenge();
                        webServer.send(200, "text/plain", "Unlocked. Lock opened.");
            return;
        }

                generateWebGameChallenge();
                webServer.send(403, "text/plain", "Game check failed. Try a new run."); });

    webServer.on("/open", HTTP_POST, []()
                 {
        webServer.send(403, "text/plain", "Direct unlock disabled. Solve puzzle at /"); });

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

void setStatusStripColor(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t color = statusStrip.Color(r, g, b);
    for (uint16_t i = 0; i < statusStrip.numPixels(); i++)
    {
        statusStrip.setPixelColor(i, color);
    }
    statusStrip.show();
}

void syncStatusStripWithRelay()
{
    bool relayActive = lockCtrl.isActive();
    if (relayActive)
    {
        if (!ledModeInitialized || !lastRelayActive)
        {
            setStatusStripColor(0, 255, 0);
            ledModeInitialized = true;
        }
        lastRelayActive = true;
        return;
    }

    if (requestInProgress)
    {
        setStatusStripColor(255, 120, 0);
        lastRelayActive = false;
        ledModeInitialized = true;
        return;
    }

    const uint8_t minRed = 26; // ~10% of 255
    if (state == PosState::IDLE)
    {
        const uint32_t periodMs = 3000;
        uint32_t phase = millis() % periodMs;
        uint32_t halfPeriod = periodMs / 2;
        uint8_t red = 255;
        if (phase < halfPeriod)
        {
            red = 255 - ((255 - minRed) * phase) / halfPeriod;
        }
        else
        {
            red = minRed + ((255 - minRed) * (phase - halfPeriod)) / halfPeriod;
        }

        if (!ledModeInitialized || lastRelayActive || red != lastRedLevel)
        {
            setStatusStripColor(red, 0, 0);
            lastRedLevel = red;
            ledModeInitialized = true;
        }
    }
    else if (!ledModeInitialized || lastRelayActive || lastRedLevel != 255)
    {
        setStatusStripColor(255, 0, 0);
        lastRedLevel = 255;
        ledModeInitialized = true;
    }

    lastRelayActive = false;
}

bool ensureWifi(uint32_t timeoutMs = 12000)
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
        syncStatusStripWithRelay();
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
        // Initialize idle screen once when entering IDLE
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
    statusStrip.begin();
    statusStrip.setBrightness(STATUS_LED_BRIGHTNESS);
    setStatusStripColor(255, 0, 0);
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
    // Two-step auth: card UID -> USBUTLER entity -> REFINANCE POS charge
    setRequestInProgress(true);
    beginRequestStatusDisplay("net check");
    displayStatusCallback("net check", "[wifi...]", nullptr);
    if (!ensureWifi())
    {
        setRequestInProgress(false);
        display.showMessage("WiFi FAIL", "try again");
        setState(PosState::ERROR_STATE);
        return;
    }
    displayStatusCallback("auth", "[starting...]", nullptr);
    auto r = api.authorizeByCardUID(posCtx.cardUID,
                                    posCtx.amount,
                                    posCtx.currency,
                                    POS_ENTITY_ID,
                                    displayStatusCallback,
                                    nullptr);
    setRequestInProgress(false);
    if (!r.ok || !r.success)
    {
        String reason = (r.error.length() > 0) ? r.error : (String("HTTP ") + r.httpCode);
        display.showMessage(String("PAYMENT FAILED"), reason);
        setState(PosState::ERROR_STATE);
        display.blink(3, 250);
        return;
    }
    posCtx.meId = r.entityId;
    posCtx.payerName = r.entityName;
    posCtx.balancePrefetched = r.balanceAvailable;
    posCtx.balanceCompleted = r.balanceCompleted;
    posCtx.balanceDraft = r.balanceDraft;
    display.showMessage(String("@") + posCtx.payerName, String("-") + formatAmountCompact(posCtx.amount) + " " + posCtx.currency);
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
    if (millis() - posCtx.stateSince > 3000)
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

    syncStatusStripWithRelay();
    pumpRequestStatusDisplay();
    // Animate any active display effects (e.g., idle price blink)
    display.tick();
}
