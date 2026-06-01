#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <mbedtls/sha256.h>
#include "Config.h"

class WebGame
{
public:
    static const int TARGET_SCORE = 24;
    static const int POW_DIFFICULTY = 22;

    void generateChallenge()
    {
        _token = (uint32_t)random(100000, 999999);
        _issuedAt = millis();
        _expiresAt = _issuedAt + MAX_PLAY_MS;
        _consumed = false;
    }

    /// Verify a solution submitted via POST form args on the given WebServer.
    bool verifySolution(WebServer &server)
    {
        if (!server.hasArg("token") || !server.hasArg("score") || !server.hasArg("timeMs") || !server.hasArg("nonce"))
            return false;
        if (_consumed)
            return false;
        if ((long)(_expiresAt - millis()) <= 0)
            return false;
        if ((uint32_t)server.arg("token").toInt() != _token)
            return false;
        int score = server.arg("score").toInt();
        uint32_t timeMs = (uint32_t)server.arg("timeMs").toInt();
        if (score < TARGET_SCORE)
            return false;
        if (timeMs < MIN_PLAY_MS || timeMs > MAX_PLAY_MS)
            return false;
        uint32_t nonce = (uint32_t)server.arg("nonce").toInt();
        if (!_verifyPoW(nonce))
            return false;
        _consumed = true;
        return true;
    }

    /// Stream the game HTML page to the client.
    void sendPage(WebServer &server, const char *posName, bool doorOpen)
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

        static const char PAGE_AFTER_TARGET[] PROGMEM = R"HTML(' data-pow=')HTML";

