#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include "Config.h"

class WebGame
{
public:
    static const int TARGET_SCORE = 10;

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
        if (!server.hasArg("token") || !server.hasArg("score") || !server.hasArg("timeMs"))
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
};
