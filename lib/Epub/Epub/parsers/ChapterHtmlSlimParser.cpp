#include "ChapterHtmlSlimParser.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <expat.h>

#include "../../Epub.h"
#include "../Page.h"

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

// Minimum file size (in bytes) to show progress bar - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_PROGRESS = 50 * 1024;  // 50KB

const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote", "tr"};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

const char* TABLE_CELL_TAGS[] = {"td", "th"};
constexpr int NUM_TABLE_CELL_TAGS = sizeof(TABLE_CELL_TAGS) / sizeof(TABLE_CELL_TAGS[0]);

const char* BOLD_TAGS[] = {"b", "strong"};
constexpr int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char* ITALIC_TAGS[] = {"i", "em"};
constexpr int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char* IMAGE_TAGS[] = {"img"};
constexpr int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

const char* SKIP_TAGS[] = {"head"};
constexpr int NUM_SKIP_TAGS = sizeof(SKIP_TAGS) / sizeof(SKIP_TAGS[0]);

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

// given the start and end of a tag, check to see if it matches a known tag
bool matches(const char* tag_name, const char* possible_tags[], const int possible_tag_count) {
  for (int i = 0; i < possible_tag_count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const TextBlock::Style style) {
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      currentTextBlock->setStyle(style);
      return;
    }

    makePages();
  }
  currentTextBlock.reset(new ParsedText(style, extraParagraphSpacing));
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    // Process image tag - extract and convert the image
    self->processImageTag(atts);
    self->depth += 1;
    return;
  }

  if (matches(name, SKIP_TAGS, NUM_SKIP_TAGS)) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  if (matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
    self->startNewTextBlock(TextBlock::CENTER_ALIGN);
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
  } else if (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    if (strcmp(name, "br") == 0) {
      self->startNewTextBlock(self->currentTextBlock->getStyle());
    } else if (strcmp(name, "tr") == 0) {
      // Table row: start new block and add bullet prefix
      self->startNewTextBlock((TextBlock::Style)self->paragraphAlignment);
      self->currentTextBlock->addWord("\xE2\x96\xB8", EpdFontFamily::REGULAR);  // ▸ (black right-pointing small triangle)
      self->isFirstCellInRow = true;  // Reset for new row
    } else {
      self->startNewTextBlock((TextBlock::Style)self->paragraphAlignment);
    }
  } else if (matches(name, BOLD_TAGS, NUM_BOLD_TAGS)) {
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
  } else if (matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS)) {
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
  } else if (strcmp(name, "td") == 0) {
    // Table data cell: add separator before cell (except first cell)
    if (!self->isFirstCellInRow) {
      self->currentTextBlock->addWord("\xC2\xB7", EpdFontFamily::REGULAR);  // · (middle dot)
    }
    self->isFirstCellInRow = false;
  } else if (strcmp(name, "th") == 0) {
    // Table header cell: add separator before cell (except first cell), and make bold
    if (!self->isFirstCellInRow) {
      self->currentTextBlock->addWord("\xC2\xB7", EpdFontFamily::BOLD);  // · (middle dot, bold to match header)
    }
    self->isFirstCellInRow = false;
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
  }

  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (self->boldUntilDepth < self->depth && self->italicUntilDepth < self->depth) {
    fontStyle = EpdFontFamily::BOLD_ITALIC;
  } else if (self->boldUntilDepth < self->depth) {
    fontStyle = EpdFontFamily::BOLD;
  } else if (self->italicUntilDepth < self->depth) {
    fontStyle = EpdFontFamily::ITALIC;
  }

  for (int i = 0; i < len; i++) {
    if (isWhitespace(s[i])) {
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->partWordBuffer[self->partWordBufferIndex] = '\0';
        self->currentTextBlock->addWord(self->partWordBuffer, fontStyle);
        self->partWordBufferIndex = 0;
      }
      // Skip the whitespace char
      continue;
    }

    // Skip soft-hyphen with UTF-8 representation (U+00AD) = 0xC2 0xAD
    const XML_Char SHY_BYTE_1 = static_cast<XML_Char>(0xC2);
    const XML_Char SHY_BYTE_2 = static_cast<XML_Char>(0xAD);
    // 1. Check for the start of the 2-byte Soft Hyphen sequence
    if (s[i] == SHY_BYTE_1) {
      // 2. Check if the next byte exists AND if it completes the sequence
      //    We must check i + 1 < len to prevent reading past the end of the buffer.
      if ((i + 1 < len) && (s[i + 1] == SHY_BYTE_2)) {
        // Sequence 0xC2 0xAD found!
        // Skip the current byte (0xC2) and the next byte (0xAD)
        i++;       // Increment 'i' one more time to skip the 0xAD byte
        continue;  // Skip the rest of the loop and move to the next iteration
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      self->partWordBuffer[self->partWordBufferIndex] = '\0';
      self->currentTextBlock->addWord(self->partWordBuffer, fontStyle);
      self->partWordBufferIndex = 0;
    }

    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }

  // If we have > 750 words buffered up, perform the layout and consume out all but the last line
  // There should be enough here to build out 1-2 full pages and doing this will free up a lot of
  // memory.
  // Spotted when reading Intermezzo, there are some really long text blocks in there.
  if (self->currentTextBlock->size() > 750) {
    Serial.printf("[%lu] [EHP] Text block too long, splitting into multiple pages\n", millis());
    self->currentTextBlock->layoutAndExtractLines(
        self->renderer, self->fontId, self->viewportWidth,
        [self](const std::shared_ptr<TextBlock>& textBlock) { self->addLineToPage(textBlock); }, false);
  }
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  if (self->partWordBufferIndex > 0) {
    // Only flush out part word buffer if we're closing a block tag or are at the top of the HTML file.
    // We don't want to flush out content when closing inline tags like <span>.
    // Currently this also flushes out on closing <b> and <i> tags, but they are line tags so that shouldn't happen,
    // text styling needs to be overhauled to fix it.
    const bool shouldBreakText =
        matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS) || matches(name, HEADER_TAGS, NUM_HEADER_TAGS) ||
        matches(name, BOLD_TAGS, NUM_BOLD_TAGS) || matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS) ||
        matches(name, TABLE_CELL_TAGS, NUM_TABLE_CELL_TAGS) || self->depth == 1;

    if (shouldBreakText) {
      EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
      if (self->boldUntilDepth < self->depth && self->italicUntilDepth < self->depth) {
        fontStyle = EpdFontFamily::BOLD_ITALIC;
      } else if (self->boldUntilDepth < self->depth) {
        fontStyle = EpdFontFamily::BOLD;
      } else if (self->italicUntilDepth < self->depth) {
        fontStyle = EpdFontFamily::ITALIC;
      }

      self->partWordBuffer[self->partWordBufferIndex] = '\0';
      self->currentTextBlock->addWord(self->partWordBuffer, fontStyle);
      self->partWordBufferIndex = 0;
    }
  }

  // Reset bold styling at end of header cell
  if (strcmp(name, "th") == 0) {
    if (self->boldUntilDepth == self->depth - 1) {
      self->boldUntilDepth = INT_MAX;
    }
  }

  self->depth -= 1;

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  // Leaving bold
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  startNewTextBlock((TextBlock::Style)this->paragraphAlignment);

  const XML_Parser parser = XML_ParserCreate(nullptr);
  int done;

  if (!parser) {
    Serial.printf("[%lu] [EHP] Couldn't allocate memory for parser\n", millis());
    return false;
  }

  FsFile file;
  if (!SdMan.openFileForRead("EHP", filepath, file)) {
    XML_ParserFree(parser);
    return false;
  }

  // Get file size for progress calculation
  const size_t totalSize = file.size();
  size_t bytesRead = 0;
  int lastProgress = -1;

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  do {
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      Serial.printf("[%lu] [EHP] Couldn't allocate memory for buffer\n", millis());
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    const size_t len = file.read(buf, 1024);

    if (len == 0 && file.available() > 0) {
      Serial.printf("[%lu] [EHP] File read error\n", millis());
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    // Update progress (call every 10% change to avoid too frequent updates)
    // Only show progress for larger chapters where rendering overhead is worth it
    bytesRead += len;
    if (progressFn && totalSize >= MIN_SIZE_FOR_PROGRESS) {
      const int progress = static_cast<int>((bytesRead * 100) / totalSize);
      if (lastProgress / 10 != progress / 10) {
        lastProgress = progress;
        progressFn(progress);
      }
    }

    done = file.available() == 0;

    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      Serial.printf("[%lu] [EHP] Parse error at line %lu:\n%s\n", millis(), XML_GetCurrentLineNumber(parser),
                    XML_ErrorString(XML_GetErrorCode(parser)));
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }
  } while (!done);

  XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
  XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
  XML_SetCharacterDataHandler(parser, nullptr);
  XML_ParserFree(parser);
  file.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();
    completePageFn(std::move(currentPage));
    currentPage.reset();
    currentTextBlock.reset();
  }

  return true;
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line) {
  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  if (currentPageNextY + lineHeight > viewportHeight) {
    completePageFn(std::move(currentPage));
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  currentPage->elements.push_back(std::make_shared<PageLine>(line, 0, currentPageNextY));
  currentPageNextY += lineHeight;
}

void ChapterHtmlSlimParser::addImageToPage(std::shared_ptr<ImageBlock> image) {
  if (!image || image->isEmpty()) {
    return;
  }

  const int imageHeight = image->getHeight();

  // Check if this is a "tall" image that should get its own dedicated page
  // An image deserves a dedicated page if it takes more than 50% of viewport HEIGHT
  // Width alone should NOT determine dedicated page - wide but short images can be inline
  const bool isTallImage = (imageHeight > viewportHeight * 50 / 100);

  // If current page has content and image doesn't fit, start new page
  if (currentPageNextY > 0 && currentPageNextY + imageHeight > viewportHeight) {
    completePageFn(std::move(currentPage));
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // Center image vertically if it's on a dedicated page (page is empty and image is tall)
  int imageY = currentPageNextY;
  if (currentPageNextY == 0 && isTallImage && imageHeight < viewportHeight) {
    // Center vertically on the page
    imageY = (viewportHeight - imageHeight) / 2;
  }

  // Store xPos as 0 - actual centering happens in ImageBlock::render using viewportWidth
  currentPage->elements.push_back(std::make_shared<PageImage>(image, 0, imageY));

  // If tall image, complete this page immediately (dedicated image page)
  if (isTallImage) {
    completePageFn(std::move(currentPage));
    currentPage.reset(new Page());
    currentPageNextY = 0;
  } else {
    currentPageNextY = imageY + imageHeight;
    // Add a small gap after the image
    const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;
    currentPageNextY += lineHeight / 2;
  }
}

void ChapterHtmlSlimParser::processImageTag(const XML_Char** atts) {
  if (!atts) {
    return;
  }

  // Find src attribute (or xlink:href for SVG images in EPUB)
  std::string imageSrc;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], "src") == 0 || strcmp(atts[i], "xlink:href") == 0 || strcmp(atts[i], "href") == 0) {
      imageSrc = atts[i + 1];
      break;
    }
  }

  if (imageSrc.empty()) {
    Serial.printf("[%lu] [EHP] Image tag without src attribute\n", millis());
    return;
  }

  // Resolve relative path to absolute path within EPUB
  std::string imageHref = imageSrc;

  // Handle relative paths (most common case)
  if (imageSrc[0] != '/') {
    // Get directory of current HTML file
    size_t lastSlash = filepath.rfind('/');
    if (lastSlash != std::string::npos) {
      std::string htmlDir = filepath.substr(0, lastSlash + 1);
      // Remove the temp directory prefix from htmlDir to get EPUB-relative path
      // The filepath is like: "/cache/.tmp_0.html" but we need the original EPUB path
    }
    // For now, use the image path as-is relative to content base path
    imageHref = epub.getBasePath() + imageSrc;
  }

  // Ensure current text block is processed before adding image
  if (currentTextBlock && !currentTextBlock->isEmpty()) {
    makePages();
  }

  // Ensure current page exists
  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // Calculate max dimensions based on screen orientation
  // Portrait mode (width < height): prioritize full width
  // Landscape mode (width > height): prioritize full height
  const bool isLandscape = viewportWidth > viewportHeight;

  uint16_t maxWidth, maxHeight;
  if (isLandscape) {
    // Landscape: prioritize full height, allow full viewport
    maxWidth = viewportWidth;
    maxHeight = viewportHeight;  // Full height for landscape
  } else {
    // Portrait: prioritize full width, allow up to 90% of height
    maxWidth = viewportWidth;
    maxHeight = viewportHeight * 9 / 10;  // 90% height for portrait
  }

  // Create the image block (this will convert JPEG to BMP and cache it)
  const std::string cacheDir = epub.getCachePath() + "/images";
  SdMan.mkdir(cacheDir.c_str());

  auto imageBlock = ImageBlock::createFromEpub(epub, imageHref, cacheDir, maxWidth, maxHeight);

  if (imageBlock) {
    addImageToPage(std::move(imageBlock));
  }
}

void ChapterHtmlSlimParser::makePages() {
  if (!currentTextBlock) {
    Serial.printf("[%lu] [EHP] !! No text block to make pages for !!\n", millis());
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, viewportWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });
  // Extra paragraph spacing if enabled
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}
