#include "NewsDownloadActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>
#include <memory>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
// SD-card config file with the GitHub repo URL and access token, e.g.:
//   { "repoUrl": "https://github.com/user/news-repo",
//     "token": "github_pat_...",
//     "path": "daily.epub" }   // optional, defaults to daily.epub
constexpr const char* NEWS_CONFIG_PATH = "/news_reader.json";
constexpr const char* NEWS_DIR = "/_news";
constexpr const char* NEWS_EPUB_PATH = "/_news/daily.epub";
// Downloaded to a temp name first so a failed transfer never destroys the
// previous edition; renamed over daily.epub only after the download succeeds.
constexpr const char* NEWS_EPUB_TMP_PATH = "/_news/daily.epub.tmp";
}  // namespace

// --- Lifecycle ---

void NewsDownloadActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void NewsDownloadActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void NewsDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }
  startDownload();
}

// --- Config ---

bool NewsDownloadActivity::loadConfig() {
  HalFile file;
  if (!Storage.openFileForRead("NEWS", NEWS_CONFIG_PATH, file)) {
    LOG_ERR("NEWS", "Missing config: %s", NEWS_CONFIG_PATH);
    errorMessage_ = "Missing news_reader.json on SD card";
    return false;
  }

  // Config is a handful of short strings; JsonDocument stays small and is
  // freed when this function returns.
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, file);
  if (err) {
    LOG_ERR("NEWS", "Config parse error: %s", err.c_str());
    errorMessage_ = "Invalid news_reader.json";
    return false;
  }

  const std::string repoUrl = doc["repoUrl"] | "";
  token_ = doc["token"] | "";
  std::string path = doc["path"] | "daily.epub";
  if (repoUrl.empty() || token_.empty()) {
    LOG_ERR("NEWS", "Config missing repoUrl or token");
    errorMessage_ = "Config needs repoUrl and token";
    return false;
  }

  // Accept "https://github.com/owner/repo" (optionally with trailing "/" or
  // ".git") or a bare "owner/repo", and derive the Contents API endpoint for
  // the file on the repo's default branch. With the raw media type the
  // response body is the file itself (supported for files up to 100 MB), so
  // it can be streamed straight to the SD card.
  std::string ownerRepo = repoUrl;
  const size_t hostPos = ownerRepo.find("github.com/");
  if (hostPos != std::string::npos) {
    ownerRepo = ownerRepo.substr(hostPos + strlen("github.com/"));
  }
  while (!ownerRepo.empty() && ownerRepo.back() == '/') {
    ownerRepo.pop_back();
  }
  if (ownerRepo.size() > 4 && ownerRepo.compare(ownerRepo.size() - 4, 4, ".git") == 0) {
    ownerRepo.erase(ownerRepo.size() - 4);
  }
  if (ownerRepo.empty() || ownerRepo.find('/') == std::string::npos) {
    LOG_ERR("NEWS", "Bad repoUrl: %s", repoUrl.c_str());
    errorMessage_ = "Invalid repoUrl in news_reader.json";
    return false;
  }

  while (!path.empty() && path.front() == '/') {
    path.erase(path.begin());
  }
  downloadUrl_ = "https://api.github.com/repos/" + ownerRepo + "/contents/" + path;
  LOG_DBG("NEWS", "News source: %s", downloadUrl_.c_str());
  return true;
}

// --- Download ---

void NewsDownloadActivity::startDownload() {
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    fileProgress_ = 0;
    fileTotal_ = 0;
    cancelRequested_ = false;
  }
  requestUpdateAndWait();

  // Re-read on every attempt so a Retry picks up an edited config file.
  if (!loadConfig()) {
    RenderLock lock(*this);
    state_ = ERROR;  // errorMessage_ set by loadConfig()
    return;
  }

  if (!Storage.ensureDirectoryExists(NEWS_DIR)) {
    LOG_ERR("NEWS", "Failed to create %s", NEWS_DIR);
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to create news folder";
    return;
  }

  const HttpDownloader::Headers headers = {
      {"Authorization", "Bearer " + token_},
      {"Accept", "application/vnd.github.raw+json"},
  };

  const auto result = HttpDownloader::downloadToFile(
      downloadUrl_, NEWS_EPUB_TMP_PATH,
      [this](size_t downloaded, size_t total) {
        fileProgress_ = downloaded;
        fileTotal_ = total;
        mappedInput.update();
        if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
            mappedInput.wasPressed(MappedInputManager::Button::Back)) {
          cancelRequested_ = true;
        }
        requestUpdate(true);
      },
      &cancelRequested_, "", "", headers);

  if (result == HttpDownloader::ABORTED) {
    finish();
    return;
  }

  if (result != HttpDownloader::OK) {
    LOG_ERR("NEWS", "Download failed (%d)", result);
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = result == HttpDownloader::FILE_ERROR ? "Failed to write to SD card" : "Check Wi-Fi and try again";
    return;
  }

  // Replace the previous edition only once the new one is fully on disk.
  if (Storage.exists(NEWS_EPUB_PATH) && !Storage.remove(NEWS_EPUB_PATH)) {
    LOG_ERR("NEWS", "Failed to remove old %s", NEWS_EPUB_PATH);
    Storage.remove(NEWS_EPUB_TMP_PATH);
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to replace old edition";
    return;
  }
  if (!Storage.rename(NEWS_EPUB_TMP_PATH, NEWS_EPUB_PATH)) {
    LOG_ERR("NEWS", "Failed to rename %s -> %s", NEWS_EPUB_TMP_PATH, NEWS_EPUB_PATH);
    Storage.remove(NEWS_EPUB_TMP_PATH);
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to save news file";
    return;
  }

  LOG_DBG("NEWS", "Downloaded %s (%zu bytes)", NEWS_EPUB_PATH, fileProgress_);
  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
  requestUpdate();
}

// --- Input handling ---

void NewsDownloadActivity::loop() {
  switch (state_) {
    case COMPLETE:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        finish();
      }
      break;
    case ERROR:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        finish();
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        startDownload();
        requestUpdateAndWait();
      }
      break;
    case WIFI_SELECTION:
    case DOWNLOADING:
      // WIFI_SELECTION: the child activity owns input. DOWNLOADING: the
      // download blocks the main loop; cancel is handled in its progress
      // callback.
      break;
  }
}

// --- Rendering ---

void NewsDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DOWNLOAD_NEWS));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_DOWNLOADING));

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }
    const int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NEWS_DOWNLOADED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_DOWNLOAD_FAILED), true, EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
