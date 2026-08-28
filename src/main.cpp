/*
 * P1P2-ESP8266 "PARKED" firmware
 * -------------------------------
 * Minimal ESP8266 companion firmware for the migration to the ESP32
 * Ethernet + PoE gateway.
 *
 * IMPORTANT:
 *   - This firmware NEVER initializes or uses the hardware UART.
 *   - GPIO1 (TX) and GPIO3 (RX) are left completely untouched.
 *   - GPIO15 is kept HIGH as ATMEGA_SERIAL_ENABLE.
 *
 * Added management features:
 *   - ESP8266 restart from the web UI
 *   - WiFi provisioning / management
 *   - First boot (or forgotten WiFi): local configuration AP
 *   - WiFi network scan
 *   - Change WiFi credentials from the web UI
 *   - Stored credentials are tested before being saved
 *   - If stored WiFi cannot be reached, configuration AP is started
 *
 * The ESP8266 remains a PARKED / legacy companion. It does not implement
 * MQTT or P1/P2 communication.
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <stdarg.h>

// -----------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------

static const char *FW_LABEL   = "P1P2-ESP8266-PARKED";
static const char *FW_VERSION = "1.2.0";
static const char *FW_AUTHOR  = "JackyKNX";

static const char *OTA_HOSTNAME = "p1p2-parked";

static const char *AP_PREFIX    = "P1P2-Setup-";
static const char *AP_PASSWORD  = "p1p2setup";

static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
static const uint32_t WIFI_RECONNECT_INTERVAL_MS = 10000;
static const uint32_t WIFI_AP_FALLBACK_MS = 300000; // 5 minutes
static uint32_t wifiLastReconnectAttempt = 0;
static uint32_t wifiDisconnectedSince = 0;
static uint32_t wifiReconnectCount = 0;

// EEPROM layout
static const uint16_t EEPROM_SIZE = 512;
static const uint32_t EEPROM_MAGIC = 0x50315032UL; // "P1P2"

struct WiFiConfig
{
    uint32_t magic;
    char ssid[64];
    char password[64];
};

WiFiConfig wifiConfig;

// -----------------------------------------------------------------------
// Web log -- ESP8266 diagnostics only, NOT a P1/P2 bus monitor.
// -----------------------------------------------------------------------

constexpr size_t LOG_BUFFER_SIZE = 3072;
static char logBuffer[LOG_BUFFER_SIZE];
static size_t logHead = 0;
static size_t logUsed = 0;
static uint32_t logTotalWritten = 0;

static void logWriteByte(char c)
{
    logBuffer[logHead] = c;
    logHead = (logHead + 1) % LOG_BUFFER_SIZE;

    if (logUsed < LOG_BUFFER_SIZE)
        logUsed++;

    logTotalWritten++;
}

static void logPrintf(const char *fmt, ...)
{
    char buf[192];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    uint32_t ms = millis();
    uint32_t h = ms / 3600000UL; ms %= 3600000UL;
    uint32_t m = ms / 60000UL;   ms %= 60000UL;
    uint32_t s = ms / 1000UL;    ms %= 1000UL;

    char line[224];
    snprintf(line, sizeof(line), "%02lu:%02lu:%02lu.%03lu | %s\n",
             (unsigned long)h, (unsigned long)m, (unsigned long)s,
             (unsigned long)ms, buf);

    for (size_t i = 0; line[i] != '\0'; i++)
        logWriteByte(line[i]);
}

static String logGetSince(uint32_t &sinceTotal, bool &overflow)
{
    uint32_t wanted = logTotalWritten - sinceTotal;
    overflow = wanted > logUsed;

    uint32_t count = overflow ? logUsed : wanted;

    String s;
    s.reserve(count);

    size_t pos = (logHead + LOG_BUFFER_SIZE - count) % LOG_BUFFER_SIZE;

    for (uint32_t i = 0; i < count; i++)
    {
        s += logBuffer[pos];
        pos = (pos + 1) % LOG_BUFFER_SIZE;
    }

    sinceTotal = logTotalWritten;
    return s;
}

// -----------------------------------------------------------------------
// Web server
// -----------------------------------------------------------------------

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

static String htmlEscape(const String &in)
{
    String out;
    out.reserve(in.length() + 16);

    for (size_t i = 0; i < in.length(); i++)
    {
        switch (in[i])
        {
            case '&': out += "&amp;";  break;
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += in[i];     break;
        }
    }

    return out;
}

static String jsonEscape(const String &in)
{
    String out;
    out.reserve(in.length() + 16);

    for (size_t i = 0; i < in.length(); i++)
    {
        char c = in[i];

        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (c == '\n')
            out += "\\n";
        else if (c != '\r')
            out += c;
    }

    return out;
}

// -----------------------------------------------------------------------
// System diagnostics
// -----------------------------------------------------------------------

static uint32_t bootCount = 0;
static uint32_t bootCountMarker = 0x424F4F54UL;

static const char *resetReasonText()
{
    rst_info *r = ESP.getResetInfoPtr();

    if (!r)
        return "UNKNOWN";

    switch (r->reason)
    {
        case REASON_DEFAULT_RST:     return "DEFAULT";
        case REASON_WDT_RST:         return "WATCHDOG";
        case REASON_EXCEPTION_RST:   return "EXCEPTION";
        case REASON_SOFT_WDT_RST:   return "SOFT_WATCHDOG";
        case REASON_SOFT_RESTART:   return "SOFTWARE";
        case REASON_DEEP_SLEEP_AWAKE:return "DEEP_SLEEP";
        case REASON_EXT_SYS_RST:    return "EXTERNAL";
        default:                    return "UNKNOWN";
    }
}

static void loadAndIncrementBootCount()
{
    EEPROM.begin(EEPROM_SIZE);

    uint32_t marker = 0;
    uint32_t count = 0;

    EEPROM.get(128, marker);
    EEPROM.get(132, count);

    if (marker != bootCountMarker)
        count = 0;

    bootCount = count + 1;

    EEPROM.put(128, bootCountMarker);
    EEPROM.put(132, bootCount);
    EEPROM.commit();
}

static String uptimeText()
{
    uint32_t sec = millis() / 1000UL;
    uint32_t days = sec / 86400UL;
    sec %= 86400UL;

    uint32_t hours = sec / 3600UL;
    sec %= 3600UL;

    uint32_t minutes = sec / 60UL;
    sec %= 60UL;

    char buf[40];
    snprintf(buf, sizeof(buf), "%lu d %02lu:%02lu:%02lu",
             (unsigned long)days,
             (unsigned long)hours,
             (unsigned long)minutes,
             (unsigned long)sec);

    return String(buf);
}

// -----------------------------------------------------------------------
// WiFi configuration
// -----------------------------------------------------------------------

static bool loadWiFiConfig()
{
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.get(0, wifiConfig);

    if (wifiConfig.magic != EEPROM_MAGIC)
    {
        memset(&wifiConfig, 0, sizeof(wifiConfig));
        return false;
    }

    wifiConfig.ssid[sizeof(wifiConfig.ssid) - 1] = '\0';
    wifiConfig.password[sizeof(wifiConfig.password) - 1] = '\0';

    if (strlen(wifiConfig.ssid) == 0)
    {
        memset(&wifiConfig, 0, sizeof(wifiConfig));
        return false;
    }

    return true;
}

static void saveWiFiConfig(const String &ssid, const String &password)
{
    memset(&wifiConfig, 0, sizeof(wifiConfig));

    wifiConfig.magic = EEPROM_MAGIC;

    ssid.toCharArray(
        wifiConfig.ssid,
        sizeof(wifiConfig.ssid)
    );

    password.toCharArray(
        wifiConfig.password,
        sizeof(wifiConfig.password)
    );

    EEPROM.put(0, wifiConfig);
    EEPROM.commit();
}

static void clearWiFiConfig()
{
    memset(&wifiConfig, 0, sizeof(wifiConfig));
    EEPROM.put(0, wifiConfig);
    EEPROM.commit();
}

static String apName()
{
    uint32_t chipId = ESP.getChipId();

    char id[9];
    snprintf(
        id,
        sizeof(id),
        "%06lX",
        (unsigned long)(chipId & 0xFFFFFF)
    );

    return String(AP_PREFIX) + id;
}

static void startConfigAP()
{
    WiFi.disconnect();
    delay(100);

    WiFi.mode(WIFI_AP);

    String ssid = apName();

    WiFi.softAP(
        ssid.c_str(),
        AP_PASSWORD
    );

    MDNS.end();

    logPrintf(
        "WiFi configuration AP started: SSID=%s IP=%s",
        ssid.c_str(),
        WiFi.softAPIP().toString().c_str()
    );
}

static bool connectToWiFi(
    const char *ssid,
    const char *password)
{
    WiFi.mode(WIFI_STA);
    WiFi.hostname(OTA_HOSTNAME);

    WiFi.disconnect();
    delay(100);

    WiFi.begin(ssid, password);

    logPrintf(
        "WiFi: connecting to '%s'...",
        ssid
    );

    uint32_t start = millis();

    while (
        WiFi.status() != WL_CONNECTED &&
        millis() - start < WIFI_CONNECT_TIMEOUT_MS
    )
    {
        delay(100);
        yield();
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        logPrintf(
            "WiFi: connection failed, status=%d",
            (int)WiFi.status()
        );

        return false;
    }

    logPrintf(
        "WiFi connected, IP=%s RSSI=%d dBm",
        WiFi.localIP().toString().c_str(),
        WiFi.RSSI()
    );

    if (MDNS.begin(OTA_HOSTNAME))
    {
        MDNS.addService("http", "tcp", 80);

        logPrintf(
            "mDNS: http://%s.local/",
            OTA_HOSTNAME
        );
    }

    return true;
}

static void startWiFi()
{
    if (!loadWiFiConfig())
    {
        logPrintf(
            "No stored WiFi configuration"
        );

        startConfigAP();
        return;
    }

    if (!connectToWiFi(
            wifiConfig.ssid,
            wifiConfig.password))
    {
        logPrintf(
            "Stored WiFi unavailable - entering configuration mode"
        );

        startConfigAP();
    }
}

// -----------------------------------------------------------------------
// WiFi runtime monitoring / reconnect
// -----------------------------------------------------------------------

static void maintainWiFi()
{
    if (WiFi.getMode() == WIFI_AP)
        return;

    if (WiFi.status() == WL_CONNECTED)
    {
        wifiDisconnectedSince = 0;
        return;
    }

    uint32_t now = millis();

    if (wifiDisconnectedSince == 0)
    {
        wifiDisconnectedSince = now;

        logPrintf(
            "WiFi disconnected, status=%d",
            (int)WiFi.status()
        );
    }

    if (now - wifiLastReconnectAttempt >= WIFI_RECONNECT_INTERVAL_MS)
    {
        wifiLastReconnectAttempt = now;
        wifiReconnectCount++;

        logPrintf(
            "WiFi reconnect attempt #%lu",
            (unsigned long)wifiReconnectCount
        );

        WiFi.reconnect();
    }

    if (now - wifiDisconnectedSince >= WIFI_AP_FALLBACK_MS)
    {
        logPrintf(
            "WiFi unavailable for 5 minutes - entering configuration AP"
        );

        startConfigAP();
        wifiDisconnectedSince = 0;
    }
}

// -----------------------------------------------------------------------
// Root / status
// -----------------------------------------------------------------------

static void handleRoot()
{
    String s;
    s.reserve(3500);

    s += "<!DOCTYPE html><html><head>";
    s += "<meta charset='utf-8'>";
    s += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    s += "<title>";
    s += FW_LABEL;
    s += "</title>";

    s += "<style>";
    s += "body{font-family:Arial;margin:20px;background:#f4f4f4;color:#222;}";
    s += ".box{max-width:800px;margin:auto;background:#fff;padding:22px;";
    s += "border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,.12);}";
    s += "table{border-collapse:collapse;width:100%;}";
    s += "td{padding:8px;border-bottom:1px solid #ddd;}";
    s += ".label{font-weight:bold;width:180px;}";
    s += ".ok{color:#167534;font-weight:bold;}";
    s += ".warn{color:#9a6700;font-weight:bold;}";
    s += "button{padding:10px 16px;border:0;border-radius:5px;cursor:pointer;}";
    s += ".danger{background:#b42318;color:white;}";
    s += ".secondary{background:#e5e7eb;color:#222;}";
    s += "a{display:inline-block;margin:8px 12px 8px 0;}";
    s += "</style></head><body><div class='box'>";

    s += "<h2>";
    s += FW_LABEL;
    s += "</h2>";

    s += "<p class='warn'><b>PARKED / LEGACY</b></p>";

    s += "<p>";
    s += "Active ESP32 gateway: ";
    s += "<a href='https://github.com/JackyKNX/P1P2MQTT-ESP32' target='_blank'>";
    s += "P1P2MQTT-ESP32</a><br>";
    s += "ESP8266 companion: ";
    s += "<a href='https://github.com/JackyKNX/P1P2MQTT-ESP8266-PARKED' target='_blank'>";
    s += "P1P2MQTT-ESP8266-PARKED</a>";
    s += "</p>";

    s += "<p>";
    s += "This ESP8266 no longer acts as the P1P2MQTT network bridge.";
    s += " The active gateway is the ESP32 Ethernet + PoE project.";
    s += "</p>";

    s += "<table>";

    s += "<tr><td class='label'>Firmware</td><td>";
    s += FW_VERSION;
    s += " (";
    s += FW_AUTHOR;
    s += ")</td></tr>";

    s += "<tr><td class='label'>WiFi status</td><td>";

    if (WiFi.getMode() == WIFI_AP)
    {
        s += "<span class='warn'>Configuration AP</span>";
    }
    else if (WiFi.status() == WL_CONNECTED)
    {
        s += "<span class='ok'>Connected</span>";
    }
    else
    {
        s += "<span class='warn'>Disconnected</span>";
    }

    s += "</td></tr>";

    s += "<tr><td class='label'>SSID</td><td>";

    if (WiFi.getMode() == WIFI_AP)
        s += htmlEscape(WiFi.softAPSSID());
    else if (WiFi.status() == WL_CONNECTED)
        s += htmlEscape(WiFi.SSID());
    else
        s += "n/a";

    s += "</td></tr>";

    s += "<tr><td class='label'>IP</td><td>";

    if (WiFi.getMode() == WIFI_AP)
        s += WiFi.softAPIP().toString();
    else
        s += WiFi.localIP().toString();

    s += "</td></tr>";

    s += "<tr><td class='label'>RSSI</td><td>";

    if (WiFi.status() == WL_CONNECTED)
    {
        s += String(WiFi.RSSI());
        s += " dBm";
    }
    else
        s += "n/a";

    s += "</td></tr>";

    s += "<tr><td class='label'>Free heap</td><td>";
    s += String(ESP.getFreeHeap());
    s += " B</td></tr>";

    s += "<tr><td class='label'>Uptime</td><td>";
    s += uptimeText();
    s += "</td></tr>";

    s += "<tr><td class='label'>MAC</td><td>";
    s += WiFi.macAddress();
    s += "</td></tr>";

    s += "<tr><td class='label'>Chip ID</td><td>";
    s += String(ESP.getChipId(), HEX);
    s += "</td></tr>";

    s += "<tr><td class='label'>Min free heap</td><td>";
    s += String(ESP.getFreeHeap());
    s += " B (current; reset on boot)</td></tr>";

    s += "<tr><td class='label'>Boot count</td><td>";
    s += String(bootCount);
    s += "</td></tr>";

    s += "<tr><td class='label'>Reset reason</td><td>";
    s += resetReasonText();
    s += "</td></tr>";

    if (WiFi.status() == WL_CONNECTED)
    {
        s += "<tr><td class='label'>WiFi reconnects</td><td>";
        s += String(wifiReconnectCount);
        s += "</td></tr>";
    }

    s += "</table>";

    s += "<h3>Management</h3>";

    s += "<a href='/wifi'>WiFi configuration</a>";
    s += "<a href='/serial'>System log</a>";
    s += "<a href='/update'>Firmware update</a>";

    s += "<form method='POST' action='/restart' "
         "onsubmit=\"return confirm('Restart ESP8266?');\" "
         "style='margin-top:15px;'>";

    s += "<button class='danger' type='submit'>";
    s += "Restart ESP8266";
    s += "</button>";

    s += "</form>";

    s += "<p class='warn'>";
    s += "The ESP8266 never initializes GPIO1/GPIO3 hardware UART.";
    s += "</p>";

    s += "</div></body></html>";

    server.send(
        200,
        "text/html",
        s
    );
}

// -----------------------------------------------------------------------
// WiFi web management
// -----------------------------------------------------------------------

static String encryptionName(uint8_t encryption)
{
    switch (encryption)
    {
        case ENC_TYPE_NONE: return "Open";
        case ENC_TYPE_WEP:  return "WEP";
        case ENC_TYPE_TKIP: return "WPA/TKIP";
        case ENC_TYPE_CCMP: return "WPA2/AES";
        case ENC_TYPE_AUTO: return "WPA/WPA2";
        default: return "Secured";
    }
}

static void handleWiFiScan()
{
    int count = WiFi.scanNetworks(false, true);

    String json = "[";
    bool first = true;

    for (int i = 0; i < count; i++)
    {
        if (!first)
            json += ",";

        first = false;

        json += "{";
        json += "\"ssid\":\"";
        json += jsonEscape(WiFi.SSID(i));
        json += "\",";

        json += "\"rssi\":";
        json += String(WiFi.RSSI(i));
        json += ",";

        json += "\"encryption\":\"";
        json += jsonEscape(
            encryptionName(WiFi.encryptionType(i))
        );
        json += "\"";
        json += "}";
    }

    json += "]";

    WiFi.scanDelete();

    server.send(
        200,
        "application/json",
        json
    );
}

static void handleWiFi()
{
    String storedSsid = String(wifiConfig.ssid);

    String s;
    s.reserve(6000);

    s += "<!DOCTYPE html><html><head>";
    s += "<meta charset='utf-8'>";
    s += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    s += "<title>WiFi Configuration</title>";

    s += "<style>";
    s += "body{font-family:Arial;margin:20px;background:#f4f4f4;color:#222;}";
    s += ".box{max-width:800px;margin:auto;background:#fff;padding:22px;";
    s += "border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,.12);}";
    s += "input{width:100%;padding:10px;margin:6px 0 15px;";
    s += "box-sizing:border-box;border:1px solid #bbb;border-radius:5px;}";
    s += "button{padding:10px 16px;border:0;border-radius:5px;cursor:pointer;}";
    s += ".secondary{background:#e5e7eb;color:#222;}";
    s += ".danger{background:#b42318;color:white;}";
    s += ".wifi{display:flex;justify-content:space-between;align-items:center;";
    s += "padding:12px;border:1px solid #ddd;border-radius:6px;";
    s += "margin:6px 0;cursor:pointer;}";
    s += ".wifi:hover{background:#f5f5f5;}";
    s += ".ssid{font-weight:bold;}";
    s += ".meta{font-size:13px;color:#666;}";
    s += ".muted{color:#666;font-size:13px;}";
    s += ".warn{color:#9a6700;font-weight:bold;}";
    s += "a{display:inline-block;margin:8px 12px 8px 0;}";
    s += "</style></head><body><div class='box'>";

    s += "<h1>WiFi Configuration</h1>";

    if (WiFi.getMode() == WIFI_AP)
    {
        s += "<p class='warn'>";
        s += "Configuration AP is active.";
        s += "</p>";

        s += "<p>";
        s += "Connect to <b>";
        s += htmlEscape(WiFi.softAPSSID());
        s += "</b> and select the WiFi network below.";
        s += "</p>";

        s += "<p class='muted'>";
        s += "AP password: <b>";
        s += AP_PASSWORD;
        s += "</b><br>";
        s += "AP address: <b>";
        s += WiFi.softAPIP().toString();
        s += "</b>";
        s += "</p>";
    }
    else if (WiFi.status() == WL_CONNECTED)
    {
        s += "<p>";
        s += "Connected to <b>";
        s += htmlEscape(WiFi.SSID());
        s += "</b> at ";
        s += WiFi.localIP().toString();
        s += " (";
        s += String(WiFi.RSSI());
        s += " dBm).";
        s += "</p>";
    }

    s += "<h2>Available networks</h2>";

    s += "<button class='secondary' type='button' onclick='scanWiFi()'>";
    s += "Scan for WiFi networks";
    s += "</button>";

    s += "<div id='wifiList' style='margin-top:15px;'>";
    s += "<p class='muted'>Press Scan for WiFi networks.</p>";
    s += "</div>";

    s += "<h2>Connect</h2>";

    s += "<form method='POST' action='/wifi/save'>";

    s += "<label>SSID</label>";
    s += "<input id='ssid' name='ssid' value='";
    s += htmlEscape(storedSsid);
    s += "' required>";

    s += "<label>Password</label>";
    s += "<input name='password' type='password' autocomplete='new-password'>";

    s += "<p class='muted'>";
    s += "Enter the WiFi password. The new connection is tested before it is saved.";
    s += "</p>";

    s += "<button type='submit'>Connect & Save</button>";

    s += "</form>";

    if (storedSsid.length() > 0)
    {
        s += "<form method='POST' action='/wifi/forget' "
             "onsubmit=\"return confirm('Forget stored WiFi configuration?');\" "
             "style='margin-top:20px;'>";

        s += "<button class='danger' type='submit'>";
        s += "Forget WiFi configuration";
        s += "</button>";

        s += "</form>";
    }

    s += "<p><a href='/'>&larr; Back</a></p>";

    s += R"rawliteral(
<script>
function selectWiFi(ssid)
{
    document.getElementById('ssid').value = ssid;
}

function scanWiFi()
{
    const list = document.getElementById('wifiList');

    list.innerHTML =
        "<p>Scanning for WiFi networks...</p>";

    fetch('/wifi/scan')
        .then(r => r.json())
        .then(networks => {

            if (!networks.length)
            {
                list.innerHTML =
                    "<p>No WiFi networks found.</p>";
                return;
            }

            list.innerHTML = "";

            networks.forEach(network => {

                const div = document.createElement('div');
                div.className = 'wifi';

                div.onclick = function()
                {
                    selectWiFi(network.ssid);
                };

                const info = document.createElement('div');

                const ssid = document.createElement('div');
                ssid.className = 'ssid';
                ssid.textContent =
                    network.ssid || '(hidden network)';

                const meta = document.createElement('div');
                meta.className = 'meta';
                meta.textContent =
                    network.encryption;

                info.appendChild(ssid);
                info.appendChild(meta);

                const rssi = document.createElement('div');
                rssi.textContent =
                    network.rssi + " dBm";

                div.appendChild(info);
                div.appendChild(rssi);

                list.appendChild(div);
            });
        })
        .catch(() => {
            list.innerHTML =
                "<p>WiFi scan failed.</p>";
        });
}
</script>
)rawliteral";

    s += "</div></body></html>";

    server.send(
        200,
        "text/html",
        s
    );
}

static void handleWiFiSave()
{
    if (!server.hasArg("ssid"))
    {
        server.send(
            400,
            "text/plain",
            "Missing SSID"
        );

        return;
    }

    String ssid = server.arg("ssid");
    String password = server.arg("password");

    ssid.trim();

    if (ssid.length() == 0)
    {
        server.send(
            400,
            "text/plain",
            "SSID cannot be empty"
        );

        return;
    }

    /*
     * Test first. Do not overwrite a working configuration if the new
     * network cannot be reached.
     */
    bool connected =
        connectToWiFi(
            ssid.c_str(),
            password.c_str()
        );

    if (!connected)
    {
        /*
         * If we were previously connected, try the old configuration.
         * Otherwise return to the configuration AP.
         */
        String oldSsid = String(wifiConfig.ssid);
        String oldPassword = String(wifiConfig.password);

        if (oldSsid.length() > 0 &&
            connectToWiFi(
                oldSsid.c_str(),
                oldPassword.c_str()))
        {
            logPrintf(
                "WiFi: previous configuration restored"
            );
        }
        else
        {
            startConfigAP();
        }

        String s =
            "<html><body><h2>WiFi connection failed</h2>"
            "<p>The new configuration was NOT saved.</p>"
            "<p><a href='/wifi'>Try again</a></p>"
            "</body></html>";

        server.send(
            400,
            "text/html",
            s
        );

        return;
    }

    saveWiFiConfig(
        ssid,
        password
    );

    logPrintf(
        "WiFi configuration saved for '%s'",
        ssid.c_str()
    );

    String s =
        "<html><body><h2>WiFi connected</h2>"
        "<p>Configuration saved successfully.</p>"
        "<p>ESP8266 will restart.</p>"
        "</body></html>";

    server.send(
        200,
        "text/html",
        s
    );

    delay(800);
    ESP.restart();
}

