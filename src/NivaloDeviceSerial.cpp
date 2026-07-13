#include "NivaloDevice.h"

namespace
{
static constexpr size_t Stm32UartRxBufferSize = 2048U;
}

nivalo_status_t NivaloDevice::init(unsigned long baud)
{
    nivalo_status_t err;

    beginSerial(baud); // Begin serial

    _baud = baud;

    return NIVALO_STATUS_SUCCESS;
}

size_t NivaloDevice::hwPrint(const char *s)
{
    if (_hardSerial != NULL)
    {
        return _hardSerial->print(s);
    }

    return (size_t)0;
}

size_t NivaloDevice::hwWrite(const char c)
{
    if (_hardSerial != NULL)
    {
        return _hardSerial->write(c);
    }

    return (size_t)0;
}

int NivaloDevice::readAvailable(char *inString)
{
    int len = 0;

    if (_hardSerial != NULL)
    {
        while (_hardSerial->available())
        {
            char c = (char)_hardSerial->read();
            if (inString != NULL)
            {
                inString[len++] = c;
            }
        }
        if (inString != NULL)
        {
            inString[len] = 0;
        }
    }

    return len;
}

char NivaloDevice::readChar(void)
{
    char ret;

    if (_hardSerial != NULL)
    {
        ret = (char)_hardSerial->read();
    }

    return ret;
}

byte NivaloDevice::readByte(void)
{
    byte ret;

    if (_hardSerial != NULL)
    {
        ret = (byte)_hardSerial->read();
    }

    return ret;
}

int NivaloDevice::hwAvailable(void)
{
    if (_hardSerial != NULL)
    {
        return _hardSerial->available();
    }

    return -1;
}

void NivaloDevice::beginSerial(unsigned long baud)
{
    if (_hardSerial != NULL)
    {
#if defined(ARDUINO_ARCH_ESP32)
        _hardSerial->setRxBufferSize(Stm32UartRxBufferSize);
        _hardSerial->begin(baud, SERIAL_8N1, RX, TX);
#else
        _hardSerial->begin(baud);
#endif
        _hardSerial->setTimeout(250);
    }

    delay(100);
}

void NivaloDevice::setTimeout(unsigned long timeout)
{
    if (_hardSerial != NULL)
    {
        _hardSerial->setTimeout(timeout);
    }
}

bool NivaloDevice::find(char *target)
{
    bool found = false;
    if (_hardSerial != NULL)
    {
        found = _hardSerial->find(target);
    }

    return found;
}

