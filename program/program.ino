#include <SPI.h>
#include <WiFiNINA.h>
#include <stdlib.h>
#include <ArduinoJson.h>

#include "arduino_secrets.h"

char ssid[] = SECRET_SSID;        // your network SSID (name)
char pass[] = SECRET_PASS;    // your network password (use for WPA, or use as key for WEP)
int status = WL_IDLE_STATUS;     // the Wifi radio's status

char server[] = "lazy-garden-api.onrender.com";    // name address for Arduino (using DNS)
String deviceId = "67bfb57423820c7ddca91392";

WiFiClient client;

// Flags and responses from API
bool isIrrigating = false;

bool waitingForGetDeviceResponse = false;
String getDeviceResponse = "";

// Irrigation configuration
long activationSeconds = 120;
long irrigateSeconds = 120;
long configurationCheckSeconds = 3600;

// Counter
long activationSecondsCounter = 0;
long irrigationSecondsCounter = 0;
long configurationCheckCounter = 0;

#define WIFI_PIN 0
#define OPEN_VALVE_PIN 1
#define VALVE_PIN 3
#define TICK_PIN 2

void setup() {
  //Serial.begin(9600);
  //while (!Serial);

  pinMode(WIFI_PIN, OUTPUT);
  pinMode(OPEN_VALVE_PIN, OUTPUT);
  pinMode(TICK_PIN, OUTPUT);
  pinMode(VALVE_PIN, OUTPUT);

  digitalWrite(TICK_PIN, HIGH);

  reconnectToWifi();

  Serial.println("You're connected to the network");

  getDevice();
}

void reconnectToWifi() {
  status = WiFi.status();

  digitalWrite(WIFI_PIN, LOW);

  while(status != WL_CONNECTED) {
    Serial.print("Attempting to connect to network: ");
    Serial.println(ssid);

    // Connect to WPA/WPA2 network
    status = WiFi.begin(ssid, pass);

    Serial.println("Waiting for connection");

    // Wait for connection
    delay(10000);
  }

  digitalWrite(WIFI_PIN, HIGH);
}

void checkForIrrigationEnablement() {
  if (activationSecondsCounter == activationSeconds) {
    openValve();
  }
}
void checkForIrrigationDisablement() {
  if (irrigationSecondsCounter >= irrigateSeconds) {
    closeValve("Irrigation stopped", true);
  }
}
bool checkForConfigurationCheck() {
  if (configurationCheckCounter >= configurationCheckSeconds) {
    configurationCheckCounter = 0;
    getDevice();
    return true;
  }
  return false;
}

void loop() {
  // Read API response
  if (waitingForGetDeviceResponse) {
    if (!client.connected()) {
      Serial.println("No valid response from get device, retrying");
      getDevice();
      return;
    }

    readGetDevice();

  } else {
    digitalWrite(TICK_PIN, HIGH);
    delay(500);
    digitalWrite(TICK_PIN, LOW);
    delay(500);

    ++configurationCheckCounter;
    if (checkForConfigurationCheck()) {
      return;
    }

    // If its not irrigating, then count to the activation
    if (!isIrrigating) {
      ++activationSecondsCounter;
      Serial.println(activationSecondsCounter);
      checkForIrrigationEnablement();
    } else {
      // Activate the irrigation counter
      ++irrigationSecondsCounter;
      Serial.println(irrigationSecondsCounter);
      checkForIrrigationDisablement();
    }

    resetActivationSecondsCounterOnMidnight();
  }
}

void resetActivationSecondsCounterOnMidnight() {
  if (activationSecondsCounter >= 86400) { // 86400 = 24:00:00
    activationSecondsCounter = 0;
    Serial.println("Activation seconds counter resetted to 0 due to midnight");
  }
}