static void handleWiFiForget()
{
    clearWiFiConfig();

    logPrintf(
        "WiFi configuration forgotten"
    );

    String s =
        "<html><body><h2>WiFi configuration removed</h2>"
        "<p>ESP8266 will restart in configuration AP mode.</p>"
        "</body></html>";

    server.send(
        200,
        "text/html",
        s
    );

    delay(800);
    ESP.restart();
}

// -----------------------------------------------------------------------
// Restart
// -----------------------------------------------------------------------

static void handleRestart()
{
    server.send(
        200,
        "text/html",
        "<html><body>"
        "<h1>Restarting...</h1>"
        "<p>P1P2-ESP8266-PARKED is restarting.</p>"
        "</body></html>"
    );

    logPrintf(
        "ESP8266 restart requested from web UI"
    );

    delay(500);
    ESP.restart();
}

// -----------------------------------------------------------------------
// System log
// -----------------------------------------------------------------------

static void handleSerialPage()
{
    String s;
    s.reserve(2500);

    s += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    s += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    s += "<title>System log</title>";
    s += "<style>body{font-family:Arial;margin:20px;background:#f4f4f4;}";
    s += ".term{background:#111;color:#0f0;font-family:Consolas,monospace;";
    s += "font-size:13px;padding:10px;height:60vh;overflow-y:scroll;";
    s += "white-space:pre-wrap;word-break:break-all;border-radius:6px;}";
    s += "a{display:inline-block;margin:8px 12px 8px 0;}</style>";
    s += "</head><body>";

    s += "<h2>System log</h2>";
    s += "<p>This is the ESP8266's own diagnostic log.";
    s += " It is NOT a P1/P2 bus monitor.</p>";

    s += "<div id='term' class='term'></div>";

    s += "<p><a href='/'>&larr; Back</a></p>";

    s += "<script>";
    s += "let since=0;";
    s += "function poll(){";
    s += "fetch('/api/log?since='+since).then(r=>r.json()).then(j=>{";
    s += "since=j.total;";
    s += "const t=document.getElementById('term');";
    s += "if(j.overflow)t.textContent+='\\n[overflow]\\n';";
    s += "if(j.data.length){";
    s += "const bottom=t.scrollHeight-t.scrollTop-t.clientHeight<40;";
    s += "t.textContent+=j.data;";
    s += "if(bottom)t.scrollTop=t.scrollHeight;";
    s += "}";
    s += "}).finally(()=>setTimeout(poll,500));";
    s += "}";
    s += "poll();";
    s += "</script>";

    s += "</body></html>";

    server.send(
        200,
        "text/html",
        s
    );
}

