// ============================================================================
//  REMOTE_STATION.cpp
//  Smart Beverage Antenna Selector — REMOTE PANEL (UI Client)
//  ----------------------------------------------------------
//  Target: ESP32 + TFT_eSPI touchscreen display + KY-040 rotary encoder
//
//  Role
//   - UI client that sends antenna selection requests to CLUB.
//   - Subscribes to authoritative state from CLUB (antenna/state, retained).
//   - Renders the CLUB-confirmed antenna state on the TFT.
//
//  Runtime Model
//   - Encoder PREVIEW → COMMIT model:
//       • Rotation updates a preview antenna (UI highlight only).
//       • When rotation stops for PREVIEW_SETTLE_MS, preview is committed.
//       • SELECT command is published to CLUB via MQTT (antenna/cmd).
//       • Dot turns YELLOW while waiting for CLUB confirmation.
//   - Map mode:
//       • First encoder detent exits map view (no selection, no MQTT traffic).
//
//  MQTT Topics (REMOTE role)
//   - SUBSCRIBE: antenna/state   (authoritative confirmed state from CLUB)
//   - PUBLISH  : antenna/cmd     (selection requests to CLUB)
//
//  Design Guarantees
//   - CLUB is the single source of truth — REMOTE never assumes state.
//   - REMOTE updates selectedAntenna only on confirmed state from CLUB.
//
//  Author : Daniel / HB9IIU
//  Date   : 2025-12-20
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "HB9IIUportalConfigurator.h"
#include <FS.h>
#include <LittleFS.h>
#include <HB9IIUsimplePngDisplay.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <PNGdec.h>
#include <Preferences.h>
#include "nvs_flash.h"
#include <time.h>
#include <lwip/apps/sntp.h>
#include "needleStuff.h"

#define TFT_RGB_ORDER TFT_BGR
Preferences preferences;

// ============================================================================
//                               PANEL SETTINGS
// ============================================================================

const float HOME_LAT_DEG = HOME_LAT_DEG_CFG;
const float HOME_LON_DEG = HOME_LON_DEG_CFG;

// ============================================================================
//                                MQTT CONFIGURATION
// ============================================================================

String gPanelIdStr = "REMOTE STATION";
const char *PANEL_ID = gPanelIdStr.c_str();

const char *MQTT_HOST = "1db5ec5c4ff44e67bcb25a811f852e53.s1.eu.hivemq.cloud";
const uint16_t MQTT_PORT = 8883;
const char *MQTT_USER = "esp32-club";
const char *MQTT_PASSWD = "@V51bP9J@H";

const char *MQTT_TOPIC_STATE     = "antenna/state";
const char *MQTT_TOPIC_CMD       = "antenna/cmd";
const char *MQTT_TOPIC_HEARTBEAT = "antenna/heartbeat";

static unsigned long lastHeartbeatMs       = 0;   // 0 = never received
static const unsigned long CLUB_HEARTBEAT_TIMEOUT_MS = 15000;

unsigned long gSetupDoneMs = 0; // stamped at end of setup(); buttons blocked for 2 s after

WiFiClientSecure mqttSecureClient;
PubSubClient mqttClient(mqttSecureClient);

// ============================================================================
//                         GLOBAL DISPLAY / APP VARIABLES
// ============================================================================

TFT_eSPI tft = TFT_eSPI();
PNG png;
fs::File pngFile;

// ----------------------------------------------------------------------------
// Views (pages) — no keypad view on REMOTE
// ----------------------------------------------------------------------------
enum View : uint8_t
{
    VIEW_DIAL = 0,
    VIEW_MAP  = 1,
};

static View gView = VIEW_DIAL;

// Antenna dial settings
const int16_t DIAL_RING_RADIUS = 152;
const int16_t DIAL_DOT_RADIUS  = 5;

const uint8_t MAX_DOTS = 16;
uint8_t numDots = 11;

float dotAzimuthDeg[MAX_DOTS + 1];
int16_t dotX[MAX_DOTS + 1];
int16_t dotY[MAX_DOTS + 1];

// Color palette
uint16_t TftGrey, TftRed, TftYellow, TftGreen;

// Pulse effect
unsigned long lastPulseUpdate = 0;
const unsigned long PULSE_INTERVAL_MS = 50;
int pulseBrightness = 255;
int pulseDirection  = -15;

// Encoder state
int8_t encoderAccum    = 0;
const int STEPS_PER_NOTCH = 4;

// ISR-side encoder state (written only from ISR)
volatile uint8_t encISRlastState = 0;
volatile int16_t encISRcount     = 0;

// Encoder lookup table
const int8_t encTable[16] = {
     0, +1, -1,  0,
    -1,  0,  0, +1,
    +1,  0,  0, -1,
     0, -1, +1,  0};

// Antenna limits
uint8_t selectedAntenna    = 1;
uint8_t preSelectedAntenna = 1;
const uint8_t ANTENNA_MIN  = 1;
uint8_t ANTENNA_MAX        = 11;

// Button debounce
unsigned long lastDebounceTime    = 0;
const unsigned long debounceDelayMs = 50;

// REMOTE awaiting CLUB confirmation
bool awaitingClubConfirm = false;

// ============================================================================
//                           FUNCTION DECLARATIONS
// ============================================================================

// WiFi / MQTT
void mqttSetup();
bool mqttEnsureConnected();
void mqttCallback(char *topic, byte *payload, unsigned int length);
void publishSelectCommand(uint8_t target);

// TFT / Filesystem
void initAntennaDial();
void initEncoderPins();

// Encoder / Button
void IRAM_ATTR encISR();
void updateEncoder();
void updateButton();

// UI and Maps
void updateDialColors();
void updateDynamicCornerLabels(bool force = false);
void drawAntennaDot(uint8_t index, uint16_t color);
void drawWiFiIndicator();
void drawMQTTIndicator();

// Calibration
void checkAndApplyTFTCalibrationData(bool recalibrate);
void calibrateTFTscreen();

// Factory Reset
void performFactoryResetAll();

String getStoredCallsign();
void drawRemoteStationLabel();
void loadDotConfig();
void updateAllDotPositions();
void drawDialBase();

// --- Map geometry constants ---
const int16_t MAP_X0 = 0;
const int16_t MAP_Y0 = 0;
const float MAP_W = 480.0f;
const float MAP_H = 320.0f;

void plotGreatCircle(float homeLat_deg, float homeLon_deg,
                     float azimuth_deg,
                     uint16_t color, uint8_t diameter);

void updateMapBottomLabel();

void displayIntroBanner();
void printSystemInfo();
void encodertestTFT();

void mountAndListFiles();

bool waitForTimeVerbose(uint32_t timeoutMs = 15000);
void printTimes();
void drawLocalAndUTCTime(int16_t y, int16_t margin);

int lastDrawnMinute = -1;
void updateTimeIfMinuteChanged();

static unsigned long lastUserInteractionMs = 0;
const unsigned long USER_IDLE_MS = 300;
void printMQTTConnectionOK();

uint8_t previewAntenna        = 0;
unsigned long lastEncoderActivityMs = 0;
const unsigned long PREVIEW_SETTLE_MS = 500;

//-------------------------------------------------------------------------------------------------------------------
// BUTTONS / MODE INDICATOR
//-------------------------------------------------------------------------------------------------------------------
#include <RobotoMono_Bold26pt7b.h>
#include <RobotoMono_Medium13pt7b.h>
#include <RobotoMono_Light8pt7b.h>
#include <JetBrainsMono_Bold8pt7b.h>

uint16_t VERY_LIGHT_GREY = tft.color565(230, 230, 230);

void stepAntennaByButtons(int dir);
void updateModeSelectButton();
void onExtraButtonPressed(bool isLeft);
void drawModeIndicator(bool force = false);
void loadPresetsFromNVS();
void savePresetToNVS(bool isLeftSlot, uint8_t antenna);

enum OperatingMode : uint8_t
{
    MODE_A = 0,
    MODE_B = 1
};
OperatingMode gMode = MODE_A;
uint8_t presetLeft  = 3;
uint8_t presetRight = 7;
const unsigned long BUTTON_DEBOUNCE_MS = 35;

