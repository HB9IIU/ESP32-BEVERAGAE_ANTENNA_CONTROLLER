#include "HB9IIUportalConfigurator.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include "nvs_flash.h"

#include "config_page.h"  // const char index_html[] PROGMEM = "..."
#include "success_page.h" // const char html_success[] PROGMEM = "..."
#include "error_page.h"   // const char html_error[] PROGMEM = "..."

#include <TFT_eSPI.h>
#include <qrcode.h> // ricmoo/QRCode

namespace HB9IIUPortal
{
    // ───────── INTERNAL STATE ─────────
    static const byte DNS_PORT = 53;
    static DNSServer dnsServer;
    static IPAddress apIP(192, 168, 4, 1);

    static WebServer server(80);
    static Preferences prefs;

    static int scanCount = 0;
    static bool inAPmode = false;
    static bool connected = false;

    // Optional hostname for STA + mDNS
    static String g_hostname;

    // Cached scan result for fast /scan response
    static String g_scanJson = "[]"; // cached JSON array of SSID labels
    static bool g_scanReady = false;

    // ───── CYD TFT + QR support (ONLY difference vs original) ─────
    static TFT_eSPI tft = TFT_eSPI();
    static QRCode qrcode;

    // QR version 6 → plenty for short Wi-Fi string
    constexpr int QR_VERSION = 6;
    constexpr int QR_PIXELS = 4 * QR_VERSION + 17;
    constexpr int QR_BUFFER_LEN = (QR_PIXELS * QR_PIXELS + 7) / 8;
    static uint8_t qrcodeData[QR_BUFFER_LEN];

    static void drawQRCodeCentered(QRCode *qrcode);

    // ───────── INTERNAL PROTOTYPES (original) ─────────
    static bool tryToConnectSavedWiFi();
    static void startConfigurationPortal();
    static void handleRootCaptivePortal();
    static void handleScanCaptivePortal();
    static void handleSaveCaptivePortal();
    static void printNetworkInfoAndMDNS();
    static void buildScanResultsCache();
    static bool testWiFiCredentials(const String &ssid, const String &password, uint16_t timeoutMs = 10000);

    // ───────── PUBLIC API ─────────

    void begin(const char *hostname)
    {
        Serial.println(F("[HB9IIUPortal] begin()"));

        // Store hostname (if provided) for STA + mDNS
        g_hostname = "";
        if (hostname != nullptr && hostname[0] != '\0')
        {
            g_hostname = hostname;
        }

        if (tryToConnectSavedWiFi())
        {
            inAPmode = false;
            connected = true;
            Serial.println(F("[HB9IIUPortal] Using saved WiFi, no captive portal needed."));
            printNetworkInfoAndMDNS(); // IP + DNS + mDNS info
        }
        else
        {
            connected = false;
            inAPmode = true;
            g_scanJson = "[]";
            g_scanReady = false;
            startConfigurationPortal();
        }
    }

    void loop()
    {
        server.handleClient();

        if (inAPmode)
        {
            dnsServer.processNextRequest(); // important for captive portal
        }
    }

    bool checkFactoryReset(uint8_t buttonPin, uint8_t ledPin)
    {
        // Assume pinMode(buttonPin, INPUT_PULLUP) and pinMode(ledPin, OUTPUT)
        // have already been called in the main sketch.

        digitalWrite(ledPin, LOW);
        Serial.println("\n[BOOT] Checking factory reset button...");

        delay(50); // small settle

        if (digitalRead(buttonPin) == LOW)
        {
            Serial.println("[BOOT] Factory reset button is held LOW at startup.");
            Serial.println("      Hold it for ~1 second to confirm factory reset...");

            unsigned long start = millis();
            bool stillHeld = true;

            while (millis() - start < 1000)
            {
                if (digitalRead(buttonPin) != LOW)
                {
                    stillHeld = false;
                    break;
                }
                // Small visual feedback
                digitalWrite(ledPin, HIGH);
                delay(50);
                digitalWrite(ledPin, LOW);
                delay(50);
            }

            if (stillHeld)
            {
                Serial.println("[BOOT] Factory reset confirmed. Erasing all preferences and restarting...");

                // Blink LED quickly before reset
                for (int i = 0; i < 8; ++i)
                {
                    digitalWrite(ledPin, HIGH);
                    delay(80);
                    digitalWrite(ledPin, LOW);
                    delay(80);
                }

                eraseAllPreferencesAndRestart();
                // does not return
            }
            else
            {
                Serial.println("[BOOT] Factory reset canceled (button released early).");
            }
        }
        else
        {
            Serial.println("[BOOT] Factory reset button is not pressed. Normal boot.");
        }

        return false;
    }