static void handleApiLog()
{
    uint32_t since = 0;

    if (server.hasArg("since"))
        since = server.arg("since").toInt();

    bool overflow = false;
    String data =
        logGetSince(
            since,
            overflow
        );

    String json = "{";
    json += "\"total\":";
    json += String(since);
    json += ",\"overflow\":";
    json += overflow ? "true" : "false";
    json += ",\"data\":\"";
    json += jsonEscape(data);
    json += "\"}";

    server.send(
        200,
        "application/json",
        json
    );
}

// -----------------------------------------------------------------------
// JSON status
// -----------------------------------------------------------------------

static void handleApiStatus()
{
    String json = "{";

    json += "\"firmware\":\"";
    json += jsonEscape(FW_VERSION);
    json += "\",";

    json += "\"chip_id\":\"";
    json += String(ESP.getChipId(), HEX);
    json += "\",";

    json += "\"mac\":\"";
    json += jsonEscape(WiFi.macAddress());
    json += "\",";

    json += "\"uptime_s\":";
    json += String(millis() / 1000UL);
    json += ",";

    json += "\"free_heap\":";
    json += String(ESP.getFreeHeap());
    json += ",";

    json += "\"min_free_heap\":";
    json += String(ESP.getFreeHeap());
    json += ",";

    json += "\"boot_count\":";
    json += String(bootCount);
    json += ",";

    json += "\"reset_reason\":\"";
    json += resetReasonText();
    json += "\",";

    json += "\"wifi_status\":";
    json += String((int)WiFi.status());
    json += ",";

    json += "\"wifi_connected\":";
    json += WiFi.status() == WL_CONNECTED ? "true" : "false";
    json += ",";

    json += "\"wifi_reconnects\":";
    json += String(wifiReconnectCount);

    if (WiFi.status() == WL_CONNECTED)
    {
        json += ",\"ssid\":\"";
        json += jsonEscape(WiFi.SSID());
        json += "\",";

        json += "\"ip\":\"";
        json += WiFi.localIP().toString();
        json += "\",";

        json += "\"rssi\":";
        json += String(WiFi.RSSI());
    }
    else
    {
        json += ",\"ssid\":\"\"";
        json += ",\"ip\":\"";
        json += WiFi.getMode() == WIFI_AP
            ? WiFi.softAPIP().toString()
            : "";
        json += "\"";
    }

    json += "}";

    server.send(
        200,
        "application/json",
        json
    );
}