bool presetSaveArmed = false;
unsigned long presetSaveArmUntilMs = 0;

const unsigned long MODE_LONGPRESS_MS    = 1200;
const unsigned long PRESET_ARM_WINDOW_MS = 5000;

// Extra buttons (GPIO34/35) debounce state
static int lastLsample = HIGH, lStable = HIGH;
static int lastRsample = HIGH, rStable = HIGH;
static unsigned long lrLastChangeMs = 0;

// Mode select button (GPIO25) debounce state
static int lastModeSample  = HIGH;
static int modeStableState = HIGH;
static unsigned long modeLastChangeMs = 0;

void updateModeSelectButton()
{
    static bool pressed       = false;
    static unsigned long pressStartMs = 0;

    int reading = digitalRead(PIN_ENC_SW);

    if (reading != lastModeSample)
    {
        modeLastChangeMs = millis();
        lastModeSample   = reading;
    }

    if ((millis() - modeLastChangeMs) <= BUTTON_DEBOUNCE_MS)
        return;

    if (reading != modeStableState)
    {
        modeStableState = reading;

        // PRESS
        if (modeStableState == LOW)
        {
            lastUserInteractionMs = millis();

            if (gView == VIEW_MAP)
            {
                gView = VIEW_DIAL;
                displayImageOnTFT(png, "greatcircleMap.png");
                initAntennaDial();
                drawModeIndicator(true);
                updateDynamicCornerLabels(true);
                drawLocalAndUTCTime(240, 10);
                pressed = false;
                return;
            }

            pressed      = true;
            pressStartMs = millis();
        }
        // RELEASE
        else
        {
            if (!pressed)
                return;
            pressed = false;

            unsigned long heldMs = millis() - pressStartMs;

            if (heldMs >= MODE_LONGPRESS_MS)
            {
                if (gMode == MODE_B)
                {
                    presetSaveArmed      = true;
                    presetSaveArmUntilMs = millis() + PRESET_ARM_WINDOW_MS;
                    Serial.println("[UI ] Preset SAVE ARMED (5s) - press LEFT/RIGHT to store");
                }
                else
                {
                    Serial.println("[UI ] Long press ignored (not in Mode B)");
                }
                return;
            }

            gMode = (gMode == MODE_A) ? MODE_B : MODE_A;
            Serial.printf("[UI ] Mode -> %s\n", (gMode == MODE_A) ? "A" : "B");
            drawModeIndicator(true);
        }
    }

    if (presetSaveArmed && (millis() > presetSaveArmUntilMs))
    {
        presetSaveArmed = false;
        Serial.println("[UI ] Preset SAVE disarmed (timeout)");
    }
}

void onExtraButtonPressed(bool isLeft)
{
    // MODE A: step CW (right) or CCW (left)
    if (gMode == MODE_A)
    {
        stepAntennaByButtons(isLeft ? -1 : +1);
        return;
    }

    // MODE B: save armed — store into the pressed slot
    if (presetSaveArmed)
    {
        savePresetToNVS(isLeft, selectedAntenna);
        presetSaveArmed = false;
        Serial.printf("[UI ] Preset SAVE disarmed (stored %s)\n", isLeft ? "LEFT" : "RIGHT");
        return;
    }

    // MODE B: normal recall
    uint8_t target = isLeft ? presetLeft : presetRight;
    Serial.printf("[BTN] Recall %s preset -> antenna %u\n", isLeft ? "LEFT" : "RIGHT", target);

    if (target >= ANTENNA_MIN && target <= ANTENNA_MAX && target != selectedAntenna)
    {
        preSelectedAntenna = target;
        awaitingClubConfirm = true;
        updateDialColors();
        updateDynamicCornerLabels();
        publishSelectCommand(target);
    }
}

// Active-low buttons with EXTERNAL pull-ups
void updateExtraButtons()
{
    if (gSetupDoneMs == 0 || (millis() - gSetupDoneMs) < 2000) return; // ignore during startup RF noise

    int l = digitalRead(PIN_PBL);
    int r = digitalRead(PIN_PBR);

    if (l != lastLsample || r != lastRsample)
    {
        lrLastChangeMs = millis();
        lastLsample    = l;
        lastRsample    = r;
    }

    if ((millis() - lrLastChangeMs) > BUTTON_DEBOUNCE_MS)
    {
        if (l != lStable)
        {
            lStable = l;
            if (lStable == LOW)
                onExtraButtonPressed(true);
        }

        if (r != rStable)
        {
            rStable = r;
            if (rStable == LOW)
                onExtraButtonPressed(false);
        }
    }
}

uint8_t nextAntennaCW(uint8_t a)
{
    return (a >= ANTENNA_MAX) ? ANTENNA_MIN : (a + 1);
}

uint8_t nextAntennaCCW(uint8_t a)
{
    return (a <= ANTENNA_MIN) ? ANTENNA_MAX : (a - 1);
}

void stepAntennaByButtons(int dir)
{
    if (gView == VIEW_MAP)
    {
        gView = VIEW_DIAL;
        displayImageOnTFT(png, "greatcircleMap.png");
        initAntennaDial();
        drawModeIndicator(true);
        updateDynamicCornerLabels(true);
        drawLocalAndUTCTime(240, 10);
        return;
    }

    previewAntenna = 0;

    uint8_t target = selectedAntenna;
    if (dir > 0)
        target = nextAntennaCW(selectedAntenna);
    else if (dir < 0)
        target = nextAntennaCCW(selectedAntenna);

    if (target == selectedAntenna)
        return;

    Serial.printf("[BTN] Step %s -> antenna %u\n", (dir > 0) ? "CW" : "CCW", target);

    preSelectedAntenna  = target;
    awaitingClubConfirm = true;

    updateDialColors();
    updateDynamicCornerLabels();

    publishSelectCommand(target);
}

void drawModeIndicator(bool force)
{
    // Mode label intentionally hidden
    (void)force;
}

void loadPresetsFromNVS()
{
    const uint8_t defL = 3;
    const uint8_t defR = 7;
    uint8_t l = defL;
    uint8_t r = defR;

    if (preferences.begin("presets", false))
    {
        l = (uint8_t)preferences.getUInt("left",  defL);
        r = (uint8_t)preferences.getUInt("right", defR);
        preferences.end();
    }

    if (l < ANTENNA_MIN || l > ANTENNA_MAX) l = defL;
    if (r < ANTENNA_MIN || r > ANTENNA_MAX) r = defR;

    presetLeft  = l;
    presetRight = r;
    Serial.printf("[NVS] Presets loaded: LEFT=%u  RIGHT=%u\n", presetLeft, presetRight);
}

void savePresetToNVS(bool isLeftSlot, uint8_t antenna)
{
    if (antenna < ANTENNA_MIN || antenna > ANTENNA_MAX)
    {
        Serial.println("[NVS] Refusing to save invalid antenna number");
        return;
    }

    if (!preferences.begin("presets", false))
    {
        Serial.println("[NVS] preferences.begin(\"presets\") FAILED");
        return;
    }

    if (isLeftSlot)
    {
        preferences.putUInt("left", antenna);
        presetLeft = antenna;
        Serial.printf("[NVS] Saved LEFT preset = %u\n", presetLeft);
    }
    else
    {
        preferences.putUInt("right", antenna);
        presetRight = antenna;
        Serial.printf("[NVS] Saved RIGHT preset = %u\n", presetRight);
    }

    preferences.end();
}

void setPanelGamma_TFT_eSPI()
{
    const uint8_t pos[15] = {0x00, 0x05, 0x0A, 0x05, 0x14, 0x08, 0x3F, 0x6F, 0x51, 0x07, 0x0B, 0x09, 0x22, 0x29, 0x0F};
    const uint8_t neg[15] = {0x00, 0x13, 0x16, 0x04, 0x11, 0x07, 0x37, 0x46, 0x4D, 0x06, 0x10, 0x0E, 0x35, 0x37, 0x0F};

    tft.startWrite();
    tft.writecommand(0xE0);
    for (int i = 0; i < 15; i++) tft.writedata(pos[i]);
    tft.writecommand(0xE1);
    for (int i = 0; i < 15; i++) tft.writedata(neg[i]);
    tft.endWrite();
}