    void eraseAllPreferencesAndRestart()
    {
        Serial.println(F("⚠️ [HB9IIUPortal] Erasing all NVS data (wifi, config, iPhonetime, etc.)..."));

        esp_err_t err = nvs_flash_erase();
        if (err == ESP_OK)
        {
            Serial.println(F("✅ [HB9IIUPortal] NVS erased successfully. Restarting..."));
        }
        else
        {
            Serial.printf("❌ [HB9IIUPortal] Failed to erase NVS. Error: %d\n", err);
        }

        delay(1000);
        ESP.restart();
    }

    bool isInAPMode()
    {
        return inAPmode;
    }

    bool isConnected()
    {
        return connected && (WiFi.status() == WL_CONNECTED);
    }

    // ───────── INTERNAL IMPLEMENTATION ─────────

    static bool tryToConnectSavedWiFi()
    {
        Serial.println("[HB9IIUPortal] Attempting to load saved WiFi credentials...");

        if (!prefs.begin("wifi", false))
        {
            Serial.println("⚠️ [HB9IIUPortal] Failed to open NVS namespace 'wifi'.");
            return false;
        }

        if (!prefs.isKey("ssid") || !prefs.isKey("pass"))
        {
            Serial.println("⚠️ [HB9IIUPortal] No saved credentials found (keys missing).");
            prefs.end();
            return false;
        }

        String ssid = prefs.getString("ssid", "");
        String pass = prefs.getString("pass", "");
        // testing with wrong SSID
        //ssid = "Kilimangaro";
        prefs.end();

        // If SSID looks like "NAME (-48 dBm)" from our own scan label, strip the suffix (extra safety)
        int parenIndex = ssid.lastIndexOf('(');
        if (parenIndex > 0 && ssid.endsWith(" dBm)"))
        {
            ssid = ssid.substring(0, parenIndex);
            ssid.trim();
        }

        if (ssid.isEmpty() || pass.isEmpty())
        {
            Serial.println("⚠️ [HB9IIUPortal] No saved credentials found (empty values).");
            return false;
        }

        Serial.printf("[HB9IIUPortal] 📡 Found SSID: %s\n", ssid.c_str());
        Serial.printf("[HB9IIUPortal] 🔐 Found Password: %s\n", pass.c_str());

        Serial.printf("[HB9IIUPortal] 🔌 Connecting to WiFi: %s", ssid.c_str());

        WiFi.mode(WIFI_STA);

        // If a hostname was provided in begin(), apply it before connecting
        if (g_hostname.length())
        {
            WiFi.setHostname(g_hostname.c_str());
        }

        WiFi.begin(ssid.c_str(), pass.c_str());

        // Wait up to ~10s
      for (int i = 0; i < 20; ++i)
{

if (WiFi.status() == WL_CONNECTED)
{
    Serial.println();
    Serial.println("✅ [HB9IIUPortal] Connected to WiFi!");

     return true;
}



    // Print a dot to serial
    Serial.print(".");

    static int dotX = 0;
    const int textX = 130;
    const int textY = 300;

    // Print header only once
    if (i == 0)
    {
        tft.setTextFont(2);
        tft.setTextSize(1);
        tft.setTextColor(TFT_WHITE);
        tft.setCursor(textX, textY);
        tft.print("Connecting to WiFi ");

        // Start dots immediately after text
        dotX = tft.getCursorX();
    }

    // Print one new dot each iteration
    tft.setCursor(dotX, textY);
    tft.print(".");
    dotX = dotX+8; // advance for next dot

    delay(300);
}

        
        Serial.println("\n❌ [HB9IIUPortal] Failed to connect to saved WiFi.");
        

        // --- Show failed connection message on TFT ---

        tft.init();
        tft.setRotation(1);  // make sure this is your full 480x320 mode
        tft.fillScreen(TFT_BLACK);
        tft.setRotation(1);         // or whichever rotation gives 480×320 layout
        tft.fillScreen(TFT_BLACK);  // confirm full clear
        tft.setTextDatum(MC_DATUM); // center text alignment
        tft.setTextSize(1);

        // --- Line 1: Main error message (Font 4, red) ---
        tft.setTextFont(4);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("Could not connect to", 240, 120); // centered at (x=240,y=120)

        // --- Line 2: SSID (Font 4, white) ---
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(ssid, 240, 160); // center below the first line

        // --- Line 3: Reboot message (Font 2, yellow) ---
        tft.setTextFont(2);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawString("Rebooting in 4 seconds...", 240, 210);

        // --- Line 4: Factory reset hint (Font 2, white) ---
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("Press encoder switch to factory Reset", 240, 240);

        // --- Wait, then reboot ---
        delay(4000);
        ESP.restart();

        return false; // never reached
    }

