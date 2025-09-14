#pragma once
#include <Arduino.h>
#include "Config.h"

class RelayLock
{
public:
  void begin()
  {
    pinMode(RELAY_PIN, OUTPUT_OPEN_DRAIN); // use open-drain to avoid powering relay coil when off
    digitalWrite(RELAY_PIN, HIGH);         // assume LOW=locked (inactive)
  }

  void open()
  {
    digitalWrite(RELAY_PIN, LOW); // energize
  }

  void close()
  {
    digitalWrite(RELAY_PIN, HIGH);
  }
};
