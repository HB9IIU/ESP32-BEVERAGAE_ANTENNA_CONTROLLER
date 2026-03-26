# ESP32 (esp32dev) GPIO / Pin Assignment Table (Current Project)

This table reflects:
- **TFT_eSPI build_flags** (TFT + Touch pins) from your PlatformIO `platformio.ini`
- **DTMF + PCF8574 + Encoder + Pushbuttons** pins from your attached code

PCF8574/PCF8574A:
- Address lines **A0/A1/A2 = GND** → **I²C address = 0x20** (7-bit)

---

+------+------------------------+-----------------------------+-----------------------+--------------------------------------------+
| GPIO | STATUS                 | USED FOR                    | SUBSYSTEM / PROTOCOL  | NOTES                                       |
+------+------------------------+-----------------------------+-----------------------+--------------------------------------------+
|  0   | FREE (STRAP)           | -                           | -                     | Boot strap pin; avoid unless necessary      |
|  1   | USED                   | Serial TX (USB console)     | UART0                 | Keep for debug                              |
|  2   | FREE (STRAP)           | -                           | -                     | Boot strap pin (often onboard LED)          |
|  3   | USED                   | Serial RX (USB console)     | UART0                 | Keep for debug                              |
|  4   | USED (STRAP)           | TFT MISO                    | TFT SPI (MISO)        | Strap pin but commonly works                |
|  5   | USED (STRAP)           | TFT MOSI                    | TFT SPI (MOSI)        | Strap pin but commonly works                |
|  6   | RESERVED (FLASH)       | internal flash              | ESP32 core            | DO NOT USE                                  |
|  7   | RESERVED (FLASH)       | internal flash              | ESP32 core            | DO NOT USE                                  |
|  8   | RESERVED (FLASH)       | internal flash              | ESP32 core            | DO NOT USE                                  |
|  9   | RESERVED (FLASH)       | internal flash              | ESP32 core            | DO NOT USE                                  |
| 10   | RESERVED (FLASH)       | internal flash              | ESP32 core            | DO NOT USE                                  |
| 11   | RESERVED (FLASH)       | internal flash              | ESP32 core            | DO NOT USE                                  |
| 12   | FREE (STRAP)           | -                           | -                     | Risky strap pin (boot mode)                 |
| 13   | FREE (SAFE)            | -                           | -                     | Good spare GPIO                             |
| 14   | USED                   | DTMF chip select (CS)       | HT9200A (CS)          | (`PIN_CS 14`)                               |
| 15   | USED (STRAP)           | TFT clock                   | TFT SPI (SCLK)        | Strap pin; must be stable at boot           |
| 16   | USED                   | Touch controller CS         | Touch SPI (CS)        | `TOUCH_CS=16`                               |
| 17   | USED                   | TFT backlight enable        | TFT backlight         | `TFT_BLP=17`                                |
| 18   | USED                   | TFT data/command select     | TFT_eSPI control      | `TFT_DC=18`                                 |
| 19   | USED                   | TFT reset                   | TFT_eSPI control      | `TFT_RST=19`                                |
| 21   | USED                   | I²C data (SDA)              | PCF8574(A)            | (`PIN_SDA 21`)                              |
| 22   | USED                   | I²C clock (SCL)             | PCF8574(A)            | (`PIN_SCL 22`)                              |
| 23   | USED                   | TFT chip select             | TFT SPI (CS)          | `TFT_CS=23`                                 |
| 25   | USED                   | Encoder push button         | UI input              | (`PIN_ENC_SW 25`)                           |
| 26   | USED                   | DTMF data (DIN)             | HT9200A (DIN)         | (`PIN_DIN 26`)                              |
| 27   | USED                   | DTMF clock (CLK)            | HT9200A (CLK)         | (`PIN_CLK 27`)                              |
| 32   | USED                   | Encoder phase (CLK)         | Rotary encoder        | (`PIN_ENC_CLK 32`)                          |
| 33   | USED                   | Encoder phase (DT)          | Rotary encoder        | (`PIN_ENC_DT 33`)                           |
| 34   | USED (INPUT-ONLY)      | Pushbutton RIGHT            | UI input              | Input-only GPIO; no internal pullups        |
| 35   | USED (INPUT-ONLY)      | Pushbutton LEFT             | UI input              | Input-only GPIO; no internal pullups        |
| 36   | FREE (INPUT-ONLY)      | -                           | -                     | Input-only / ADC (SVP)                      |
| 39   | FREE (INPUT-ONLY)      | -                           | -                     | Input-only / ADC (SVN)                      |
+------+------------------------+-----------------------------+-----------------------+--------------------------------------------+

---
