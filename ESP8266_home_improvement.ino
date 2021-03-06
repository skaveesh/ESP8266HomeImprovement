#include "Arduino.h"
#include <IRremote.h>  //including infrared remote header file     
#include <ESP8266WiFi.h>
#include <ESP8266Ping.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

const char* ssid     = "ABCD"; //Wi-Fi SSID
const char* password = "ABCD"; //Wi-Fi Password

//Only used if using Static IP - IP of NodeMCU
IPAddress ip(192, 168, 1, 5); //IP
IPAddress gateway(192, 168, 1, 1);//DNS and Gateway
IPAddress netmask(255, 255, 255, 0); //Netmask
IPAddress primaryDNS(192, 168, 1, 4);   //optional
IPAddress secondaryDNS(8, 8, 8, 8); //optional

//Server IP of Plex client host machine
const char* host = "192.168.1.3";
const int httpPort = 8998; //plex port

//IR receiver
int RECV_PIN = 4;
IRrecv irrecv(RECV_PIN);
decode_results results;

//relay 1
const int roomLightPin = 0;
bool roomLightRelayOn = false;

//relay 2
const int bluetoothPin = 5;
bool bluetoothPinRelayOn = false;

//web server
ESP8266WebServer server(80);

// relay on and off on single and long press
long int lastExecutedIRCommand = 0;
unsigned long startTime = 0;
unsigned long currentTime = 0;
unsigned long elapsedTime = 0;
bool executed = false;

//RPi shutdown dispatcher variables
static const unsigned long RPI_REFRESH_INTERVAL = 60000*5; // ms
static unsigned long lastRefreshTimeOfRPi = 0;
IPAddress rPiHost(192,168,1,4);

void setup()
{
  Serial.begin(115200);
  irrecv.enableIRIn(); // Start the receiver

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(roomLightPin, OUTPUT);
  pinMode(bluetoothPin, OUTPUT);

  //turning off relays
  digitalWrite(roomLightPin, HIGH);
  digitalWrite(bluetoothPin, HIGH);

  delay(10);

  // start by connecting to a WiFi network
  Serial.println();
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.config(ip, gateway, netmask, primaryDNS, secondaryDNS); //Only used if using Static IP
  WiFi.begin(ssid, password); //Connecting to the network

  while (WiFi.status() != WL_CONNECTED) { //Wait till connects
    digitalWrite(LED_BUILTIN, LOW);
    delay(300);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(300);
    Serial.print(".");
  }

  if (MDNS.begin("esp8266")) {
    Serial.println("MDNS responder started");
  }

  // Set server routing
  restServerRouting();
  // Set not found response
  server.onNotFound(handleNotFound);
  // Start server
  server.begin();
}

void loop()
{
  if (irrecv.decode(&results))
  {

    switch (results.value) {
      case 0X5EA103FD:
        previous();
        ledIndicateOnSignalReceive();
        break;
      case 0X5EA1837D:
        next();
        ledIndicateOnSignalReceive();
        break;
      case 0X5EA143BD:
        playPause();
        ledIndicateOnSignalReceive();
        break;
      case 0X5EA1C33D:
        pauseAndMinimize();
        ledIndicateOnSignalReceive();
        break;
      case 0X2FD9867:
        // turning on relay 1 on single press
        // turning on relay 2 on long press
        lastExecutedIRCommand = results.value; // 50174055
        startTime = millis();
        executed = false;
        digitalWrite(LED_BUILTIN, LOW); // indicate that signal received
        break;
      case 0XFFFFFFFF:
        {
          elapsedTime = (millis() - startTime) / 100;

          if (!executed && (elapsedTime == 19 || elapsedTime == 20 || elapsedTime == 21) && lastExecutedIRCommand  == 50174055) {
            Serial.println("Executing 0X2FD9867 on long press");

            // Executing Bluetooth relay
            ledIndicateOnSignalReceive();

            if (bluetoothPinRelayOn) {
              digitalWrite(bluetoothPin, HIGH);
              bluetoothPinRelayOn = false;
            } else {
              digitalWrite(bluetoothPin, LOW);
              bluetoothPinRelayOn = true;
            }

            executed = true;
          }

          break;
        }
      default:
        Serial.print("other IR code long -  ");
        Serial.print(results.value);
        Serial.print(", HEX -  ");
        Serial.print(results.value, HEX);
        Serial.println();
        break;
    }

    irrecv.resume(); // Receive the next value
  }

  elapsedTime = (millis() - startTime) / 100;

  if (!executed && elapsedTime > 21 && lastExecutedIRCommand  == 50174055) {
    Serial.println("Executing 0X2FD9867 on single press");

    // Executing room light relay
    ledIndicateOnSignalReceive();
    if (roomLightRelayOn) {
      digitalWrite(roomLightPin, HIGH);
      roomLightRelayOn = false;
    } else {
      digitalWrite(roomLightPin, LOW);
      roomLightRelayOn = true;
    }

    executed = true;
  }

  //RPI shutdown dispatcher
  if (millis() - lastRefreshTimeOfRPi >= RPI_REFRESH_INTERVAL)
  {
    lastRefreshTimeOfRPi += RPI_REFRESH_INTERVAL;
    dispatchShutdown();
  }

  server.handleClient();
  delay(150);
}

