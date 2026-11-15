// Credentials for LvglMqttStatus.  Included once, by the .ino.

// WiFi: used on the JC8012 (no Ethernet), and on a P486 with no cable.
// Required even if your P486 is wired: the same image runs on both boards.
const char* WIFI_SSID     = "YourWiFiSSID";
const char* WIFI_PASSWORD = "YourWiFiPassword";

// MQTT broker (TLS, port 8883)
const char* MQTT_SERVER   = "mqtt.local";
const char* MQTT_USER     = "username";
const char* MQTT_PASSWORD = "password";

// CA certificate for the broker (nullptr skips certificate verification)
const char* CA_CERT = nullptr;

// ArduinoOTA password; empty string disables over-the-network updates
const char* OTA_PASSWORD = "";
