#pragma once

#include <string>

#include "activities/Activity.h"

/**
 * NewsDownloadActivity connects to the configured Wi-Fi network (via
 * WifiSelectionActivity, which auto-connects to saved networks) and downloads
 * the daily news EPUB from a GitHub repository into /_news/daily.epub on the
 * SD card, replacing any previous edition. The repository URL and access
 * token are read from /news_reader.json on the SD card.
 */
class NewsDownloadActivity final : public Activity {
 public:
  explicit NewsDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("NewsDownload", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    // The download is synchronous and blocks the main loop until it
    // completes, so activityManager.preventAutoSleep() is never polled
    // during downloading; keep COMPLETE/ERROR awake for the user to read.
    return state_ == DOWNLOADING || state_ == COMPLETE || state_ == ERROR;
  }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  State state_ = WIFI_SELECTION;

  // Loaded from /news_reader.json by loadConfig()
  std::string downloadUrl_;
  std::string token_;

  // Download progress
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  bool cancelRequested_ = false;
  std::string errorMessage_;

  void onWifiSelectionComplete(bool success);
  bool loadConfig();
  void startDownload();
};
