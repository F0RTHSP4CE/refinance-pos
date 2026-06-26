#pragma once
#include <Adafruit_NeoPixel.h>
#include "Config.h"

class StatusLed
{
public:
    StatusLed(uint8_t pin, uint16_t count, uint8_t brightness)
        : _strip(count, pin, NEO_GRB + NEO_KHZ800), _brightness(brightness) {}

    void begin()
    {
        _strip.begin();
        _strip.setBrightness(_brightness);
        setColor(255, 0, 0);
    }

    /// Set an explicit color that persists until invalidate() is called.
    void setColor(uint8_t r, uint8_t g, uint8_t b)
    {
        _setAll(r, g, b);
        _hold = true;
        _blinkUntil = 0;
        _initialized = true;
    }

    /// Start a red blink error effect for the given duration.
    void blinkError(uint32_t durationMs = 5000, uint32_t intervalMs = 300)
    {
        _blinkStart = millis();
        _blinkUntil = _blinkStart + durationMs;
        _blinkInterval = intervalMs;
        _hold = false;
        _initialized = true;
    }

    /// Release explicit color hold; next sync() resumes automatic mode.
    void invalidate()
    {
        _hold = false;
        _blinkUntil = 0;
        _initialized = false;
    }

    /// Release explicit color hold without cancelling an active blink effect.
    void releaseHold()
    {
        _hold = false;
        if (_blinkUntil == 0)
            _initialized = false;
    }

    /// Update LED based on current state.  Call every loop iteration.
    ///   relayActive      – door relay is energized
    ///   requestInProgress – HTTP request running
    ///   isIdle            – POS is in IDLE state
    ///   chargeFailed      – charge was attempted and failed (relay still open)
    void sync(bool relayActive, bool requestInProgress, bool isIdle, bool chargeFailed)
    {
        // Blink error effect takes priority over everything.
        if (_blinkUntil > 0)
        {
            unsigned long now = millis();
            if ((long)(_blinkUntil - now) <= 0)
            {
                _blinkUntil = 0;
                _initialized = false;
                // fall through to normal logic
            }
            else
            {
                bool on = ((now - _blinkStart) / _blinkInterval) % 2 == 0;
                _setAll(on ? 255 : 0, 0, 0);
                _lastRelayActive = relayActive;
                return;
            }
        }

        // Explicit color hold — skip automatic updates.
        if (_hold)
        {
            _lastRelayActive = relayActive;
            return;
        }

        if (requestInProgress)
        {
            _setAll(255, 120, 0);
            _lastRelayActive = relayActive;
            _initialized = true;
            return;
        }

        if (relayActive)
        {
            if (chargeFailed)
            {
                if (!_initialized || !_lastRelayActive)
                {
                    _setAll(255, 0, 0);
                    _initialized = true;
                }
            }
            else
            {
                if (!_initialized || !_lastRelayActive)
                {
                    _setAll(0, 255, 0);
                    _initialized = true;
                }
            }
            _lastRelayActive = true;
            return;
        }

        const uint8_t minRed = 26; // ~10% of 255
        if (isIdle)
        {
            const uint32_t periodMs = 3000;
            uint32_t phase = millis() % periodMs;
            uint32_t halfPeriod = periodMs / 2;
            uint8_t red;
            if (phase < halfPeriod)
            {
                red = 255 - ((255 - minRed) * phase) / halfPeriod;
            }
            else
            {
                red = minRed + ((255 - minRed) * (phase - halfPeriod)) / halfPeriod;
            }

            if (!_initialized || _lastRelayActive || red != _lastRedLevel)
            {
                _setAll(red, 0, 0);
                _lastRedLevel = red;
                _initialized = true;
            }
        }
        else if (!_initialized || _lastRelayActive || _lastRedLevel != 255)
        {
            _setAll(255, 0, 0);
            _lastRedLevel = 255;
            _initialized = true;
        }

        _lastRelayActive = false;
    }

private:
    Adafruit_NeoPixel _strip;
    uint8_t _brightness;
    bool _initialized = false;
    bool _lastRelayActive = false;
    bool _hold = false;
    uint8_t _lastRedLevel = 255;
    unsigned long _blinkStart = 0;
    unsigned long _blinkUntil = 0;
    uint32_t _blinkInterval = 300;

    void _setAll(uint8_t r, uint8_t g, uint8_t b)
    {
        uint32_t color = _strip.Color(r, g, b);
        for (uint16_t i = 0; i < _strip.numPixels(); i++)
        {
            _strip.setPixelColor(i, color);
        }
        _strip.show();
    }
};
