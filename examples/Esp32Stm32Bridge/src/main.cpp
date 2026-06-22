#include <Arduino.h>
// #include "Adafruit_DAP.h"

// #define SWDIO 12
// #define SWCLK 14
// #define SWRST 4

// // the more the better
// #define BUFSIZE   (16*1024)

// // buffer should be word algined for STM32
// uint8_t buf[BUFSIZE]  __attribute__ ((aligned(4)));

// //create a programming DAP
// Adafruit_DAP_STM32 dap;

// // STM32 auto map 0x00 to 0x08000000, use 0 for simplicity
// #define FLASH_START_ADDR    0

// // Function called when there's an SWD error
// void error(const char *text) {
//   Serial.println(text);
//   while (1);
// }

// // dumping stm32 memory for verification
// void print_memory(uint32_t addr, uint8_t* buffer, uint32_t bufsize)
// {
//   memset(buffer, 0xff, bufsize);
//   dap.dap_read_block(addr, buffer, bufsize);

//   for(uint32_t i=0; i < bufsize; i++)
//   {
//     if (i % 16 == 0) 
//     {
//       if ( i != 0 ) Serial.println();
//       // print offset
//       if ( i < 0x100 ) Serial.print("0");
//       if ( i < 0x10  ) Serial.print("0");
//       Serial.print(i, HEX);
//       Serial.print(": ");
//     }

//     if ( buffer[i] < 0x10 ) Serial.print("0");
//     Serial.print(buffer[i], HEX);
//     Serial.print(" ");
//   }
//   Serial.println();  
// }

// void setup() {
//   pinMode(13, OUTPUT);
//   Serial.begin(115200);
//   while(!Serial) {
//     delay(1);         // will pause the chip until it opens serial console
//   }

//   dap.begin(SWCLK, SWDIO, SWRST, &error);

//   Serial.println("Connecting...");
//   if ( !dap.targetConnect() ) {
//     error(dap.error_message);
//   }

//   char debuggername[100];
//   dap.dap_get_debugger_info(debuggername);
//   Serial.print(debuggername); Serial.print("\n\r");

//   uint32_t dsu_did;
//   if (! dap.select(&dsu_did)) {
//     error("No STM32 device found!");
//   }

//   Serial.print("Found Target\t");
//   Serial.println(dap.target_device.name);
//   Serial.print("Flash size\t");
//   Serial.print(dap.target_device.flash_size / 1024);
//   Serial.println(" KBs");
  
//   uint32_t start_ms, duaration;

//   //------------- Preparing sectors -------------//
//   Serial.print("Preparing ... ");
  
//   start_ms = millis();

//   // preparing flash sector with address = 0, size = Binary size
//   dap.programPrepare(FLASH_START_ADDR, sizeof(buf));
//   duaration = millis()-start_ms;
  
//   Serial.print(" done in ");
//   Serial.print(duaration);
//   Serial.println(" ms");

//   //------------- Programming -------------//
//   Serial.print("Programming & Verifying ");
//   Serial.print(sizeof(buf)/1024);
//   Serial.print(" KBs ...");
  
//   // prepare data
//   for(uint32_t i=0; i<sizeof(buf); i++) buf[i] = i;

//   start_ms = millis();
//   bool verified = dap.programFlash(FLASH_START_ADDR, buf, sizeof(buf), true);
  
//   duaration = millis()-start_ms;
//   Serial.print(" done in ");
//   Serial.print(duaration);
//   Serial.print(" ms, ");

//   Serial.print("Speed ");
//   Serial.print((double) sizeof(buf)/(duaration*1.024) );
//   Serial.println(" KBs/s");

//   Serial.print("Verified: ");
//   if (verified) {
//     Serial.println("matched");
//   }else {
//     Serial.print("mis-matched");
//   }

//   dap.deselect();
//   dap.dap_disconnect();
// }

// void loop() {
//   //blink led on the host to show we're done
//   digitalWrite(13, HIGH);
//   delay(500);
//   digitalWrite(13, LOW);
//   delay(500);
// }


#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <NivaloDevice.h>
#include <FS.h>          // this needs to be first, or it all crashes and burns...
#include <ArduinoJson.h> // https://github.com/bblanchon/ArduinoJson
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <SPIFFS.h>
#include <time.h>
#include "html.h"

#if __has_include("nivalo_config.h")
#include "nivalo_config.h"
#else
#include "nivalo_config.example.h"
#endif

// FORWARD DECLARATION
bool setupSpiffsAndGetSettings();
void formatSpiffs();
void startWifi();
// void setupWebserver();

