#ifndef NIVALO_DEVICE_H
#define NIVALO_DEVICE_H

#if (ARDUINO >= 100)
#include "Arduino.h"
#else
#include "WProgram.h"
#endif

#include "NivaloLinkSpiTransport.h"

#ifndef NIVALO_HAS_SECONDARY_MCU
#define NIVALO_HAS_SECONDARY_MCU 0
#endif

typedef enum
{
	NIVALO_STATUS_INVALID = -1,		   // -1
	NIVALO_STATUS_SUCCESS = 0,		   // 0
	NIVALO_STATUS_OUT_OF_MEMORY,	   // 1
	NIVALO_STATUS_TIMEOUT,			   // 2
	NIVALO_STATUS_UNEXPECTED_PARAM,	   // 3
	NIVALO_STATUS_UNEXPECTED_RESPONSE, // 4
	NIVALO_STATUS_NO_RESPONSE,		   // 5
	NIVALO_STATUS_DEREGISTERED		   // 6
} nivalo_status_t;

typedef enum
{
	NIVALO_STATUS_GREEN,	
	NIVALO_STATUS_PULSE_BLUE,		  
	NIVALO_STATUS_RED,	
	NIVALO_STATUS_YELLOW
} NivaloStatusColor;


class NivaloDevice
{
public:
    //  Constructor
    NivaloDevice();

    boolean begin(unsigned long baud = 115200);
    boolean begin(HardwareSerial &hardSerial, unsigned long baud = 115200);
    boolean beginMqtt(
        const char *deviceId,
        const char *clientId,
        const char *username,
        const char *password,
        const char *host,
        uint16_t port,
        const char *firmwareVersion,
        const char *hardwareName,
        String macAddress);
    void setColor(NivaloStatusColor color);

    size_t publish(const char *eventName, const char *eventData);
    size_t publishTelemetry(const char *name, const char *value, const char *unit = NULL);
    size_t publishEvent(const char *name, const char *data, const char *severity = "info");
    size_t publishAvailability(const char *status, const char *reason = NULL);

    void loop();

private:
    String _deviceId;
    String _clientId;
    String _username;
    String _password;
    String _mqttHost;
    String _firmwareVersion;
    String _hardwareName;
    uint16_t _mqttPort;
    String _macAddr;
    char _telemetryTopic[150];
    char _stateTopic[150];
    char _eventsTopic[150];
    char _definitionsTopic[150];
    char _availabilityTopic[150];
    char _commandsTopic[150];
    char _ackTopicPrefix[150];

    HardwareSerial *_hardSerial;

    unsigned long _baud;

    nivalo_status_t init(unsigned long baud = 115200);

    void mqttCallback(char *topic, byte *message, unsigned int length);
    void mqttReconnect();
    void drainQueuedReports();
    void drainNivaloLink(bool forcePoll = false);
    void handleNivaloLinkFrame(const NivaloLinkReceivedFrame &frame);
    void publishNivaloLinkHeartbeat(const char *payload);
    void publishCommandAck(const char *commandId, const char *status, const char *message = NULL, float progress = -1);
    String getCommandIdFromTopic(const char *topic);
    String createMessageId();
    String createTimestamp();

    // UART Functions
    size_t hwPrint(const char *s);
    size_t hwWrite(const char c);
    int readAvailable(char *inString);
    char readChar(void);
    byte readByte(void);
    int hwAvailable(void);
    void beginSerial(unsigned long baud);
    void setTimeout(unsigned long timeout);
    bool find(char *target);
};

#endif
