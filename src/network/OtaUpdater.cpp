#include "OtaUpdater.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/danoooob/crosspoint-reader-vi/releases/latest";
// Limit to 10 releases to reduce memory usage on ESP32
constexpr char allReleasesUrl[] = "https://api.github.com/repos/danoooob/crosspoint-reader-vi/releases?per_page=10";

bool isDevVersion() {
  const std::string version = CROSSPOINT_VERSION;
  return version.find("-dev") != std::string::npos;
}
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  if (isDevVersion()) {
    return checkForPrereleaseUpdate();
  }
  return checkForStableUpdate();
}

OtaUpdater::OtaUpdaterError OtaUpdater::checkForStableUpdate() {
  const std::unique_ptr<WiFiClientSecure> client(new WiFiClientSecure);
  client->setInsecure();
  HTTPClient http;

  Serial.printf("[%lu] [OTA] Fetching: %s\n", millis(), latestReleaseUrl);

  http.begin(*client, latestReleaseUrl);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[%lu] [OTA] HTTP error: %d\n", millis(), httpCode);
    http.end();
    return HTTP_ERROR;
  }

  JsonDocument doc;
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["size"] = true;
  const DeserializationError error = deserializeJson(doc, *client, DeserializationOption::Filter(filter));
  http.end();
  if (error) {
    Serial.printf("[%lu] [OTA] JSON parse failed: %s\n", millis(), error.c_str());
    return JSON_PARSE_ERROR;
  }

  if (!doc["tag_name"].is<std::string>()) {
    Serial.printf("[%lu] [OTA] No tag_name found\n", millis());
    return JSON_PARSE_ERROR;
  }
  if (!doc["assets"].is<JsonArray>()) {
    Serial.printf("[%lu] [OTA] No assets found\n", millis());
    return JSON_PARSE_ERROR;
  }

  latestVersion = doc["tag_name"].as<std::string>();

  for (int i = 0; i < doc["assets"].size(); i++) {
    if (doc["assets"][i]["name"] == "firmware.bin") {
      otaUrl = doc["assets"][i]["browser_download_url"].as<std::string>();
      otaSize = doc["assets"][i]["size"].as<size_t>();
      totalSize = otaSize;
      updateAvailable = true;
      break;
    }
  }

  if (!updateAvailable) {
    Serial.printf("[%lu] [OTA] No firmware.bin asset found\n", millis());
    return NO_UPDATE;
  }

  Serial.printf("[%lu] [OTA] Found update: %s\n", millis(), latestVersion.c_str());
  return OK;
}