int lastNeedleIndex = 0;

// ============================================================================
//                                    SETUP
// ============================================================================
void setup()
{
    // 1️⃣ Serial console & startup diagnostics
    Serial.begin(115200);

    displayIntroBanner();
    printSystemInfo();

    mountAndListFiles();

    // Open dialBg.raw for needle background restores
    initNeedleCache(240, 160);

    // 2️⃣ Hardware initialization
    pinMode(TFT_BLP, OUTPUT);
    initEncoderPins();

    // LEFT and RIGHT buttons
    pinMode(PIN_PBL, INPUT); // GPIO34 (external pull-up required)
    pinMode(PIN_PBR, INPUT); // GPIO35 (external pull-up required)

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    setPanelGamma_TFT_eSPI();

    checkAndApplyTFTCalibrationData(false);

    // 3️⃣ Boot splash screen
    digitalWrite(TFT_BLP, LOW);
    displayImageOnTFT(png, "logo.png");
    digitalWrite(TFT_BLP, HIGH);

    // 4️⃣ TFT color palette
    TftGrey   = tft.color565(100, 100, 100);
    TftRed    = tft.color565(255,   0,   0);
    TftYellow = tft.color565(255, 255,   0);
    TftGreen  = tft.color565(  0, 255,   0);

    // 5️⃣ Factory reset (encoder button held at boot)
    if (!digitalRead(PIN_ENC_SW))
    {
        performFactoryResetAll();
    }

    // 6️⃣ WiFi configuration / captive portal
    HB9IIUPortal::begin("beverage");

    // 7️⃣ Connected → normal operational mode
    if (HB9IIUPortal::isConnected())
    {
        gPanelIdStr = getStoredCallsign();
        PANEL_ID    = gPanelIdStr.c_str();

        Serial.println(F("──────────────────────────────────────────────"));
        Serial.println(F("[APP] 📡 WiFi connected successfully"));
        Serial.printf("   ↳ Remote Panel ID  : %s\n", PANEL_ID);
        Serial.printf("   ↳ Local IP Address : %s\n",
                      WiFi.localIP().toString().c_str());
        Serial.println(F("──────────────────────────────────────────────\n"));

        displayImageOnTFT(png, "remote.png");
        drawRemoteStationLabel();

        mqttSetup();

        waitForTimeVerbose();
        delay(1000);
        printTimes();

        displayImageOnTFT(png, "greatcircleMap.png");
        drawLocalAndUTCTime(240, 10);
        initAntennaDial();
        drawModeIndicator(true);
        loadPresetsFromNVS();

        gSetupDoneMs = millis();
        Serial.println(F("✅ Setup complete.\n"));
    }

    // 8️⃣ Not connected → AP mode / portal
    else
    {
        wifi_mode_t mode   = WiFi.getMode();
        wl_status_t status = WiFi.status();

        Serial.println(F("──────────────────────────────────────────────"));
        Serial.println(F("[APP] ⚠️  WiFi not connected at setup stage"));
        Serial.printf("   ↳ Current WiFi mode : %s\n",
                      (mode == WIFI_AP)       ? "Access Point (AP)"
                      : (mode == WIFI_STA)    ? "Station (Client)"
                      : (mode == WIFI_AP_STA) ? "AP + STA (mixed)"
                                              : "WiFi OFF");
        Serial.printf("   ↳ WiFi status       : %s\n",
                      (status == WL_NO_SSID_AVAIL)    ? "No SSID available"
                      : (status == WL_CONNECT_FAILED) ? "Connection failed"
                      : (status == WL_DISCONNECTED)   ? "Disconnected"
                      : (status == WL_IDLE_STATUS)    ? "Idle (trying...)"
                      : (status == WL_CONNECTED)      ? "Connected (unexpected)"
                                                      : "Unknown");
        Serial.println();
        Serial.println(F("   ↳ AP / portal mode active"));
        Serial.println(F("   ↳ QR code and setup UI handled by HB9IIUPortal"));
        Serial.println(F("──────────────────────────────────────────────\n"));
    }
}

// ============================================================================
//                                    LOOP
// ============================================================================
void loop()
{
    bool userIdle = (millis() - lastUserInteractionMs) > USER_IDLE_MS;

    // Portal servicing (idle-only)
    if (userIdle)
        HB9IIUPortal::loop();

    if (HB9IIUPortal::isConnected())
    {
        mqttClient.loop();

        // High-priority physical inputs
        updateModeSelectButton();
        updateExtraButtons();
        updateEncoder();

        // ── Antenna dot pulse animation (dial view only) ──────────────────
        if (gView == VIEW_DIAL &&
            selectedAntenna >= ANTENNA_MIN &&
            selectedAntenna <= ANTENNA_MAX)
        {
            unsigned long now = millis();

            if (now - lastPulseUpdate >= PULSE_INTERVAL_MS)
            {
                lastPulseUpdate = now;
                pulseBrightness += pulseDirection;

                if (pulseBrightness <= 80 || pulseBrightness >= 255)
                {
                    pulseDirection  = -pulseDirection;
                    pulseBrightness = constrain(pulseBrightness, 80, 255);
                }

                uint16_t color;
                uint8_t  dotIndex;

                if (awaitingClubConfirm)
                {
                    // YELLOW pulse on pending dot while waiting for CLUB
                    color    = tft.color565(pulseBrightness, pulseBrightness, 0);
                    dotIndex = preSelectedAntenna;
                }
                else
                {
                    // GREEN pulse on confirmed dot
                    color    = tft.color565(0, pulseBrightness, 0);
                    dotIndex = selectedAntenna;
                }

                tft.drawCircle(dotX[dotIndex], dotY[dotIndex],
                               DIAL_DOT_RADIUS + 1, TFT_WHITE);
                drawAntennaDot(dotIndex, color);
            }
        }

        // WiFi RSSI indicator (idle-only)
        static long lastRssi = 0;
        long rssi = WiFi.RSSI();
        if (userIdle && abs(rssi - lastRssi) > 2)
        {
            drawWiFiIndicator();
            lastRssi = rssi;
        }

        // MQTT status indicator (idle-only)
        static unsigned long lastMQTTDraw = 0;
        if (userIdle && millis() - lastMQTTDraw > 100)
        {
            lastMQTTDraw = millis();
            drawMQTTIndicator();
        }

        // MQTT auto-reconnect watchdog (idle-only)
        static unsigned long lastMQTTCheck = 0;
        const unsigned long MQTT_RECHECK_MS = 10000;

        if (userIdle && millis() - lastMQTTCheck >= MQTT_RECHECK_MS)
        {
            lastMQTTCheck = millis();
            if (!mqttEnsureConnected())
                Serial.println(F("[MQTT] Reconnect attempt failed"));
            else
                printMQTTConnectionOK();
        }

        // Touchscreen polling (map ↔ dial toggle)
        static unsigned long lastTouchCheck = 0;
        if (millis() - lastTouchCheck > 100)
        {
            lastTouchCheck = millis();

            uint16_t x, y;
            if (tft.getTouch(&x, &y))
            {
                lastUserInteractionMs = millis();

                gView = (gView == VIEW_MAP) ? VIEW_DIAL : VIEW_MAP;

                if (gView != VIEW_MAP)
                {
                    lastNeedleIndex = -1;
                    displayImageOnTFT(png, "greatcircleMap.png");
                    initAntennaDial();
                    drawModeIndicator(true);
                    updateDynamicCornerLabels(true);
                    drawLocalAndUTCTime(240, 10);
                }
                else
                {
                    tft.fillScreen(TFT_BLACK);
                    displayImageOnTFT(png, "equirectangularMap.png");
                    float az = dotAzimuthDeg[preSelectedAntenna];
                    plotGreatCircle(HOME_LAT_DEG, HOME_LON_DEG, az, TFT_GOLD, 2);
                    updateMapBottomLabel();
                    drawWiFiIndicator();
                }
            }
        }

        // Time update (minute resolution)
        updateTimeIfMinuteChanged();
    }
    else
    {
        static unsigned long lastPortalMsg = 0;
        if (millis() - lastPortalMsg > 5000)
        {
            lastPortalMsg = millis();
            Serial.println(F("[PORTAL] Waiting for WiFi configuration..."));
        }
    }
}