        static const char PAGE_AFTER_POW[] PROGMEM = R"HTML('>
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
    <p id='lockMsg' style='max-width:360px;min-height:1.5rem;margin:0.3rem auto 0 auto;font-size:1.5rem;font-weight:bold;text-align:center;padding:0.5rem 0.6rem;display:none;'></p>
    <p id='msg' style='max-width:360px;min-height:1.5rem;margin:0.3rem auto 0 auto;white-space:pre-line;background:#fff;color:#000;padding:0.4rem 0.6rem;'></p>
    <p style='opacity:.85;font-size:.92rem;max-width:360px;margin-left:auto;margin-right:auto;'>Avoid monsters and enemy shots. Refresh for a fresh run.</p>

<script>
(() => {
    const posName = document.body.dataset.name || 'POS';
    const token = document.body.dataset.token || '';
    const targetScore = Number(document.body.dataset.target || '12');
    const powDifficulty = Number(document.body.dataset.pow || '20');
    const canvas = document.getElementById('game');
    const ctx = canvas.getContext('2d');
    const hud = document.getElementById('hud');
    const msg = document.getElementById('msg');
    const lockMsg = document.getElementById('lockMsg');
    const MSGS = [
        "maybe it's time to find a job",
        "is it worth your time?",
        "this snack is not free.",
        "i hope you are doing well",
        "hackers should hack their way to success",
        "cmon, what are you doing?",
        "this game is not an excuse for not paying, it's a tool to open the fridge in emergency.",
        "the complexity of this game will increase over time. find a better way.",
        "more you play = more you pay later.",
        "better than a debt, init?",
        "give it a taste, pleasant?",
        "why pay when you can play... yes?",
        "linkedin.com",
        "send your cv already.",
        "MONEY MONEY MONEY BITCH. WHERE? it's not free.",
        "economy needs economy.",
        "save your time, use an NFC card next time. time is precious.",
    ];
    function randomMsg() { return MSGS[(Math.random() * MSGS.length) | 0]; }
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

    // Compact pure-JS SHA-256 (ASCII input only).
    function sha256hex(s){var rr=function(n,x){return(n>>>x)|(n<<(32-x));};var K=[0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2];var H=[0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19];var bytes=[];for(var i=0;i<s.length;i++)bytes.push(s.charCodeAt(i));bytes.push(0x80);while(bytes.length%64!==56)bytes.push(0);var bl=s.length*8;bytes.push(0,0,0,0,(bl>>>24)&0xff,(bl>>>16)&0xff,(bl>>>8)&0xff,bl&0xff);for(var i=0;i<bytes.length;i+=64){var w=[];for(var j=0;j<16;j++)w.push((bytes[i+j*4]<<24)|(bytes[i+j*4+1]<<16)|(bytes[i+j*4+2]<<8)|bytes[i+j*4+3]);for(var j=16;j<64;j++){var s0=rr(w[j-15],7)^rr(w[j-15],18)^(w[j-15]>>>3);var s1=rr(w[j-2],17)^rr(w[j-2],19)^(w[j-2]>>>10);w.push((w[j-16]+s0+w[j-7]+s1)|0);}var a=H[0],b=H[1],c=H[2],d=H[3],e=H[4],f=H[5],g=H[6],h=H[7];for(var j=0;j<64;j++){var S1=rr(e,6)^rr(e,11)^rr(e,25);var ch=(e&f)^(~e&g);var t1=(h+S1+ch+K[j]+w[j])|0;var S0=rr(a,2)^rr(a,13)^rr(a,22);var maj=(a&b)^(a&c)^(b&c);var t2=(S0+maj)|0;h=g;g=f;f=e;e=(d+t1)|0;d=c;c=b;b=a;a=(t1+t2)|0;}H[0]=(H[0]+a)|0;H[1]=(H[1]+b)|0;H[2]=(H[2]+c)|0;H[3]=(H[3]+d)|0;H[4]=(H[4]+e)|0;H[5]=(H[5]+f)|0;H[6]=(H[6]+g)|0;H[7]=(H[7]+h)|0;}return H.map(function(x){return('00000000'+((x>>>0).toString(16))).slice(-8);}).join('');}

    // Proof-of-work: find nonce where SHA256("<token_hex8>:<nonce>") has powDifficulty leading zero bits.
    // Solved in background chunks so the game UI stays responsive.
    function powCheck(h){var fn=powDifficulty>>2,rb=powDifficulty&3;for(var i=0;i<fn;i++)if(h[i]!=='0')return false;return rb===0||(parseInt(h[fn],16)>>>(4-rb))===0;}
    var _powNonce = -1, _powDone = false, _powWait = [];
    (function(){
        var challenge = ('00000000' + (Number(token) >>> 0).toString(16)).slice(-8);
        var nonce = 0;
        function step(){
            for(var end = nonce + 500; nonce < end; nonce++){
                var h = sha256hex(challenge + ':' + nonce);
                if(powCheck(h)){
                    _powNonce = nonce; _powDone = true;
                    _powWait.forEach(function(fn){fn();}); _powWait = [];
                    return;
                }
            }
            setTimeout(step, 0);
        }
        step();
    })();

    function submitWin() {
        const elapsed = Date.now() - state.startMs;
        msg.style.background = '#000';
        msg.style.color = '#ff0';
        msg.style.fontSize = '1.4rem';
        msg.textContent = 'Target score reached. Computing proof-of-work...';
        function doSubmit() {
            const body = new URLSearchParams();
            body.set('token', token);
            body.set('score', String(state.score));
            body.set('timeMs', String(elapsed));
            body.set('nonce', String(_powNonce));
            fetch('/solve', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: body.toString(),
            }).then(async (res) => {
                const txt = await res.text();
                lockMsg.textContent = txt;
                lockMsg.style.display = 'block';
                msg.textContent = 'Target score reached!\n' + randomMsg();
            }).catch(() => {
                lockMsg.textContent = 'Network error while submitting score.';
                lockMsg.style.display = 'block';
            });
        }
        if (_powDone) doSubmit();
        else _powWait.push(doSubmit);
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
                    msg.textContent = 'Game over: monsters reached you.\n' + randomMsg();
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
                                msg.style.background = '#000';
                                msg.style.color = '#ff0';
                                msg.style.fontSize = '1.4rem';
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
                msg.textContent = 'Game over: you were hit.\n' + randomMsg();
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

        server.sendHeader("Cache-Control", "no-store");
        server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        server.send(200, "text/html", "");
        server.sendContent_P(PAGE_HEAD);
        server.sendContent(posName);
        server.sendContent_P(PAGE_AFTER_TITLE);
        server.sendContent(posName);
        server.sendContent_P(PAGE_AFTER_NAME);
        server.sendContent(String(_token));
        server.sendContent_P(PAGE_AFTER_TOKEN);
        server.sendContent(String(TARGET_SCORE));
        server.sendContent_P(PAGE_AFTER_TARGET);
        server.sendContent(String(POW_DIFFICULTY));
        server.sendContent_P(PAGE_AFTER_POW);
        server.sendContent(doorOpen ? "OPEN" : "CLOSED");
        server.sendContent_P(PAGE_REST);
        server.sendContent("");
    }

private:
    static const uint32_t MIN_PLAY_MS = 7000;
    static const uint32_t MAX_PLAY_MS = 180000;

    uint32_t _token = 0;
    unsigned long _issuedAt = 0;
    unsigned long _expiresAt = 0;
    bool _consumed = false;

    /// Verify SHA-256 proof-of-work: SHA256("<token_hex8>:<nonce>") must have POW_DIFFICULTY leading zero bits.
    bool _verifyPoW(uint32_t nonce) const
    {
        char input[32];
        snprintf(input, sizeof(input), "%08x:%u", (unsigned)_token, (unsigned)nonce);
        uint8_t hash[32];
        mbedtls_sha256((const uint8_t *)input, strlen(input), hash, 0);
        int bits = POW_DIFFICULTY;
        for (int i = 0; i < 32 && bits > 0; i++)
        {
            int check = bits >= 8 ? 8 : bits;
            uint8_t mask = (uint8_t)(0xFF << (8 - check));
            if (hash[i] & mask)
                return false;
            bits -= check;
        }
        return true;
    }
};
