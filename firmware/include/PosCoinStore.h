#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include "Config.h"

struct PosCoinRecord
{
    uint8_t uuid[16];
    char generatedAt[24];
};

class PosCoinStore
{
public:
    bool begin()
    {
        if (!_prefs.begin("poscoins", false))
            return false;
        return load();
    }

    uint8_t count() const
    {
        return _count;
    }

    const PosCoinRecord &record(uint8_t index) const
    {
        return _records[index];
    }

    bool contains(const uint8_t uuid[16]) const
    {
        return findIndex(uuid) >= 0;
    }

    bool addOrUpdate(const uint8_t uuid[16], const String &generatedAt)
    {
        int index = findIndex(uuid);
        if (index < 0)
        {
            if (_count >= POS_COIN_MAX_TOKENS)
                return false;
            index = _count++;
        }

        memcpy(_records[index].uuid, uuid, 16);
        copyTimestamp(_records[index].generatedAt, generatedAt);
        return save();
    }

    bool remove(const uint8_t uuid[16])
    {
        int index = findIndex(uuid);
        if (index < 0)
            return true;

        for (uint8_t i = index; i + 1 < _count; ++i)
        {
            _records[i] = _records[i + 1];
        }
        _count--;
        return save();
    }

    bool consume(const uint8_t uuid[16])
    {
        int index = findIndex(uuid);
        if (index < 0)
            return false;

        for (uint8_t i = index; i + 1 < _count; ++i)
        {
            _records[i] = _records[i + 1];
        }
        _count--;
        return save();
    }

private:
    static const uint32_t STORE_MAGIC = 0x31435052; // "RPC1", little-endian.
    static const uint8_t STORE_VERSION = 1;

    struct StoreHeader
    {
        uint32_t magic;
        uint8_t version;
        uint8_t count;
        uint16_t reserved;
    };

    Preferences _prefs;
    PosCoinRecord _records[POS_COIN_MAX_TOKENS];
    uint8_t _count = 0;

    int findIndex(const uint8_t uuid[16]) const
    {
        for (uint8_t i = 0; i < _count; ++i)
        {
            if (memcmp(_records[i].uuid, uuid, 16) == 0)
                return i;
        }
        return -1;
    }

    bool load()
    {
        _count = 0;
        size_t size = _prefs.getBytesLength("tokens");
        if (size == 0)
            return true;
        if (size < sizeof(StoreHeader))
            return false;

        uint8_t buffer[sizeof(StoreHeader) + (sizeof(PosCoinRecord) * POS_COIN_MAX_TOKENS)];
        if (size > sizeof(buffer))
            return false;

        size_t read = _prefs.getBytes("tokens", buffer, size);
        if (read != size)
            return false;

        StoreHeader header;
        memcpy(&header, buffer, sizeof(header));
        if (header.magic != STORE_MAGIC || header.version != STORE_VERSION || header.count > POS_COIN_MAX_TOKENS)
            return false;

        size_t expected = sizeof(StoreHeader) + (sizeof(PosCoinRecord) * header.count);
        if (size != expected)
            return false;

        memcpy(_records, buffer + sizeof(StoreHeader), sizeof(PosCoinRecord) * header.count);
        _count = header.count;
        return true;
    }

    bool save()
    {
        uint8_t buffer[sizeof(StoreHeader) + (sizeof(PosCoinRecord) * POS_COIN_MAX_TOKENS)];
        StoreHeader header = {STORE_MAGIC, STORE_VERSION, _count, 0};
        size_t size = sizeof(StoreHeader) + (sizeof(PosCoinRecord) * _count);

        memcpy(buffer, &header, sizeof(header));
        if (_count > 0)
            memcpy(buffer + sizeof(StoreHeader), _records, sizeof(PosCoinRecord) * _count);

        return _prefs.putBytes("tokens", buffer, size) == size;
    }

    static void copyTimestamp(char *dest, const String &timestamp)
    {
        size_t len = timestamp.length();
        const size_t maxLen = sizeof(((PosCoinRecord *)0)->generatedAt);
        if (len >= maxLen)
            len = maxLen - 1;
        memcpy(dest, timestamp.c_str(), len);
        dest[len] = '\0';
    }
};