    // NEW: QR drawing helper
    static void drawQRCodeCentered(QRCode *qrcode)
    {
        int qrSize = qrcode->size; // modules per side

        // Choose scale so QR fits nicely on the display
        int maxModulePixelsX = tft.width() / (qrSize + 4);  // + margin
        int maxModulePixelsY = tft.height() / (qrSize + 8); // + margin + text
        int scale = maxModulePixelsX;
        if (maxModulePixelsY < scale)
            scale = maxModulePixelsY;
        if (scale < 2)
            scale = 2; // don't go too tiny

        int qrPixelSize = qrSize * scale;

        // Centered position
        int x0 = (tft.width() - qrPixelSize) / 2;
        int y0 = (tft.height() - qrPixelSize) / 2;

        // White background block around QR
        tft.fillRect(x0 - 4, y0 - 4, qrPixelSize + 8, qrPixelSize + 8, TFT_WHITE);

        // Draw modules
        for (int y = 0; y < qrSize; y++)
        {
            for (int x = 0; x < qrSize; x++)
            {
                bool pixelOn = qrcode_getModule(qrcode, x, y);
                if (pixelOn)
                {
                    // Only draw black modules; white area is already white
                    tft.fillRect(x0 + x * scale, y0 + y * scale, scale, scale, TFT_BLACK);
                }
            }
        }
    }

    static void startConfigurationPortal()
    {
        Serial.println("🌐 [HB9IIUPortal] Starting Wi-Fi configuration portal...");

        // AP + STA so we CAN scan networks
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
        WiFi.softAP("HB9IIUSetup");

        Serial.print("📶 [HB9IIUPortal] AP IP Address: ");
        Serial.println(WiFi.softAPIP());

        // ─── CYD QR CODE ON TFT (ONLY addition) ───

        tft.init();
        tft.setRotation(1); // adjust if your CYD orientation differs

#ifdef TFT_BL
        pinMode(TFT_BL, OUTPUT);
        digitalWrite(TFT_BL, HIGH); // backlight on
#endif

        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(2);
        tft.drawString("HB9IIUSetup Wi-Fi", tft.width() / 2, 20);
        tft.drawString("Scan QR to connect", tft.width() / 2, 40);

        // Build Wi-Fi QR payload for OPEN AP:
        //   WIFI:T:nopass;S:HB9IIUSetup;;
        String qrPayload = "WIFI:T:nopass;S:HB9IIUSetup;;";

        Serial.print("📱 [HB9IIUPortal] QR payload: ");
        Serial.println(qrPayload);

        qrcode_initText(
            &qrcode,
            qrcodeData,
            QR_VERSION,
            ECC_MEDIUM,
            qrPayload.c_str());

        drawQRCodeCentered(&qrcode);

        tft.setTextDatum(BC_DATUM);
        tft.setTextFont(2);
        tft.drawString("Open phone camera and scan",
                       tft.width() / 2,
                       tft.height() - 4);

        // ─── ORIGINAL PORTAL LOGIC BELOW (unchanged) ───

        // Pre-scan once, cache results
        buildScanResultsCache();

        // DNS for captive portal
        dnsServer.start(DNS_PORT, "*", apIP);

        // Main config page + captive URLs
        server.on("/", handleRootCaptivePortal);
        server.on("/generate_204", handleRootCaptivePortal);        // Android
        server.on("/fwlink", handleRootCaptivePortal);              // Windows
        server.on("/hotspot-detect.html", handleRootCaptivePortal); // Apple

        server.on("/scan", handleScanCaptivePortal);
        server.on("/save", HTTP_POST, handleSaveCaptivePortal);

        server.begin();
        Serial.println("✅ [HB9IIUPortal] Web server started. Connect to 'HB9IIUSetup' Wi-Fi.");
    }

    static void handleRootCaptivePortal()
    {
        // Serve configuration HTML page
        server.send_P(200, "text/html", index_html);
    }

