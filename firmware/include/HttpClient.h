#pragma once
#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include "ILogger.h"
#include "Config.h"

struct AuthLookupResult
{
    bool ok = false;
    bool success = false;
    String entityId;
    String entityName;
    bool balanceAvailable = false;
    double balanceCompleted = 0;
    double balanceDraft = 0;
    int httpCode = 0;
    String body;
    String error;
};

class ApiClient
{
public:
    ApiClient(ILogger &logger) : _logger(logger) {}

    bool begin() { return true; }

    AuthLookupResult authorizeByCardUID(const String &cardUID, double amount, const String &currency, int toEntityId)
    {
        AuthLookupResult r;
        String entityName;
        bool fromCache = lookupCachedEntityName(cardUID, entityName);
        int code = 0;

        if (!fromCache)
        {
            JsonDocument butlerDoc;
            JsonDocument butlerReq;
            butlerReq["value"] = cardUID;
            String butlerPayload;
            serializeJson(butlerReq, butlerPayload);

            code = postJson(String(USBUTLER_API_URL) + "/api/public/users/by-identifier", butlerPayload, nullptr, butlerDoc, r.body, r.error);
            r.httpCode = code;
            if (code != 200)
                return r;

            entityName = firstNonEmpty({
                butlerDoc["entity_name"],
                butlerDoc["data"]["entity_name"],
                butlerDoc["entity"]["name"],
                butlerDoc["data"]["entity"]["name"],
                butlerDoc["name"],
                butlerDoc["data"]["name"],
                butlerDoc["user"]["name"],
                butlerDoc["identifier"],
                butlerDoc["user"]["identifier"],
                butlerDoc["username"],
                butlerDoc["user"]["username"]});
            if (entityName.length() == 0)
            {
                r.error = F("USBUTLER response missing entity_name");
                return r;
            }

            cacheEntityName(cardUID, entityName);
        }

        JsonDocument chargeReq;
        chargeReq["entity_name"] = entityName;
        chargeReq["amount"] = amount;
        chargeReq["currency"] = currency;
        chargeReq["to_entity_id"] = toEntityId;
        String chargePayload;
        serializeJson(chargeReq, chargePayload);

        JsonDocument chargeDoc;
        code = postJson(String(REFINANCE_API_URL) + "/pos/charge", chargePayload, POS_SECRET, chargeDoc, r.body, r.error);
        r.httpCode = code;
        if (code != 200)
            return r;

        r.entityName = firstNonEmpty({
            chargeDoc["entity"]["name"],
            chargeDoc["data"]["entity"]["name"],
            chargeDoc["name"],
            chargeDoc["data"]["name"]});
        if (r.entityName.length() == 0)
            r.entityName = entityName;
        r.entityId = firstNonEmpty({
            chargeDoc["entity"]["id"],
            chargeDoc["data"]["entity"]["id"],
            chargeDoc["id"],
            chargeDoc["data"]["id"]});

        JsonVariantConst balance = chargeDoc["balance"];
        r.balanceAvailable = !balance.isNull();
        r.balanceCompleted = balanceValue(balance["completed"], currency);
        r.balanceDraft = balanceValue(balance["draft"], currency);
        r.ok = true;
        r.success = true;
        return r;
    }

private:
    static constexpr size_t UID_NAME_CACHE_SIZE = 12;

    struct UidNameCacheEntry
    {
        String cardUID;
        String entityName;
        bool valid = false;
    };

    bool lookupCachedEntityName(const String &cardUID, String &entityName)
    {
        for (size_t i = 0; i < UID_NAME_CACHE_SIZE; ++i)
        {
            if (_uidNameCache[i].valid && _uidNameCache[i].cardUID == cardUID)
            {
                entityName = _uidNameCache[i].entityName;
                return true;
            }
        }
        return false;
    }

    void cacheEntityName(const String &cardUID, const String &entityName)
    {
        for (size_t i = 0; i < UID_NAME_CACHE_SIZE; ++i)
        {
            if (_uidNameCache[i].valid && _uidNameCache[i].cardUID == cardUID)
            {
                _uidNameCache[i].entityName = entityName;
                return;
            }
        }

        _uidNameCache[_uidNameCacheNext].cardUID = cardUID;
        _uidNameCache[_uidNameCacheNext].entityName = entityName;
        _uidNameCache[_uidNameCacheNext].valid = true;
        _uidNameCacheNext = (_uidNameCacheNext + 1) % UID_NAME_CACHE_SIZE;
    }

    static String firstNonEmpty(std::initializer_list<JsonVariantConst> values)
    {
        for (JsonVariantConst value : values)
        {
            String s = value.isNull() ? String() : value.as<String>();
            s.trim();
            if (s.length() > 0)
                return s;
        }
        return String();
    }

    static double balanceValue(JsonVariantConst value, const String &currency)
    {
        if (value.isNull())
            return 0.0;
        if (!value.is<JsonObjectConst>())
            return value.as<double>();

        JsonObjectConst obj = value.as<JsonObjectConst>();
        JsonVariantConst direct = obj[currency.c_str()];
        if (!direct.isNull())
            return direct.as<double>();
        for (JsonPairConst kv : obj)
        {
            if (String(kv.key().c_str()).equalsIgnoreCase(currency))
                return kv.value().as<double>();
        }
        for (JsonPairConst kv : obj)
        {
            return kv.value().as<double>();
        }
        return 0.0;
    }

    int postJson(const String &url,
                 const String &payload,
                 const char *posSecret,
                 JsonDocument &outDoc,
                 String &responseBody,
                 String &error)
    {
        HTTPClient http;
        int code = -1;
        auto perform = [&](auto &client)
        {
            if (!http.begin(client, url))
            {
                error = F("http.begin failed");
                code = -1;
                return;
            }

            http.addHeader("Content-Type", "application/json");
            if (posSecret != nullptr)
                http.addHeader("x-pos-secret", posSecret);

#ifdef API_HTTP_DEBUG
            _logger.info(String(F("HTTP POST ")) + url + F(" payload=") + payload);
#endif
            code = http.POST(payload);
            responseBody = http.getString();
            http.end();

            if (code != 200)
            {
                error = String(F("HTTP ")) + code;
                return;
            }

            auto jsonErr = deserializeJson(outDoc, responseBody);
            if (jsonErr)
            {
                code = -1;
                error = String(F("JSON err: ")) + jsonErr.c_str();
            }
        };

        if (url.startsWith(F("https://")))
        {
            WiFiClientSecure client;
#ifdef API_INSECURE_TLS
            client.setInsecure();
#endif
            perform(client);
        }
        else
        {
            WiFiClient client;
            perform(client);
        }

        return code;
    }

    ILogger &_logger;
    UidNameCacheEntry _uidNameCache[UID_NAME_CACHE_SIZE];
    size_t _uidNameCacheNext = 0;
};
