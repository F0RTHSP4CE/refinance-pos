#pragma once
#include <LiquidCrystal_I2C.h>
#include <Arduino.h>
#include "ILogger.h"
#include "Config.h"

#ifndef IDLE_PROMPT_BLINK_MS
#define IDLE_PROMPT_BLINK_MS 1000
#endif

#ifndef IDLE_PROMPT_EFFECT_SWITCH_MS
#define IDLE_PROMPT_EFFECT_SWITCH_MS 10000
#endif

#ifndef IDLE_PROMPT_FLICKER_MS
#define IDLE_PROMPT_FLICKER_MS 500
#endif

class Display
{
public:
    Display(ILogger &logger) : _lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS), _logger(logger) {}

    void begin()
    {
        _lcd.init(); // low-level init
        _lcd.backlight();
        uint8_t leftArrow[8] = {
            B00001,
            B00011,
            B00111,
            B01111,
            B00111,
            B00011,
            B00001,
            B00000,
        };
        _lcd.createChar(0, leftArrow);
        clear();
        printLine("Booting...", 0);
        // no return
    }

    void blink(int times = 3, int delayMs = 150)
    {
        for (int i = 0; i < times; i++)
        {
            _lcd.noBacklight();
            delay(delayMs);
            _lcd.backlight();
            delay(delayMs);
        }
    }

    void clear() { _lcd.clear(); }

    void printLine(const String &text, uint8_t line)
    {
        if (line >= LCD_ROWS)
            return;
        String t = text;
        if (t.length() > LCD_COLS)
            t = t.substring(0, LCD_COLS);
        _lcd.setCursor(0, line);
        for (uint8_t i = 0; i < LCD_COLS; i++)
            _lcd.print(' '); // clear line
        _lcd.setCursor(0, line);
        _lcd.print(t);
    }

    void showMessage(const String &l1, const String &l2 = "")
    {
        clear();
        printLine(l1, 0);
        if (LCD_ROWS > 1)
            printLine(l2, 1);
        // When an explicit message is shown, stop any idle-only effects
        _idle.active = false; // do not rotate hints outside of IDLE
    }

    void showIdle(const String &posName, double amount, const String &currency)
    {
        clear();
        // Top line will rotate hints; start with provided posName immediately (centered)
        printCentered(posName, 0);
        // Bottom line prompt
        if (LCD_ROWS > 1)
            printIdlePrompt(true);
        // Enable idle rotation
        _idle.active = true;
        _idle.lastRotate = millis();
        _idle.index = 0; // next after POS_NAME
        _idle.posName = posName;
        _idle.promptVisible = true;
        _idle.lastPromptBlink = millis();
        _idle.effect = IdlePromptEffect::BLINK;
        _idle.lastEffectSwitch = millis();
        _idle.lastPromptFlicker = millis();
        _idle.flickerRestorePending = false;
        _idle.flickerCharIndex = 0;
    }

    void showStatus(const String &status, const String &posName = POS_NAME)
    {
        // Status screens are not IDLE; disable idle rotation
        _idle.active = false;
        printLine(posName, 0);
        if (LCD_ROWS > 1)
            printLine(status, 1);
    }

    void tick()
    {
        unsigned long now = millis();
        // Handle idle hint rotation (top line)
        if (_idle.active)
        {
            if (now - _idle.lastRotate >= HELP_HINTS_SHOW_MS)
            {
                _idle.lastRotate = now;
                // Determine next text: cycle POS_NAME + HELP_HINTS
                String nextText;
                if (_idle.index == 0)
                {
                    nextText = _idle.posName; // show POS_NAME
                }
                else
                {
                    uint8_t hintIdx = (_idle.index - 1) % HELP_HINTS_COUNT;
                    nextText = String(HELP_HINTS[hintIdx]);
                }
                printCentered(nextText, 0);
                _idle.index = (_idle.index + 1) % (HELP_HINTS_COUNT + 1);
            }

            if (LCD_ROWS > 1 && now - _idle.lastPromptBlink >= IDLE_PROMPT_BLINK_MS)
            {
                if (now - _idle.lastEffectSwitch >= IDLE_PROMPT_EFFECT_SWITCH_MS)
                {
                    _idle.lastEffectSwitch = now;
                    _idle.effect = (_idle.effect == IdlePromptEffect::BLINK) ? IdlePromptEffect::FLICKER : IdlePromptEffect::BLINK;
                    _idle.promptVisible = true;
                    _idle.flickerRestorePending = false;
                    printIdlePrompt(true);
                }

                if (_idle.effect == IdlePromptEffect::BLINK)
                {
                    _idle.lastPromptBlink = now;
                    _idle.promptVisible = !_idle.promptVisible;
                    printIdlePrompt(_idle.promptVisible);
                }
                else
                {
                    if (_idle.flickerRestorePending)
                    {
                        printIdlePrompt(true);
                        _idle.flickerRestorePending = false;
                    }
                    else if (now - _idle.lastPromptFlicker >= IDLE_PROMPT_FLICKER_MS)
                    {
                        _idle.lastPromptFlicker = now;
                        printIdlePromptFlicker(_idle.flickerCharIndex);
                        do
                        {
                            _idle.flickerCharIndex = (_idle.flickerCharIndex + 1) % IDLE_PROMPT_TEXT_LEN;
                        } while (IDLE_PROMPT_TEXT[_idle.flickerCharIndex] == ' ');
                        _idle.flickerRestorePending = true;
                    }
                }
            }
        }
    }

