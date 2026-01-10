#include "FileSelectionActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include "MappedInputManager.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;
}  // namespace

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    if (str1.back() == '/' && str2.back() != '/') return true;
    if (str1.back() != '/' && str2.back() == '/') return false;
    return lexicographical_compare(
        begin(str1), end(str1), begin(str2), end(str2),
        [](const char& char1, const char& char2) { return tolower(char1) < tolower(char2); });
  });
}

void FileSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<FileSelectionActivity*>(param);
  self->displayTaskLoop();
}

void FileSelectionActivity::loadFiles() {
  files.clear();

  auto root = SdMan.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
    } else {
      auto filename = std::string(name);
      if (StringUtils::checkFileExtension(filename, ".epub") || StringUtils::checkFileExtension(filename, ".xtch") ||
          StringUtils::checkFileExtension(filename, ".xtc")) {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);
}

void FileSelectionActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // basepath is set via constructor parameter (defaults to "/" if not specified)
  loadFiles();
  selectorIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&FileSelectionActivity::taskTrampoline, "FileSelectionActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void FileSelectionActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  files.clear();
}

void FileSelectionActivity::loop() {
  // Long press BACK (1s+) goes to root folder
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS) {
    if (basepath != "/") {
      basepath = "/";
      loadFiles();
      updateRequired = true;
    }
    return;
  }

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (files.empty()) {
      return;
    }

    if (basepath.back() != '/') basepath += "/";
    if (files[selectorIndex].back() == '/') {
      basepath += files[selectorIndex].substr(0, files[selectorIndex].length() - 1);
      loadFiles();
      selectorIndex = 0;
      updateRequired = true;
    } else {
      onSelect(basepath + files[selectorIndex]);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();

        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName);

        updateRequired = true;
      } else {
        onGoHome();
      }
    }
  } else if (prevReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS - 1) * PAGE_ITEMS + files.size()) % files.size();
    } else {
      selectorIndex = (selectorIndex + files.size() - 1) % files.size();
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS + 1) * PAGE_ITEMS) % files.size();
    } else {
      selectorIndex = (selectorIndex + 1) % files.size();
    }
    updateRequired = true;
  }
}

void FileSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void FileSelectionActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Books", true, EpdFontFamily::BOLD);

  // Help text
  const auto labels = mappedInput.mapLabels("« Home", "Open", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, 20, 60, "No books found");
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
  const int maxTextWidth = renderer.getScreenWidth() - 40;

  // Check if selected item needs 2 lines
  const int selectedTextWidth = renderer.getTextWidth(UI_10_FONT_ID, files[selectorIndex].c_str());
  const bool selectedNeedsTwoLines = selectedTextWidth > maxTextWidth;
  const int selectedRowInPage = selectorIndex % PAGE_ITEMS;

  // Draw selection highlight (1.5x height if 2 lines needed)
  const int highlightHeight = selectedNeedsTwoLines ? 52 : 30;
  renderer.fillRect(0, 60 + selectedRowInPage * 30 - 2, pageWidth - 1, highlightHeight);

  for (size_t i = pageStartIndex; i < files.size() && i < pageStartIndex + PAGE_ITEMS; i++) {
    const int rowInPage = i % PAGE_ITEMS;
    int yOffset = 0;

    // Shift items below the selected item down if selected needs 2 lines
    if (selectedNeedsTwoLines && rowInPage > selectedRowInPage) {
      yOffset = 22;  // Extra offset to match the taller highlight
    }

    if (i == selectorIndex && selectedNeedsTwoLines) {
      // Draw selected item on 2 lines
      const std::string& text = files[i];
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

      // Find a good split point (try to split at space near middle)
      size_t splitPos = text.length() / 2;
      size_t spacePos = text.rfind(' ', splitPos + 10);
      if (spacePos != std::string::npos && spacePos > text.length() / 4) {
        splitPos = spacePos;
      } else {
        // No good space found, just split in half
        spacePos = text.find(' ', splitPos);
        if (spacePos != std::string::npos && spacePos < text.length() * 3 / 4) {
          splitPos = spacePos;
        }
      }

      std::string line1 = text.substr(0, splitPos);
      std::string line2 = (splitPos < text.length()) ? text.substr(splitPos + 1) : "";

      // Truncate each line if still too long
      auto item1 = renderer.truncatedText(UI_10_FONT_ID, line1.c_str(), maxTextWidth);
      auto item2 = renderer.truncatedText(UI_10_FONT_ID, line2.c_str(), maxTextWidth);

      renderer.drawText(UI_10_FONT_ID, 20, 60 + rowInPage * 30, item1.c_str(), false);
      renderer.drawText(UI_10_FONT_ID, 20, 60 + rowInPage * 30 + lineHeight, item2.c_str(), false);
    } else {
      // Draw single line item
      auto item = renderer.truncatedText(UI_10_FONT_ID, files[i].c_str(), maxTextWidth);
      renderer.drawText(UI_10_FONT_ID, 20, 60 + rowInPage * 30 + yOffset, item.c_str(), i != selectorIndex);
    }
  }

  renderer.displayBuffer();
}

size_t FileSelectionActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
