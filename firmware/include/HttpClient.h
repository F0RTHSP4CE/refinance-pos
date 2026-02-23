#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
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
    using StatusCallback = void (*)(const String &line1, const String &line2, void *context);

    ApiClient(ILogger &logger) : _logger(logger) {}

    bool begin() { return true; }

    AuthLookupResult authorizeByCardUID(const String &cardUID,
                                        double amount,
                                        const String &currency,
                                        int toEntityId,
                                        StatusCallback statusCb = nullptr,
                                        void *statusCtx = nullptr)
    {
        AuthLookupResult r;
        String entityName;
        bool fromCache = lookupCachedEntityName(cardUID, entityName);
        int code = 0;

        emitStatus(statusCb, statusCtx, F("auth"), F("[card detected...]"));

        if (!fromCache)
        {
            emitStatus(statusCb, statusCtx, F("card lookup"), F("[usbutler...]"));

            JsonDocument butlerDoc;
            JsonDocument butlerReq;
            butlerReq["value"] = cardUID;
            String butlerPayload;
            serializeJson(butlerReq, butlerPayload);

            code = postJson(String(USBUTLER_API_URL) + "/api/public/users/by-identifier",
                            butlerPayload,
                            nullptr,
                            butlerDoc,
                            r.body,
                            r.error,
                            F("lookup"),
                            statusCb,
                            statusCtx);
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
                r.error = F("usbutler response missing entity_name");
                return r;
            }

            cacheEntityName(cardUID, entityName);
        }
        else
        {
            emitStatus(statusCb, statusCtx, F("card lookup"), F("[cache hit...]"));
        }

        emitStatus(statusCb, statusCtx, F("charge"), F("[preparing...]"));

        JsonDocument chargeReq;
        chargeReq["entity_name"] = entityName;
        chargeReq["amount"] = amount;
        chargeReq["currency"] = currency;
        chargeReq["to_entity_id"] = toEntityId;

        String chargePayload;
        serializeJson(chargeReq, chargePayload);

        JsonDocument chargeDoc;
        code = postJson(String(REFINANCE_API_URL) + "/pos/charge",
                        chargePayload,
                        POS_SECRET,
                        chargeDoc,
                        r.body,
                        r.error,
                        F("charge"),
                        statusCb,
                        statusCtx);
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

        emitStatus(statusCb, statusCtx, F("charge"), F("[approved]"));

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
                 String &error,
                 const String &statusPrefix,
                 StatusCallback statusCb,
                 void *statusCtx)
    {
        int code = -1;
        int attempts = API_HTTP_RETRIES + 1;

        for (int attempt = 1; attempt <= attempts; ++attempt)
        {
            HTTPClient http;
            auto perform = [&](WiFiClient &client)
            {
                client.setNoDelay(true);
                client.setTimeout(API_HTTP_TIMEOUT_MS);
                http.setTimeout(API_HTTP_TIMEOUT_MS);
                http.setReuse(false);
                http.useHTTP10(true);

                emitStatus(statusCb,
                           statusCtx,
                           statusPrefix,
                           String(F("[connect ")) + attempt + String(F("/")) + attempts + F("...]"));

                String host;
                if (!parseUrlHost(url, host))
                {
                    error = F("bad url");
                    code = -1;
                    return;
                }

                IPAddress resolvedIp;
                emitStatus(statusCb, statusCtx, statusPrefix, F("[resolving host...]"));
                if (!resolveHost(host, resolvedIp))
                {
                    error = String(F("dns lookup failed: ")) + host;
                    code = -1;
                    return;
                }

                _logger.info(String(F("DNS ")) + host + F(" -> ") + resolvedIp.toString() +
                             F(" via ") + WiFi.dnsIP(0).toString());

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

                emitStatus(statusCb, statusCtx, statusPrefix, F("[sending...]"));
                code = http.POST(payload);

                if (code > 0)
                {
                    emitStatus(statusCb, statusCtx, statusPrefix, F("[reading reply...]"));
                    responseBody = http.getString();
                }

                http.end();

                if (code != 200)
                {
                    if (code < 0)
                        error = httpCodeToError(code);
                    else
                        error = String(F("http ")) + code;
                    return;
                }

                emitStatus(statusCb, statusCtx, statusPrefix, F("[parsing...]"));
                auto jsonErr = deserializeJson(outDoc, responseBody);
                if (jsonErr)
                {
                    code = -1;
                    error = String(F("json err: ")) + jsonErr.c_str();
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

            if (code == 200)
                return code;

            bool retryable = isRetryableError(code);
            if (!retryable || attempt >= attempts)
                return code;

            emitStatus(statusCb,
                       statusCtx,
                       statusPrefix,
                       String(F("[retrying ")) + (attempt + 1) + String(F("/")) + attempts + F("...]"));

#if API_HTTP_RETRY_WIFI_RESET
            if (WiFi.status() != WL_CONNECTED)
            {
                WiFi.disconnect(false);
                delay(60);
                WiFi.reconnect();
            }
#endif

            delay(API_HTTP_RETRY_BACKOFF_MS * attempt);
            yield();
        }

        return code;
    }

    static bool isRetryableError(int code)
    {
        if (code < 0)
            return true;
        return code == 408 || code == 425 || code == 429 || code == 500 || code == 502 || code == 503 || code == 504;
    }

    static String httpCodeToError(int code)
    {
        switch (code)
        {
        case HTTPC_ERROR_CONNECTION_REFUSED:
            return F("http conn refused");
        case HTTPC_ERROR_SEND_HEADER_FAILED:
            return F("http send header failed");
        case HTTPC_ERROR_SEND_PAYLOAD_FAILED:
            return F("http send payload failed");
        case HTTPC_ERROR_NOT_CONNECTED:
            return F("http not connected");
        case HTTPC_ERROR_CONNECTION_LOST:
            return F("http connection lost");
        case HTTPC_ERROR_NO_STREAM:
            return F("http no stream");
        case HTTPC_ERROR_NO_HTTP_SERVER:
            return F("http no server");
        case HTTPC_ERROR_TOO_LESS_RAM:
            return F("http low ram");
        case HTTPC_ERROR_ENCODING:
            return F("http encoding err");
        case HTTPC_ERROR_STREAM_WRITE:
            return F("http stream write err");
        case HTTPC_ERROR_READ_TIMEOUT:
            return F("http read timeout");
        default:
            return String(F("http transport ")) + code;
        }
    }

    static void emitStatus(StatusCallback statusCb, void *statusCtx, const String &line1, const String &line2)
    {
        if (statusCb != nullptr)
            statusCb(line1, line2, statusCtx);
    }

    static bool parseUrlHost(const String &url, String &host)
    {
        int schemeEnd = url.indexOf("://");
        int authorityStart = (schemeEnd >= 0) ? (schemeEnd + 3) : 0;
        int authorityEnd = url.indexOf('/', authorityStart);
        if (authorityEnd < 0)
            authorityEnd = url.length();
        if (authorityStart >= authorityEnd)
            return false;

        String authority = url.substring(authorityStart, authorityEnd);
        authority.trim();
        if (authority.length() == 0)
            return false;

        int colon = authority.lastIndexOf(':');
        host = (colon >= 0) ? authority.substring(0, colon) : authority;

        host.trim();
        if (host.length() == 0)
            return false;
        return true;
    }

    static bool resolveHost(const String &host, IPAddress &outIp)
    {
        for (int i = 0; i < 2; ++i)
        {
            if (WiFi.hostByName(host.c_str(), outIp))
                return true;
            delay(50);
            yield();
        }
        return false;
    }

    ILogger &_logger;
    UidNameCacheEntry _uidNameCache[UID_NAME_CACHE_SIZE];
    size_t _uidNameCacheNext = 0;
};
