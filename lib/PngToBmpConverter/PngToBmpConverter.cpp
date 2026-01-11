#include "PngToBmpConverter.h"

#include <HardwareSerial.h>
#include <SdFat.h>
#include <pngle.h>

// ============================================================================
// PNG to BMP Conversion Settings (match JpegToBmpConverter for consistency)
// ============================================================================
// Brightness/Contrast adjustments:
constexpr bool USE_BRIGHTNESS = true;
constexpr int BRIGHTNESS_BOOST = 25;
constexpr bool GAMMA_CORRECTION = true;
constexpr float CONTRAST_FACTOR = 1.05f;
// Pre-resize settings
constexpr bool USE_PRESCALE = true;
constexpr int TARGET_MAX_WIDTH = 800;
constexpr int TARGET_MAX_HEIGHT = 800;
// ============================================================================

// Context for PNG decoding
struct PngDecodeContext {
  FsFile* pngFile;
  Print* bmpOut;
  int srcWidth;              // Original PNG width
  int srcHeight;             // Original PNG height
  int outWidth;              // Output BMP width (after scaling)
  int outHeight;             // Output BMP height (after scaling)
  uint16_t targetMaxWidth;   // Target max width for scaling
  uint16_t targetMaxHeight;  // Target max height for scaling
  int currentSrcY;           // Current source row being accumulated
  int currentOutY;           // Current output row
  uint32_t* rowAccum;        // Accumulator for each output X (for area averaging)
  uint16_t* rowCount;        // Count of source pixels accumulated per output X
  uint8_t* srcRowBuffer;     // Current source row of grayscale pixels
  int16_t* errorRow0;        // Atkinson dithering error buffers
  int16_t* errorRow1;
  int16_t* errorRow2;
  uint8_t* bmpRowBuffer;  // BMP output row
  int bmpBytesPerRow;
  uint32_t scaleX_fp;          // Fixed-point scale factor X (16.16)
  uint32_t scaleY_fp;          // Fixed-point scale factor Y (16.16)
  uint32_t nextOutY_srcStart;  // Source Y where next output row starts (16.16 fixed point)
  bool needsScaling;
  bool headerWritten;
  bool error;
};

// Integer approximation of gamma correction
static inline int applyGamma(int gray) {
  if (!GAMMA_CORRECTION) return gray;
  const int product = gray * 255;
  int x = gray;
  if (x > 0) {
    x = (x + product / x) >> 1;
    x = (x + product / x) >> 1;
  }
  return x > 255 ? 255 : x;
}

// Apply contrast adjustment
static inline int applyContrast(int gray) {
  constexpr int factorNum = static_cast<int>(CONTRAST_FACTOR * 100);
  int adjusted = ((gray - 128) * factorNum) / 100 + 128;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;
  return adjusted;
}

// Combined brightness/contrast/gamma adjustment
static inline int adjustPixel(int gray) {
  if (!USE_BRIGHTNESS) return gray;
  gray = applyContrast(gray);
  gray += BRIGHTNESS_BOOST;
  if (gray > 255) gray = 255;
  if (gray < 0) gray = 0;
  gray = applyGamma(gray);
  return gray;
}

// Forward declaration for helper function
static void writeBmpHeaderHelper(Print& bmpOut, int width, int height);

// Write BMP header helper functions
inline void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