private:

    enum class IdlePromptEffect
    {
        BLINK,
        FLICKER
    };

    LiquidCrystal_I2C _lcd;
    ILogger &_logger;
    struct IdleRotation
    {
        bool active = false;
        unsigned long lastRotate = 0;
        unsigned long lastPromptBlink = 0;
        unsigned long lastPromptFlicker = 0;
        unsigned long lastEffectSwitch = 0;
        uint8_t index = 0; // 0 -> POS_NAME, 1..N -> hints
        uint8_t flickerCharIndex = 0;
        String posName;
        bool promptVisible = true;
        bool flickerRestorePending = false;
        IdlePromptEffect effect = IdlePromptEffect::BLINK;
    } _idle;

    void printCentered(const String &text, uint8_t line)
    {
        if (line >= LCD_ROWS)
            return;
        String t = text;
        if ((int)t.length() > LCD_COLS)
            t = t.substring(0, LCD_COLS);
        uint8_t startCol = (t.length() >= LCD_COLS) ? 0 : ((LCD_COLS - t.length()) / 2);
        // Clear the line first
        _lcd.setCursor(0, line);
        for (uint8_t i = 0; i < LCD_COLS; i++)
            _lcd.print(' ');
        _lcd.setCursor(startCol, line);
        _lcd.print(t);
    }

    void printIdlePrompt(bool visible)
    {
        _lcd.setCursor(0, 1);
        for (uint8_t i = 0; i < LCD_COLS; i++)
            _lcd.print(' ');
        if (visible)
        {
            _lcd.setCursor(0, 1);
            _lcd.write((uint8_t)0);
            _lcd.print(IDLE_PROMPT_TEXT);
        }
    }

    void printIdlePromptFlicker(uint8_t blankIndex)
    {
        char buffer[IDLE_PROMPT_TEXT_LEN + 1];
        for (uint8_t i = 0; i < IDLE_PROMPT_TEXT_LEN; i++)
            buffer[i] = IDLE_PROMPT_TEXT[i];
        buffer[IDLE_PROMPT_TEXT_LEN] = '\0';

        if (blankIndex < IDLE_PROMPT_TEXT_LEN)
            buffer[blankIndex] = ' ';

        _lcd.setCursor(0, 1);
        for (uint8_t i = 0; i < LCD_COLS; i++)
            _lcd.print(' ');

        _lcd.setCursor(0, 1);
        _lcd.write((uint8_t)0);
        _lcd.print(buffer);
    }
};
