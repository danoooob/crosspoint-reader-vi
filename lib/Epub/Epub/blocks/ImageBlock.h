#pragma once
#include <SdFat.h>

#include <memory>
#include <string>

#include "Block.h"

class Epub;

// Represents an image block in an EPUB chapter
// Images are cached as BMP files on SD card to avoid repeated JPEG decoding
class ImageBlock final : public Block {
 private:
  std::string cachedBmpPath;  // Path to cached BMP file on SD card
  uint16_t width = 0;         // Image width (after scaling to viewport)
  uint16_t height = 0;        // Image height (after scaling to viewport)

 public:
  explicit ImageBlock(std::string cachedBmpPath, uint16_t width, uint16_t height)
      : cachedBmpPath(std::move(cachedBmpPath)), width(width), height(height) {}
  ~ImageBlock() override = default;

  bool isEmpty() override { return cachedBmpPath.empty() || width == 0 || height == 0; }
  void layout(GfxRenderer& renderer) override {}
  BlockType getType() override { return IMAGE_BLOCK; }

  const std::string& getCachedBmpPath() const { return cachedBmpPath; }
  uint16_t getWidth() const { return width; }
  uint16_t getHeight() const { return height; }

  // Render the image at the specified position (x is viewport left margin, centers automatically)
  void render(const GfxRenderer& renderer, int x, int y, int viewportWidth = 0) const;

  // Serialization for caching
  bool serialize(FsFile& file) const;
  static std::unique_ptr<ImageBlock> deserialize(FsFile& file);

  // Factory method to create ImageBlock from EPUB image
  // Converts JPEG/PNG to BMP and caches it, returns nullptr on failure
  static std::unique_ptr<ImageBlock> createFromEpub(const Epub& epub, const std::string& imageHref,
                                                    const std::string& cacheDir, uint16_t maxWidth, uint16_t maxHeight);
};