void previous() {
  connectClientGET("/plex?action=back");
}

void next() {
  connectClientGET("/plex?action=forward");
}

void playPause() {
  connectClientGET("/plex?action=playpause");
}

void pauseAndMinimize() {
  connectClientGET("/plex?action=pausemin");
}

void connectClientGET(String url) {

  Serial.print("connecting to ");
  Serial.println(host);

  WiFiClient client; //Client to handle TCP Connection

  if (!client.connect(host, httpPort)) { //Connect to server using port httpPort
    Serial.println("connection failed");
    return;
  }

  // This will send the request to the server
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "Connection: close\r\n\r\n");
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 25000) { //Try to fetch response for 25 seconds
      Serial.println(">>> Client Timeout !");
      client.stop();
      return;
    }
  }

  //not reading output from the server
  Serial.println();
  Serial.println("closing connection");
  client.stop(); //Close Connection
}

// Serving room lights relay on
void getRoomLightRelayOn() {
  if (!roomLightRelayOn) {
    ledIndicateOnSignalReceive();
    digitalWrite(roomLightPin, LOW);
    roomLightRelayOn = true;
    server.send(200, "text/json", "{\"roomLightRelayOn\": true}");
  } else {
    server.send(200, "text/json", "{\"roomLightRelayOn\": true}");
  }
}

// Serving room lights relay off
void getRoomLightRelayOff() {
  if (roomLightRelayOn) {
    ledIndicateOnSignalReceive();
    digitalWrite(roomLightPin, HIGH);
    roomLightRelayOn = false;
    server.send(200, "text/json", "{\"roomLightRelayOn\": false}");
  } else {
    server.send(200, "text/json", "{\"roomLightRelayOn\": false}");
  }
}

// Serving room lights relay on and off
void getRoomLightRelaySwitch() {
  ledIndicateOnSignalReceive();
  if (roomLightRelayOn) {
    digitalWrite(roomLightPin, HIGH);
    roomLightRelayOn = false;
    server.send(200, "text/json", "{\"roomLightRelayOn\": false}");
  } else {
    digitalWrite(roomLightPin, LOW);
    roomLightRelayOn = true;
    server.send(200, "text/json", "{\"roomLightRelayOn\": true}");
  }
}


// Define routing
void restServerRouting() {
  server.on("/", HTTP_GET, []() {
    server.send(200, F("text/html"),
                F("ESP8266 Web Server"));
  });
  server.on(F("/roomLightRelayOn"), HTTP_GET, getRoomLightRelayOn);
  server.on(F("/roomLightRelayOff"), HTTP_GET, getRoomLightRelayOff);
  server.on(F("/roomLightRelaySwitch"), HTTP_GET, getRoomLightRelaySwitch);
}

// Manage not found
void handleNotFound() {
  String message = "404 Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

//shutdown dispatcher for RPi
void dispatchShutdown() {
  bool rpiPingStatus = Ping.ping(rPiHost, 4);
  Serial.println("RPi ping");
  Serial.print(rpiPingStatus);
  Serial.println();
}

// Builtin LED Signal
void ledIndicateOnSignalReceive() {
  digitalWrite(LED_BUILTIN, LOW);
  delay(80);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(80);
  digitalWrite(LED_BUILTIN, LOW);
  delay(80);
  digitalWrite(LED_BUILTIN, HIGH);
}
