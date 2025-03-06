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
long configurationCheckSeconds = 120;

// Counter
long activationSecondsCounter = 0;
long irrigationSecondsCounter = 0;
long configurationCheckCounter = 0;

void resetActionCounters() {
  activationSecondsCounter = 0;
  irrigationSecondsCounter = 0;
  Serial.println("Action counters resetted");
}

void setup() {
  Serial.begin(9600);
  while (!Serial);

  reconnectToWifi();

  Serial.println("You're connected to the network");

  closeValve("Irrigation stopped", false);
  getDevice();
}

void reconnectToWifi() {
  status = WiFi.status();

  while(status != WL_CONNECTED) {
    Serial.print("Attempting to connect to network: ");
    Serial.println(ssid);

    // Connect to WPA/WPA2 network
    status = WiFi.begin(ssid, pass);

    Serial.println("Waiting for connection");

    // Wait for connection
    delay(10000);
  }
}

void checkForIrrigationEnablement() {
  if (activationSecondsCounter >= activationSeconds) {
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
    delay(1000);

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
  }

  // if the server's disconnected, stop the client:
  /*if (!client.connected()) {
    Serial.println();
    Serial.println("disconnecting.");
    client.stop();

    // Turn off all API response waiting flags
    waitingForGetDeviceResponse = false;

    // do nothing forevermore:
    //for(;;)
    //  ;
  }*/
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
    char c = client.read();
    
    // Just process line by lines
    if ((int) c == 10 || (int) c == 13) {

      Serial.println(getDeviceResponse);

      if (waitingForGetDeviceResponse && getDeviceResponse.indexOf("HTTP/1.1") > -1 && !isRequestOk(getDeviceResponse)) {
        // Wrong requests causes to stop fetching the configuration
        waitingForGetDeviceResponse = false;
        client.stop();
        Serial.println("Cannot get the device configuration due to " + getDeviceResponse + " retrying");
        getDevice();
        return;
      }

      JsonDocument doc;
      
      // Deserialize the JSON document
      DeserializationError error = deserializeJson(doc, getDeviceResponse);

      // Test if parsing succeeds.
      if (error) {
        //Serial.print(F("deserializeJson() failed: "));
        //Serial.println(error.f_str());
      } else if (getDeviceResponse.indexOf("{") > -1) { // Ensure that the line contains a JSON object
        if (waitingForGetDeviceResponse) {
          Serial.print("Get Device Response: ");
          Serial.println(getDeviceResponse);

          waitingForGetDeviceResponse = false;

          updateConfiguration(doc);
        }
      }

      getDeviceResponse = "";
    }

    getDeviceResponse += c;
  }
}

bool isRequestOk(String responseCodeLine) {
  return responseCodeLine.indexOf("200") > -1;
}

void updateConfiguration(JsonDocument serverDeviceDoc) {
    long serverActivationSeconds = serverDeviceDoc["activationSeconds"].as<long>();
    long serverIrrigateSeconds = serverDeviceDoc["irrigateSeconds"].as<long>();
    String updateMessage = "";

    if (serverActivationSeconds != activationSeconds) {
      updateMessage += "Activation seconds updated from " + String(activationSeconds) + "s to " + String(serverActivationSeconds) + "s. ";
      activationSeconds = serverActivationSeconds;
      resetActionCounters();
    }

    if (serverIrrigateSeconds != irrigateSeconds) {
      updateMessage += "Irrigate seconds updated from " + String(irrigateSeconds) + "s to " + String(serverIrrigateSeconds) + "s. ";
      irrigateSeconds = serverIrrigateSeconds;
      resetActionCounters();
    }

    if (updateMessage != "") {
      closeValve(updateMessage, false);
    }
}

void closeValve(String message, bool notifyAPI) {
  // TODO: Send signal to close the water valve
  resetActionCounters();
  isIrrigating = false;
   
  // Update device using the API
  if (notifyAPI) {
    patchLastUpdate(message, "CLOSED");
  }
}

void openValve() {
  // Update device using the API
  patchLastUpdate("Irrigating your plants", "IRRIGATING");

  // TODO: Send signal to open the water valve
  resetActionCounters();
  isIrrigating = true;
}

void patchLastUpdate(String message, String state) {
  reconnectToWifi();

  if (client.connected()) {
    client.stop();
  }

  Serial.println("Updating device with message: " + message + " and state: " + state);
  // TODO: Call API to patch error message and add last update date

  if (client.connectSSL(server, 443)) {
    String body = "{\"error\": \"" + message + "\",\"state\": \"" + state + "\"}";

    Serial.println("Connected to server");
    // Make the HTTP request
    client.println("PATCH /api/v1/device/" + deviceId + " HTTP/1.1");
    client.println("Host: lazy-garden-api.onrender.com");
    client.println("Connection: close");
    client.println("Content-Type: application/json");
    client.print("Content-Length: ");
    client.println(body.length());
    client.println();
    client.println(body);
    Serial.println("Patch Device Request sent with body " + body);
  } else {
    Serial.println("Cannot connect to the server");
  }
}

void printData() {
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

}