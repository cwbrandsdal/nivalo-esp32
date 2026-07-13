#include "NivaloStatusPixel.h"

NivaloStatusPixel::NivaloStatusPixel()
#if NIVALO_STATUS_NEOPIXEL_ENABLED
    : _strip(1, NIVALO_STATUS_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800)
#endif
{
}

void NivaloStatusPixel::begin()
{
#if NIVALO_STATUS_NEOPIXEL_ENABLED
    _strip.begin();
    _strip.setBrightness(_brightness);
    _strip.setPixelColor(0, _strip.Color(0, 0, 255));
    _strip.show();
#endif
}

void NivaloStatusPixel::set(uint8_t red, uint8_t green, uint8_t blue)
{
#if NIVALO_STATUS_NEOPIXEL_ENABLED
    _strip.setBrightness(_brightness);
    _strip.setPixelColor(0, _strip.Color(red, green, blue));
    _strip.show();
#else
    (void)red;
    (void)green;
    (void)blue;
#endif
}

void NivaloStatusPixel::pulse()
{
#if NIVALO_STATUS_NEOPIXEL_ENABLED
    static constexpr uint8_t loopCountSteps = 30U;
    if (_loopCount++ < loopCountSteps)
    {
        return;
    }
    _loopCount = 0U;

    if (_increasing && _brightness < _maxBrightness)
    {
        ++_brightness;
    }
    else if (_increasing)
    {
        _increasing = false;
        --_brightness;
    }
    else if (_brightness > 1U)
    {
        --_brightness;
    }
    else
    {
        _increasing = true;
        ++_brightness;
    }
    set(52, 204, 235);
#endif
}

void NivaloStatusPixel::rainbow(uint8_t waitMs)
{
#if NIVALO_STATUS_NEOPIXEL_ENABLED
    for (uint16_t cycle = 0; cycle < 256U * 5U; ++cycle)
    {
        for (uint16_t pixel = 0; pixel < _strip.numPixels(); ++pixel)
        {
            _strip.setPixelColor(pixel, wheel(((pixel * 256U / _strip.numPixels()) + cycle) & 255U));
        }
        _strip.show();
        delay(waitMs);
    }
#else
    (void)waitMs;
#endif
}

#if NIVALO_STATUS_NEOPIXEL_ENABLED
uint32_t NivaloStatusPixel::wheel(uint8_t position)
{
    position = 255U - position;
    if (position < 85U)
    {
        return _strip.Color(255U - position * 3U, 0, position * 3U);
    }
    if (position < 170U)
    {
        position -= 85U;
        return _strip.Color(0, position * 3U, 255U - position * 3U);
    }
    position -= 170U;
    return _strip.Color(position * 3U, 255U - position * 3U, 0);
}
#endif
