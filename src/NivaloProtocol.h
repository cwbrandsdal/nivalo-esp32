#ifndef NIVALO_PROTOCOL_H
#define NIVALO_PROTOCOL_H

#include <Arduino.h>

class NivaloProtocol
{
public:
    void beginClock(const char *primaryNtp, const char *secondaryNtp);
    bool timeValid() const;
    String timestamp() const;
    String messageId() const;
};

#endif
