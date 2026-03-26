// ESP32 Demo: Animate a rotating needle over a PNG background using TFT_eSPI + PNGdec + LittleFS
//
// Key idea:
// - Draw the PNG background once
// - Each frame: restore ONLY the previous needle area by redrawing the PNG region (clipped decode)
// - Draw the needle (triangle + hub) and store its bounding box for the next restore

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <PNGdec.h>
#include "HB9IIUsimplePngDisplay.h"
#include <FS.h>
#include <LittleFS.h>
#include <math.h>

// ============================================================================
// CONFIG
// ============================================================================

// Needle appearance
static const int      NEEDLE_L    = 144;        // Needle length (px)
static const int      BASE_W      = 9;          // Needle base width at pivot (px)
static const int      HUB_R       = 9;          // Hub (pivot) radius (px)
static const int      SHAFT_R     = 2;          // Center shaft radius (px)
static const uint16_t COL_NEEDLE  = TFT_RED;   // Needle color
static const uint16_t NEEDLE_SHAFT  = TFT_DARKGREY;   // Needle color

// Needle placement (screen center for the demo)
static const int  NEEDLE_CENTER_X = 240;
static const int  NEEDLE_CENTER_Y = 160;

// Simple 3D-ish drop shadow
static const bool DRAW_SHADOW = true;

// Background image (LittleFS path)
static const char *BG = "/greatcircleMap.png";

// ============================================================================
// GLOBALS (display + decoder)
// ============================================================================

// PNGdec filesystem callback state
static File g_pngFile;

// PNG draw state (used by pngDraw callback)
static int  g_imgX = 0, g_imgY = 0;                 // top-left where PNG is drawn
static bool g_useClip = false;                      // enable clip region during decode
static int  g_clipX = 0, g_clipY = 0, g_clipW = 0, g_clipH = 0;

// Scanline buffer (RGB565) reused across decodes
static uint16_t *g_lineBuf  = nullptr;
static int       g_pngWidth = 0;

// Previous needle bounding box (for “restore background” step)
static int  prevBX = 0, prevBY = 0, prevBW = 0, prevBH = 0;
static bool havePrev = false;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

// --- PNGdec FS callbacks ---
void*   pngOpen(const char *filename, int32_t *size);
void    pngClose(void *handle);
int32_t pngRead(PNGFILE *page, uint8_t *buffer, int32_t length);
int32_t pngSeek(PNGFILE *page, int32_t position);

// --- PNG drawing helpers ---
bool ensureLineBuf(int w);
void pngDraw(PNGDRAW *pDraw);
bool drawPng(const char *filename, int x, int y);
bool redrawPngRegion(const char *filename, int imgX, int imgY,
                     int clipX, int clipY, int clipW, int clipH);