// ============================================================================
//                            WIFI / MQTT HELPERS
// ============================================================================

void mqttSetup()
{
    Serial.println(F("[MQTT] Setting up secure client ..."));
    mqttSecureClient.setInsecure();
    mqttSecureClient.setTimeout(15000);
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttEnsureConnected();
}

bool mqttEnsureConnected()
{
    if (WiFi.status() != WL_CONNECTED)
        return false;

    if (mqttClient.connected())
        return true;

    Serial.println(F("[MQTT] (Re)connecting to broker..."));

    String clientId = String("cyd-remote-") + String((uint32_t)ESP.getEfuseMac(), HEX);

    if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWD))
    {
        Serial.println(F("[MQTT] Connected to HiveMQ Cloud"));

        if (mqttClient.subscribe(MQTT_TOPIC_STATE))
        {
            Serial.print(F("[MQTT] Subscribed to "));
            Serial.println(MQTT_TOPIC_STATE);
        }
        else
        {
            Serial.println(F("[MQTT] Failed to subscribe to antenna/state"));
        }

        if (mqttClient.subscribe(MQTT_TOPIC_HEARTBEAT))
        {
            Serial.print(F("[MQTT] Subscribed to "));
            Serial.println(MQTT_TOPIC_HEARTBEAT);
        }
        else
        {
            Serial.println(F("[MQTT] Failed to subscribe to antenna/heartbeat"));
        }

        lastHeartbeatMs = 0; // reset — must hear from CLUB fresh after reconnect
        return true;
    }
    else
    {
        Serial.print(F("[MQTT] Connection FAILED, rc="));
        Serial.println(mqttClient.state());
        return false;
    }
}

// Incoming MQTT messages — REMOTE receives authoritative CLUB state
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
    if (strcmp(topic, MQTT_TOPIC_HEARTBEAT) == 0)
    {
        lastHeartbeatMs = millis();
        return;
    }

    if (strcmp(topic, MQTT_TOPIC_STATE) != 0)
        return;

    char buf[128];
    if (length >= sizeof(buf))
        length = sizeof(buf) - 1;
    memcpy(buf, payload, length);
    buf[length] = '\0';

    int sel = -1;
    if (sscanf(buf, "{\"selected\":%d", &sel) == 1)
    {
        if (sel >= ANTENNA_MIN && sel <= ANTENNA_MAX)
        {
            Serial.printf("[REMOTE] CLUB confirmed antenna %d\n", sel);

            selectedAntenna    = (uint8_t)sel;
            preSelectedAntenna = (uint8_t)sel;
            awaitingClubConfirm = false;

            updateDialColors();
            updateDynamicCornerLabels();
        }
        else
        {
            Serial.printf("[REMOTE] Invalid antenna in state: %d\n", sel);
        }
    }
    else
    {
        Serial.print(F("[REMOTE] Invalid STATE payload: "));
        Serial.println(buf);
    }
}

// Send a SELECT command to CLUB
void publishSelectCommand(uint8_t target)
{
    if (!mqttEnsureConnected())
    {
        Serial.println(F("[REMOTE] MQTT not connected, cannot publish SELECT"));
        return;
    }

    char payload[80];
    snprintf(payload, sizeof(payload),
             "{\"select\":%u,\"source\":\"%s\"}",
             target, PANEL_ID);

    bool ok = mqttClient.publish(MQTT_TOPIC_CMD, payload);
    Serial.printf("[REMOTE] SELECT %u -> %s  (%s)\n",
                  target, MQTT_TOPIC_CMD, ok ? "OK" : "FAIL");
}

// ============================================================================
//                            SYSTEM INIT HELPERS
// ============================================================================

void initAntennaDial()
{
    Serial.println(F("[INIT] Antenna dial"));

    needleInvalidate();
    loadDotConfig();
    ANTENNA_MAX = numDots;

    updateAllDotPositions();
    drawDialBase();

    lastNeedleIndex = -1;
    updateDialColors();
    updateDynamicCornerLabels();
}

void initEncoderPins()
{
    Serial.println();
    Serial.println(F("───────────────────────────────────────────────────────────────────────────────"));
    pinMode(PIN_ENC_DT,  INPUT_PULLUP);
    pinMode(PIN_ENC_CLK, INPUT_PULLUP);
    pinMode(PIN_ENC_SW,  INPUT_PULLUP);

    int dt  = digitalRead(PIN_ENC_DT);
    int clk = digitalRead(PIN_ENC_CLK);
    int sw  = digitalRead(PIN_ENC_SW);
    encISRlastState = (uint8_t)((dt << 1) | clk);

    Serial.println(F("GPIO configuration and idle signal state:"));
    Serial.printf("   ↳ DT  pin : %-2d   (INPUT)  level=%d  → signal A, %s\n",
                  PIN_ENC_DT, dt, dt ? "idle HIGH (open contact)" : "active LOW (closed to GND)");
    Serial.printf("   ↳ CLK pin : %-2d   (INPUT)  level=%d  → signal B, %s\n",
                  PIN_ENC_CLK, clk, clk ? "idle HIGH (open contact)" : "active LOW (closed to GND)");
    Serial.printf("   ↳ SW  pin : %-2d   (INPUT_PULLUP) level=%d  → push-button, %s\n",
                  PIN_ENC_SW, sw, sw ? "released (HIGH)" : "pressed (LOW)");
    Serial.println();
    Serial.printf("   ↳ Initial encoder state: 0b%02d (DT=%d, CLK=%d)\n", encISRlastState, dt, clk);
    Serial.println();
    Serial.println(F("   ✅ Encoder GPIOs all configured as inputs."));
    Serial.println(F("   Encoder lines read HIGH → mechanical contacts are open (idle state)."));

    attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT),  encISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), encISR, CHANGE);
    Serial.println(F("   ✅ Encoder interrupts attached (DT + CLK)."));
    Serial.println(F("───────────────────────────────────────────────────────────────────────────────"));
    Serial.println();
}

// ============================================================================
//                             ANTENNA DIAL HELPERS
// ============================================================================

void loadDotConfig()
{
    numDots = 11;

    dotAzimuthDeg[1]  =   0.0f;
    dotAzimuthDeg[2]  =  20.0f;
    dotAzimuthDeg[3]  =  45.0f;
    dotAzimuthDeg[4]  =  60.0f;
    dotAzimuthDeg[5]  =  90.0f;
    dotAzimuthDeg[6]  = 135.0f;
    dotAzimuthDeg[7]  = 180.0f;
    dotAzimuthDeg[8]  = 225.0f;
    dotAzimuthDeg[9]  = 275.0f;
    dotAzimuthDeg[10] = 310.0f;
    dotAzimuthDeg[11] = 330.0f;
}

void updateAllDotPositions()
{
    int16_t cx = tft.width()  / 2;
    int16_t cy = tft.height() / 2;

    for (uint8_t i = 1; i <= numDots; i++)
    {
        float rad  = dotAzimuthDeg[i] * PI / 180.0f;
        dotX[i] = cx + (int16_t)round(DIAL_RING_RADIUS * sin(rad));
        dotY[i] = cy - (int16_t)round(DIAL_RING_RADIUS * cos(rad));
    }
}

void drawAntennaDot(uint8_t index, uint16_t color)
{
    if (index < 1 || index > numDots)
        return;

    tft.fillCircle(dotX[index], dotY[index], DIAL_DOT_RADIUS, color);
    tft.drawCircle(dotX[index], dotY[index], DIAL_DOT_RADIUS + 1, TFT_WHITE);
}

void drawDialBase()
{
    for (uint8_t i = 1; i <= numDots; i++)
        drawAntennaDot(i, TftGrey);
}

void updateDialColors()
{
    for (uint8_t i = 1; i <= numDots; i++)
    {
        uint16_t color = TftGrey;

        if (i == selectedAntenna && !awaitingClubConfirm)
            color = TftGreen;

        if (awaitingClubConfirm && i == preSelectedAntenna)
            color = TftYellow;

        drawAntennaDot(i, color);
    }

    uint8_t needleTarget = awaitingClubConfirm ? preSelectedAntenna : selectedAntenna;

    if (lastNeedleIndex != (int)needleTarget)
    {
        drawNeedle(240, 160, dotAzimuthDeg[needleTarget]);
        lastNeedleIndex = needleTarget;
    }
}