void getDevice() {
  reconnectToWifi();

  Serial.println("Connecting to the API");
  getDeviceResponse = "";
  waitingForGetDeviceResponse = false;

  if (client.connected()) {
    client.stop();
  }

  if (client.connectSSL(server, 443)) {
    Serial.println("Connected to server");
    // Make the HTTP request
    client.println("GET /api/v1/device/" + deviceId + " HTTP/1.1");
    client.println("Host: lazy-garden-api.onrender.com");
    client.println("Connection: close");
    client.println();
    Serial.println("Get Device Request sent");

    waitingForGetDeviceResponse = true; // Tell arduino to catch the data
  } else {
    Serial.println("Cannot connect to the server, retrying");
    client.stop();
    waitingForGetDeviceResponse = false;
    getDevice();
  }
}

void readGetDevice() {
  if (client.available()) {
    //char c = client.read();
    
    // Just process line by lines
    //if ((int) c == 10 || (int) c == 13) {

      //Serial.println(getDeviceResponse);

      getDeviceResponse = client.readStringUntil('\n');

      if (waitingForGetDeviceResponse && getDeviceResponse.indexOf("HTTP/1.1") > -1 && !isRequestOk(getDeviceResponse)) {
        // Wrong requests causes to stop fetching the configuration
        retryGetDevice(getDeviceResponse);
        return;
      }

      // Test if parsing succeeds.
      if (getDeviceResponse.indexOf("{") > -1) { // Ensure that the line contains a JSON object
          //Serial.print("Get Device Response: ");
          //Serial.println(getDeviceResponse);

        JsonDocument doc;
        
        // Deserialize the JSON document
        DeserializationError error = deserializeJson(doc, getDeviceResponse);

        if (error) {
          // Wrong JSON
          retryGetDevice(getDeviceResponse);
          return;
        }

        waitingForGetDeviceResponse = false;

        updateConfiguration(doc);

      } else if (getDeviceResponse.indexOf("sync") > -1) {

        Serial.println(getDeviceResponse);
        Serial.println(getDeviceResponse.substring(6));

        String secondsSinceMidnight = getDeviceResponse.substring(6);
        activationSecondsCounter = secondsSinceMidnight.toInt();

        Serial.println("Synced activation seconds counter: " + String(activationSecondsCounter));
      }

      getDeviceResponse = "";
    //}

    //getDeviceResponse += c;
  }
}

void retryGetDevice(String response) {
  waitingForGetDeviceResponse = false;
  client.stop();
  Serial.println("Cannot get the device configuration due to " + response + " retrying");
  delay(5000);
  getDevice();
}

bool isRequestOk(String responseCodeLine) {
  return responseCodeLine.indexOf("200 OK") > -1;
}

void updateConfiguration(JsonDocument serverDeviceDoc) {
    long serverActivationSeconds = serverDeviceDoc["activationSeconds"].as<long>();
    long serverIrrigateSeconds = serverDeviceDoc["irrigateSeconds"].as<long>();
    String updateMessage = "";

    if (serverActivationSeconds != activationSeconds) {
      updateMessage += "Activation seconds updated from " + String(activationSeconds) + "s to " + String(serverActivationSeconds) + "s. ";
      activationSeconds = serverActivationSeconds;
    }

    if (serverIrrigateSeconds != irrigateSeconds) {
      updateMessage += "Irrigate seconds updated from " + String(irrigateSeconds) + "s to " + String(serverIrrigateSeconds) + "s. ";
      irrigateSeconds = serverIrrigateSeconds;
    }

    if (updateMessage != "") {
      closeValve(updateMessage, true);
    }
}

void closeValve(String message, bool notifyAPI) {
  irrigationSecondsCounter = 0;
  isIrrigating = false;
  digitalWrite(OPEN_VALVE_PIN, LOW);
  digitalWrite(VALVE_PIN, LOW);
}

void openValve() {
  digitalWrite(OPEN_VALVE_PIN, HIGH);
  digitalWrite(VALVE_PIN, HIGH);
  isIrrigating = true;
}

/*void printData() {
  Serial.println("Board Information:");
  // print your board's IP address:
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);

  Serial.println();
  Serial.println("Network Information:");
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  // print the received signal strength:
  long rssi = WiFi.RSSI();
  Serial.print("signal strength (RSSI):");
  Serial.println(rssi);

}*/