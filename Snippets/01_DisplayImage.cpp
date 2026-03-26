#include <TFT_eSPI.h> // Bodmer's TFT display library
//#include <SPI.h>
#include <Preferences.h> // For saving calibration data
#include <PNGdec.h>      // Include the PNG decoder library
#include "logo.h"        // Image is stored here in an 8-bit array  https://notisrac.github.io/FileToCArray/ (select treat as binary)
#include "club.h"        // Image is stored here in an 8-bit array  https://notisrac.github.io/FileToCArray/ (select treat as binary)
#include "remote.h"      // Image is stored here in an 8-bit array  https://notisrac.github.io/FileToCArray/ (select treat as binary)

Preferences preferences;
TFT_eSPI tft = TFT_eSPI(); // TFT instance
PNG png;                   // PNG decoder instance

// Prototypes
void checkAndApplyTFTCalibrationData(bool recalibrate);
void calibrateTFTscreen();
void displayPicture(const char *imageName);
void testTouchscreen();

// Setup
void setup()
{
    Serial.begin(115200);

    // --- Backlight control sequence ---
    pinMode(TFT_BLP, OUTPUT);

    // --- TFT setup ---
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    checkAndApplyTFTCalibrationData(false); // set to true to force recalibration
    displayPicture("logo");
    digitalWrite(TFT_BLP, HIGH);
    delay(1000);
    displayPicture("remote");
    delay(1000);
    displayPicture("club");
}

void loop()
{
    testTouchscreen();
}

void checkAndApplyTFTCalibrationData(bool recalibrate)
{
    if (recalibrate)
    {
        calibrateTFTscreen();
        return;
    }

    uint16_t calibrationData[5];
    preferences.begin("TFT", true);
    bool dataValid = true;

    for (int i = 0; i < 5; i++)
    {
        calibrationData[i] = preferences.getUInt(("calib" + String(i)).c_str(), 0xFFFF);
        if (calibrationData[i] == 0xFFFF)
        {
            dataValid = false;
        }
    }
    preferences.end();

    if (dataValid)
    {
        tft.setTouch(calibrationData);
        Serial.println("Calibration data applied.");
    }
    else
    {
        Serial.println("Invalid calibration data. Recalibrating...");
        calibrateTFTscreen();
    }
}

void calibrateTFTscreen()
{
    uint16_t calibrationData[5];

    // Display recalibration message
    tft.fillScreen(TFT_BLACK);             // Clear screen
    tft.setTextColor(TFT_BLACK, TFT_GOLD); // Set text color (black on gold)
    tft.setFreeFont(&FreeSansBold12pt7b);  // Use custom free font
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

    while (true)
    {
        uint16_t x, y;
        if (tft.getTouch(&x, &y))
        {
            break;
        }
    }

    tft.fillScreen(TFT_BLACK);

    tft.calibrateTouch(calibrationData, TFT_GREEN, TFT_BLACK, 12);

    preferences.begin("TFT", false);
    for (int i = 0; i < 5; i++)
    {
        preferences.putUInt(("calib" + String(i)).c_str(), calibrationData[i]);
    }
    preferences.end();

    // Test calibration with dynamic feedback
    tft.fillScreen(TFT_BLACK);
    uint16_t x, y;
    int16_t lastX = -1, lastY = -1;
    String lastResult = "";

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
            String result = "x=" + String(x) + "  y=" + String(y) + "  p=" + String(tft.getTouchRawZ());

            tft.setTextColor(TFT_BLACK, TFT_BLACK);
            tft.setCursor(lastX, lastY);
            tft.print(lastResult);

            int16_t textWidth = tft.textWidth(result);
            int16_t xPos = (tft.width() - textWidth) / 2;
            int16_t yPos = (tft.height() - tft.fontHeight()) / 2;

            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.setCursor(xPos, yPos);
            tft.print(result);

            lastX = xPos;
            lastY = yPos;
            lastResult = result;

            tft.fillCircle(x, y, 2, TFT_RED);

            if (x > btn_x && x < btn_x + btn_w && y > btn_y && y < btn_y + btn_h)
            {
                Serial.println("Exit Touched");
                break;
            }
        }
    }
    tft.setTextSize(2);
    tft.fillScreen(TFT_BLACK);
}

void displayPicture(const char *imageName)
{
    const uint8_t *imageData = nullptr;
    uint32_t imageSize = 0;

    if (strcmp(imageName, "logo") == 0)
    {
        imageData = logo;
        imageSize = sizeof(logo);
    }
    else if (strcmp(imageName, "club") == 0)
    {
        imageData = club;
        imageSize = sizeof(club);
    }
    else if (strcmp(imageName, "remote") == 0)
    {
        imageData = remote;
        imageSize = sizeof(remote);
    }
    else
    {
        Serial.printf("Unknown image name '%s', defaulting to logo\n", imageName);
        imageData = logo;
        imageSize = sizeof(logo);
    }

    // ✅ Static draw buffer (safe for stack)
    static uint16_t lineBuffer[480];

    // ✅ Store TFT and PNG references statically for callback access
    static TFT_eSPI *tftPtr = &tft;
    static PNG *pngPtr = &png;

    // ✅ Local callback defined as a static function (inside this function’s scope)
    auto drawCallback = [](PNGDRAW *pDraw)
    {
        pngPtr->getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_BIG_ENDIAN, 0xFFFFFFFF);
        tftPtr->pushImage(0, pDraw->y, pDraw->iWidth, 1, lineBuffer);
    };

    // ✅ Pass the callback to PNG decoder
    int16_t rc = png.openFLASH((uint8_t *)imageData, imageSize, drawCallback);

    if (rc == PNG_SUCCESS)
    {
        Serial.printf("Opened PNG '%s' successfully\n", imageName);
        Serial.printf("Image specs: (%d x %d), %d bpp, pixel type: %d\n",
                      png.getWidth(), png.getHeight(),
                      png.getBpp(), png.getPixelType());

        tft.startWrite();
        uint32_t dt = millis();
        rc = png.decode(NULL, 0);
        Serial.printf("Displayed in %lu ms\n", millis() - dt);
        tft.endWrite();
        png.close();
    }
    else
    {
        Serial.printf("Failed to open PNG '%s' (error %d)\n", imageName, rc);
    }

    digitalWrite(TFT_BLP, HIGH);
}

void testTouchscreen()
{
    uint16_t x, y;
    if (tft.getTouch(&x, &y))
    {
        // Draw red touch dot
        tft.fillCircle(x, y, 4, TFT_RED);

        // Show coordinates at top center
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.setTextSize(2);
        char buf[40];
        sprintf(buf, "x=%d  y=%d", x, y);
        tft.fillRect(0, 0, tft.width(), 30, TFT_BLACK);
        tft.drawString(buf, tft.width() / 2, 10);
        Serial.printf("🖱️  Touch at: x=%d y=%d\n", x, y);
    }

    delay(50);
}