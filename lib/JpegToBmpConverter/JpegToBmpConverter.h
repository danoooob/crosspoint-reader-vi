#pragma once

#include <cstdint>

class FsFile;
class Print;
class ZipFile;

class JpegToBmpConverter {
  static void writeBmpHeader(Print& bmpOut, int width, int height);
  // [COMMENTED OUT] static uint8_t grayscaleTo2Bit(uint8_t grayscale, int x, int y);
  static unsigned char jpegReadCallback(unsigned char* pBuf, unsigned char buf_size,
                                        unsigned char* pBytes_actually_read, void* pCallback_data);

 public:
  // Convert JPEG file to 2-bit grayscale BMP, scaling to fit within maxWidth x maxHeight
  // If maxWidth/maxHeight are 0, uses internal defaults (800x800)
  static bool jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, uint16_t maxWidth = 0, uint16_t maxHeight = 0);
};