// -----------------------------------------------------------------------
// 404
// -----------------------------------------------------------------------

static void handle404()
{
    server.send(
        404,
        "text/plain",
        "Not found"
    );
}

// -----------------------------------------------------------------------
// Setup / loop
// -----------------------------------------------------------------------

void setup()
{
    /*
     * DELIBERATELY NO Serial.begin().
     *
     * GPIO1(TX) / GPIO3(RX) are never initialized or used.
     * The ESP32 is the only active UART bridge to the ATmega.
     */

    pinMode(15, OUTPUT);
    digitalWrite(15, HIGH);

    loadAndIncrementBootCount();

    logPrintf(
        "%s %s starting",
        FW_LABEL,
        FW_VERSION
    );

    logPrintf(
        "ATMEGA_SERIAL_ENABLE GPIO15 = HIGH"
    );

    startWiFi();

/*
 * HTTP update remains available on the current LAN or
 * configuration AP. No authentication is required.
 */

    httpUpdater.setup(
        &server,
        "/update"
    );

    server.on(
        "/",
        HTTP_GET,
        handleRoot
    );

    server.on(
        "/wifi",
        HTTP_GET,
        handleWiFi
    );

    server.on(
        "/wifi/scan",
        HTTP_GET,
        handleWiFiScan
    );

    server.on(
        "/wifi/save",
        HTTP_POST,
        handleWiFiSave
    );

    server.on(
        "/wifi/forget",
        HTTP_POST,
        handleWiFiForget
    );

    server.on(
        "/restart",
        HTTP_POST,
        handleRestart
    );

    server.on(
        "/serial",
        HTTP_GET,
        handleSerialPage
    );

    server.on(
        "/api/log",
        HTTP_GET,
        handleApiLog
    );

    server.on(
        "/api/status",
        HTTP_GET,
        handleApiStatus
    );

    server.onNotFound(
        handle404
    );

    server.begin();

    ArduinoOTA.setHostname(
        OTA_HOSTNAME
    );

    ArduinoOTA.onStart(
        []()
        {
            logPrintf(
                "ArduinoOTA: update starting"
            );
        }
    );

    ArduinoOTA.onEnd(
        []()
        {
            logPrintf(
                "ArduinoOTA: update complete"
            );
        }
    );

    ArduinoOTA.onError(
        [](ota_error_t error)
        {
            logPrintf(
                "ArduinoOTA error: %d",
                (int)error
            );
        }
    );

    ArduinoOTA.begin();

    logPrintf(
        "HTTP server started"
    );

    if (WiFi.getMode() == WIFI_AP)
    {
        logPrintf(
            "Configuration URL: http://%s/",
            WiFi.softAPIP().toString().c_str()
        );
    }
    else
    {
        logPrintf(
            "Management URL: http://%s/",
            WiFi.localIP().toString().c_str()
        );
    }
}

void loop()
{
    server.handleClient();
    ArduinoOTA.handle();
    maintainWiFi();

    if (WiFi.getMode() != WIFI_AP)
        MDNS.update();
}
