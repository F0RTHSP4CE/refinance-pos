#pragma once
#include "Config.h" // ensure pin macros first
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <string.h>
#include "ILogger.h"

struct NfcTag
{
    String uidHex;
    uint8_t uid[7];
    uint8_t uidLength = 0;
};

struct PosCoinPayload
{
    uint8_t uuid[16];
};

enum class PosCoinWriteStatus
{
    Ok,
    NoTag,
    UnsupportedTag,
    WriteFailed
};

enum class PosCoinReadStatus
{
    Ok,
    NoTag,
    UnsupportedTag,
    ReadFailed,
    BadMagic
};

class NfcReader
{
public:
    // Adafruit_PN532 I2C constructor signature: (irq, reset, TwoWire*)
    NfcReader(TwoWire &wire, ILogger &logger)
        : _wire(wire), _logger(logger), _pn532(PN532_IRQ, PN532_RESET, &wire) {}

    bool begin()
    {
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

    bool readCardUID(String &uidHex)
    {
        NfcTag tag;
        if (!pollTag(tag))
            return false;

        uidHex = tag.uidHex;
        return true;
    }

    bool pollTag(NfcTag &tag)
    {
        uint8_t uid[7];
        uint8_t uidLength;

        if (!_pn532.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50))
            return false;

        if (uidLength > sizeof(tag.uid))
            return false;

        memcpy(tag.uid, uid, uidLength);
        tag.uidLength = uidLength;
        tag.uidHex = uidToHex(uid, uidLength);
        return true;
    }

    bool readPosCoin(PosCoinPayload &payload, String &tagUidHex)
    {
        return readPosCoinDetailed(payload, tagUidHex) == PosCoinReadStatus::Ok;
    }

    PosCoinReadStatus readPosCoinDetailed(PosCoinPayload &payload, String &tagUidHex)
    {
        NfcTag tag;
        if (!pollTag(tag))
            return PosCoinReadStatus::NoTag;

        return readPosCoinDetailedForTag(tag, payload, tagUidHex);
    }

    PosCoinReadStatus readPosCoinDetailedForTag(const NfcTag &tag, PosCoinPayload &payload, String &tagUidHex)
    {
        tagUidHex = tag.uidHex;
        if (tag.uidLength != 7)
            return PosCoinReadStatus::UnsupportedTag;

        return readPosCoinPayload(payload);
    }

    PosCoinWriteStatus writePosCoin(const uint8_t uuid[16], String &tagUidHex, uint8_t oldUuid[16] = nullptr, bool *hadOldUuid = nullptr)
    {
        NfcTag tag;
        if (!pollTag(tag))
            return PosCoinWriteStatus::NoTag;

        return writePosCoinForTag(tag, uuid, tagUidHex, oldUuid, hadOldUuid);
    }

    PosCoinWriteStatus writePosCoinForTag(const NfcTag &tag, const uint8_t uuid[16], String &tagUidHex, uint8_t oldUuid[16] = nullptr, bool *hadOldUuid = nullptr)
    {
        if (hadOldUuid != nullptr)
            *hadOldUuid = false;

        tagUidHex = tag.uidHex;
        if (tag.uidLength != 7)
            return PosCoinWriteStatus::UnsupportedTag;

        PosCoinPayload oldPayload;
        if (readPosCoinPayload(oldPayload) == PosCoinReadStatus::Ok && oldUuid != nullptr)
        {
            memcpy(oldUuid, oldPayload.uuid, sizeof(oldPayload.uuid));
            if (hadOldUuid != nullptr)
                *hadOldUuid = true;
        }

        uint8_t raw[POS_COIN_PAYLOAD_PAGE_COUNT * 4];
        raw[0] = 'R';
        raw[1] = 'P';
        raw[2] = 'C';
        raw[3] = '1';
        memcpy(raw + 4, uuid, 16);

        for (uint8_t i = 0; i < POS_COIN_PAYLOAD_PAGE_COUNT; ++i)
        {
            uint8_t page[4];
            memcpy(page, raw + (i * 4), sizeof(page));
            if (!_pn532.ntag2xx_WritePage(POS_COIN_PAYLOAD_START_PAGE + i, page))
                return PosCoinWriteStatus::WriteFailed;
        }

        return PosCoinWriteStatus::Ok;
    }

private:
    TwoWire &_wire;
    ILogger &_logger;
    Adafruit_PN532 _pn532;

    PosCoinReadStatus readPosCoinPayload(PosCoinPayload &payload)
    {
        uint8_t page[4];
        uint8_t raw[POS_COIN_PAYLOAD_PAGE_COUNT * 4];
        for (uint8_t i = 0; i < POS_COIN_PAYLOAD_PAGE_COUNT; ++i)
        {
            if (!_pn532.ntag2xx_ReadPage(POS_COIN_PAYLOAD_START_PAGE + i, page))
                return PosCoinReadStatus::ReadFailed;
            memcpy(raw + (i * 4), page, sizeof(page));
        }

        if (raw[0] != 'R' || raw[1] != 'P' || raw[2] != 'C' || raw[3] != '1')
            return PosCoinReadStatus::BadMagic;

        memcpy(payload.uuid, raw + 4, sizeof(payload.uuid));
        return PosCoinReadStatus::Ok;
    }

    static String uidToHex(const uint8_t *uid, uint8_t uidLength)
    {
        char buf[3];
        String out;
        for (uint8_t i = 0; i < uidLength; i++)
        {
            sprintf(buf, "%02X", uid[i]);
            out += buf;
        }
        return out;
    }
};
