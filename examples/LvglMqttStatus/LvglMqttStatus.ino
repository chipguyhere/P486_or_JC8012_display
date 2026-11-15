/*******************************************************************************
 * LVGL MQTT Status — for the JC8012P4A1C or the P486 Panel
 *
 * A network-status screen built on chipguy_MQTT_netclient, and the pattern to
 * copy for a display-centric app that has to run on both boards from one
 * image:
 *
 *   - The P486's Ethernet header is included.  On a P486, NetClient uses the
 *     Ethernet jack.  On a JC8012, which has no Ethernet, the header's
 *     configureEthernet() asks this display library (through a weak hook) and
 *     does nothing — so NetClient falls back to WiFi and the P486's RMII pins,
 *     which are header and battery-sense lines on the JC8012, are left alone.
 *   - The screen shows the board, the transport, the IP address, the MQTT
 *     state, and the time from the broker's unix_time/unix_time clock if it
 *     has one.
 *
 * Being a dashboard, it publishes nothing of its own by default beyond the
 * online-status LWT (see the chipguy MQTT house rules).  A diagnostic
 * publication block — hardware, stats, uptime — is included, commented out.
 *
 * Edit creds.hpp with your WiFi, MQTT and OTA settings.  WiFi credentials are
 * needed even for a wired P486: the same image runs on the JC8012.
 *
 * Requires the following libraries:
 *   - chipguy_P486_or_JC8012_display  (this library)
 *   - chipguy_MQTT_netclient
 *   - lvgl 9
 *
 * Arduino IDE Board Settings (the same for both boards):
 *   - Board: ESP32P4 Dev Module
 *   - PSRAM: Enabled
 *   - Flash Size: 16MB (128Mb)
 *   - Partition Scheme: Custom (uses the partitions.csv in this sketch folder:
 *     two 7.9 MB OTA app slots)
 *   - USB CDC On Boot: Disabled
 ******************************************************************************/

#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

#include <chipguy_MQTT_netclient_P486Panel.h>

// Refuse the default partition scheme — its 1.2 MB app slots fill up and OTA
// starts failing once the sketch grows. The IDE silently reverts to it.
// See Arduino/CLAUDE.md, "Partition scheme".
static constexpr bool fqbnHasOption(const char* s, const char* opt) {
    for (; *s; ++s) {
        const char* a = s; const char* b = opt;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b && (*a == ',' || *a == 0)) return true;
    }
    return false;
}
#ifdef ARDUINO_FQBN
static_assert(!fqbnHasOption(ARDUINO_FQBN, "PartitionScheme=default"),
    "Default partition scheme selected: app slots are too small for OTA growth. "
    "Build with an OTA scheme that has larger app slots, e.g. PartitionScheme=no_fs.");
#endif
#ifdef ARDUINO_PARTITION_default
#error "Partition scheme resolved to 'default' (no PartitionScheme= in the FQBN). Pick an OTA scheme with larger app slots, e.g. PartitionScheme=no_fs."
#endif

#include <esp_ota_ops.h>
#include "creds.hpp"

static lv_obj_t *lblBoard, *lblNet, *lblIp, *lblMqtt, *lblClock;

static lv_obj_t *addRow(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static void buildUi() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(scr, display.height() / 30, 0);

    // Bigger type on the bigger canvas.
    bool big = display.width() >= 700;
    const lv_font_t *title = big ? &lv_font_montserrat_48 : &lv_font_montserrat_28;
    const lv_font_t *body  = big ? &lv_font_montserrat_28 : &lv_font_montserrat_20;

    lblBoard = addRow(scr, title, lv_color_white());
    lblNet   = addRow(scr, body, lv_color_hex(0xA0C0E0));
    lblIp    = addRow(scr, body, lv_color_hex(0xA0C0E0));
    lblMqtt  = addRow(scr, body, lv_color_hex(0xA0C0E0));
    lblClock = addRow(scr, title, lv_color_hex(0xFFD080));

    lv_label_set_text_fmt(lblBoard, "%s  %dx%d", display.boardName(),
                          display.width(), display.height());
    lv_label_set_text_fmt(lblNet, "Starting %s...", NetClient.isEthernet() ? "Ethernet" : "WiFi");
    lv_label_set_text(lblIp, "IP: -");
    lv_label_set_text(lblMqtt, "MQTT: not connected");
    lv_label_set_text(lblClock, "");
}

// Evaluate in the .ino so __FILE__ is the sketch.
static const char* sketchNameFromPath(const char* path) {
    static char name[64];
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = strcspn(base, ".");
    if (n >= sizeof(name)) n = sizeof(name) - 1;
    memcpy(name, base, n);
    name[n] = 0;
    return name;
}

// Diagnostic publications.  A dashboard ships with these off; uncomment the
// calls in setup() and loop() to turn them on.
static void persistHardwareInfo() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    char json[360];
    snprintf(json, sizeof(json),
        "{\"sketchName\":\"%s\",\"sketchSize\":%lu,\"partitionSize\":%lu,"
        "\"flashSize\":%lu,\"psramSize\":%lu,\"boardType\":\"%s\",\"panel\":\"%s\","
        "\"cores\":%u,\"compiledWhen\":\"%s\",\"tlsSecure\":%d}",
        sketchNameFromPath(__FILE__),
        (unsigned long)ESP.getSketchSize(),
        (unsigned long)(running ? running->size : 0),
        (unsigned long)ESP.getFlashChipSize(),
        (unsigned long)ESP.getPsramSize(),
        ARDUINO_BOARD, display.boardName(),
        (unsigned)ESP.getChipCores(),
        __DATE__ " " __TIME__,
        NetClient.isTlsVerified() ? 1 : 0);
    NetClient.persistDaily("%P/hardware", json);
}