// ============================================================================
// NEEDLE DRAW (DO NOT CHANGE FUNCTION BEHAVIOR)
// ============================================================================
//
// Draws a needle (triangle + hub) at (cx, cy), angleDeg where:
//   0° = up (north), 90° = right (east)
//
// Responsibilities:
//  1) Restore previous needle area (clip-redraw PNG background)
//  2) Compute needle geometry
//  3) Optional shadow
//  4) Draw needle + hub + center shaft
//  5) Compute and store bounding box (including shadow) for next restore
//
static inline void drawNeedle(int cx, int cy, int angleDeg)
{
  // --------------------------------------------------------------------------
  // 1) Restore previous needle area by redrawing background (clipped PNG decode)
  // --------------------------------------------------------------------------
  if (havePrev) {
    int x = prevBX, y = prevBY, w = prevBW, h = prevBH;
    redrawPngRegion(BG, 0, 0, x, y, w, h);
  }

  // --------------------------------------------------------------------------
  // 2) Geometry (unit direction + perpendicular)
  // --------------------------------------------------------------------------
  const float a = (float)angleDeg * (float)M_PI / 180.0f;

  // Convention: 0° = up
  const float ux = sinf(a);
  const float uy = -cosf(a);

  // Perpendicular vector for base width
  const float px = -uy;
  const float py =  ux;

  const float halfW = BASE_W * 0.5f;

  const int tipX  = cx + (int)lroundf(ux * NEEDLE_L);
  const int tipY  = cy + (int)lroundf(uy * NEEDLE_L);

  const int inBLx = cx + (int)lroundf(px * (-halfW));
  const int inBLy = cy + (int)lroundf(py * (-halfW));

  const int inBRx = cx + (int)lroundf(px * ( halfW));
  const int inBRy = cy + (int)lroundf(py * ( halfW));

  // --------------------------------------------------------------------------
  // 3) Shadow (simple offset dark triangle)
  // --------------------------------------------------------------------------
  if (DRAW_SHADOW) {
    const int SHADOW_OFFSET = 2;
    const uint16_t shadowColor = TFT_DARKGREY;

    tft.fillTriangle(
      inBLx + SHADOW_OFFSET, inBLy + SHADOW_OFFSET,
      inBRx + SHADOW_OFFSET, inBRy + SHADOW_OFFSET,
      tipX  + SHADOW_OFFSET, tipY  + SHADOW_OFFSET,
      shadowColor
    );
  }

  // --------------------------------------------------------------------------
  // 4) Needle body + hub + center shaft
  // --------------------------------------------------------------------------
  tft.fillTriangle(inBLx, inBLy, inBRx, inBRy, tipX, tipY, COL_NEEDLE);
  tft.fillCircle(cx, cy, HUB_R,   COL_NEEDLE); // hub
  tft.fillCircle(cx, cy, SHAFT_R, NEEDLE_SHAFT);  // black pivot center

  // 5) Optimized bounding box for background restore (triangle, hub, and shadow)
  // Triangle points
  int xs[3] = { tipX, inBLx, inBRx };
  int ys[3] = { tipY, inBLy, inBRy };

  // Start with triangle bounds
  int minX = xs[0], maxX = xs[0], minY = ys[0], maxY = ys[0];
  for (int i = 1; i < 3; ++i) {
    if (xs[i] < minX) minX = xs[i];
    if (xs[i] > maxX) maxX = xs[i];
    if (ys[i] < minY) minY = ys[i];
    if (ys[i] > maxY) maxY = ys[i];
  }

  // Expand for hub (circle at center)
  if (cx - HUB_R < minX) minX = cx - HUB_R;
  if (cx + HUB_R > maxX) maxX = cx + HUB_R;
  if (cy - HUB_R < minY) minY = cy - HUB_R;
  if (cy + HUB_R > maxY) maxY = cy + HUB_R;

  // Expand for shadow if enabled
  if (DRAW_SHADOW) {
    const int SHADOW_OFFSET = 2;
    // Shadow triangle
    for (int i = 0; i < 3; ++i) {
      int sx = xs[i] + SHADOW_OFFSET;
      int sy = ys[i] + SHADOW_OFFSET;
      if (sx < minX) minX = sx;
      if (sx > maxX) maxX = sx;
      if (sy < minY) minY = sy;
      if (sy > maxY) maxY = sy;
    }
    // Shadow hub
    if (cx + SHADOW_OFFSET - HUB_R < minX) minX = cx + SHADOW_OFFSET - HUB_R;
    if (cx + SHADOW_OFFSET + HUB_R > maxX) maxX = cx + SHADOW_OFFSET + HUB_R;
    if (cy + SHADOW_OFFSET - HUB_R < minY) minY = cy + SHADOW_OFFSET - HUB_R;
    if (cy + SHADOW_OFFSET + HUB_R > maxY) maxY = cy + SHADOW_OFFSET + HUB_R;
  }

  prevBX   = minX;
  prevBY   = minY;
  prevBW   = (maxX - minX) + 1;
  prevBH   = (maxY - minY) + 1;
  havePrev = true;
}

// ============================================================================
// 

// ============================================================================
// IMPLEMENTATIONS
// ============================================================================

// ---------------------------------------------------------------------------
// PNGdec FS callbacks (LittleFS)
// ---------------------------------------------------------------------------

