#pragma once
#include <Arduino.h>
#include "ILogger.h"

enum class PosState
{
    BOOT,
    IDLE,
    CARD_DETECTED,
    TRANSACTION_OK,
    SHOW_BALANCE,
    ERROR_STATE
};

struct PosContext
{
    String cardUID;
    String meId;      // entity id returned from auth lookup
    String payerName; // human-readable entity name for display
    double amount = POS_PRICE;
    String currency = POS_CURRENCY;
    unsigned long stateSince = 0;
    // Optional cached balance fields from /pos/charge response
    bool balancePrefetched = false;
    double balanceCompleted = 0;
    double balanceDraft = 0;
};