static void publishStats() {
    static unsigned long lastStats = 0;
    unsigned long now = millis();
    if (now - lastStats < 20000) return;
    lastStats = now;
    char json[192];
    if (NetClient.isEthernet()) {
        snprintf(json, sizeof(json),
            "{\"uptime\":%lu,\"heap\":%lu,\"minHeap\":%lu,\"psramFree\":%lu,\"link\":%u}",
            now / 1000, (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
            (unsigned long)ESP.getFreePsram(), NetClient.getEthernetLinkSpeed());
    } else {
        snprintf(json, sizeof(json),
            "{\"uptime\":%lu,\"heap\":%lu,\"minHeap\":%lu,\"psramFree\":%lu,\"rssi\":%d}",
            now / 1000, (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
            (unsigned long)ESP.getFreePsram(), NetClient.getWiFiRSSI());
    }
    NetClient.persist("%P/stats", json);
    NetClient.publishf("%P/uptime-seconds", "%lu", now / 1000);
}

void setup() {
    Serial0.begin(115200);

    // Display first.  On a P486 after a warm restart, begin() may reboot once
    // more (its black-panel recovery), and anything before it would run twice;
    // starting the network after it keeps that harmless.
    lv_setup.begin();      // native resolution on either board

    NetClientConfig config;
    configureEthernet(config);     // P486: the Ethernet jack.  JC8012: no-op.
    config.wifiSSID = WIFI_SSID;   // used whenever Ethernet isn't
    config.wifiPassword = WIFI_PASSWORD;

    // Many instances of one project: name each by the MAC (6 hex digits).
    config.otaHostname = "p4panel-%6X";
    config.otaPassword = OTA_PASSWORD;
    config.mqtt.clientId = "p4panel-%X";
    config.topicPrefix = "p4panel/%6X";     // replace with your project's root topic

    config.mqtt.server = MQTT_SERVER;
    config.mqtt.port = 8883;
    config.mqtt.username = MQTT_USER;
    config.mqtt.password = MQTT_PASSWORD;
    config.mqtt.caCert = CA_CERT;
    config.mqtt.lastWillTopic = "%P/online-status";
    config.mqtt.onlineMessage = "1";
    config.mqtt.lastWillMessage = "0";

    // NetClient's watchdog reboots the board if no MQTT message arrives for
    // this long.  A dashboard that publishes nothing gets nothing back unless
    // the broker has something it subscribes to updating regularly (the
    // unix_time clock, or the stats self-subscription below), so the watchdog
    // is off here.  Turn it back on (e.g. 120) once one of those is in place.
    config.watchdogTimeoutSec = 0;

    if (!NetClient.begin(config)) {
        Serial0.println("Failed to start NetClient!");
    }
    Serial0.printf("%s: NetClient started in %s mode\n", display.boardName(),
                   NetClient.isEthernet() ? "Ethernet" : "WiFi");

    NetClient.autoSubscribe("%P/control/#");
    NetClient.autoSubscribe("unix_time/unix_time");
    // persistHardwareInfo();                 // diagnostics: uncomment to publish
    // NetClient.autoSubscribe("%P/stats");   // and to feed the watchdog with it

    buildUi();
}

static void handleEvent(const NetClientEvent &client_event) {
    switch (client_event.type) {
        case EventType::NETWORK_CONNECTED:
            lv_label_set_text_fmt(lblNet, "%s connected", NetClient.isEthernet() ? "Ethernet" : "WiFi");
            lv_label_set_text_fmt(lblIp, "IP: %s", NetClient.getLocalIP());
            // NetClient.persistDaily("%P/ipaddress", NetClient.getLocalIP());
            break;
        case EventType::NETWORK_CONNECTION_LOST:
            lv_label_set_text_fmt(lblNet, "%s: no connection", NetClient.isEthernet() ? "Ethernet" : "WiFi");
            lv_label_set_text(lblIp, "IP: -");
            break;
        case EventType::MQTT_SERVER_CONNECTED:
            lv_label_set_text(lblMqtt, "MQTT: connected");
            break;
        case EventType::MQTT_SERVER_CONNECTION_LOST:
            lv_label_set_text(lblMqtt, "MQTT: reconnecting...");
            break;
        case EventType::MQTT_MESSAGE_RECEIVED: {
            const auto &event = client_event.mqtt_message_received_event;
            if (strcmp(event.topic, "unix_time/unix_time") == 0) {
                time_t t = (time_t)strtoul(event.payload, nullptr, 10);
                struct tm tm;
                gmtime_r(&t, &tm);
                lv_label_set_text_fmt(lblClock, "%02d:%02d:%02d UTC", tm.tm_hour, tm.tm_min, tm.tm_sec);
                break;
            }
            // Commands must be fresh and non-empty.  This sketch has none, so
            // it answers help and does not advertise %P/control/?.
            if (event.retained || event.payloadLength == 0) break;
            std::string controlRoot = NetClient.expandTopic("%P/control/");
            if (strncmp(event.topic, controlRoot.c_str(), controlRoot.size()) != 0) break;
            const char *cmd = event.topic + controlRoot.size();
            if (strcmp(cmd, "?") == 0 && strcmp(event.payload, "1") == 0) {
                NetClient.publish("%P/control/help",
                    "This device accepts no commands. ? is the only control topic.", false);
            }
            break;
        }
        default:
            break;
    }
}

void loop() {
    while (NetClient.eventAvailable()) handleEvent(NetClient.getEvent());
    // publishStats();                         // diagnostics: uncomment to publish

    lv_timer_handler();
    delay(5);
}
