#include "TrmnlSleepClient.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <Logging.h>
#include <TrmnlDisplayJsonParser.h>
#include <WiFi.h>

#include <cstdarg>
#include <cstdio>

#include "WifiCredentialStore.h"
#include "network/HttpDownloader.h"
#include "util/UrlUtils.h"

namespace {
constexpr const char* TRMNL_CRASH_REPORT_PATH = "/.crosspoint/crash_report.txt";

void appendCrashReportLine(const char* line) {
  FsFile report = Storage.open(TRMNL_CRASH_REPORT_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!report) {
    return;
  }
  report.write(reinterpret_cast<const uint8_t*>(line), strlen(line));
  report.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  report.flush();
  report.close();
}

void trmnlDiag(const char* fmt, ...) {
  char message[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  char line[320];
  snprintf(line, sizeof(line), "[%lu] [TRM] %s", static_cast<unsigned long>(millis()), message);
  appendCrashReportLine(line);
}

TrmnlDisplayJsonParser& trmnlDisplayParser() {
  // Keep parser storage off the call stack; this path runs in constrained contexts.
  static TrmnlDisplayJsonParser parser;
  parser.reset();
  return parser;
}

const WifiCredential* getTrmnlWifiCredential() {
  const std::string& lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (!lastSsid.empty()) {
    if (const auto* cred = WIFI_STORE.findCredential(lastSsid)) {
      return cred;
    }
  }
  const auto& credentials = WIFI_STORE.getCredentials();
  return credentials.empty() ? nullptr : &credentials.front();
}

bool validateBmpFile(const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("TRM", path.c_str(), file)) {
    return false;
  }
  Bitmap bitmap(file, true);
  const bool ok = bitmap.parseHeaders() == BmpReaderError::Ok;
  file.close();
  return ok;
}
}  // namespace

bool TrmnlSleepClient::hasConfig(const Config& config) {
  return config.serverUrl && config.serverUrl[0] != '\0' && config.apiKey && config.apiKey[0] != '\0';
}

bool TrmnlSleepClient::connectWifi() {
  WIFI_STORE.loadFromFile();
  const auto* cred = getTrmnlWifiCredential();
  if (!cred || cred->ssid.empty()) {
    LOG_ERR("TRM", "No saved Wi-Fi credential for TRMNL");
    return false;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.setSleep(false);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());

  for (uint8_t attempt = 0; attempt < WIFI_RETRIES; attempt++) {
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
      WIFI_STORE.setLastConnectedSsid(cred->ssid);
      return true;
    }
    delay(WIFI_RETRY_DELAY_MS);
  }

  LOG_ERR("TRM", "Wi-Fi connect timed out for TRMNL");
  return false;
}

void TrmnlSleepClient::disconnectWifi() {
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

std::string TrmnlSleepClient::resolveDeviceId(const char* configuredDeviceId) {
  if (configuredDeviceId && configuredDeviceId[0] != '\0') {
    return configuredDeviceId;
  }
  uint8_t mac[6] = {};
  WiFi.macAddress(mac);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return macStr;
}

TrmnlSleepClient::ImageKind TrmnlSleepClient::detectImageKind(const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("TRM", path.c_str(), file)) {
    return ImageKind::Unknown;
  }
  uint8_t header[8] = {};
  const int bytesRead = file.read(header, sizeof(header));
  file.close();
  if (bytesRead >= 2 && header[0] == 'B' && header[1] == 'M') {
    return ImageKind::Bmp;
  }
  if (bytesRead >= 8 && header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G' &&
      header[4] == '\r' && header[5] == '\n' && header[6] == 0x1A && header[7] == '\n') {
    return ImageKind::Png;
  }
  return ImageKind::Unknown;
}

bool TrmnlSleepClient::validateImage(const std::string& path, ImageKind& kind) {
  kind = detectImageKind(path);
  if (kind == ImageKind::Bmp) {
    return validateBmpFile(path);
  }
  return kind == ImageKind::Png;
}

const char* TrmnlSleepClient::cachePathFor(const ImageKind kind) {
  if (kind == ImageKind::Png) return CACHE_PNG;
  if (kind == ImageKind::Bmp) return CACHE_BMP;
  return nullptr;
}

bool TrmnlSleepClient::replaceCache(const std::string& tmpPath, const ImageKind kind) {
  const char* destPath = cachePathFor(kind);
  if (!destPath) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  const char* staleOther = kind == ImageKind::Png ? CACHE_BMP : CACHE_PNG;
  if (Storage.exists(destPath) && !Storage.remove(destPath)) {
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), destPath)) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (Storage.exists(staleOther)) {
    Storage.remove(staleOther);
  }
  return true;
}

