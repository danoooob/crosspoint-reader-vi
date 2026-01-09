#include "ImageBlock.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <JpegToBmpConverter.h>
#include <PngToBmpConverter.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include "../../Epub.h"
#include "Bitmap.h"

void ImageBlock::render(const GfxRenderer& renderer, const int x, const int y, const int viewportWidth) const {
  if (cachedBmpPath.empty()) {
    Serial.printf("[%lu] [IMG] !! No cached BMP path\n", millis());
    return;
  }

  FsFile bmpFile;
  if (!SdMan.openFileForRead("IMG", cachedBmpPath, bmpFile)) {
    Serial.printf("[%lu] [IMG] !! Failed to open BMP: %s\n", millis(), cachedBmpPath.c_str());
    return;
  }

  Bitmap bitmap(bmpFile);
  const BmpReaderError err = bitmap.parseHeaders();
  if (err != BmpReaderError::Ok) {
    Serial.printf("[%lu] [IMG] !! BMP parse error: %s\n", millis(), Bitmap::errorToString(err));
    bmpFile.close();
    return;
  }

  // Always center the image horizontally in the viewport
  // x is the left margin (viewport start), viewportWidth is the available width
  // If viewportWidth is 0 (not provided), fall back to using image width
  const int availableWidth = (viewportWidth > 0) ? viewportWidth : width;
  const int imgX = x + (availableWidth - bitmap.getWidth()) / 2;

  renderer.drawBitmap(bitmap, imgX, y, width, height);
  bmpFile.close();
}

bool ImageBlock::serialize(FsFile& file) const {
  serialization::writeString(file, cachedBmpPath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(FsFile& file) {
  std::string cachedBmpPath;
  uint16_t width, height;

  serialization::readString(file, cachedBmpPath);
  serialization::readPod(file, width);
  serialization::readPod(file, height);

  return std::unique_ptr<ImageBlock>(new ImageBlock(std::move(cachedBmpPath), width, height));
}

std::unique_ptr<ImageBlock> ImageBlock::createFromEpub(const Epub& epub, const std::string& imageHref,
                                                       const std::string& cacheDir, const uint16_t maxWidth,
                                                       const uint16_t maxHeight) {
  // Generate a unique cache filename based on the image href AND target dimensions
  // This ensures different orientations (portrait/landscape) get properly sized cached versions
  size_t hash = std::hash<std::string>{}(imageHref);
  hash ^= std::hash<uint16_t>{}(maxWidth) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
  hash ^= std::hash<uint16_t>{}(maxHeight) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
  const std::string cachedBmpPath = cacheDir + "/img_" + std::to_string(hash) + ".bmp";

  // Check if cached BMP already exists
  if (SdMan.exists(cachedBmpPath.c_str())) {
    // Load the cached BMP to get its dimensions
    FsFile bmpFile;
    if (SdMan.openFileForRead("IMG", cachedBmpPath, bmpFile)) {
      Bitmap bitmap(bmpFile);
      const BmpReaderError err = bitmap.parseHeaders();
      bmpFile.close();

      if (err == BmpReaderError::Ok) {
        Serial.printf("[%lu] [IMG] Using cached: %s (%dx%d)\n", millis(), cachedBmpPath.c_str(), bitmap.getWidth(),
                      bitmap.getHeight());
        // Use actual BMP dimensions - no need for post-hoc scaling
        return std::unique_ptr<ImageBlock>(new ImageBlock(cachedBmpPath, bitmap.getWidth(), bitmap.getHeight()));
      }
    }
    // Cache file exists but is invalid, remove it
    SdMan.remove(cachedBmpPath.c_str());
  }

  // Determine image type from extension
  const std::string lowerHref = [&]() {
    std::string lower = imageHref;
    for (char& c : lower) c = std::tolower(c);
    return lower;
  }();

  const bool isJpeg = lowerHref.find(".jpg") != std::string::npos || lowerHref.find(".jpeg") != std::string::npos;
  const bool isPng = lowerHref.find(".png") != std::string::npos;

  if (!isJpeg && !isPng) {
    // Only JPEG and PNG are supported
    Serial.printf("[%lu] [IMG] Skipping unsupported image format: %s\n", millis(), imageHref.c_str());
    return nullptr;
  }

  // Extract image from EPUB to a temporary file
  const std::string tmpImagePath = cacheDir + (isPng ? "/.tmp_img.png" : "/.tmp_img.jpg");

  FsFile tmpImage;
  if (!SdMan.openFileForWrite("IMG", tmpImagePath, tmpImage)) {
    Serial.printf("[%lu] [IMG] !! Failed to create temp image file\n", millis());
    return nullptr;
  }

  if (!epub.readItemContentsToStream(imageHref, tmpImage, 4096)) {
    Serial.printf("[%lu] [IMG] !! Failed to extract image from EPUB: %s\n", millis(), imageHref.c_str());
    tmpImage.close();
    SdMan.remove(tmpImagePath.c_str());
    return nullptr;
  }
  tmpImage.close();

  // Reopen for reading and convert to BMP
  FsFile imageFile;
  if (!SdMan.openFileForRead("IMG", tmpImagePath, imageFile)) {
    Serial.printf("[%lu] [IMG] !! Failed to open temp image for reading\n", millis());
    SdMan.remove(tmpImagePath.c_str());
    return nullptr;
  }

  FsFile bmpFile;
  if (!SdMan.openFileForWrite("IMG", cachedBmpPath, bmpFile)) {
    Serial.printf("[%lu] [IMG] !! Failed to create BMP file\n", millis());
    imageFile.close();
    SdMan.remove(tmpImagePath.c_str());
    return nullptr;
  }

  // Convert based on image type, passing target dimensions for pre-scaling
  // This avoids runtime scaling in GfxRenderer::drawBitmap
  bool converted;
  if (isPng) {
    converted = PngToBmpConverter::pngFileToBmpStream(imageFile, bmpFile, maxWidth, maxHeight);
  } else {
    converted = JpegToBmpConverter::jpegFileToBmpStream(imageFile, bmpFile, maxWidth, maxHeight);
  }
  imageFile.close();
  bmpFile.close();

  // Clean up temp image
  SdMan.remove(tmpImagePath.c_str());

  if (!converted) {
    Serial.printf("[%lu] [IMG] !! Image to BMP conversion failed\n", millis());
    SdMan.remove(cachedBmpPath.c_str());
    return nullptr;
  }

  // Read the generated BMP to get its actual dimensions (already scaled by converter)
  if (!SdMan.openFileForRead("IMG", cachedBmpPath, bmpFile)) {
    Serial.printf("[%lu] [IMG] !! Failed to open converted BMP\n", millis());
    return nullptr;
  }

  Bitmap bitmap(bmpFile);
  const BmpReaderError err = bitmap.parseHeaders();
  bmpFile.close();

  if (err != BmpReaderError::Ok) {
    Serial.printf("[%lu] [IMG] !! BMP parse error after conversion: %s\n", millis(), Bitmap::errorToString(err));
    SdMan.remove(cachedBmpPath.c_str());
    return nullptr;
  }

  // Use actual BMP dimensions - the converter already scaled to fit maxWidth/maxHeight
  const uint16_t finalWidth = bitmap.getWidth();
  const uint16_t finalHeight = bitmap.getHeight();

  Serial.printf("[%lu] [IMG] Created: %s (%dx%d)\n", millis(), cachedBmpPath.c_str(), finalWidth, finalHeight);

  return std::unique_ptr<ImageBlock>(new ImageBlock(cachedBmpPath, finalWidth, finalHeight));
}