// ============================================================================
//                           ROTARY ENCODER LOGIC
// ============================================================================
//
//  REMOTE model:
//   • Preview phase: spinner shows candidate antenna (yellow dot, needle moves)
//   • Commit: SELECT command sent to CLUB; dot turns yellow; needle stays
//   • CLUB confirm (via MQTT): selectedAntenna updated; dot turns green
//
// ============================================================================

void IRAM_ATTR encISR()
{
    uint8_t cur = (uint8_t)((digitalRead(PIN_ENC_DT) << 1) | digitalRead(PIN_ENC_CLK));
    encISRcount += encTable[(encISRlastState << 2) | cur];
    encISRlastState = cur;
}

void updateEncoder()
{
    portDISABLE_INTERRUPTS();
    int16_t delta = encISRcount;
    encISRcount = 0;
    portENABLE_INTERRUPTS();

    bool anyDetent = false;

    if (delta != 0)
    {
        encoderAccum += delta;

        while (encoderAccum >= STEPS_PER_NOTCH || encoderAccum <= -STEPS_PER_NOTCH)
        {
            bool cw;
            if (encoderAccum >= STEPS_PER_NOTCH)
            {
                encoderAccum -= STEPS_PER_NOTCH;
                cw = true;
            }
            else
            {
                encoderAccum += STEPS_PER_NOTCH;
                cw = false;
            }

            // Exit map on first detent
            if (gView == VIEW_MAP)
            {
                encoderAccum = 0;
                gView = VIEW_DIAL;
                displayImageOnTFT(png, "greatcircleMap.png");
                initAntennaDial();
                drawModeIndicator(true);
                updateDynamicCornerLabels(true);
                drawLocalAndUTCTime(240, 10);
                return;
            }

            if (previewAntenna == 0)
                previewAntenna = selectedAntenna;

            previewAntenna = cw
                ? ((previewAntenna >= ANTENNA_MAX) ? ANTENNA_MIN : (previewAntenna + 1))
                : ((previewAntenna <= ANTENNA_MIN) ? ANTENNA_MAX : (previewAntenna - 1));

            anyDetent = true;
        }
    }

    // Redraw dots after all detents
    if (anyDetent)
    {
        lastUserInteractionMs  = millis();
        lastEncoderActivityMs  = millis();

        for (uint8_t i = 1; i <= ANTENNA_MAX; i++)
        {
            if (i == selectedAntenna && !awaitingClubConfirm)
                drawAntennaDot(i, TftGreen);
            else if (i == previewAntenna)
                drawAntennaDot(i, TftYellow);
            else
                drawAntennaDot(i, TftGrey);
        }

        if (lastNeedleIndex != (int)previewAntenna)
        {
            drawNeedle(240, 160, dotAzimuthDeg[previewAntenna]);
            lastNeedleIndex = previewAntenna;
        }

        Serial.printf("[UI ] Preview antenna %u\n", previewAntenna);
    }

    // Commit after settle time → send SELECT to CLUB
    if (previewAntenna != 0 &&
        (millis() - lastEncoderActivityMs) > PREVIEW_SETTLE_MS)
    {
        unsigned long now = millis();
        bool clubOk = mqttClient.connected() && lastHeartbeatMs != 0 &&
                      (now - lastHeartbeatMs) < CLUB_HEARTBEAT_TIMEOUT_MS;

        if (!clubOk)
        {
            Serial.println(F("[REMOTE] Commit blocked — CLUB offline"));
            // leave previewAntenna set so needle stays at preview position
        }
        else if (previewAntenna != selectedAntenna || awaitingClubConfirm)
        {
            Serial.printf("[REMOTE] Commit -> SELECT %u\n", previewAntenna);

            preSelectedAntenna  = previewAntenna;
            awaitingClubConfirm = true;

            updateDialColors();
            updateDynamicCornerLabels();

            publishSelectCommand(previewAntenna);
            previewAntenna = 0;
        }
        else
        {
            previewAntenna = 0;
        }
    }
}

void updateButton()
{
    static int lastButtonSample = HIGH;
    static int stableState      = HIGH;

    int reading = digitalRead(PIN_ENC_SW);

    if (reading != lastButtonSample)
    {
        lastDebounceTime  = millis();
        lastButtonSample  = reading;
    }

    if ((millis() - lastDebounceTime) > debounceDelayMs)
    {
        if (reading != stableState)
        {
            stableState = reading;

            if (stableState == LOW)
            {
                // Re-send current selection as a SELECT command
                Serial.printf("[REMOTE] BUTTON: re-SELECT antenna %u\n", selectedAntenna);
                publishSelectCommand(selectedAntenna);
            }
        }
    }
}

// ============================================================================
//                           CORNER TEXT / OVERLAY
// ============================================================================
void updateDynamicCornerLabels(bool force)
{
    static uint8_t lastAntenna = 255;
    static int     lastAzimuth = -999;
    if (force)
    {
        lastAntenna = 255;
        lastAzimuth = -999;
    }
    int16_t w = tft.width();

    uint8_t ant = preSelectedAntenna;
    int     az  = round(dotAzimuthDeg[ant]);

    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(VERY_LIGHT_GREY, TFT_BLACK);
    tft.setFreeFont(&RobotoMono_Light8pt7b);

    tft.setTextDatum(TL_DATUM);
    tft.drawString("Antenna", 6, 1);

    tft.setTextDatum(TR_DATUM);
    tft.drawString("Azimuth", w - 6, 2);

    tft.setTextFont(4);
    tft.setTextSize(1);

    char bufAnt[8];
    snprintf(bufAnt, sizeof(bufAnt), "%u", ant);

    if (ant != lastAntenna)
    {
        tft.setFreeFont(&RobotoMono_Bold26pt7b);
        tft.setTextDatum(TR_DATUM);

        char bufOld[8];
        snprintf(bufOld, sizeof(bufOld), "%u", lastAntenna);
        tft.setTextColor(TFT_BLACK, TFT_BLACK);
        tft.drawString(bufOld, 50, 24);

        tft.setTextColor(VERY_LIGHT_GREY, TFT_BLACK);
        tft.drawString(bufAnt, 50, 24);

        lastAntenna = ant;
    }

    char bufAz[16];
    snprintf(bufAz, sizeof(bufAz), "%d%c", az, (char)176);

    if (az != lastAzimuth)
    {
        tft.setFreeFont(&RobotoMono_Bold26pt7b);
        tft.setTextDatum(TR_DATUM);

        char bufOld[16];
        snprintf(bufOld, sizeof(bufOld), "%d%c", lastAzimuth, (char)176);
        tft.setTextColor(TFT_BLACK, TFT_BLACK);
        tft.drawString(bufOld, w - 10, 24);

        tft.setTextColor(VERY_LIGHT_GREY, TFT_BLACK);
        tft.drawString(bufAz, w - 10, 24);

        lastAzimuth = az;

        int r0 = 3;
        for (uint8_t i = 0; i < 3; i++)
            tft.drawCircle(w - 7, 26, r0 + i, VERY_LIGHT_GREY);
    }
}

String getStoredCallsign()
{
    String cs;
    if (preferences.begin("config", true))
    {
        cs = preferences.getString("callsign", "");
        preferences.end();
    }
    return cs;
}

// ============================================================================
//                         GREAT-CIRCLE / MAP HELPERS
// ============================================================================

void latLonToXY(float lat_deg, float lon_deg, int16_t &x, int16_t &y)
{
    if (lat_deg >  90.0f) lat_deg =  90.0f;
    if (lat_deg < -90.0f) lat_deg = -90.0f;

    while (lon_deg < -180.0f) lon_deg += 360.0f;
    while (lon_deg >  180.0f) lon_deg -= 360.0f;

    float xf = (lon_deg + 180.0f) / 360.0f * MAP_W;
    float yf = (90.0f - lat_deg)  / 180.0f * MAP_H;

    x = MAP_X0 + (int16_t)roundf(xf);
    y = MAP_Y0 + (int16_t)roundf(yf);
}