static void writeBmpHeaderHelper(Print& bmpOut, const int width, const int height) {
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);
  write32(bmpOut, 70);

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative = top-down
  write16(bmpOut, 1);
  write16(bmpOut, 2);  // 2 bits per pixel
  write32(bmpOut, 0);
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);
  write32(bmpOut, 2835);
  write32(bmpOut, 4);
  write32(bmpOut, 4);

  // Color Palette (4 colors x 4 bytes)
  uint8_t palette[16] = {
      0x00, 0x00, 0x00, 0x00,  // Black
      0x55, 0x55, 0x55, 0x00,  // Dark gray
      0xAA, 0xAA, 0xAA, 0x00,  // Light gray
      0xFF, 0xFF, 0xFF, 0x00   // White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// Process a complete output row with Atkinson dithering (after area averaging if scaled)
static void processOutputRowWithDithering(PngDecodeContext* ctx) {
  const int outWidth = ctx->outWidth;

  // Clear BMP row buffer
  memset(ctx->bmpRowBuffer, 0, ctx->bmpBytesPerRow);

  // Process each pixel with Atkinson dithering
  for (int x = 0; x < outWidth; x++) {
    // Get grayscale value (from accumulator if scaling, direct if not)
    int gray;
    if (ctx->needsScaling) {
      gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 255;
    } else {
      gray = ctx->srcRowBuffer[x];
    }
    gray = adjustPixel(gray);

    // Add accumulated error
    int adjusted = gray + ctx->errorRow0[x + 2];
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 255) adjusted = 255;

    // Quantize to 4 levels
    uint8_t quantized;
    int quantizedValue;
    if (adjusted < 43) {
      quantized = 0;
      quantizedValue = 0;
    } else if (adjusted < 128) {
      quantized = 1;
      quantizedValue = 85;
    } else if (adjusted < 213) {
      quantized = 2;
      quantizedValue = 170;
    } else {
      quantized = 3;
      quantizedValue = 255;
    }

    // Calculate and distribute error (Atkinson: 6/8 = 75%)
    int error = (adjusted - quantizedValue) >> 3;
    ctx->errorRow0[x + 3] += error;
    ctx->errorRow0[x + 4] += error;
    ctx->errorRow1[x + 1] += error;
    ctx->errorRow1[x + 2] += error;
    ctx->errorRow1[x + 3] += error;
    ctx->errorRow2[x + 2] += error;

    // Pack into BMP row
    const int byteIndex = (x * 2) / 8;
    const int bitOffset = 6 - ((x * 2) % 8);
    ctx->bmpRowBuffer[byteIndex] |= (quantized << bitOffset);
  }

  // Write BMP row
  ctx->bmpOut->write(ctx->bmpRowBuffer, ctx->bmpBytesPerRow);

  // Rotate error buffers
  int16_t* temp = ctx->errorRow0;
  ctx->errorRow0 = ctx->errorRow1;
  ctx->errorRow1 = ctx->errorRow2;
  ctx->errorRow2 = temp;
  memset(ctx->errorRow2, 0, (outWidth + 4) * sizeof(int16_t));

  // Reset accumulators for next output row (if scaling)
  if (ctx->needsScaling) {
    memset(ctx->rowAccum, 0, outWidth * sizeof(uint32_t));
    memset(ctx->rowCount, 0, outWidth * sizeof(uint16_t));
  }
}

