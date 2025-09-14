#pragma once
#include "ILogger.h"

// Accept any Print (HardwareSerial, USB CDC (HWCDC), SoftwareSerial, etc.)
class LoggerSerial : public ILogger
{
public:
  LoggerSerial() : _out(nullptr) {}
  void begin(Print &out) { _out = &out; }
  void info(const String &msg) override
  {
    if (_out)
    {
      _out->print(F("[INFO] "));
      _out->println(msg);
    }
  }
  void warn(const String &msg) override
  {
    if (_out)
    {
      _out->print(F("[WARN] "));
      _out->println(msg);
    }
  }
  void error(const String &msg) override
  {
    if (_out)
    {
      _out->print(F("[ERROR] "));
      _out->println(msg);
    }
  }

private:
  Print *_out;
};
