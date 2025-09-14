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
    String cardHash;
    String meId;      // entity id returned from POS charge (payer)
    String payerName; // human-readable entity name for display
    double amount = POS_PRICE;
    String currency = POS_CURRENCY;
    unsigned long stateSince = 0;
    // Optional cached balance from POS charge response (avoids extra balance HTTP call)
    bool balancePrefetched = false;
    double balanceCompleted = 0;
    double balanceDraft = 0;
};
