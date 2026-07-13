#ifndef NIVALO_STATUS_PIXEL_H
#define NIVALO_STATUS_PIXEL_H

#include <Arduino.h>

#ifndef NIVALO_STATUS_NEOPIXEL_ENABLED
#define NIVALO_STATUS_NEOPIXEL_ENABLED 1
#endif

#ifndef NIVALO_STATUS_NEOPIXEL_PIN
#define NIVALO_STATUS_NEOPIXEL_PIN 27
#endif

#if NIVALO_STATUS_NEOPIXEL_ENABLED
#include <Adafruit_NeoPixel.h>
#endif

class NivaloStatusPixel
{
public:
    NivaloStatusPixel();
    void begin();
    void set(uint8_t red, uint8_t green, uint8_t blue);
    void pulse();
    void rainbow(uint8_t waitMs);

private:
#if NIVALO_STATUS_NEOPIXEL_ENABLED
    uint32_t wheel(uint8_t position);
    Adafruit_NeoPixel _strip;
    uint8_t _brightness = 10U;
    uint8_t _maxBrightness = 15U;
    uint8_t _loopCount = 0U;
    bool _increasing = true;
#endif
};

#endif
