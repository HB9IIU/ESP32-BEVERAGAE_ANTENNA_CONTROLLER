#include "HB9IIUsimplePngDisplay.h"

#include <TFT_eSPI.h>
#include <PNGdec.h>
#include <LittleFS.h>


// ------------------------------------------------
// Internal objects
// ------------------------------------------------
static File pngFile;

static struct {
  int16_t x;
  int16_t y;
} pngCtx;

// ------------------------------------------------
// TFT instance provided by the sketch
// ------------------------------------------------
extern TFT_eSPI tft;

// ------------------------------------------------
// PNGdec callbacks
// ------------------------------------------------
static void* pngOpen(const char* filename, int32_t* size)
{
  pngFile = LittleFS.open(filename, "r");
  if (!pngFile) return nullptr;
  *size = pngFile.size();
  return &pngFile;
}

static void pngClose(void* handle)
{
  if (pngFile) pngFile.close();
}

static int32_t pngRead(PNGFILE* handle, uint8_t* buffer, int32_t length)
{
  return pngFile.read(buffer, length);
}

static int32_t pngSeek(PNGFILE* handle, int32_t position)
{
  return pngFile.seek(position);
}

// Pointer to the PNG object for the callback
static PNG* g_pngPtr = nullptr;

static void pngDraw(PNGDRAW* pDraw)
{
  uint16_t lineBuffer[pDraw->iWidth];

  if (g_pngPtr) {
    g_pngPtr->getLineAsRGB565(
      pDraw,
      lineBuffer,
      PNG_RGB565_BIG_ENDIAN,
      0xFFFFFFFF
    );
  }

  tft.pushImage(
    pngCtx.x,
    pngCtx.y + pDraw->y,
    pDraw->iWidth,
    1,
    lineBuffer
  );
}

// ------------------------------------------------
// Public API
// ------------------------------------------------
bool displayImageOnTFT(PNG& png, const char* filename, int16_t x, int16_t y)
{
  char path[64];

  // Normalize path (allow "logo.png" or "/logo.png")
  if (filename[0] == '/')
    strncpy(path, filename, sizeof(path));
  else {
    path[0] = '/';
    strncpy(path + 1, filename, sizeof(path) - 1);
  }
  path[sizeof(path) - 1] = 0;

  pngCtx.x = x;
  pngCtx.y = y;

  // Set the global pointer for the callback
  g_pngPtr = &png;

  int rc = png.open(
    path,
    pngOpen,
    pngClose,
    pngRead,
    pngSeek,
    pngDraw
  );

  if (rc != PNG_SUCCESS) {
    g_pngPtr = nullptr;
    return false;
  }

  tft.startWrite();
  png.decode(nullptr, 0);
  tft.endWrite();

  png.close();
  g_pngPtr = nullptr;
  return true;
}