// Program states
enum State
{
	STATE_LOAD_SETTINGS,
	STATE_START_BLUETOOTH,
	STATE_LOOP
};
State state = STATE_LOAD_SETTINGS;

WiFiClient espClient;

long lastMsg = 0;
char msg[50];
int value = 0;
String macAddr;
String bleName;
int incomingByte = 0; // for incoming serial data
char host[60];
char ssid[60] = NIVALO_WIFI_SSID;
char password[100] = NIVALO_WIFI_PASSWORD;
char deviceId[40] = NIVALO_IOT_DEVICE_ID;
char mqttClientId[80] = NIVALO_IOT_MQTT_CLIENT_ID;
char mqttUsername[80] = NIVALO_IOT_MQTT_USERNAME;
char mqttPassword[120] = NIVALO_IOT_MQTT_PASSWORD;
char mqttHost[80] = NIVALO_IOT_MQTT_HOST;
uint16_t mqttPort = NIVALO_IOT_MQTT_PORT;

NivaloDevice device;

// define the number of bytes you want to access
#define EEPROM_SIZE 1

//WebServer
// WebServer server(80);

void setup(void)
{	
	Serial.begin(115200);
	 
	Serial.print("CPU Freq: ");
    Serial.println(getCpuFrequencyMhz());
 
    setCpuFrequencyMhz(80);
 
    Serial.print("CPU Freq: ");
    Serial.println(getCpuFrequencyMhz());


	device.begin();

	byte mac[6];
	WiFi.macAddress(mac);
	char macText[13];
	snprintf(macText, sizeof(macText), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	macAddr = String(macText);
	bleName = String("Nivalo-") + macAddr;
	
	Serial.println();
	Serial.print("Unique ID: ");
	Serial.print(macAddr);
	Serial.println();
	startWifi();
	// setupWebserver();
	device.beginMqtt(
		deviceId,
		mqttClientId,
		mqttUsername,
		mqttPassword,
		mqttHost,
		mqttPort,
		NIVALO_IOT_FIRMWARE_VERSION,
		NIVALO_IOT_HARDWARE_NAME,
		macAddr);

	//setupSpiffsAndGetSettings();
}

void loop(void)
{
	switch (state)
	{
	
	case STATE_LOOP:
	{

		break;
		}
	case STATE_LOAD_SETTINGS:
break;
	
	default:
		break;
	}
	// server.handleClient();
	device.loop();
	delay(2); //allow the cpu to switch to other tasks

	if (millis() - lastMsg > 3000)
	{
		lastMsg = millis();
	}
}

bool setupSpiffsAndGetSettings()
{
	//SPIFFS.format();
	Serial.println(host);
	//read configuration from FS json
	Serial.println("mounting FS...");

	if (SPIFFS.begin())
	{
		Serial.println("mounted file system");
		if (SPIFFS.exists("/config.json"))
		{
			//file exists, reading and loading
			Serial.println("reading config file");
			File configFile = SPIFFS.open("/config.json");
			if (configFile)
			{
				Serial.println("opened config file");
				size_t size = configFile.size();
				// Allocate a buffer to store contents of the file.
				std::unique_ptr<char[]> buf(new char[size]);

				configFile.readBytes(buf.get(), size);
				DynamicJsonDocument doc(1024);
				auto error = deserializeJson(doc, buf.get());
				serializeJson(doc, Serial);

				if (!error)
				{
					Serial.println("\nparsed json");

					strcpy(host, doc["host"]);
					strcpy(ssid, doc["ssid"]);
					strcpy(password, doc["password"]);
					strcpy(deviceId, doc["deviceId"]);
					strcpy(mqttClientId, doc["mqttClientId"]);
					strcpy(mqttUsername, doc["mqttUsername"]);
					strcpy(mqttPassword, doc["mqttPassword"]);
					strcpy(mqttHost, doc["mqttHost"] | NIVALO_IOT_MQTT_HOST);
					mqttPort = doc["mqttPort"] | NIVALO_IOT_MQTT_PORT;

					return true;
				}
				else
				{
					Serial.println("failed to load json config");
					return false;
				}
			}
		}
		else
		{
			Serial.println("could not find config file");
			return false;
		}
	}
	else
	{
		Serial.println("failed to mount FS");
		return false;
	}

	return false;
}

void formatSpiffs()
{
	SPIFFS.format();
}

void printWifiDiagnostics()
{
	Serial.println();
	Serial.printf("WiFi connection failed, status=%d\n", WiFi.status());
	WiFi.disconnect(false, true);
	delay(500);
	int networkCount = WiFi.scanNetworks(false, true, false, 500);
	Serial.printf("WiFi scan found %d networks\n", networkCount);

	int matches = 0;
	for (int i = 0; i < networkCount; i++)
	{
		Serial.printf("SSID: %s RSSI=%d dBm channel=%d auth=%d\n",
					  WiFi.SSID(i).c_str(),
					  WiFi.RSSI(i),
					  WiFi.channel(i),
					  WiFi.encryptionType(i));

		if (WiFi.SSID(i) == String(ssid))
		{
			matches++;
			Serial.printf("Target SSID visible: RSSI=%d dBm channel=%d auth=%d\n",
						  WiFi.RSSI(i),
						  WiFi.channel(i),
						  WiFi.encryptionType(i));
		}
	}

	if (matches == 0)
	{
		Serial.println("Target SSID was not visible in scan results");
	}

	WiFi.scanDelete();
}

void startWifi()
{
	// Setup WiFi
	//WiFi.mode(WIFI_AP_STA);
	WiFi.persistent(false);
	WiFi.mode(WIFI_STA);
	WiFi.disconnect(true, true);
	delay(500);
	WiFi.begin(ssid, password);

	// Wait for WiFi connection
	Serial.printf("Connecting to WiFi AP: %s", ssid);
	int lastStatus = -1;
	unsigned long attemptStarted = millis();
	while (WiFi.status() != WL_CONNECTED)
	{
		int currentStatus = WiFi.status();
		if (currentStatus != lastStatus)
		{
			Serial.printf(" status=%d", currentStatus);
			lastStatus = currentStatus;
		}

		if (millis() - attemptStarted > 45000)
		{
			printWifiDiagnostics();
			Serial.printf("Retrying WiFi AP: %s", ssid);
			WiFi.disconnect(false, true);
			delay(1000);
			WiFi.begin(ssid, password);
			attemptStarted = millis();
			lastStatus = -1;
		}

		delay(500);
		Serial.print(".");
	}
	Serial.print(WiFi.localIP());
	
	configTime(0, 0, "pool.ntp.org", "time.google.com");
	WiFi.setSleep(true);
}

// void setupWebserver()
// {
// 	// Start multicast DNS for host
// 	MDNS.begin(host);

// 	// Upload form
// 	server.on("/", HTTP_GET, []()
// 			  {
// 				  Serial.println("Serving upload form\n");
// 				  server.sendHeader("Connection", "close");
// 				  server.send(200, "text/html", serverIndex);
// 			  });
// 	// Process received update
// 	server.on(
// 		"/update", HTTP_POST, []()
// 		{
// 			// if this point is reached the upload to the ESP was successful
// 			server.sendHeader("Connection", "close");
// 			server.send(200, "text/plain", "OK");
// 		},
// 		[]() {

// 		});

// 	server.on("/firmware", HTTP_GET, []()
// 			  {
// 				  Serial.println("Serving upload form\n");
// 				  server.sendHeader("Connection", "close");
// 				  server.send(200, "text/html", serverIndex2);
// 			  });

// 	/*handling uploading firmware file */
// 	server.on(
// 		"/firmwareupdate", HTTP_POST, []()
// 		{
// 			server.sendHeader("Connection", "close");
// 			server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
// 			ESP.restart();
// 		},
// 		[]()
// 		{
// 			HTTPUpload &upload = server.upload();
// 			if (upload.status == UPLOAD_FILE_START)
// 			{
// 				Serial.printf("Update: %s\n", upload.filename.c_str());
// 				if (!Update.begin(UPDATE_SIZE_UNKNOWN))
// 				{ //start with max available size
// 					Update.printError(Serial);
// 				}
// 			}
// 			else if (upload.status == UPLOAD_FILE_WRITE)
// 			{
// 				/* flashing firmware to ESP*/
// 				if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
// 				{
// 					Update.printError(Serial);
// 				}
// 			}
// 			else if (upload.status == UPLOAD_FILE_END)
// 			{
// 				if (Update.end(true))
// 				{ //true to set the size to the current progress
// 					Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
// 				}
// 				else
// 				{
// 					Update.printError(Serial);
// 				}
// 			}
// 		});

// 	Serial.print("Setting up server & MDNS... ");
// 	server.begin();
// 	MDNS.addService("http", "tcp", 80);

// 	Serial.printf("done\nOpen http://%s.local or http://", host);
// 	Serial.print(WiFi.localIP());
// 	Serial.println(" in your browser\n");
// }
