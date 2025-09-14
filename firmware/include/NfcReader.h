#pragma once
#include "Config.h" // ensure pin macros first
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include "ILogger.h"

class NfcReader
{
public:
    // Adafruit_PN532 I2C constructor signature: (irq, reset, TwoWire*)
    NfcReader(TwoWire &wire, ILogger &logger)
        : _wire(wire), _logger(logger), _pn532(PN532_IRQ, PN532_RESET, &wire) {}

    bool begin()
    {
        // I2C mode: board address is fixed; ensure Wire already begun.
        if (!_pn532.begin())
        {
            _logger.error("PN532 init failed");
            return false;
        }
        uint32_t versiondata = _pn532.getFirmwareVersion();
        if (!versiondata)
        {
            _logger.error("Didn't find PN53x board");
            return false;
        }
        _pn532.SAMConfig();
        _logger.info("PN532 ready");
        return true;
    }

    // Poll for a passive target. Returns true and fills uidHex if found.
    bool readCardUID(String &uidHex)
    {
        boolean success;
        uint8_t uid[7];
        uint8_t uidLength;

        success = _pn532.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50); // 50ms timeout
        if (!success)
            return false;

        char buf[15];
        String out;
        for (uint8_t i = 0; i < uidLength; i++)
        {
            sprintf(buf, "%02X", uid[i]);
            out += buf;
        }
        uidHex = out;
        return true;
    }

private:
    TwoWire &_wire;
    ILogger &_logger;
    Adafruit_PN532 _pn532;
};