void plotGreatCircle(float homeLat_deg, float homeLon_deg,
                     float azimuth_deg,
                     uint16_t color, uint8_t diameter)
{
    if (diameter == 0) diameter = 1;
    uint8_t radius = diameter / 2;

    float lat1   = homeLat_deg * PI / 180.0f;
    float lon1   = homeLon_deg * PI / 180.0f;
    float brg    = azimuth_deg * PI / 180.0f;
    float sinLat1 = sinf(lat1);
    float cosLat1 = cosf(lat1);
    float sinBrg  = sinf(brg);
    float cosBrg  = cosf(brg);

    Serial.printf("Plotting great-circle: lat=%.4f lon=%.4f brg=%.2f°\n",
                  homeLat_deg, homeLon_deg, azimuth_deg);

    float prevLat   = homeLat_deg;
    bool  goingNorth = (azimuth_deg < 90.0f || azimuth_deg > 270.0f);

    for (float theta_deg = 0.0f; theta_deg <= 180.0f; theta_deg += 0.5f)
    {
        float d    = theta_deg * PI / 180.0f;
        float sinD = sinf(d);
        float cosD = cosf(d);

        float lat2 = asinf(sinLat1 * cosD + cosLat1 * sinD * cosBrg);
        lat2 = constrain(lat2, -PI / 2.0f, PI / 2.0f);

        float lon2;
        if (fabsf(sinBrg) < 1e-6f)
        {
            lon2 = lon1;
        }
        else
        {
            float yTerm = sinBrg * sinD * cosLat1;
            float xTerm = cosD - sinLat1 * sinf(lat2);
            lon2 = lon1 + atan2f(yTerm, xTerm);
            if (lon2 < -PI)       lon2 += 2.0f * PI;
            else if (lon2 > PI)   lon2 -= 2.0f * PI;
        }

        float lat2_deg = lat2 * 180.0f / PI;
        float lon2_deg = lon2 * 180.0f / PI;

        int16_t x, y;
        latLonToXY(lat2_deg, lon2_deg, x, y);

        if (x >= 0 && x < tft.width() && y >= 0 && y < tft.height())
            tft.fillCircle(x, y, radius, color);

        if ((fabsf(azimuth_deg) < 1.0f || fabsf(azimuth_deg - 180.0f) < 1.0f))
        {
            if (( goingNorth && lat2_deg < prevLat) ||
                (!goingNorth && lat2_deg > prevLat))
                break;
            prevLat = lat2_deg;
        }

        if (fabsf(lat2_deg) >= 89.9f)
            break;
    }
}

void updateMapBottomLabel()
{
    float az = dotAzimuthDeg[preSelectedAntenna];

    char buf[32];
    snprintf(buf, sizeof(buf), "Antenna %u (%.0f deg.)", preSelectedAntenna, az);

    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.fillRect(0, 280, tft.width(), 40, TFT_BLACK);
    tft.drawString(buf, tft.width() / 2, 295);
}

void drawRemoteStationLabel()
{
    char line[40];
    snprintf(line, sizeof(line), "Station %s", PANEL_ID);

    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(line, tft.width() / 2, 310);
}

void performFactoryResetAll()
{
    int16_t w = tft.width();
    int16_t h = tft.height();

    Serial.println(F("[RESET] Performing factory reset: erasing ALL NVS."));

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Factory Reset...",         w / 2, h / 2 - 20);
    tft.drawString("Erasing User Configuration", w / 2, h / 2 + 20);

    esp_err_t err = nvs_flash_erase();
    Serial.printf("[RESET] nvs_flash_erase(): %s\n", (err == ESP_OK) ? "OK" : "ERROR");
    err = nvs_flash_init();
    Serial.printf("[RESET] nvs_flash_init():  %s\n", (err == ESP_OK) ? "OK" : "ERROR");

    delay(2000);
    tft.fillScreen(TFT_BLACK);
    tft.drawString("Restarting...", w / 2, h / 2);
    delay(2000);
    ESP.restart();
}

void checkAndApplyTFTCalibrationData(bool recalibrate)
{
    Serial.println(F("───────────────────────────────────────────────────────────────────────────────"));
    Serial.println(F("[INIT] 🖥️  TFT Touch Calibration Check"));
    Serial.println(F("───────────────────────────────────────────────────────────────────────────────"));

    if (recalibrate)
    {
        calibrateTFTscreen();
        return;
    }

    uint16_t calibrationData[5];
    bool     dataValid = true;

    preferences.begin("TFT", true);
    for (int i = 0; i < 5; i++)
    {
        calibrationData[i] = preferences.getUInt(("calib" + String(i)).c_str(), 0xFFFF);
        if (calibrationData[i] == 0xFFFF)
        {
            dataValid = false;
            Serial.printf("   ⚠️  calib%d = 0xFFFF → missing\n", i);
        }
        else
        {
            Serial.printf("   ✓ calib%d = %u\n", i, calibrationData[i]);
        }
    }
    preferences.end();

    if (dataValid)
    {
        tft.setTouch(calibrationData);
        Serial.println(F("✅ TFT touch calibration applied."));
    }
    else
    {
        Serial.println(F("❌ Calibration invalid — starting recalibration..."));
        digitalWrite(TFT_BLP, HIGH);
        calibrateTFTscreen();
    }
    Serial.println(F("───────────────────────────────────────────────────────────────────────────────\n"));
}

void calibrateTFTscreen()
{
    uint16_t calibrationData[5];

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_BLACK, TFT_GOLD);
    tft.setFreeFont(&FreeSansBold12pt7b);
    tft.setCursor(10, 22);
    tft.fillRect(0, 0, 480, 30, TFT_GOLD);
    tft.print("   TFT TOUCHSCREEN CALIBRATION");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setFreeFont(&FreeSans12pt7b);

    String instructions[] = {
        "Welcome to the initial touchscreen calibration.",
        "This procedure will only be required once.",
        "On the next screen you will see arrows",
        "appearing one after the other at each corner.",
        "Just tap them until completion.",
        "",
        "Tap anywhere on the screen to begin"};

    int16_t yPos = 100;
    for (String line : instructions)
    {
        int16_t xPos = (tft.width() - tft.textWidth(line)) / 2;
        tft.setCursor(xPos, yPos);
        tft.print(line);
        yPos += tft.fontHeight();
    }

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor((tft.width() - tft.textWidth(instructions[6])) / 2, yPos - tft.fontHeight());
    tft.print(instructions[6]);

    while (true) { uint16_t x, y; if (tft.getTouch(&x, &y)) break; }

    tft.fillScreen(TFT_BLACK);
    tft.calibrateTouch(calibrationData, TFT_GREEN, TFT_BLACK, 12);

    preferences.begin("TFT", false);
    for (int i = 0; i < 5; i++)
        preferences.putUInt(("calib" + String(i)).c_str(), calibrationData[i]);
    preferences.end();

    // Quick test
    tft.fillScreen(TFT_BLACK);
    uint16_t x, y;
    int16_t lastX = -1, lastY = -1;
    String  lastResult = "";

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(4);
    String text = "Tap anywhere to test";
    tft.setCursor((tft.width() - tft.textWidth(text)) / 2, 8);
    tft.print(text);

    text = "Click Here to Exit";
    int16_t btn_w = tft.textWidth(text) + 20;
    int16_t btn_h = tft.fontHeight() + 12;
    int16_t btn_x = (tft.width() - btn_w) / 2;
    int16_t btn_y = 280;
    int16_t btn_r = 10;

    tft.fillRoundRect(btn_x, btn_y, btn_w, btn_h, btn_r, TFT_BLUE);
    tft.drawRoundRect(btn_x, btn_y, btn_w, btn_h, btn_r, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_BLUE);
    tft.setCursor((tft.width() - tft.textWidth(text)) / 2, btn_y + 10);
    tft.print(text);

    while (true)
    {
        if (tft.getTouch(&x, &y))
        {
            String result = "x=" + String(x) + "  y=" + String(y);
            lastUserInteractionMs = millis();

            tft.setTextColor(TFT_BLACK, TFT_BLACK);
            tft.setCursor(lastX, lastY);
            tft.print(lastResult);

            int16_t textWidth = tft.textWidth(result);
            int16_t xPos = (tft.width()  - textWidth)      / 2;
            int16_t yPos = (tft.height() - tft.fontHeight()) / 2;

            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.setCursor(xPos, yPos);
            tft.print(result);

            lastX = xPos; lastY = yPos; lastResult = result;
            tft.fillCircle(x, y, 2, TFT_RED);

            if (x > btn_x && x < btn_x + btn_w && y > btn_y && y < btn_y + btn_h)
                break;
        }
    }
    tft.setTextSize(2);
    tft.fillScreen(TFT_BLACK);
}

