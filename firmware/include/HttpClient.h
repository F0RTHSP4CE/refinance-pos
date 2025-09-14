#pragma once
#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <ArduinoJson.h>
#pragma GCC diagnostic pop
#include "ILogger.h"
#include "Config.h"

struct TokenResponse
{
    String token;
    bool ok = false;
    int httpCode = 0;
    String body; // raw body (for debugging / non-200)
};
struct Entity
{
    String id;
    String name;
    bool ok = false;
    int httpCode = 0;
    String body;
};
struct TransactionResult
{
    bool success = false;
    String id;
    String status;
    String error;
    int httpCode = 0;
    String body;
};
struct Balance
{
    bool ok = false;
    String completed = "0";
    String draft = "0";
    int httpCode = 0;
    String body;
};

struct PosChargeResult
{
    bool ok = false;             // overall HTTP and parse success
    bool success = false;        // transaction success (entity + balance present)
    String entityId;             // entity id returned
    String entityName;           // entity name returned
    double balanceCompleted = 0; // completed balance after charge
    double balanceDraft = 0;     // draft balance after charge
    int httpCode = 0;
    String body;  // raw body for diagnostics
    String error; // parse / HTTP error detail
};

class ApiClient
{
public:
    ApiClient(ILogger &logger) : _logger(logger) {}

    bool begin() { return true; }
    // (Legacy methods fetchTokenByCardHash/getMe/createTransaction/getBalance removed after migration to posCharge)

    // New single-call POS charge endpoint: POST /pos/transaction/by-card
    // Request: {card_hash, amount, currency, to_entity_id}
    // Response: { entity: EntitySchema, balance: BalanceSchema }
    PosChargeResult posCharge(const String &cardHash, double amount, const String &currency, const String &toEntityId)
    {
        PosChargeResult r;
        HTTPClient http;
        String url = String(API_BASE) + "/pos/charge/by-card";
        bool isHttps = url.startsWith(F("https://"));

        auto perform = [&](auto &client)
        {
            if (http.begin(client, url))
            {
                http.addHeader("Content-Type", "application/json");
                JsonDocument doc; // small request
                doc.clear();
                doc["card_hash"] = cardHash;
                doc["amount"] = amount;
                doc["currency"] = currency;
                doc["to_entity_id"] = toEntityId.toInt();
                String payload;
                serializeJson(doc, payload);
#ifdef API_HTTP_DEBUG
                _logger.info(String(F("HTTP POST ")) + url + F(" payload=") + payload);
#endif
                int code = http.POST(payload);
                r.httpCode = code;
                if (code == 200)
                {
                    r.body = http.getString();
                    parsePosChargeBody(r.body, currency, r);
                }
                else
                {
                    r.error = String("HTTP ") + code;
                    r.body = http.getString();
#ifdef API_HTTP_DEBUG
                    if (r.body.length())
                        _logger.warn(String(F("posCharge error body=")) + r.body);
#endif
                }
                http.end();
            }
            else
            {
                r.error = F("http.begin posCharge failed");
                _logger.error(r.error);
            }
        };

        if (isHttps)
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
        return r;
    }

private:
    // ---- Parsing helpers to keep HTTP flow clean ----
    static String toStringSafe(JsonVariant v)
    {
        return v.isNull() ? String() : v.as<String>(); // ArduinoJson handles numeric -> String
    }

    static double toNumber(JsonVariant v)
    {
        return v.isNull() ? 0.0 : v.as<double>(); // Handles int/long/double/"123.4"
    }

    double extractCurrency(JsonObject section, const String &currency)
    {
        if (!section)
            return 0.0;

        JsonVariant direct = section[currency];
        if (!direct.isNull())
            return toNumber(direct);

        for (JsonPair kv : section)
        {
            if (String(kv.key().c_str()).equalsIgnoreCase(currency))
                return toNumber(kv.value());
        }
        for (JsonPair kv : section)
        {
#ifdef API_HTTP_DEBUG
            _logger.warn(String(F("balance fallback using key ")) + kv.key().c_str());
#endif
            return toNumber(kv.value());
        }
        return 0.0;
    }

    void parsePosChargeBody(const String &body, const String &currency, PosChargeResult &r)
    {
        StaticJsonDocument<4096> resp; // sized for entity + balance objects
        auto err = deserializeJson(resp, body);
        if (err)
        {
            r.error = String(F("JSON err: ")) + err.c_str();
            _logger.error(r.error);
            return;
        }

        JsonObject ent = resp["entity"].as<JsonObject>();
        JsonObject bal = resp["balance"].as<JsonObject>();

        if (ent)
        {
            r.entityId = toStringSafe(ent["id"]);
            r.entityName = toStringSafe(ent["name"]);
        }

        if (bal)
        {
            r.balanceCompleted = extractCurrency(bal["completed"].as<JsonObject>(), currency);
            r.balanceDraft = extractCurrency(bal["draft"].as<JsonObject>(), currency);
        }

        r.ok = true;
        r.success = r.entityId.length() > 0;
        if (!r.success)
        {
            r.error = F("missing entity in response");
        }
    }

    ILogger &_logger;
};
