#pragma once
#include <Arduino.h>
#include <SPI.h>

// Provenance: UC8253 LUTs vendored from freeink-sdk e93f67a
// Source: CrossInk/freeink-sdk/libs/display/FreeInkDisplay/src/lut/Uc8253X3Luts.h
// License: MIT (freeink-sdk)

class EInkDisplay {
 public:
  // Constructor with pin configuration
  EInkDisplay(int8_t sclk, int8_t mosi, int8_t cs, int8_t dc, int8_t rst, int8_t busy);

  // Destructor
  ~EInkDisplay() = default;

  // Refresh modes (guarded to avoid redefinition in test builds)
  enum RefreshMode {
    FULL_REFRESH,  // Full refresh with complete waveform
    HALF_REFRESH,  // Half refresh - balanced quality and speed
    FAST_REFRESH   // Fast refresh using custom LUT
  };

  // Initialize the display hardware and driver
  // panelType: 0 = auto-detect (uses deviceType param), 1 = SSD1677 (X4), 2 = UC8253 (X3)
  void begin(uint8_t deviceType = 1);

  // Display dimensions - X4 (SSD1677) constants
  static constexpr uint16_t X4_DISPLAY_WIDTH = 800;
  static constexpr uint16_t X4_DISPLAY_HEIGHT = 480;
  // X3 (UC8253) constants
  static constexpr uint16_t X3_DISPLAY_WIDTH = 792;
  static constexpr uint16_t X3_DISPLAY_HEIGHT = 528;

  // Maximum dimensions across both panels (for static buffer sizing)
  static constexpr uint16_t MAX_DISPLAY_WIDTH = X3_DISPLAY_HEIGHT;   // 528 (portrait max)
  static constexpr uint16_t MAX_DISPLAY_HEIGHT = X4_DISPLAY_WIDTH;   // 800 (landscape max)
  static constexpr uint16_t MAX_DISPLAY_WIDTH_BYTES = MAX_DISPLAY_HEIGHT / 8;  // 100
  static constexpr uint32_t MAX_BUFFER_SIZE = MAX_DISPLAY_WIDTH_BYTES * MAX_DISPLAY_HEIGHT;  // 800*100 = 80,000... too big

  // Actual buffer sizing: X4 landscape = 800x480 = 48,000 B, X3 portrait = 528x792 = 52,272 B
  // Use the larger of the two common orientations
  static constexpr uint32_t BUFFER_SIZE = (X3_DISPLAY_WIDTH / 8) * X3_DISPLAY_HEIGHT;  // 52,272 B

  // Runtime geometry getters
  uint16_t getDisplayWidth() const { return _displayWidth; }
  uint16_t getDisplayHeight() const { return _displayHeight; }
  uint16_t getDisplayWidthBytes() const { return _displayWidthBytes; }
  uint32_t getBufferSize() const { return _bufferSize; }

  // Legacy constexprs for backward compatibility (X4 defaults)
  static constexpr uint16_t DISPLAY_WIDTH = X4_DISPLAY_WIDTH;
  static constexpr uint16_t DISPLAY_HEIGHT = X4_DISPLAY_HEIGHT;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;

  // Frame buffer operations
  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fromProgmem = false) const;

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  void swapBuffers();
#endif
  void setFramebuffer(const uint8_t* bwBuffer) const;

  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);
#endif

  void displayBuffer(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false);

  // Non-blocking refresh API
  void beginRefresh(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false);
  bool isRefreshing() const { return _refreshState != IDLE; }
  bool pollRefresh();
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  void displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen = false);
  void displayGrayBuffer(bool turnOffScreen = false);

  void refreshDisplay(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false);

  // Non-X4 panels need a full resync after sleep/wake to prevent ghosting
  void requestResync(uint8_t settlePasses = 1);

  // debug function
  void grayscaleRevert();

  // LUT control
  void setCustomLUT(bool enabled, const unsigned char* lutData = nullptr);

  // Power management
  void deepSleep();

  // Access to frame buffer
  uint8_t* getFrameBuffer() const {
    return frameBuffer;
  }

  // Save the current framebuffer to a PBM file (desktop/test builds only)
  void saveFrameBufferAsPBM(const char* filename);

  // Panel type query
  bool isX3() const { return _panelType == PANEL_UC8253; }
  bool isX4() const { return _panelType == PANEL_SSD1677; }

 private:
  enum PanelType { PANEL_SSD1677, PANEL_UC8253 };

  // Pin configuration
  int8_t _sclk, _mosi, _cs, _dc, _rst, _busy;

  // Panel type and runtime geometry
  PanelType _panelType = PANEL_SSD1677;
  uint16_t _displayWidth;
  uint16_t _displayHeight;
  uint16_t _displayWidthBytes;
  uint32_t _bufferSize;

  // Frame buffer (statically allocated - sized for largest panel)
  uint8_t frameBuffer0[BUFFER_SIZE];
  uint8_t* frameBuffer;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  uint8_t frameBuffer1[BUFFER_SIZE];
  uint8_t* frameBufferActive;
#endif

  // SPI settings
  SPISettings spiSettings;

  // State
  bool isScreenOn;
  bool customLutActive;
  bool inGrayscaleMode;
  bool drawGrayscale;

  // Non-blocking refresh state
  enum RefreshState { IDLE, REFRESHING, NEEDS_RED_SYNC };
  RefreshState _refreshState = IDLE;
  RefreshMode _pendingMode = FAST_REFRESH;
  unsigned long _refreshStartMs = 0;

  // X3 resync tracking
  uint8_t _pendingResyncs = 0;

  // ========== SSD1677 (X4) commands ==========
  void initSSD1677();
  void refreshSSD1677(RefreshMode mode, bool turnOffScreen);

  // ========== UC8253 (X3) commands ==========
  void initUC8253();
  void refreshUC8253(RefreshMode mode, bool turnOffScreen);
  void loadUC8253Bank(const uint8_t* vcom, const uint8_t* ww, const uint8_t* bw, const uint8_t* wb, const uint8_t* bb);
  void loadUC8253BankCdi(uint8_t cdi0, uint8_t cdi1, const uint8_t* vcom, const uint8_t* ww,
                         const uint8_t* bw, const uint8_t* wb, const uint8_t* bb);
  void triggerUC8253Refresh(bool turnOff);
  void fillUC8253Plane(uint8_t cmd, uint8_t value, uint16_t rows, uint16_t widthBytes);
  void sendUC8253Plane(uint8_t cmd, const uint8_t* data, uint16_t rows, uint16_t widthBytes);

  // Low-level display control (shared)
  void resetDisplay();
  void sendCommand(uint8_t command);
  void sendData(uint8_t data);
  void sendData(const uint8_t* data, uint16_t length);
  void waitWhileBusy(const char* comment = nullptr);

  // Low-level display operations
  void setRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
  void writeRamBuffer(uint8_t ramBuffer, const uint8_t* data, uint32_t size);
};