void* pngOpen(const char *filename, int32_t *size)
{
  g_pngFile = LittleFS.open(filename, "r");
  if (!g_pngFile) return nullptr;

  *size = g_pngFile.size();
  return (void*)&g_pngFile;
}

void pngClose(void *handle)
{
  File *f = (File*)handle;
  if (f) f->close();
}

int32_t pngRead(PNGFILE *page, uint8_t *buffer, int32_t length)
{
  File *f = (File*)page->fHandle;
  return f->read(buffer, length);
}

int32_t pngSeek(PNGFILE *page, int32_t position)
{
  File *f = (File*)page->fHandle;
  return f->seek(position) ? position : -1;
}

// ---------------------------------------------------------------------------
// PNG helpers
// ---------------------------------------------------------------------------

// Allocate (or reallocate) one RGB565 scanline buffer, sized to PNG width.
bool ensureLineBuf(int w)
{
  if (g_lineBuf && g_pngWidth == w) return true;

  if (g_lineBuf) {
    free(g_lineBuf);
    g_lineBuf = nullptr;
  }

  g_pngWidth = w;
  g_lineBuf = (uint16_t*)malloc(g_pngWidth * sizeof(uint16_t));
  if (!g_lineBuf) {
    Serial.println("No RAM for line buffer");
    return false;
  }
  return true;
}

// PNGdec scanline callback: converts a line to RGB565 and pushes (optionally clipped).
void pngDraw(PNGDRAW *pDraw)
{
  const int yAbs = g_imgY + pDraw->y;

  // Vertical clip rejection
  if (g_useClip) {
    if (yAbs < g_clipY || yAbs >= (g_clipY + g_clipH)) return;
  }

  // BIG_ENDIAN + setSwapBytes(false) => correct colors
  png.getLineAsRGB565(pDraw, g_lineBuf, PNG_RGB565_BIG_ENDIAN, 0xFFFFFFFF);

  int drawLeft  = 0;
  int drawRight = (int)pDraw->iWidth;

  // Horizontal clipping (convert screen clip to PNG-local coordinates)
  if (g_useClip) {
    const int clipLeft  = g_clipX - g_imgX;
    const int clipRight = clipLeft + g_clipW;

    drawLeft  = max(drawLeft,  clipLeft);
    drawRight = min(drawRight, clipRight);

    if (drawRight <= drawLeft) return;
  }

  const int span = drawRight - drawLeft;
  const int xAbs = g_imgX + drawLeft;

  tft.pushImage(xAbs, yAbs, span, 1, g_lineBuf + drawLeft);
}

// Decode and draw the full PNG at (x, y).
bool drawPng(const char *filename, int x, int y)
{
  g_imgX = x;
  g_imgY = y;
  g_useClip = false;

  const int16_t rc = png.open(filename, pngOpen, pngClose, pngRead, pngSeek, pngDraw);
  if (rc != PNG_SUCCESS) {
    Serial.printf("PNG open failed (%d)\n", rc);
    return false;
  }

  const int w = png.getWidth();
  if (!ensureLineBuf(w)) {
    png.close();
    return false;
  }

  tft.setSwapBytes(false);
  png.decode(nullptr, 0);
  png.close();

  return true;
}

// Decode the PNG but only draw within clip rectangle (screen coordinates).
bool redrawPngRegion(const char *filename,
                     int imgX, int imgY,
                     int clipX, int clipY, int clipW, int clipH)
{
  g_imgX = imgX;
  g_imgY = imgY;

  g_useClip = true;
  g_clipX = clipX; g_clipY = clipY; g_clipW = clipW; g_clipH = clipH;

  const int16_t rc = png.open(filename, pngOpen, pngClose, pngRead, pngSeek, pngDraw);
  if (rc != PNG_SUCCESS) {
    Serial.printf("PNG open failed (%d)\n", rc);
    g_useClip = false;
    return false;
  }

  const int w = png.getWidth();
  if (!ensureLineBuf(w)) {
    png.close();
    g_useClip = false;
    return false;
  }

  tft.setSwapBytes(false);
  png.decode(nullptr, 0);
  png.close();

  g_useClip = false;
  return true;
}