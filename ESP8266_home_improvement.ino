#include "Arduino.h"
#include <IRremote.h>  //including infrared remote header file     
#include <ESP8266WiFi.h>
#include <ESP8266Ping.h>
#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

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

//Night fall server host
const char* nfHost = "192.168.1.4";
const int nfHttpPort = 8082; //night fall port

//Nightfall server response
const String timeEqualOrGreaterThanSix = "{\"isTimeEqualOrGreaterThanSix\": true}";
const String timeNotEqualOrGreaterThanSix = "{\"isTimeEqualOrGreaterThanSix\": false}";

//IR receiver
int RECV_PIN = 4;
IRrecv irrecv(RECV_PIN);
decode_results results;

//relay 1
const int roomLightPin = 0;
bool roomLightRelayOn = false;

//relay 2
const int homeLightPin = 5;
bool homeLightRelayOn = false;

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

//AC relay pin
int acRelayInputPin = 16;
int acRelayState = 0;

int nightFallCounter = 0;

//time client
WiFiUDP ntpUDP;
const long utcOffsetInSeconds = 19800;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);

void setup()
{
  Serial.begin(115200);

//Serial.setDebugOutput(true);
  
  irrecv.enableIRIn(); // Start the receiver

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(roomLightPin, OUTPUT);
  pinMode(homeLightPin, OUTPUT);
  pinMode(acRelayInputPin, INPUT);

  //turning off relays
  digitalWrite(roomLightPin, HIGH);
  digitalWrite(homeLightPin, HIGH);

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
  // Start time client
  timeClient.begin();
}

void loop()
{
  if (irrecv.decode(&results)){

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

            if (homeLightRelayOn) {
              digitalWrite(homeLightPin, HIGH);
              homeLightRelayOn = false;
            } else {
              digitalWrite(homeLightPin, LOW);
              homeLightRelayOn = true;
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

  server.handleClient();

  handleAcRelayAndNightFallServer();

  delay(150);
}

void previous() {
  connectPlexServer("/plex?action=back");
}

void next() {
  connectPlexServer("/plex?action=forward");
}

void playPause() {
  connectPlexServer("/plex?action=playpause");
}

void pauseAndMinimize() {
  connectPlexServer("/plex?action=pausemin");
}

void connectPlexServer(String url) {
  connectClientGET(url, host, httpPort);
}

// Auto relay on only works after 6PM
void handleAcRelayAndNightFallServer() {
  acRelayState = digitalRead(acRelayInputPin);

  if (acRelayState == LOW && getNightfallTimer()){
    timeClient.update();

    if (timeClient.getHours() >= 18){
      if(!roomLightRelayOn) {
        digitalWrite(roomLightPin, LOW);
      }
  
      if (!homeLightRelayOn) {
        digitalWrite(homeLightPin, LOW);
      }
      
    }   
  }

  if(acRelayState == HIGH){
    nightFallCounter = 0;

    if(!roomLightRelayOn) {
      digitalWrite(roomLightPin, HIGH);
    }

    if (!homeLightRelayOn) {
      digitalWrite(homeLightPin, HIGH);
    }
  }  

}

// Since the main loop goes on every 150ms, execute every 5 minutes (150ms x 400 x 5 = 60s x 5 = 5min)
bool getNightfallTimer() {
  if(nightFallCounter > 40*5) {
   nightFallCounter = 0;
  }
  nightFallCounter++;
  return nightFallCounter-1 == 0;
}

String connectClientGET(String url, const char * host, int port) {

  Serial.print("connecting to ");
  Serial.println(host);
   
  WiFiClient client; //Client to handle TCP Connection
  client.setTimeout(30000);
  
  HTTPClient http;

  String payload;

  if (!client.connect(host, port)) { //Connect to server using port httpPort
    Serial.println("Connection Failed");
    return "";
  }

  char serverPath[200];
  sprintf(serverPath, "http://%s:%d%s", host, port, url);

  Serial.print("Http server path: ");
  Serial.println(serverPath);

  http.begin(client, serverPath);  //Specify request destination
 
  int httpResponseCode = http.GET();


  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 25000) { //Try to fetch response for 25 seconds
      Serial.println(">>> Client Timeout !");
      client.stop();
      return "";
    }
  }

  if (httpResponseCode > 0) { //Check the returning code
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
    payload = http.getString();
    Serial.println(payload);
  } else {
    Serial.print("Error code: ");
    Serial.println(httpResponseCode);
  }
  
  http.end();  

  //not reading output from the server
  Serial.println("closing connection");
  client.stop(); //Close Connection

  return payload;
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
