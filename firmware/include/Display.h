#pragma once
#include <LiquidCrystal_I2C.h>
#include <Arduino.h>
#include "ILogger.h"
#include "Config.h"

class Display
{
public:
    Display(ILogger &logger) : _lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS), _logger(logger) {}

    void begin()
    {
        _lcd.init(); // low-level init
        _lcd.backlight();
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
        // Prepare centered price text on bottom line (no blinking)
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f %s", amount, currency.c_str());
        String t(buf);
        if ((int)t.length() > LCD_COLS)
            t = t.substring(0, LCD_COLS); // trim to fit
        uint8_t startCol = (t.length() >= LCD_COLS) ? 0 : ((LCD_COLS - t.length()) / 2);
        _lcd.setCursor(startCol, (LCD_ROWS > 1 ? 1 : 0));
        _lcd.print(t);
        // Enable idle rotation
        _idle.active = true;
        _idle.lastRotate = millis();
        _idle.index = 0; // next after POS_NAME
        _idle.posName = posName;
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
            if (now - _idle.lastRotate >= HINT_ROTATE_MS)
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
        }
    }

private:
    LiquidCrystal_I2C _lcd;
    ILogger &_logger;
    struct IdleRotation
    {
        bool active = false;
        unsigned long lastRotate = 0;
        uint8_t index = 0; // 0 -> POS_NAME, 1..N -> hints
        String posName;
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
};