OtaUpdater::OtaUpdaterError OtaUpdater::checkForPrereleaseUpdate() {
  const std::unique_ptr<WiFiClientSecure> client(new WiFiClientSecure);
  client->setInsecure();
  client->setTimeout(30000);  // 30 second timeout
  HTTPClient http;

  Serial.printf("[%lu] [OTA] Fetching prereleases: %s\n", millis(), allReleasesUrl);

  http.begin(*client, allReleasesUrl);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  http.setTimeout(30000);  // 30 second timeout

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[%lu] [OTA] HTTP error: %d\n", millis(), httpCode);
    http.end();
    return HTTP_ERROR;
  }

  // Read entire response into string first to avoid stream timeout issues
  const String payload = http.getString();
  http.end();

  JsonDocument doc;
  JsonDocument filter;
  filter[0]["tag_name"] = true;
  filter[0]["prerelease"] = true;
  filter[0]["assets"][0]["name"] = true;
  filter[0]["assets"][0]["browser_download_url"] = true;
  filter[0]["assets"][0]["size"] = true;
  const DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));

  if (error) {
    Serial.printf("[%lu] [OTA] JSON parse failed: %s\n", millis(), error.c_str());
    return JSON_PARSE_ERROR;
  }

  if (!doc.is<JsonArray>()) {
    Serial.printf("[%lu] [OTA] Expected array of releases\n", millis());
    return JSON_PARSE_ERROR;
  }

  // Find the latest prerelease with "-dev-" in tag name and firmware.bin asset
  for (JsonVariant release : doc.as<JsonArray>()) {
    if (!release["prerelease"].as<bool>()) {
      continue;
    }

    const std::string tagName = release["tag_name"].as<std::string>();

    // Must have "-dev-" in tag name
    if (tagName.find("-dev-") == std::string::npos) {
      continue;
    }

    if (!release["assets"].is<JsonArray>()) {
      continue;
    }

    for (JsonVariant asset : release["assets"].as<JsonArray>()) {
      if (asset["name"] == "firmware.bin") {
        latestVersion = tagName;
        otaUrl = asset["browser_download_url"].as<std::string>();
        otaSize = asset["size"].as<size_t>();
        totalSize = otaSize;
        updateAvailable = true;
        Serial.printf("[%lu] [OTA] Found prerelease update: %s (size: %u, url: %s)\n", millis(), latestVersion.c_str(),
                      otaSize, otaUrl.c_str());
        return OK;
      }
    }
  }

  Serial.printf("[%lu] [OTA] No prerelease with firmware.bin found\n", millis());
  return NO_UPDATE;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty()) {
    return false;
  }

  const std::string currentVersion = CROSSPOINT_VERSION;

  // If versions are exactly the same, no update needed
  if (latestVersion == currentVersion) {
    return false;
  }

  // For dev versions, compare using the timestamp suffix
  // Format: <version>-dev-<YYMMDDhhmm> (e.g., 0.13.1-dev-2512312259)
  if (isDevVersion()) {
    // Extract the base version and timestamp from both versions

    // Find the last hyphen to extract the timestamp
    const auto currentLastHyphen = currentVersion.rfind('-');
    const auto latestLastHyphen = latestVersion.rfind('-');

    if (currentLastHyphen != std::string::npos && latestLastHyphen != std::string::npos) {
      const std::string currentTimestamp = currentVersion.substr(currentLastHyphen + 1);
      const std::string latestTimestamp = latestVersion.substr(latestLastHyphen + 1);

      // Convert to numbers for proper numeric comparison
      // This handles different timestamp lengths correctly
      const unsigned long long currentTs = strtoull(currentTimestamp.c_str(), nullptr, 10);
      const unsigned long long latestTs = strtoull(latestTimestamp.c_str(), nullptr, 10);

      return latestTs > currentTs;
    }

    // Fallback to string comparison if we can't parse timestamps
    return latestVersion > currentVersion;
  }

  // For stable versions, use semantic version comparison
  int currentMajor = 0, currentMinor = 0, currentPatch = 0;
  int latestMajor = 0, latestMinor = 0, latestPatch = 0;

  sscanf(latestVersion.c_str(), "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentVersion.c_str(), "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  return latestPatch > currentPatch;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(const std::function<void(size_t, size_t)>& onProgress) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  const std::unique_ptr<WiFiClientSecure> client(new WiFiClientSecure);
  client->setInsecure();
  HTTPClient http;

  Serial.printf("[%lu] [OTA] Fetching: %s\n", millis(), otaUrl.c_str());

  http.begin(*client, otaUrl.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  const int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[%lu] [OTA] Download failed: %d\n", millis(), httpCode);
    http.end();
    return HTTP_ERROR;
  }

  // 2. Get length and stream
  const size_t contentLength = http.getSize();

  if (contentLength != otaSize) {
    Serial.printf("[%lu] [OTA] Invalid content length\n", millis());
    http.end();
    return HTTP_ERROR;
  }

  // 3. Begin the ESP-IDF Update process
  if (!Update.begin(otaSize)) {
    Serial.printf("[%lu] [OTA] Not enough space. Error: %s\n", millis(), Update.errorString());
    http.end();
    return INTERNAL_UPDATE_ERROR;
  }

  this->totalSize = otaSize;
  Serial.printf("[%lu] [OTA] Update started\n", millis());
  Update.onProgress([this, onProgress](const size_t progress, const size_t total) {
    this->processedSize = progress;
    this->totalSize = total;
    onProgress(progress, total);
  });
  const size_t written = Update.writeStream(*client);
  http.end();

  if (written == otaSize) {
    Serial.printf("[%lu] [OTA] Successfully written %u bytes\n", millis(), written);
  } else {
    Serial.printf("[%lu] [OTA] Written only %u/%u bytes. Error: %s\n", millis(), written, otaSize,
                  Update.errorString());
    return INTERNAL_UPDATE_ERROR;
  }

  if (Update.end() && Update.isFinished()) {
    Serial.printf("[%lu] [OTA] Update complete\n", millis());
    return OK;
  } else {
    Serial.printf("[%lu] [OTA] Error Occurred: %s\n", millis(), Update.errorString());
    return INTERNAL_UPDATE_ERROR;
  }
}
