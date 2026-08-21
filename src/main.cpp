/*
 * P1P2-ESP8266 "PARKED" firmware
 * -------------------------------
 * Minimal placeholder firmware for the ESP8266 that used to run Arnold's
 * P1P2-bridge-esp8266 (P1P2MQTT) on this board.
 *
 * PURPOSE
 * -------
 * Take the ESP8266 fully OFF the P1/P2 UART link to the ATmega, so the
 * ESP32 (P1P2MQTT-ESP32 project) can safely drive UART2 TX -> ATmega RX
 * without two active transmitters fighting on the same wire.
 *
 * This firmware deliberately:
 *   - NEVER calls Serial.begin() and never touches GPIO1(TX)/GPIO3(RX)
 *     (the hardware UART pins wired to the ATmega on this board) for
 *     any purpose. It contains no Arnold / P1P2Monitor / P1P2Serial code
 *     at all.
 *   - Keeps WiFi + a small web UI alive so the board stays manageable:
 *       "/"        status page (uptime, RSSI, IP, free heap)
 *       "/serial"  a tiny web log viewer -- this is the ESP8266's OWN
 *                  diagnostic log (WiFi/OTA events), NOT a P1/P2 bus
 *                  monitor, since this firmware doesn't touch that UART
 *                  at all.
 *       "/update"  browser-based OTA firmware upload (ESP8266HTTPUpdateServer)
 *     ArduinoOTA (IDE/espota-based OTA) is also enabled.
 *
 * FLASHING THIS FOR THE FIRST TIME
 * ---------------------------------
 * Your current Arnold firmware already exposes an unauthenticated
 * browser upload at:
 *
 *     http://<esp8266-ip>/update
 *
 * (confirmed: P1P2MQTT-bridge.ino calls httpUpdater.setup(&httpServer)
 * with no username/password). Build this project (produces a .bin),
 * then open that URL in a browser and upload the .bin directly --
 * no USB, no Arduino IDE required for this first switch-over.
 *
 * SECURITY
 * --------
 * WIFI_PASSWORD and OTA_PASSWORD are hardcoded below in plain text --
 * anyone with the .bin (or the source) can extract them. Change
 * OTA_PASSWORD before flashing, and treat both this device's /update
 * endpoint and its OTA port as LAN-only, same as the rest of this
 * project.
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <stdarg.h>

// -----------------------------------------------------------------------
// Configuration -- edit before flashing
// -----------------------------------------------------------------------

static const char *FW_LABEL   = "P1P2-ESP8266-PARKED";
static const char *FW_VERSION = "1.0.0";
static const char *FW_AUTHOR  = "JackyKNX";

// Hardcoded WiFi credentials (no captive portal / WiFiManager).
static const char *WIFI_SSID     = "Centralna";
static const char *WIFI_PASSWORD = "Fil95klo!Fil95klo!";

// Used both for ArduinoOTA and for basic-auth on the /update page.
static const char *OTA_HOSTNAME = "p1p2-parked";
static const char *OTA_USER     = "admin";
static const char *OTA_PASSWORD = "ChangeThisOtaPwd";    // >>> CHANGE ME <<<

// -----------------------------------------------------------------------
// Web log -- this board's own diagnostics only, NOT a P1/P2 bus monitor.
// Deliberately independent of hardware Serial (which we never initialize).
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

static String jsonEscape(const String &in)
{
    String out;
    out.reserve(in.length());

    for (size_t i = 0; i < in.length(); i++)
    {
        char c = in[i];

        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (c == '\n')
        {
            out += "\\n";
        }
        else if (c == '\r')
        {
            // skip
        }
        else
        {
            out += c;
        }
    }

    return out;
}

void handleRoot()
{
    String s;
    s.reserve(1400);

    s += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    s += "<meta http-equiv='refresh' content='5'>";
    s += "<title>";
    s += FW_LABEL;
    s += "</title>";
    s += "<style>body{font-family:Arial;margin:30px;background:#f4f4f4;}";
    s += "table{border-collapse:collapse;}td{padding:6px 12px;border-bottom:1px solid #ddd;}</style>";
    s += "</head><body>";

    s += "<h2>";
    s += FW_LABEL;
    s += "</h2>";

    s += "<p style='color:#a00;font-weight:bold;'>";
    s += "Arnold / P1P2Monitor is disabled on this device.<br>";
    s += "This firmware never touches the P1/P2 UART (GPIO1/GPIO3).";
    s += "</p>";

    s += "<table>";
    s += "<tr><td>Firmware</td><td>";
    s += FW_VERSION;
    s += " (";
    s += FW_AUTHOR;
    s += ")</td></tr>";
    s += "<tr><td>IP</td><td>";
    s += WiFi.localIP().toString();
    s += "</td></tr>";
    s += "<tr><td>RSSI</td><td>";
    s += WiFi.RSSI();
    s += " dBm</td></tr>";
    s += "<tr><td>Free heap</td><td>";
    s += ESP.getFreeHeap();
    s += " B</td></tr>";
    s += "<tr><td>Uptime</td><td>";
    s += (millis() / 1000UL);
    s += " s</td></tr>";
    s += "</table>";

    s += "<br><a href='/serial'>System log</a> &nbsp;|&nbsp; <a href='/update'>Firmware update</a>";

    s += "</body></html>";

    server.send(200, "text/html", s);
}

void handleSerialPage()
{
    String s;
    s.reserve(1700);

    s += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    s += "<title>System log</title>";
    s += "<style>body{font-family:Arial;margin:20px;background:#f4f4f4;}";
    s += ".term{background:#111;color:#0f0;font-family:Consolas,monospace;";
    s += "font-size:13px;padding:10px;height:60vh;overflow-y:scroll;";
    s += "white-space:pre-wrap;word-break:break-all;border-radius:6px;}</style>";
    s += "</head><body>";

    s += "<h2>System log</h2>";
    s += "<p><a href='/'>&larr; Back</a> -- this is the ESP8266's own diagnostic log ";
    s += "(WiFi/OTA events), not a P1/P2 bus monitor.</p>";
    s += "<div id='term' class='term'></div>";

    s += "<script>";
    s += "let since=0;";
    s += "function poll(){";
    s += "fetch('/api/log?since='+since).then(function(r){return r.json();}).then(function(j){";
    s += "since=j.total;";
    s += "if(j.overflow) document.getElementById('term').textContent+='\\n[overflow]\\n';";
    s += "if(j.data.length){";
    s += "var t=document.getElementById('term');";
    s += "var atBottom = t.scrollHeight - t.scrollTop - t.clientHeight < 40;";
    s += "t.textContent+=j.data;";
    s += "if(atBottom) t.scrollTop=t.scrollHeight;";
    s += "}";
    s += "}).finally(function(){setTimeout(poll,500);});";
    s += "}";
    s += "poll();";
    s += "</script>";

    s += "</body></html>";

    server.send(200, "text/html", s);
}

void handleApiLog()
{
    uint32_t since = 0;

    if (server.hasArg("since"))
        since = server.arg("since").toInt();

    bool overflow = false;
    String data = logGetSince(since, overflow);

    String json = "{";
    json += "\"total\":";
    json += since;
    json += ",\"overflow\":";
    json += (overflow ? "true" : "false");
    json += ",\"data\":\"";
    json += jsonEscape(data);
    json += "\"}";

    server.send(200, "application/json", json);
}

void handle404()
{
    server.send(404, "text/plain", "Not found");
}

// -----------------------------------------------------------------------
// Setup / loop
// -----------------------------------------------------------------------

void setup()
{
    // Deliberately NO Serial.begin() here -- GPIO1(TX)/GPIO3(RX) stay
    // completely untouched, so they can never fight ESP32's UART2 TX on
    // the ATmega RX line. All logging below goes to the web log
    // (logPrintf), never to hardware Serial.

    // ATMEGA_SERIAL_ENABLE (Arnold's P1P2MQTT-bridge.ino, GPIO15 -> ATmega
    // PD4). This is a separate control line, NOT part of the UART data
    // path (GPIO1/GPIO3) -- it just tells P1P2Monitor "you're allowed to
    // enable your serial input/output". Without driving this HIGH, the
    // ATmega stays silent even though nothing is wrong with the UART
    // wiring itself. Setting this does not conflict with ESP32's UART2,
    // since it's a distinct physical pin.
    pinMode(15, OUTPUT);
    digitalWrite(15, HIGH);

    logPrintf("%s %s starting", FW_LABEL, FW_VERSION);
    logPrintf("ATMEGA_SERIAL_ENABLE (GPIO15) driven HIGH");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    logPrintf("Connecting to WiFi SSID '%s'...", WIFI_SSID);

    uint32_t waitStart = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(250);

        if (millis() - waitStart > 30000)
        {
            logPrintf("WiFi connect timeout after 30s, retrying...");
            waitStart = millis();
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
    }

    logPrintf("WiFi connected, IP=%s", WiFi.localIP().toString().c_str());

    MDNS.begin(OTA_HOSTNAME);

    httpUpdater.setup(&server, "/update", OTA_USER, OTA_PASSWORD);

    server.on("/", handleRoot);
    server.on("/serial", handleSerialPage);
    server.on("/api/log", handleApiLog);
    server.onNotFound(handle404);
    server.begin();

    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() { logPrintf("ArduinoOTA: update starting"); });
    ArduinoOTA.onEnd([]() { logPrintf("ArduinoOTA: update complete, rebooting"); });
    ArduinoOTA.onError([](ota_error_t error) {
        logPrintf("ArduinoOTA error: %d", (int)error);
    });
    ArduinoOTA.begin();

    MDNS.addService("http", "tcp", 80);

    logPrintf("Ready. Web UI: http://%s/  (mDNS: http://%s.local/)",
              WiFi.localIP().toString().c_str(), OTA_HOSTNAME);
}

void loop()
{
    server.handleClient();
    ArduinoOTA.handle();
    MDNS.update();
}