void encodertestTFT()
{
    // (identical to CLUB — retained for hardware debug use)
    digitalWrite(TFT_BLP, HIGH);
    Serial.println("=== Encoder test mode (TFT ring animation) ===");

    uint16_t ledOff    = tft.color565(70, 70, 70);
    uint16_t ledOn     = TFT_GREEN;
    uint16_t ledCenter = TFT_YELLOW;

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(2);
    tft.drawCentreString("Encoder Test Mode", tft.width() / 2, 10, 4);

    const uint8_t NUM_LEDS = 16;
    const int16_t cx = tft.width()  / 2;
    const int16_t cy = tft.height() / 2 + 10;
    const int16_t RADIUS = 90;
    const uint8_t LED_R  = 9;

    int16_t ledX[NUM_LEDS], ledY[NUM_LEDS];
    for (uint8_t i = 0; i < NUM_LEDS; i++)
    {
        float a = (360.0f / NUM_LEDS) * i * PI / 180.0f;
        ledX[i] = cx + RADIUS * sin(a);
        ledY[i] = cy - RADIUS * cos(a);
    }
    for (uint8_t i = 0; i < NUM_LEDS; i++)
        tft.fillCircle(ledX[i], ledY[i], LED_R, ledOff);

    int dt  = digitalRead(PIN_ENC_DT);
    int clk = digitalRead(PIN_ENC_CLK);
    uint8_t lastState  = (dt << 1) | clk;
    int8_t  encAccum   = 0;
    int8_t  ledIndex   = 0;
    unsigned long counter = 0;

    tft.fillCircle(ledX[ledIndex], ledY[ledIndex], LED_R, ledOn);

    while (1)
    {
        dt  = digitalRead(PIN_ENC_DT);
        clk = digitalRead(PIN_ENC_CLK);
        uint8_t currentState = (dt << 1) | clk;
        uint8_t index        = (lastState << 2) | currentState;
        int8_t  movement     = encTable[index];

        if (movement != 0)
        {
            encAccum  += movement;
            lastState  = currentState;

            if (encAccum >= STEPS_PER_NOTCH)
            {
                encAccum = 0; counter++;
                tft.fillCircle(ledX[ledIndex], ledY[ledIndex], LED_R, ledOff);
                ledIndex = (ledIndex + 1) % NUM_LEDS;
                tft.fillCircle(ledX[ledIndex], ledY[ledIndex], LED_R, ledOn);
                Serial.printf("%lu CW\n", counter);
            }
            else if (encAccum <= -STEPS_PER_NOTCH)
            {
                encAccum = 0; counter++;
                tft.fillCircle(ledX[ledIndex], ledY[ledIndex], LED_R, ledOff);
                ledIndex = (ledIndex - 1 + NUM_LEDS) % NUM_LEDS;
                tft.fillCircle(ledX[ledIndex], ledY[ledIndex], LED_R, ledOn);
                Serial.printf("%lu CCW\n", counter);
            }
        }
        else
        {
            lastState = currentState;
        }

        if (digitalRead(PIN_ENC_SW) == LOW)
        {
            for (int r = 5; r <= 20; r += 3) { tft.fillCircle(cx, cy, r, ledCenter); delay(20); }
            delay(80);
            for (int r = 20; r >= 5; r -= 3) { tft.fillCircle(cx, cy, r, TFT_BLACK); delay(20); }
            for (uint8_t i = 0; i < NUM_LEDS; i++)
                tft.fillCircle(ledX[i], ledY[i], LED_R, ledOff);
            tft.fillCircle(ledX[ledIndex], ledY[ledIndex], LED_R, ledOn);
            delay(200);
        }
        delay(2);
    }
}

// ============================================================================
//                          STATUS INDICATORS
// ============================================================================

