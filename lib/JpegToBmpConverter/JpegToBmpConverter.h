#pragma once

#include <cstdint>

class FsFile;
class Print;
class ZipFile;

class JpegToBmpConverter {
  static unsigned char jpegReadCallback(unsigned char* pBuf, unsigned char buf_size,
                                        unsigned char* pBytes_actually_read, void* pCallback_data);
  static bool jpegFileToBmpStreamInternal(class FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit);

 public:
   // Convert JPEG file to 2-bit grayscale BMP, scaling to fit within maxWidth x maxHeight
  // If maxWidth/maxHeight are 0, uses internal defaults (800x800)
  static bool jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, uint16_t maxWidth = 0, uint16_t maxHeight = 0);
  // Convert with custom target size (for thumbnails)
  static bool jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
};