// pngle callback for each pixel
static void pngleDrawCallback(pngle_t* pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
  auto* ctx = static_cast<PngDecodeContext*>(pngle_get_user_data(pngle));
  if (ctx->error) return;

  // Write header on first pixel
  if (!ctx->headerWritten) {
    ctx->srcWidth = pngle_get_width(pngle);
    ctx->srcHeight = pngle_get_height(pngle);

    // Calculate output dimensions with scaling
    ctx->outWidth = ctx->srcWidth;
    ctx->outHeight = ctx->srcHeight;
    ctx->needsScaling = false;
    ctx->scaleX_fp = 65536;  // 1.0 in 16.16 fixed point
    ctx->scaleY_fp = 65536;

    const int targetMaxW = (ctx->targetMaxWidth > 0) ? ctx->targetMaxWidth : TARGET_MAX_WIDTH;
    const int targetMaxH = (ctx->targetMaxHeight > 0) ? ctx->targetMaxHeight : TARGET_MAX_HEIGHT;

    if (ctx->srcWidth > targetMaxW || ctx->srcHeight > targetMaxH) {
      const float scaleToFitWidth = static_cast<float>(targetMaxW) / ctx->srcWidth;
      const float scaleToFitHeight = static_cast<float>(targetMaxH) / ctx->srcHeight;
      const float scale = (scaleToFitWidth < scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;

      ctx->outWidth = static_cast<int>(ctx->srcWidth * scale);
      ctx->outHeight = static_cast<int>(ctx->srcHeight * scale);
      if (ctx->outWidth < 1) ctx->outWidth = 1;
      if (ctx->outHeight < 1) ctx->outHeight = 1;

      ctx->scaleX_fp = (static_cast<uint32_t>(ctx->srcWidth) << 16) / ctx->outWidth;
      ctx->scaleY_fp = (static_cast<uint32_t>(ctx->srcHeight) << 16) / ctx->outHeight;
      ctx->needsScaling = true;

      Serial.printf("[%lu] [PNG] Pre-scaling %dx%d -> %dx%d\n", millis(), ctx->srcWidth, ctx->srcHeight, ctx->outWidth,
                    ctx->outHeight);
    }

    ctx->bmpBytesPerRow = (ctx->outWidth * 2 + 31) / 32 * 4;

    // Allocate buffers
    ctx->srcRowBuffer = new uint8_t[ctx->srcWidth]();
    ctx->errorRow0 = new int16_t[ctx->outWidth + 4]();
    ctx->errorRow1 = new int16_t[ctx->outWidth + 4]();
    ctx->errorRow2 = new int16_t[ctx->outWidth + 4]();
    ctx->bmpRowBuffer = new uint8_t[ctx->bmpBytesPerRow]();

    if (ctx->needsScaling) {
      ctx->rowAccum = new uint32_t[ctx->outWidth]();
      ctx->rowCount = new uint16_t[ctx->outWidth]();
      ctx->nextOutY_srcStart = ctx->scaleY_fp;  // First boundary
    }

    if (!ctx->srcRowBuffer || !ctx->errorRow0 || !ctx->errorRow1 || !ctx->errorRow2 || !ctx->bmpRowBuffer ||
        (ctx->needsScaling && (!ctx->rowAccum || !ctx->rowCount))) {
      Serial.printf("[%lu] [PNG] Failed to allocate buffers\n", millis());
      ctx->error = true;
      return;
    }

    writeBmpHeaderHelper(*ctx->bmpOut, ctx->outWidth, ctx->outHeight);
    ctx->headerWritten = true;
    ctx->currentSrcY = 0;
    ctx->currentOutY = 0;

    Serial.printf("[%lu] [PNG] Dimensions: %dx%d -> %dx%d\n", millis(), ctx->srcWidth, ctx->srcHeight, ctx->outWidth,
                  ctx->outHeight);
  }

  // Convert RGBA to grayscale
  uint8_t gray;
  if (rgba[3] == 0) {
    // Fully transparent - use white background
    gray = 255;
  } else {
    // Weighted grayscale conversion
    gray = (rgba[0] * 25 + rgba[1] * 50 + rgba[2] * 25) / 100;

    // Alpha blending with white background
    if (rgba[3] < 255) {
      gray = (gray * rgba[3] + 255 * (255 - rgba[3])) / 255;
    }
  }

  // Store in source row buffer
  if (x < static_cast<uint32_t>(ctx->srcWidth)) {
    ctx->srcRowBuffer[x] = gray;
  }

  // When source row is complete, process it
  if (x == static_cast<uint32_t>(ctx->srcWidth - 1)) {
    if (!ctx->needsScaling) {
      // No scaling - direct output
      processOutputRowWithDithering(ctx);
    } else {
      // Accumulate source row into output row(s) using area averaging
      for (int outX = 0; outX < ctx->outWidth; outX++) {
        const int srcXStart = (static_cast<uint32_t>(outX) * ctx->scaleX_fp) >> 16;
        const int srcXEnd = (static_cast<uint32_t>(outX + 1) * ctx->scaleX_fp) >> 16;

        int sum = 0;
        int count = 0;
        for (int srcX = srcXStart; srcX < srcXEnd && srcX < ctx->srcWidth; srcX++) {
          sum += ctx->srcRowBuffer[srcX];
          count++;
        }
        if (count == 0 && srcXStart < ctx->srcWidth) {
          sum = ctx->srcRowBuffer[srcXStart];
          count = 1;
        }

        ctx->rowAccum[outX] += sum;
        ctx->rowCount[outX] += count;
      }

      // Check if we've crossed into the next output row
      const uint32_t srcY_fp = static_cast<uint32_t>(ctx->currentSrcY + 1) << 16;
      if (srcY_fp >= ctx->nextOutY_srcStart && ctx->currentOutY < ctx->outHeight) {
        processOutputRowWithDithering(ctx);
        ctx->currentOutY++;
        ctx->nextOutY_srcStart = static_cast<uint32_t>(ctx->currentOutY + 1) * ctx->scaleY_fp;
      }
    }

    ctx->currentSrcY++;

    // Clear source row buffer for next row
    memset(ctx->srcRowBuffer, 255, ctx->srcWidth);
  }
}

// pngle init callback
static void pngleInitCallback(pngle_t* pngle, uint32_t w, uint32_t h) {
  auto* ctx = static_cast<PngDecodeContext*>(pngle_get_user_data(pngle));

  // Check size limits
  if (w > TARGET_MAX_WIDTH || h > TARGET_MAX_HEIGHT) {
    Serial.printf("[%lu] [PNG] Image too large: %dx%d (max %dx%d)\n", millis(), w, h, TARGET_MAX_WIDTH,
                  TARGET_MAX_HEIGHT);
    // Note: pngle doesn't support scaling, so large images may need to be handled differently
  }

  Serial.printf("[%lu] [PNG] Init: %dx%d\n", millis(), w, h);
}

bool PngToBmpConverter::pngFileToBmpStream(FsFile& pngFile, Print& bmpOut, uint16_t maxWidth, uint16_t maxHeight) {
  Serial.printf("[%lu] [PNG] Converting PNG to BMP (target: %dx%d)\n", millis(), maxWidth, maxHeight);

  pngle_t* pngle = pngle_new();
  if (!pngle) {
    Serial.printf("[%lu] [PNG] Failed to create pngle instance\n", millis());
    return false;
  }

  PngDecodeContext ctx = {};
  ctx.pngFile = &pngFile;
  ctx.bmpOut = &bmpOut;
  ctx.targetMaxWidth = maxWidth;
  ctx.targetMaxHeight = maxHeight;
  ctx.headerWritten = false;
  ctx.error = false;
  ctx.rowAccum = nullptr;
  ctx.rowCount = nullptr;

  pngle_set_user_data(pngle, &ctx);
  pngle_set_init_callback(pngle, pngleInitCallback);
  pngle_set_draw_callback(pngle, pngleDrawCallback);

  // Read and feed PNG data to pngle
  uint8_t buffer[256];
  while (pngFile.available() && !ctx.error) {
    const size_t bytesRead = pngFile.read(buffer, sizeof(buffer));
    if (bytesRead == 0) break;

    const int fed = pngle_feed(pngle, buffer, bytesRead);
    if (fed < 0) {
      Serial.printf("[%lu] [PNG] Decode error: %s\n", millis(), pngle_error(pngle));
      ctx.error = true;
      break;
    }
  }

  // Cleanup
  delete[] ctx.srcRowBuffer;
  delete[] ctx.errorRow0;
  delete[] ctx.errorRow1;
  delete[] ctx.errorRow2;
  delete[] ctx.bmpRowBuffer;
  delete[] ctx.rowAccum;
  delete[] ctx.rowCount;
  pngle_destroy(pngle);

  if (ctx.error || !ctx.headerWritten) {
    Serial.printf("[%lu] [PNG] Conversion failed\n", millis());
    return false;
  }

  Serial.printf("[%lu] [PNG] Conversion complete: %dx%d\n", millis(), ctx.outWidth, ctx.outHeight);
  return true;
}