    // Build and cache JSON array of SSID labels from WiFi.scanNetworks()
    static void buildScanResultsCache()
    {
        Serial.println("🔍 [HB9IIUPortal] Pre-scanning Wi-Fi networks...");
        int n = WiFi.scanNetworks();
        scanCount++;

        Serial.printf("📡 [HB9IIUPortal] Pre-scan #%d: Found %d networks.\n", scanCount, n);

        String json = "[";
        for (int i = 0; i < n; ++i)
        {
            if (i > 0)
                json += ",";

            String label = WiFi.SSID(i);
            int32_t rssi = WiFi.RSSI(i);

            // Build "SSID (-63 dBm)"
            label += " (";
            label += String(rssi);
            label += " dBm)";

            // Minimal JSON string escaping
            label.replace("\\", "\\\\");
            label.replace("\"", "\\\"");

            json += "\"";
            json += label;
            json += "\"";
        }
        json += "]";

        g_scanJson = json;
        g_scanReady = true;
    }

    // /scan now just returns the cached result (fast)
    static void handleScanCaptivePortal()
    {
        Serial.println("📨 [HB9IIUPortal] /scan requested.");

        // If for some reason cache is not ready, build it now
        if (!g_scanReady)
        {
            buildScanResultsCache();
        }

        server.send(200, "application/json", g_scanJson);
    }

    // Test credentials while remaining in AP+STA mode
    static bool testWiFiCredentials(const String &ssid, const String &password, uint16_t timeoutMs)
    {
        Serial.printf("🔐 [HB9IIUPortal] Testing credentials for SSID '%s'...\n", ssid.c_str());

        // Keep AP alive, ensure we are in AP+STA
        WiFi.mode(WIFI_AP_STA);

        // Start STA connection with the new credentials
        WiFi.begin(ssid.c_str(), password.c_str());

        unsigned long start = millis();

        while (millis() - start < timeoutMs)
        {
            wl_status_t st = WiFi.status();
            if (st == WL_CONNECTED)
            {
                Serial.println("✅ [HB9IIUPortal] Test connection successful (credentials OK).");
                return true;
            }

            delay(500);
        }

        Serial.println("❌ [HB9IIUPortal] Test connection failed or timed out. Credentials likely invalid.");
        // Disconnect STA, but keep AP
        WiFi.disconnect(false /*wifioff*/, false /*erase*/);
        return false;
    }

