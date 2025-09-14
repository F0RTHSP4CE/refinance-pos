#pragma once
#include <Arduino.h>

class ILogger {
public:
  virtual ~ILogger() = default;
  virtual void info(const String &msg) = 0;
  virtual void warn(const String &msg) = 0;
  virtual void error(const String &msg) = 0;
};