bool TrmnlSleepClient::fetchLatest(const Config& config) {
  Storage.mkdir("/.crosspoint");
  trmnlDiag("fetchLatest start server=%s hasKey=%d hasDeviceId=%d", config.serverUrl ? config.serverUrl : "(null)",
            (config.apiKey && config.apiKey[0] != '\0') ? 1 : 0, (config.deviceId && config.deviceId[0] != '\0') ? 1 : 0);

  if (!hasConfig(config)) {
    LOG_DBG("TRM", "TRMNL settings incomplete");
    trmnlDiag("settings incomplete");
    return false;
  }

  if (!connectWifi()) {
    trmnlDiag("wifi connect failed");
    disconnectWifi();
    return false;
  }
  trmnlDiag("wifi connected ip=%s", WiFi.localIP().toString().c_str());

  const std::string deviceId = resolveDeviceId(config.deviceId);
  char widthHeader[8];
  char heightHeader[8];
  snprintf(widthHeader, sizeof(widthHeader), "%d", config.displaySize.width);
  snprintf(heightHeader, sizeof(heightHeader), "%d", config.displaySize.height);
  const HttpDownloader::Header headers[] = {{"ID", deviceId.c_str()},
                                            {"Access-Token", config.apiKey},
                                            {"Width", widthHeader},
                                            {"Height", heightHeader},
                                            {"Model", config.model ? config.model : "og"}};
  const std::string serverUrl = UrlUtils::ensureProtocol(config.serverUrl);
  const std::string displayUrl = UrlUtils::buildUrl(serverUrl, "/api/display");
  trmnlDiag("display url=%s", displayUrl.c_str());

  std::string response;
  bool fetched = HttpDownloader::fetchUrl(displayUrl, response, "", "", headers, sizeof(headers) / sizeof(headers[0]),
                                          DISPLAY_JSON_MAX_BYTES);
  trmnlDiag("display fetch result=%d bytes=%u", fetched ? 1 : 0, static_cast<unsigned>(response.size()));
  if (!fetched) {
    LOG_ERR("TRM", "Display JSON fetch failed (url=%s, maxBytes=%u)", displayUrl.c_str(),
            static_cast<unsigned>(DISPLAY_JSON_MAX_BYTES));
    trmnlDiag("display fetch failed maxBytes=%u", static_cast<unsigned>(DISPLAY_JSON_MAX_BYTES));
  }
  if (fetched) {
    TrmnlDisplayJsonParser& parser = trmnlDisplayParser();
    parser.feed(response.c_str(), response.length());
    fetched = !parser.hasError() && parser.foundImageUrl();
    trmnlDiag("display parse result=%d parseErr=%d foundImageUrl=%d", fetched ? 1 : 0, parser.hasError() ? 1 : 0,
              parser.foundImageUrl() ? 1 : 0);
    if (!fetched) {
      LOG_ERR("TRM", "Display JSON parse failed (parseErr=%d, imageUrlFound=%d)",
              parser.hasError() ? 1 : 0, parser.foundImageUrl() ? 1 : 0);
    }
    if (fetched) {
      const std::string imageUrl = UrlUtils::buildUrl(serverUrl, parser.getImageUrl());
      const bool sameHost = UrlUtils::extractHost(imageUrl) == UrlUtils::extractHost(serverUrl);
      trmnlDiag("image url len=%u sameHost=%d", static_cast<unsigned>(imageUrl.size()), sameHost ? 1 : 0);
      const HttpDownloader::Header* imageHeaders = sameHost ? headers : nullptr;
      const size_t imageHeaderCount = sameHost ? sizeof(headers) / sizeof(headers[0]) : 0;
      if (Storage.exists(CACHE_TMP)) {
        Storage.remove(CACHE_TMP);
      }
      const HttpDownloader::DownloadOptions options(false, false, nullptr, 1024, IMAGE_MAX_BYTES);
      const HttpDownloader::DownloadError downloadError =
          HttpDownloader::downloadToFile(imageUrl, CACHE_TMP, nullptr, nullptr, "", "", options, imageHeaders,
                                         imageHeaderCount);
      fetched = downloadError == HttpDownloader::OK;
      trmnlDiag("image download result=%d err=%d", fetched ? 1 : 0, static_cast<int>(downloadError));
      if (!fetched) {
        LOG_ERR("TRM", "Image download failed (err=%d, maxBytes=%u, sameHost=%d, urlLen=%u)",
                static_cast<int>(downloadError), static_cast<unsigned>(IMAGE_MAX_BYTES), sameHost ? 1 : 0,
                static_cast<unsigned>(imageUrl.size()));
        trmnlDiag("image download diagnostics httpCode=%d streamErr=%d", HttpDownloader::getLastHttpCode(),
                  HttpDownloader::getLastStreamError());
      }
      if (fetched) {
        ImageKind kind = ImageKind::Unknown;
        fetched = validateImage(CACHE_TMP, kind) && replaceCache(CACHE_TMP, kind);
        trmnlDiag("image validate+cache result=%d kind=%d", fetched ? 1 : 0, static_cast<int>(kind));
        if (!fetched) {
          LOG_ERR("TRM", "Downloaded file was not a valid TRMNL sleep image");
        }
      }
    }
  }

  disconnectWifi();
  trmnlDiag("wifi disconnected fetched=%d", fetched ? 1 : 0);
  if (!fetched) {
    LOG_ERR("TRM", "TRMNL fetch failed");
    Storage.remove(CACHE_TMP);
    trmnlDiag("fetch failed; removed tmp");
  }
  trmnlDiag("fetchLatest end result=%d", fetched ? 1 : 0);
  return fetched;
}
