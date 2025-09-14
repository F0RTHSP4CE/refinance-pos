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
        _blink.active = false; // cancel idle blink when explicit message shown
    }

    void showIdle(const String &posName, double amount, const String &currency)
    {
        clear();
        printLine(posName, 0);
        // Prepare bottom-right price text and blinking
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f %s", amount, currency.c_str());
        String t(buf);
        // If longer than display, show rightmost part
        if ((int)t.length() > LCD_COLS)
            t = t.substring(t.length() - LCD_COLS);
        uint8_t startCol = (t.length() >= LCD_COLS) ? 0 : (LCD_COLS - t.length());
        // Blink with asymmetric timing: visible 2000ms, invisible 1000ms
        startBlink(t, /*row*/ (LCD_ROWS > 1 ? 1 : 0), startCol, /*visibleMs*/ 2000, /*invisibleMs*/ 1000);
    }

    void showStatus(const String &status, const String &posName = POS_NAME)
    {
        _blink.active = false;
        printLine(posName, 0);
        if (LCD_ROWS > 1)
            printLine(status, 1);
    }

    void tick()
    {
        if (!_blink.active)
            return;
        unsigned long now = millis();
        uint16_t interval = _blink.visible ? _blink.visibleMs : _blink.invisibleMs;
        if (now - _blink.lastToggle < interval)
            return;
        _blink.lastToggle = now;
        _blink.visible = !_blink.visible;
        // Render only the region of the price at bottom-right
        _lcd.setCursor(_blink.col, _blink.row);
        if (_blink.visible)
        {
            _lcd.print(_blink.text);
        }
        else
        {
            for (uint8_t i = 0; i < _blink.text.length(); ++i)
                _lcd.print(' ');
        }
    }

private:
    LiquidCrystal_I2C _lcd;
    ILogger &_logger;
    struct BlinkState
    {
        bool active = false;
        bool visible = true;
        String text;
        uint8_t row = 1;
        uint8_t col = 0;
        unsigned long lastToggle = 0;
        uint16_t visibleMs = 2000;   // visible duration
        uint16_t invisibleMs = 1000; // invisible duration
    } _blink;

    void startBlink(const String &text, uint8_t row, uint8_t col, uint16_t visibleMs = 2000, uint16_t invisibleMs = 1000)
    {
        _blink.active = true;
        _blink.visible = true;
        _blink.text = text;
        _blink.row = row;
        _blink.col = col;
        _blink.visibleMs = visibleMs;
        _blink.invisibleMs = invisibleMs;
        // Draw initial text and set timing baseline
        _lcd.setCursor(_blink.col, _blink.row);
        _lcd.print(_blink.text);
        _blink.lastToggle = millis();
    }
};
