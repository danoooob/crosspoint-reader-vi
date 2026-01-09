#pragma once

#include <cstdint>

class FsFile;
class Print;

class PngToBmpConverter {
  static void writeBmpHeader(Print& bmpOut, int width, int height);

 public:
  // Convert PNG file to 2-bit grayscale BMP, scaling to fit within maxWidth x maxHeight
  // If maxWidth/maxHeight are 0, uses internal defaults (800x800)
  static bool pngFileToBmpStream(FsFile& pngFile, Print& bmpOut, uint16_t maxWidth = 0, uint16_t maxHeight = 0);
};