void drawWiFiIndicator()
{
    int16_t x = 20;
    int16_t y = 310;
    int barWidth  = 5;
    int barHeight = 5;
    int barGap    = 3;

    tft.fillRect(x - 3, y - 20, 60, 28, TFT_BLACK);

    if (WiFi.status() != WL_CONNECTED)
    {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.setTextFont(2);
        tft.drawString("x", x + 10, y - 8);
        tft.setTextFont(1);
        tft.drawString("No WiFi", x, y);
        return;
    }

    long rssi = WiFi.RSSI();
    rssi = constrain(rssi, -90, -40);

    float t = (float)(rssi + 90) / 50.0f;
    uint8_t r, g, b;
    if (t < 0.5f)
    {
        t *= 2.0f;
        r = 255; g = (uint8_t)(t * 255); b = 0;
    }
    else
    {
        t = (t - 0.5f) * 2.0f;
        r = (uint8_t)(255 * (1.0f - t)); g = 255; b = 0;
    }
    uint16_t color = tft.color565(r, g, b);

    uint8_t level = 0;
    if      (rssi > -55) level = 4;
    else if (rssi > -65) level = 3;
    else if (rssi > -75) level = 2;
    else if (rssi > -85) level = 1;

    for (uint8_t i = 0; i < 4; i++)
    {
        int barH = (i + 1) * barHeight;
        int bx   = x + i * (barWidth + barGap);
        int by   = y - barH - 5;

        if (i < level)
            tft.fillRect(bx, by, barWidth, barH, color);
        else
        {
            tft.fillRect(bx, by, barWidth, barH, TFT_BLACK);
            tft.drawRect(bx, by, barWidth, barH, TFT_DARKGREY);
        }
    }

    char buf[12];
    snprintf(buf, sizeof(buf), "%ld dB", rssi);
    tft.setTextFont(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawCentreString(buf, x + 20, y + 3, 1);
}

void drawMQTTIndicator()
{
    int16_t w  = tft.width();
    int16_t cx = w - 20;
    int16_t cy = 302; // bottom-right, fixed position (no keypad button on REMOTE)
    uint8_t r  = 6;

    tft.drawCircle(cx, cy, r + 2, TFT_WHITE);

    static unsigned long lastBlink  = 0;
    static bool          blinkState = true;
    unsigned long now = millis();

    bool wifiOk = HB9IIUPortal::isConnected();
    bool mqttOk = mqttClient.connected();
    bool clubOk = mqttOk && lastHeartbeatMs != 0 &&
                  (now - lastHeartbeatMs) < CLUB_HEARTBEAT_TIMEOUT_MS;

    // Determine color + blink interval (0 = solid)
    uint16_t fillColor   = TFT_RED;
    uint16_t blinkPeriod = 0; // 0 = solid

    if (!wifiOk)
    {
        fillColor   = TFT_RED;      // no WiFi — solid red
    }
    else if (!mqttOk)
    {
        fillColor   = TFT_RED;      // MQTT down — blinking red
        blinkPeriod = 400;
    }
    else if (!clubOk)
    {
        fillColor   = TFT_ORANGE;   // CLUB offline — solid orange
    }
    else if (awaitingClubConfirm)
    {
        fillColor   = TFT_YELLOW;   // waiting for confirm — blinking yellow
        blinkPeriod = 300;
    }
    else
    {
        fillColor   = TFT_GREEN;    // all good — solid green
    }

    // Advance blink state
    if (blinkPeriod == 0)
    {
        blinkState = true; // solid
    }
    else if (now - lastBlink >= blinkPeriod)
    {
        blinkState = !blinkState;
        lastBlink  = now;
    }

    tft.fillCircle(cx, cy, r, blinkState ? fillColor : TFT_BLACK);
}

// ============================================================================
//                            STARTUP DIAGNOSTICS
// ============================================================================

void displayIntroBanner()
{
    Serial.println();
    Serial.println(F("╔══════════════════════════════════════════════════════════════════════════╗"));
    Serial.println(F("║                                                                          ║"));
    Serial.println(F("║                 🛰️  SMART BEVERAGE ANTENNA SELECTOR                       ║"));
    Serial.println(F("║                               by HB9IIU                                  ║"));
    Serial.println(F("║                                                                          ║"));
    Serial.println(F("║                           📡 REMOTE STATION                              ║"));
    Serial.println(F("║                                                                          ║"));
    Serial.println(F("╚══════════════════════════════════════════════════════════════════════════╝"));
}

void printSystemInfo()
{
    Serial.println();
    Serial.println(F("───────────────────────────────────────────────────────────────────────────────"));
    Serial.println(F("[SYS] 📊  SYSTEM INFORMATION SUMMARY"));
    Serial.println(F("───────────────────────────────────────────────────────────────────────────────"));

    uint32_t flashSize   = ESP.getFlashChipSize();
    uint32_t sketchUsed  = ESP.getSketchSize();
    uint32_t sketchFree  = ESP.getFreeSketchSpace();
    uint32_t heapFree    = ESP.getFreeHeap();
    uint32_t heapTotal   = ESP.getHeapSize();

    float flashUsedPct = (flashSize  > 0) ? (100.0f * sketchUsed / flashSize)              : 0.0f;
    float heapUsedPct  = (heapTotal  > 0) ? (100.0f * (heapTotal - heapFree) / heapTotal)  : 0.0f;

    const char *flashVerdict = (flashUsedPct < 70.0f) ? "✅ OK" : (flashUsedPct < 85.0f) ? "⚠️  Moderate" : "❌ HIGH";
    const char *heapVerdict  = (heapUsedPct  < 60.0f) ? "✅ OK" : (heapUsedPct  < 80.0f) ? "⚠️  Moderate" : "❌ LOW";

    Serial.printf("ESP32 Chip Model     : %s\n",   ESP.getChipModel());
    Serial.printf("CPU Cores            : %d\n",   ESP.getChipCores());
    Serial.printf("CPU Frequency        : %d MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("Flash Chip Size      : %-10u bytes\n", flashSize);
    Serial.printf("Sketch Size (used)   : %-10u bytes  (%.1f%%)  → %s\n", sketchUsed, flashUsedPct, flashVerdict);
    Serial.printf("Free Sketch Space    : %-10u bytes\n", sketchFree);
    Serial.printf("Heap (free/total)    : %u / %u bytes  (used %.1f%%)  → %s\n",
                  heapFree, heapTotal, heapUsedPct, heapVerdict);

    Serial.println(F("───────────────────────────────────────────────────────────────────────────────"));
    if (flashUsedPct < 70.0f && heapUsedPct < 60.0f)
        Serial.println(F("✅ System Health Summary : OK"));
    else if (flashUsedPct < 85.0f && heapUsedPct < 80.0f)
        Serial.println(F("⚠️  System Health Summary : Moderate"));
    else
        Serial.println(F("❌ System Health Summary : Critical"));
    Serial.println(F("───────────────────────────────────────────────────────────────────────────────"));
}

void mountAndListFiles()
{
    Serial.println(F("───────────────────────────────────────────────────────────────────────────────"));
    Serial.println(F("[INIT] 📂 LittleFS Mount & File Listing"));
    Serial.println(F("───────────────────────────────────────────────────────────────────────────────"));

    if (!LittleFS.begin(true))
    {
        Serial.println(F("❌ LittleFS mount FAILED"));
        return;
    }

    Serial.println(F("OK"));
    Serial.println(F("📄 Listing filesystem contents:"));
    Serial.println(F("---------------------------------------------------------------"));

    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) { Serial.println(F("❌ Failed to open root.")); return; }

    File     file      = root.openNextFile();
    uint16_t fileCount = 0;
    uint32_t totalSize = 0;

    while (file)
    {
        fileCount++;
        totalSize += file.size();
        if (file.isDirectory())
            Serial.printf("📁  %-24s\n", file.name());
        else
            Serial.printf("📄  %-24s  %8u bytes\n", file.name(), file.size());
        file = root.openNextFile();
    }

    Serial.println(F("---------------------------------------------------------------"));
    Serial.printf("📊 %u entries, %u bytes total\n", fileCount, totalSize);
    Serial.println(F("✅ LittleFS mounted and ready."));
    Serial.println(F("───────────────────────────────────────────────────────────────────────────────\n"));
}

bool waitForTimeVerbose(uint32_t timeoutMs)
{
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();

    time_t   now;
    uint32_t start = millis();

    Serial.println(F("───────────────────────────────────────────────────────────────────────────────"));
    Serial.println(F("[TIME] 🕒 SNTP Time Synchronization"));
    Serial.println(F("───────────────────────────────────────────────────────────────────────────────"));
    Serial.printf("[NET ] 📡 IP  : %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[NET ] 🌐 DNS : %s\n", WiFi.dnsIP().toString().c_str());

    Serial.print(F("[TIME] ⏳ Waiting for valid epoch"));

    while (millis() - start < timeoutMs)
    {
        time(&now);
        Serial.print(F(" · ")); Serial.print(now);

        if (now > 1700000000)
        {
            tm t;
            Serial.println();
            gmtime_r(&now, &t);
            Serial.printf("UTC   : %04d-%02d-%02d %02d:%02d:%02d\n",
                          t.tm_year+1900, t.tm_mon+1, t.tm_mday,
                          t.tm_hour, t.tm_min, t.tm_sec);
            localtime_r(&now, &t);
            Serial.printf("LOCAL : %04d-%02d-%02d %02d:%02d:%02d\n",
                          t.tm_year+1900, t.tm_mon+1, t.tm_mday,
                          t.tm_hour, t.tm_min, t.tm_sec);
            Serial.println(F("[TIME] ✅ SNTP synchronization successful"));
            Serial.println(F("───────────────────────────────────────────────────────────────────────────────\n"));
            return true;
        }
        delay(1000);
    }

    Serial.println();
    Serial.println(F("[TIME] ❌ SNTP synchronization TIMEOUT"));
    Serial.println(F("───────────────────────────────────────────────────────────────────────────────\n"));
    return false;
}

void printTimes()
{
    time_t now; tm t;
    time(&now);
    gmtime_r(&now, &t);
    Serial.printf("UTC   : %04d-%02d-%02d %02d:%02d:%02d\n",
                  t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    localtime_r(&now, &t);
    Serial.printf("LOCAL : %04d-%02d-%02d %02d:%02d:%02d\n",
                  t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

void drawLocalAndUTCTime(int16_t y, int16_t margin)
{
    time_t now; tm t;
    time(&now);
    y = y - 20;

    tft.setTextFont(1);
    tft.setFreeFont(&RobotoMono_Medium13pt7b);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    const int lineSpacing = 28;
    const int timeY = y + lineSpacing;
    const int timeH = tft.fontHeight() + 4;
    const int timeW = tft.textWidth("88:88") + 6;

    tft.setTextDatum(TL_DATUM);
    tft.drawString("Local", margin, y);
    tft.setTextDatum(TR_DATUM);
    tft.drawString("UTC", tft.width() - margin - 12, y);

    localtime_r(&now, &t);
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    tft.fillRect(margin - 2, timeY - 2, timeW, timeH - 10, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(buf, margin, timeY);

    gmtime_r(&now, &t);
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    int xRight = tft.width() - margin;
    tft.fillRect(xRight - timeW + 2, timeY - 2, timeW + 4, timeH - 10, TFT_BLACK);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(buf, xRight, timeY);

    tft.setTextDatum(TL_DATUM);
}

void updateTimeIfMinuteChanged()
{
    time_t now; tm t;
    time(&now);
    localtime_r(&now, &t);

    if (t.tm_min != lastDrawnMinute)
    {
        lastDrawnMinute = t.tm_min;
        drawLocalAndUTCTime(240, 10);
    }
}

void printMQTTConnectionOK()
{
    time_t now; tm t;
    time(&now);
    localtime_r(&now, &t);

    char ts[16];
    snprintf(ts, sizeof(ts), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    Serial.printf("[MQTT] Connection OK @ %s\n", ts);
}