    static void handleSaveCaptivePortal()
    {
        Serial.println("💾 [HB9IIUPortal] Processing Wi-Fi credentials save request...");

        // Now require ssid, password AND callsign
        if (!server.hasArg("ssid") || !server.hasArg("password") || !server.hasArg("callsign"))
        {
            Serial.println("⚠️ [HB9IIUPortal] Missing required fields (ssid/password/callsign).");
            server.send(400, "text/plain", "Missing ssid, password, or callsign.");
            return;
        }

        // Raw form values
        String ssidLabel = server.arg("ssid");
        String password = server.arg("password");
        String timeStr = server.hasArg("time") ? server.arg("time") : "";
        String callsign = server.arg("callsign");
        callsign.trim();

        // Strip " (… dBm)" to get the real SSID from label
        String ssid = ssidLabel;
        int parenIndex = ssid.indexOf(" (");
        if (parenIndex > 0)
        {
            ssid = ssid.substring(0, parenIndex);
            ssid.trim();
        }

        Serial.printf("📝 [HB9IIUPortal] Received SSID label: '%s'\n", ssidLabel.c_str());
        Serial.printf("📝 [HB9IIUPortal] Using raw SSID: '%s'\n", ssid.c_str());
        Serial.printf("📝 [HB9IIUPortal] Received Password: '%s'\n", password.c_str());
        Serial.printf("🕒 [HB9IIUPortal] Received Time: '%s'\n", timeStr.c_str());
        Serial.printf("📝 [HB9IIUPortal] Received Callsign: '%s'\n", callsign.c_str());

        // Extra safety: callsign must not be empty (JS already checks, but we enforce here too)
        if (callsign.length() == 0)
        {
            Serial.println("❌ [HB9IIUPortal] Callsign is empty. Staying in portal.");
            server.send(400, "text/plain", "Callsign must not be empty.");
            return;
        }

        // 1) Test the credentials BEFORE saving/restarting, using the RAW SSID
        if (!testWiFiCredentials(ssid, password, 10000)) // 10s timeout
        {
            Serial.println("❌ [HB9IIUPortal] Entered Wi-Fi credentials do NOT work. Staying in portal.");

            // Show the existing HTML error page
            server.send(200, "text/html", html_error);
            return; // IMPORTANT: do not save or restart
        }

        // If we reach here, credentials worked at least once 👍

        // 2) Save Wi-Fi credentials in prefs namespace "wifi"
        if (prefs.begin("wifi", false))
        {
            prefs.putString("ssid", ssid); // save cleaned SSID
            prefs.putString("pass", password);
            prefs.end();
            Serial.println("✅ [HB9IIUPortal] Wi-Fi credentials saved to NVS (namespace 'wifi').");
        }
        else
        {
            Serial.println("❌ [HB9IIUPortal] Failed to open prefs namespace 'wifi' for writing.");
        }

        // 2b) Save callsign in its own namespace
        if (prefs.begin("callsign", false))
        {
            prefs.putString("userCallsign", callsign);
            prefs.end();
            Serial.printf("✅ [HB9IIUPortal] Callsign '%s' saved to NVS (namespace 'callsign').\n",
                          callsign.c_str());
        }
        else
        {
            Serial.println("❌ [HB9IIUPortal] Failed to open prefs namespace 'callsign' for writing.");
        }
        // 2b) Save Callsign in prefs namespace "config"
        if (prefs.begin("config", false))
        {
            if (callsign.length() > 0)
            {
                prefs.putString("callsign", callsign);
                Serial.println("✅ [HB9IIUPortal] Callsign saved to NVS (namespace 'config', key 'callsign').");
            }
            else
            {
                Serial.println("⚠️ [HB9IIUPortal] Callsign empty, not saving.");
            }
            prefs.end();
        }
        else
        {
            Serial.println("❌ [HB9IIUPortal] Failed to open prefs namespace 'config' for writing.");
        }
        // 3) Optional: parse and save iPhone time JSON (if provided)
        if (timeStr.length() > 0)
        {
            JsonDocument timeDoc;
            DeserializationError err = deserializeJson(timeDoc, timeStr);

            if (!err)
            {
                // HTML sends: {iso: isoTime, unix: unixMillis, offset: offsetMinutes}
                const char *isoTime = timeDoc["iso"]; // e.g. "2025-11-22T10:15:30Z"
                int64_t unixMillis = timeDoc["unix"] | 0;
                int offsetMins = timeDoc["offset"] | 0;

                Serial.printf("🕒 Parsed time JSON:\n   isoTime: %s\n   unixMillis: %lld\n   offsetMinutes: %d\n",
                              isoTime ? isoTime : "(null)",
                              unixMillis,
                              offsetMins);

                if (prefs.begin("iPhonetime", false))
                {
                    prefs.putString("localTime", isoTime ? isoTime : "");
                    prefs.putLong64("unixMillis", unixMillis);
                    prefs.putInt("offsetMinutes", offsetMins);
                    prefs.end();

                    Serial.println("✅ [HB9IIUPortal] iPhone time JSON saved to NVS (namespace 'iPhonetime').");
                }
                else
                {
                    Serial.println("❌ [HB9IIUPortal] Failed to open prefs namespace 'iPhonetime' for writing.");
                }
            }
            else
            {
                Serial.println("⚠️ [HB9IIUPortal] Failed to parse time JSON, saving raw string instead.");
                if (prefs.begin("iPhonetime", false))
                {
                    prefs.putString("localTime", timeStr);
                    prefs.end();
                }
            }
        }

        // 4) Show success page and reboot as before
        server.send_P(200, "text/html", html_success);
        delay(500);
        ESP.restart();
    }

    static void printNetworkInfoAndMDNS()
    {
        IPAddress ip = WiFi.localIP();
        IPAddress gw = WiFi.gatewayIP();
        IPAddress sn = WiFi.subnetMask();
        IPAddress dns1 = WiFi.dnsIP(0);
        IPAddress dns2 = WiFi.dnsIP(1);

        Serial.println();
        Serial.printf("   📍 IP Address : %s\n", ip.toString().c_str());
        Serial.printf("   🚪 Gateway    : %s\n", gw.toString().c_str());
        Serial.printf("   📦 Subnet     : %s\n", sn.toString().c_str());
        Serial.printf("   🟢 DNS 1 (DHCP): %s\n", dns1.toString().c_str());

        IPAddress fallbackDNS2(8, 8, 8, 8);

        if ((uint32_t)dns2 == 0) // 0.0.0.0 -> not provided by DHCP
        {
            Serial.printf("   🔵 DNS 2 (fallback): %s\n", fallbackDNS2.toString().c_str());
        }
        else
        {
            Serial.printf("   🔵 DNS 2 (DHCP)   : %s\n", dns2.toString().c_str());
        }

        if (g_hostname.length())
        {
            if (MDNS.begin(g_hostname.c_str()))
            {
                MDNS.addService("http", "tcp", 80);
                Serial.printf("✅ mDNS ready at http://%s.local\n", g_hostname.c_str());
            }
            else
            {
                Serial.println("⚠️ Failed to start mDNS responder.");
            }
        }

        Serial.println();
    }

} // namespace HB9IIUPortal
