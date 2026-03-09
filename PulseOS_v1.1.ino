// ---- Laser Sweep type ----
struct Laser {
  bool active = false;
  bool vertical = false;  // true = moves in X, segment spans Y; false = moves in Y, segment spans X
  float pos = 0;          // x for vertical, y for horizontal
  float speed = 0;
  int thick = 0;
  int dir = 1;
  int span0 = 0;          // start of segment (y for vertical, x for horizontal)
  int spanLen = 0;        // length of segment
};
#include <M5Unified.h>
#include <math.h>
#include <Preferences.h>
#include "pulseos_boot_img_full.h"

// === StickS3 IR via ESP-IDF RMT (per M5 docs) ===
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

struct Rect { int x, y, w, h; };

// =======================================================
// Remote Clone storage (RMT symbols)
// =======================================================
static constexpr int RC_SYMBOL_MAX = 256;

struct RcCode {
  bool valid = false;
  bool isNEC = false;
  uint16_t address = 0;
  uint8_t command = 0;
  uint8_t repeats = 0;
};

// ------------------- UI Beeps -------------------
#define BEEP_NAV  1
#define BEEP_OK   2
#define BEEP_BACK 3

// ------------------- Speaker / Mic coexist helpers -------------------
static bool speakerPausedForMic = false;

static void speakerEnsureOn() {
  if (!speakerPausedForMic) return;
  M5.Speaker.begin();
  speakerPausedForMic = false;
}

static void speakerPauseForMic() {
  if (speakerPausedForMic) return;
  M5.Speaker.end();
  speakerPausedForMic = true;
}

static void uiBeep(int type) {
  if (type == BEEP_NAV)       { M5.Speaker.tone(1800, 18); }
  else if (type == BEEP_OK)   { M5.Speaker.tone(2300, 28); }
  else if (type == BEEP_BACK) { M5.Speaker.tone(1200, 35); }
}

// IR Tools: keep amp OFF, do burst beeps
static bool irToolsActive = false;

static void irBeepBurstHzMs(int hz, int ms) {
  M5.Speaker.begin();
  M5.Speaker.setVolume(90);
  M5.Speaker.tone(hz, ms);
  delay(ms + 2);
  M5.Speaker.end();
}

static void uiBeepIR(int type) {
  if (type == BEEP_NAV)       irBeepBurstHzMs(1800, 18);
  else if (type == BEEP_OK)   irBeepBurstHzMs(2300, 28);
  else if (type == BEEP_BACK) irBeepBurstHzMs(1200, 35);
}

// ------------------- Display size -------------------
static int W = 240;
static int H = 135;

// ------------------- Menu -------------------
enum AppId : uint8_t {
  APP_MENU = 0,
  APP_PLAY,
  APP_VISUALIZER,
  APP_IRTOOLS,
  APP_SETTINGS,

  // PLAY sub-apps
  APP_TILT_PUZZLE,
  APP_GRAVITY_DOCK,
  APP_PARTICLE_LAB,

  APP_COUNT
};

static AppId currentApp = APP_MENU;
static AppId lastApp    = APP_MENU;
static int selectedIndex = 1;

// Main menu items (exclude MENU itself)
static const char* menuItemsMain[] = {
  "MENU",
  "PLAY",
  "VISUALIZER",
  "IR TOOLS",
  "SETTINGS",
};
static constexpr int MAIN_COUNT = 5; // MENU + 4 items

// ------------------- App table -------------------
typedef void (*AppFn)();
struct App { const char* name; AppFn onEnter; AppFn onLoop; };

// Forward declarations
static void tiltLoop();
static void loadLevel(int level);
static void visualizerLoop();
static void visualizerEnter();
static void visualizerStopAudio();

static void irEnter();
static void irLoop();

static void settingsEnter();
static void settingsLoop();

static void playEnter();
static void playLoop();

static void gravityEnter();
static void gravityLoop();

static void particleEnter();
static void particleLoop();

// IMPORTANT: prototype for enterApp
static void enterApp(AppId id);

static void tiltEnter() { loadLevel(0); }

static App apps[APP_COUNT] = {
  { "MENU",        nullptr,         nullptr },
  { "PLAY",        playEnter,       playLoop },
  { "VISUALIZER",  visualizerEnter, visualizerLoop },
  { "IR TOOLS",    irEnter,         irLoop },
  { "SETTINGS",    settingsEnter,   settingsLoop },

  { "TILT PUZZLE", tiltEnter,       tiltLoop },
  { "LASER SWEEP", gravityEnter,    gravityLoop },  // renamed only
  { "PARTICLE LAB",particleEnter,   particleLoop },
};

// ------------------- Sprites -------------------
static M5Canvas menuCanvas(&M5.Display);
static M5Canvas appCanvas(&M5.Display);
static bool menuSpriteOK = false;
static bool appSpriteOK  = false;

// ------------------- Pulse (DISABLED: static UI) -------------------
static uint8_t pulse = 220;  // fixed brightness for theme colors
static void updatePulse() {}

// ------------------- Colors -------------------
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline uint16_t neonCyan(uint8_t p) {
  uint8_t r = 0;
  uint8_t g = (uint8_t)min(255, (int)(160 + (p * 95) / 255));
  uint8_t b = (uint8_t)min(255, (int)(170 + (p * 85) / 255));
  return rgb565(r, g, b);
}
static inline uint16_t neonGreen(uint8_t p) {
  uint8_t r = 0;
  uint8_t g = (uint8_t)min(255, (int)(190 + (p * 65) / 255));
  uint8_t b = (uint8_t)min(255, (int)(30  + (p * 30) / 255));
  return rgb565(r, g, b);
}
static inline uint16_t hotPink(uint8_t p) {
  uint8_t r = (uint8_t)min(255, (int)(210 + (p * 45) / 255));
  uint8_t g = (uint8_t)min(255, (int)(15  + (p * 25) / 255));
  uint8_t b = (uint8_t)min(255, (int)(140 + (p * 70) / 255));
  return rgb565(r, g, b);
}
static inline uint16_t dimCyan(uint8_t p) {
  uint8_t r = 0;
  uint8_t g = (uint8_t)min(255, (int)(90 + (p * 60) / 255));
  uint8_t b = (uint8_t)min(255, (int)(110 + (p * 70) / 255));
  return rgb565(r, g, b);
}

// ------------------- HSV -> RGB565 (visualizer) -------------------
static uint16_t hsvTo565(uint8_t h, uint8_t s, uint8_t v) {
  uint8_t region = h / 43;
  uint8_t remainder = (h - (region * 43)) * 6;

  uint8_t p = (uint16_t)v * (255 - s) / 255;
  uint8_t q = (uint16_t)v * (255 - ((uint16_t)s * remainder / 255)) / 255;
  uint8_t t = (uint16_t)v * (255 - ((uint16_t)s * (255 - remainder) / 255)) / 255;

  uint8_t r, g, b;
  switch (region) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default:r = v; g = p; b = q; break;
  }
  return rgb565(r, g, b);
}

// =======================================================
// SETTINGS: Rotation + Theme (persistent)
// =======================================================
static Preferences uiPrefs;
// =======================================================
// SCORES (persistent) - v1: Tilt best time only
// =======================================================
static Preferences scorePrefs;
static const char* SCORE_PREFS_NS = "pulseos_sc";
static const uint8_t SCORE_PREFS_VER = 1;

static uint32_t tiltBestCs = 0;        // best time in centiseconds (0 = none yet)
static uint32_t tiltLastCs = 0;        // last completion time (centiseconds)
static uint32_t tiltLevelStartMs = 0;  // when current level started
static uint32_t laserBestMs = 0;  // longest survival
static uint32_t particleBestMs = 0;      // fastest fill time
static uint32_t particleStartMs = 0;     // run start

static void scoreLoad() {
  scorePrefs.begin(SCORE_PREFS_NS, true);
  uint8_t ver = scorePrefs.getUChar("ver", 0);
  if (ver == SCORE_PREFS_VER) {
    tiltBestCs = scorePrefs.getUInt("tiltBest", 0);
    laserBestMs = scorePrefs.getUInt("laserBest", 0);
    particleBestMs = scorePrefs.getUInt("particleBest", 0);
  } else {
    tiltBestCs = 0;
  }
  scorePrefs.end();
}

static void scoreSaveTiltBest() {
  scorePrefs.begin(SCORE_PREFS_NS, false);
  scorePrefs.putUChar("ver", SCORE_PREFS_VER);
  scorePrefs.putUInt("tiltBest", tiltBestCs);
  scorePrefs.end();
}

static void fmtCs(uint32_t cs, char* out, size_t outLen) {
  uint32_t s  = cs / 100;
  uint32_t cc = cs % 100;
  snprintf(out, outLen, "%lu.%02lu", (unsigned long)s, (unsigned long)cc);
}
static const char* UI_PREFS_NS = "pulseos_ui";
static const uint8_t UI_PREFS_VER = 1;

static bool    ui_flipRotation = false;  // false = blue btn right, true = blue btn left
static uint8_t ui_theme = 0;             // 0..2

enum SettingsPage : uint8_t {
  SET_PAGE_LIST = 0,
  SET_PAGE_ROTATION,
  SET_PAGE_TIMER,
  SET_PAGE_BATTERY,
  SET_PAGE_THEME
};

static SettingsPage settingsPage = SET_PAGE_LIST;
static int settingsSel = 0;

// Timer
static bool timerRunning = false;
static uint32_t timerStartMs = 0;
static uint32_t timerAccumMs = 0;

static void uiSave() {
  uiPrefs.begin(UI_PREFS_NS, false);
  uiPrefs.putUChar("ver", UI_PREFS_VER);
  uiPrefs.putBool("flip", ui_flipRotation);
  uiPrefs.putUChar("theme", ui_theme);
  uiPrefs.end();
}

static void uiApplyRotation() {
  M5.Display.setRotation(ui_flipRotation ? 3 : 1);
  W = 240;
  H = 135;
}

static void uiLoad() {
  uiPrefs.begin(UI_PREFS_NS, true);
  uint8_t ver = uiPrefs.getUChar("ver", 0);
  if (ver == UI_PREFS_VER) {
    ui_flipRotation = uiPrefs.getBool("flip", false);
    ui_theme = uiPrefs.getUChar("theme", 0);
    if (ui_theme > 2) ui_theme = 0;
  }
  uiPrefs.end();
  uiApplyRotation();
}

// Theme wrappers (3 neon schemes)
static inline uint16_t themeCyan(uint8_t p) {
  if (ui_theme == 0) return neonCyan(p);
  if (ui_theme == 1) return rgb565(170, 70 + p/4, 255);
  return rgb565(60,  160 + p/5, 255);
}
static inline uint16_t themeGreen(uint8_t p) {
  if (ui_theme == 0) return neonGreen(p);
  if (ui_theme == 1) return rgb565(0,  210 + p/8, 140);
  return rgb565(255, 150 + p/6, 0);
}
static inline uint16_t themePink(uint8_t p) {
  if (ui_theme == 0) return hotPink(p);
  if (ui_theme == 1) return rgb565(255, 40 + p/10, 180);
  return rgb565(255, 60 + p/10, 60);
}
static inline uint16_t themeSub(uint8_t p) {
  if (ui_theme == 0) return dimCyan(p);
  if (ui_theme == 1) return rgb565(120, 120, 180);
  return rgb565(120, 140, 160);
}

// ------------------- HUD helpers -------------------
static void drawCornerBrackets(M5Canvas &c, int x, int y, int w, int h, uint16_t col) {
  int L = 10;
  c.drawLine(x, y, x + L, y, col);
  c.drawLine(x, y, x, y + L, col);

  c.drawLine(x + w, y, x + w - L, y, col);
  c.drawLine(x + w, y, x + w, y + L, col);

  c.drawLine(x, y + h, x + L, y + h, col);
  c.drawLine(x, y + h, x, y + h - L, col);

  c.drawLine(x + w, y + h, x + w - L, y + h, col);
  c.drawLine(x + w, y + h, x + w, y + h - L, col);
}

// Helper: trim string to fit width 
static void drawTrimmedText(M5Canvas& c, int x, int y, int maxW, const char* txt, uint16_t col) {
  String s(txt);
  while (s.length() > 0 && c.textWidth(s.c_str()) > maxW) {
    s.remove(s.length() - 1);
  }
  c.setTextColor(col);
  c.setCursor(x, y);
  c.print(s);
}

// ------------------- Input helper -------------------
static bool btnBHoldExit() {
  static bool holding = false;
  static uint32_t holdStart = 0;

  bool down = M5.BtnB.isPressed();
  if (down && !holding) {
    holding = true;
    holdStart = millis();
  }
  if (!down) {
    holding = false;
    return false;
  }
  if (holding && (millis() - holdStart) > 450) {
    holding = false;
    return true;
  }
  return false;
}

// ------------------- Math helpers -------------------
static float clampf(float v, float a, float b) { return (v < a) ? a : (v > b) ? b : v; }
static int clampi(int v, int lo, int hi) { return (v < lo) ? lo : (v > hi) ? hi : v; }

static bool rectOverlap(const Rect& a, const Rect& b) {
  return !(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y);
}

static bool circleIntersectsRect(float cx, float cy, int r, const Rect& rc) {
  float closestX = clampf(cx, rc.x, rc.x + rc.w);
  float closestY = clampf(cy, rc.y, rc.y + rc.h);
  float dx = cx - closestX;
  float dy = cy - closestY;
  return (dx*dx + dy*dy) < (float)(r*r);
}

// =======================================================
// MENU render
// =======================================================
static void drawMenuSprite() {
  updatePulse();
  uint16_t cyan   = themeCyan(pulse);
  uint16_t green  = themeGreen(pulse);
  uint16_t pink   = themePink(pulse);
  uint16_t subC   = themeSub(pulse);

  menuCanvas.fillScreen(TFT_BLACK);
  menuCanvas.fillRect(0, 0, W, 2, cyan);

  menuCanvas.setTextSize(2);
  menuCanvas.setTextColor(cyan);
  menuCanvas.setCursor(12, 6);
  menuCanvas.println("PulseOS");

  menuCanvas.setTextSize(1);
  menuCanvas.setTextColor(subC);
  menuCanvas.setCursor(12, 28);
  menuCanvas.println("v1.1");

  drawCornerBrackets(menuCanvas, 6, 44, W - 12, H - 52, green);

  menuCanvas.setTextSize(1);
  int startY = 56;
  int stepY  = 16;
  int boxH   = 14;

  for (int i = 1; i < MAIN_COUNT; ++i) {
    int y = startY + (i - 1) * stepY;
    bool sel = (i == selectedIndex);

    if (sel) {
      menuCanvas.drawRect(11, y - 3, W - 22, boxH, pink);
      menuCanvas.drawRect(12, y - 2, W - 24, boxH - 2, pink);
    }

    menuCanvas.setTextColor(sel ? pink : green);
    menuCanvas.setCursor(20, y);
    menuCanvas.println(menuItemsMain[i]);
  }

  menuCanvas.setTextSize(1);
  menuCanvas.setTextColor(subC);
  menuCanvas.setCursor(12, H - 14);
  menuCanvas.println("B: Next   A: Launch");
}

// =======================================================
// PLAY MENU (submenu)
// =======================================================
static int playSel = 0;
static const char* playItems[] = { "TILT PUZZLE", "LASER SWEEP", "PARTICLE LAB", "BACK" };
static constexpr int PLAY_COUNT = 4;

static void playEnter() {
  irToolsActive = false;
  speakerEnsureOn();
  playSel = 0;
}

static void playLoop() {
  updatePulse();
  uint16_t cyan  = themeCyan(pulse);
  uint16_t green = themeGreen(pulse);
  uint16_t pink  = themePink(pulse);
  uint16_t subC  = themeSub(pulse);

  if (btnBHoldExit()) {
    uiBeep(BEEP_BACK);
    enterApp(APP_MENU);
    return;
  }

  if (M5.BtnB.wasPressed()) {
    playSel = (playSel + 1) % PLAY_COUNT;
    uiBeep(BEEP_NAV);
  }
  if (M5.BtnA.wasPressed()) {
    uiBeep(BEEP_OK);
    if (playSel == 0) enterApp(APP_TILT_PUZZLE);
    else if (playSel == 1) enterApp(APP_GRAVITY_DOCK);
    else if (playSel == 2) enterApp(APP_PARTICLE_LAB);
    else enterApp(APP_MENU);
    return;
  }

  appCanvas.fillScreen(TFT_BLACK);
  appCanvas.fillRect(0, 0, W, 2, cyan);

  appCanvas.setTextSize(1);
  appCanvas.setTextColor(subC);
  appCanvas.setCursor(8, 8);
  appCanvas.println("PLAY  (hold B to exit)");

  drawCornerBrackets(appCanvas, 6, 26, W - 12, H - 40, green);

  int y0 = 44;
  for (int i = 0; i < PLAY_COUNT; i++) {
    bool sel = (i == playSel);
    if (sel) {
      appCanvas.drawRect(11, y0 + i*16 - 3, W - 22, 14, pink);
      appCanvas.drawRect(12, y0 + i*16 - 2, W - 24, 12, pink);
    }
    appCanvas.setTextColor(sel ? pink : green);
    appCanvas.setCursor(18, y0 + i*16);
    appCanvas.println(playItems[i]);
  }

  appCanvas.setTextColor(subC);
  appCanvas.setCursor(8, H - 14);
  appCanvas.println("B: Next   A: Select");
}

// =======================================================
// TILT PUZZLE (unchanged)
// =======================================================
static float bx = 24, by = 46;
static float vx = 0, vy = 0;
static const int br = 5;

static int  mazeLevel = 0;
static bool hasKey = false;

static Rect goal    = { 196, 86, 18, 18 };
static Rect keyRect = { 24, 92, 12, 12 };

static bool winLatched = false;
static uint32_t winStartMs = 0;
static const uint32_t CLEAR_DELAY_MS = 650;

static const int PF_X0 = 12;
static const int PF_X1 = 228;
static const int PF_Y0 = 34;
static const int PF_Y1 = 114;

static void resolveCircleRect(float &cx, float &cy, float &svx, float &svy, int r, const Rect& rc) {
  float closestX = clampf(cx, rc.x, rc.x + rc.w);
  float closestY = clampf(cy, rc.y, rc.y + rc.h);
  float dx = cx - closestX;
  float dy = cy - closestY;

  if (dx == 0 && dy == 0) {
    cy = rc.y - r - 0.5f;
    svy = 0;
    return;
  }

  float dist2 = dx*dx + dy*dy;
  float rr = (float)r;
  if (dist2 < rr*rr) {
    float dist = sqrtf(dist2);
    float overlap = rr - dist;
    float nx = dx / (dist + 1e-6f);
    float ny = dy / (dist + 1e-6f);
    cx += nx * overlap;
    cy += ny * overlap;

    float dot = svx*nx + svy*ny;
    if (dot < 0) {
      svx -= dot * nx;
      svy -= dot * ny;
    }
  }
}

static const Rect* activeWalls = nullptr;
static int activeWallCount = 0;

static Rect generatedL1[14];
static int  generatedL1Count = 0;

static Rect generatedL2[20];
static int  generatedL2Count = 0;

static void generateObstaclesLevel1() {
  const int count      = 8;
  const int margin     = 10;
  const int minSpacing = 20;

  generatedL1Count = 0;

  for (int tries = 0; tries < 140 && generatedL1Count < count; tries++) {
    bool vertical = random(0, 2);

    int w = vertical ? 6 : random(34, 70);
    int h = vertical ? random(26, 58) : 6;

    int x = random(PF_X0 + margin, PF_X1 - margin - w);
    int y = random(PF_Y0 + margin, PF_Y1 - margin - h);

    Rect candidate = { x, y, w, h };

    Rect startZone = { (int)bx - 26, (int)by - 26, 52, 52 };
    if (rectOverlap(candidate, startZone)) continue;

    Rect goalZone = { goal.x - 10, goal.y - 10, goal.w + 20, goal.h + 20 };
    if (rectOverlap(candidate, goalZone)) continue;

    bool conflict = false;
    for (int i = 0; i < generatedL1Count; i++) {
      Rect padded = {
        generatedL1[i].x - minSpacing,
        generatedL1[i].y - minSpacing,
        generatedL1[i].w + minSpacing * 2,
        generatedL1[i].h + minSpacing * 2
      };
      if (rectOverlap(candidate, padded)) { conflict = true; break; }
    }
    if (conflict) continue;

    generatedL1[generatedL1Count++] = candidate;
  }

  activeWalls = generatedL1;
  activeWallCount = generatedL1Count;
}

static void generateObstaclesLevel2() {
  const int count      = 12;
  const int margin     = 8;
  const int minSpacing = 14;

  generatedL2Count = 0;

  for (int tries = 0; tries < 220 && generatedL2Count < count; tries++) {
    bool vertical = (random(0, 100) < 60);

    int w = vertical ? 6 : random(30, 58);
    int h = vertical ? random(28, 66) : 6;

    int x = random(PF_X0 + margin, PF_X1 - margin - w);
    int y = random(PF_Y0 + margin, PF_Y1 - margin - h);

    Rect candidate = { x, y, w, h };

    Rect startZone = { (int)bx - 30, (int)by - 30, 60, 60 };
    if (rectOverlap(candidate, startZone)) continue;

    Rect goalZone = { goal.x - 12, goal.y - 12, goal.w + 24, goal.h + 24 };
    if (rectOverlap(candidate, goalZone)) continue;

    bool conflict = false;
    for (int i = 0; i < generatedL2Count; i++) {
      Rect padded = {
        generatedL2[i].x - minSpacing,
        generatedL2[i].y - minSpacing,
        generatedL2[i].w + minSpacing * 2,
        generatedL2[i].h + minSpacing * 2
      };
      if (rectOverlap(candidate, padded)) { conflict = true; break; }
    }
    if (conflict) continue;

    generatedL2[generatedL2Count++] = candidate;
  }

  activeWalls = generatedL2;
  activeWallCount = generatedL2Count;
}

static void placeKeyRandom() {
  const int kw = 12, kh = 12;
  const int margin = 8;
  const int minDist2 = 70 * 70;

  for (int tries = 0; tries < 70; tries++) {
    int x = random(PF_X0 + margin, PF_X1 - margin - kw);
    int y = random(PF_Y0 + margin, PF_Y1 - margin - kh);

    Rect candidate = { x, y, kw, kh };

    if (rectOverlap(candidate, goal)) continue;

    int kx = x + kw / 2;
    int ky = y + kh / 2;
    int dx = kx - (int)bx;
    int dy = ky - (int)by;
    if (dx*dx + dy*dy < minDist2) continue;

    bool hitWall = false;
    for (int i = 0; i < activeWallCount; i++) {
      if (rectOverlap(candidate, activeWalls[i])) { hitWall = true; break; }
    }
    if (hitWall) continue;

    keyRect = candidate;
    return;
  }

  keyRect = { PF_X0 + 18, PF_Y1 - 18, 12, 12 };
}

static void loadLevel(int level) {
  mazeLevel = level;
  hasKey = false;
  vx = 0; vy = 0;

  winLatched = false;
  winStartMs = 0;
  tiltLevelStartMs = millis();

  if (mazeLevel == 0) {
    bx = 24;  by = 46;
    goal = { 196, 86, 18, 18 };
    generateObstaclesLevel1();
    placeKeyRandom();
  } else {
    bx = 24;  by = 100;
    goal = { 198, 92, 18, 18 };
    generateObstaclesLevel2();
    placeKeyRandom();
  }
}

static void tiltLoop() {
  if (btnBHoldExit()) {
    uiBeep(BEEP_BACK);
    enterApp(APP_PLAY);
    return;
  }

  updatePulse();
  uint16_t cyan  = themeCyan(pulse);
  uint16_t green = themeGreen(pulse);
  uint16_t pink  = themePink(pulse);
  uint16_t subC  = themeSub(pulse);

  if (!winLatched) {
    float ax = 0, ay = 0, az = 0;
    if (M5.Imu.isEnabled()) {
      if (M5.Imu.getAccel(&ax, &ay, &az)) {
        float tiltLR = ay;
        float tiltUD = -ax;
        ay = tiltLR;
        ax = tiltUD;
      } else {
        ax = 0; ay = 0;
      }
    }

    vx += ax * 0.30f;
    vy += ay * 0.30f;
    vx *= 0.96f;
    vy *= 0.96f;

    bx += vx;
    by += vy;

    const float MIN_X = 12 + br + 1;
    const float MAX_X = 228 - br - 1;
    const float MIN_Y = 34 + br + 1;
    const float MAX_Y = 114 - br - 1;

    bx = clampf(bx, MIN_X, MAX_X);
    by = clampf(by, MIN_Y, MAX_Y);

    for (int i = 0; i < activeWallCount; ++i) {
      if (circleIntersectsRect(bx, by, br, activeWalls[i])) {
        resolveCircleRect(bx, by, vx, vy, br, activeWalls[i]);
      }
    }

    if (!hasKey && circleIntersectsRect(bx, by, br, keyRect)) {
      hasKey = true;
    }

   if (hasKey && circleIntersectsRect(bx, by, br, goal)) {
  winLatched = true;
  winStartMs = millis();
  vx = 0; vy = 0;

  // ---- scoring ----
  uint32_t dtMs = (tiltLevelStartMs > 0) ? (winStartMs - tiltLevelStartMs) : 0;
  tiltLastCs = dtMs / 10; // centiseconds
  if (tiltLastCs == 0) tiltLastCs = 1; // avoid 0.00 edge case

  if (tiltBestCs == 0 || tiltLastCs < tiltBestCs) {
    tiltBestCs = tiltLastCs;
    scoreSaveTiltBest();
  }
}
  } else {
    if (millis() - winStartMs > CLEAR_DELAY_MS) {
      if (mazeLevel == 0) loadLevel(1);
      else loadLevel(0);
      return;
    }
  }

  appCanvas.fillScreen(TFT_BLACK);
  appCanvas.fillRect(0, 0, W, 2, cyan);

  appCanvas.setTextSize(1);
  appCanvas.setTextColor(subC);
  appCanvas.setCursor(8, 8);
  appCanvas.print("TILT PUZZLE L");
  appCanvas.print(mazeLevel + 1);
  appCanvas.println("  (hold B to exit)");
  // Live timer (runs until CLEAR)
uint32_t endMs = winLatched ? winStartMs : millis();
uint32_t tms = (tiltLevelStartMs > 0 && endMs > tiltLevelStartMs) ? (endMs - tiltLevelStartMs) : 0;
uint32_t sec = tms / 1000;
uint32_t cs  = (tms % 1000) / 10;

appCanvas.setTextSize(1);
appCanvas.setTextColor(themeSub(pulse));
appCanvas.setCursor(8, 20);
appCanvas.printf("TIME:%lu.%02lu", (unsigned long)sec, (unsigned long)cs);
  // BEST time
if (tiltBestCs > 0) {
  char buf[16];
  fmtCs(tiltBestCs, buf, sizeof(buf));
  appCanvas.setTextSize(1);
  appCanvas.setTextColor(themeSub(pulse));
  appCanvas.setCursor(150, 20);
  appCanvas.print("BEST ");
  appCanvas.print(buf);
  appCanvas.print("s");
}

  drawCornerBrackets(appCanvas, 6, 26, W - 12, H - 40, green);

  if (!winLatched) {
    for (int i = 0; i < activeWallCount; ++i) {
      appCanvas.fillRect(activeWalls[i].x, activeWalls[i].y, activeWalls[i].w, activeWalls[i].h, green);
    }
  }

  uint16_t goalCol = hasKey ? pink : subC;
  appCanvas.drawRect(goal.x, goal.y, goal.w, goal.h, goalCol);
  appCanvas.drawRect(goal.x + 1, goal.y + 1, goal.w - 2, goal.h - 2, goalCol);

  if (!winLatched) {
    if (!hasKey) {
      int kx = keyRect.x + keyRect.w / 2;
      int ky = keyRect.y + keyRect.h / 2;
      appCanvas.drawCircle(kx, ky, 5, pink);
      appCanvas.fillCircle(kx, ky, 3, pink);
    } else {
      appCanvas.setTextSize(1);
      appCanvas.setTextColor(green);
      appCanvas.setCursor(150, 8);
      appCanvas.println("KEY✓");
    }
  } else {
    appCanvas.setTextSize(2);
    appCanvas.setTextColor(pink);
    appCanvas.setCursor(W/2 - 34, H/2 - 10);
    appCanvas.println("CLEAR!");
  }

  appCanvas.fillCircle((int)bx, (int)by, br, cyan);
}

// =======================================================
// LASER SWEEP 
// =======================================================

static float ls_x = 40, ls_y = 70;
static float ls_vx = 0, ls_vy = 0;
static const int ls_r = 6;

static bool ls_dead = false;
static uint32_t ls_startMs = 0;
static uint32_t ls_deadMs  = 0;

static int ls_diff = 1; // 0..2
static const float ls_diffSpeedMul[3] = { 0.90f, 1.00f, 1.15f };
static const uint16_t ls_spawnBaseMs[3] = { 980, 760, 620 };

// Laser segment struct (short beam)


static const int LS_MAX = 6;
static Laser ls_l[LS_MAX];

static uint32_t ls_nextSpawnMs = 0;

// Segment length tuning (THIS is what makes them dodgeable)
static const int LS_SEG_MIN = 26;
static const int LS_SEG_MAX = 48;

static void lsReset() {
  ls_x = 40; ls_y = 70;
  ls_vx = 0; ls_vy = 0;
  ls_dead = false;
  ls_startMs = millis();
  ls_deadMs  = 0;

  for (int i = 0; i < LS_MAX; i++) ls_l[i].active = false;
  ls_nextSpawnMs = millis() + 650;
}

static void lsSpawnOne(uint32_t now) {
  int idx = -1;
  for (int i = 0; i < LS_MAX; i++) {
    if (!ls_l[i].active) { idx = i; break; }
  }
  if (idx < 0) return;

  Laser &L = ls_l[idx];
  L.active = true;
  L.vertical = (random(0, 100) < 50);

  float tsec = (ls_startMs > 0) ? ((now - ls_startMs) / 1000.0f) : 0.0f;
  float ramp = 1.0f + clampf(tsec / 18.0f, 0.0f, 2.2f) * 0.55f;

  L.thick = random(8, 14);

  float base = random(55, 95) / 100.0f; // 0.55..0.95
  L.speed = base * ls_diffSpeedMul[ls_diff] * ramp;

  // Short segment length
  int segLen = random(LS_SEG_MIN, LS_SEG_MAX + 1);

  if (L.vertical) {
    // segment spans Y
    int maxStart = (PF_Y1 - PF_Y0) - segLen;
    if (maxStart < 0) maxStart = 0;
    L.span0 = PF_Y0 + random(0, maxStart + 1);
    L.spanLen = segLen;

    L.dir = (random(0, 2) == 0) ? 1 : -1;
    L.pos = (L.dir > 0) ? (PF_X0 - L.thick - 2) : (PF_X1 + 2);
  } else {
    // segment spans X
    int maxStart = (PF_X1 - PF_X0) - segLen;
    if (maxStart < 0) maxStart = 0;
    L.span0 = PF_X0 + random(0, maxStart + 1);
    L.spanLen = segLen;

    L.dir = (random(0, 2) == 0) ? 1 : -1;
    L.pos = (L.dir > 0) ? (PF_Y0 - L.thick - 2) : (PF_Y1 + 2);
  }
}

static bool lsCircleHitsLaser(float cx, float cy, int r, const Laser& L) {
  if (!L.active) return false;

  if (L.vertical) {
    Rect rc = { (int)L.pos, L.span0, L.thick, L.spanLen };
    return circleIntersectsRect(cx, cy, r, rc);
  } else {
    Rect rc = { L.span0, (int)L.pos, L.spanLen, L.thick };
    return circleIntersectsRect(cx, cy, r, rc);
  }
}

static void gravityEnter() {   
  irToolsActive = false;
  speakerEnsureOn();
  lsReset();
}

static void gravityLoop() {    
  if (btnBHoldExit()) {
    uiBeep(BEEP_BACK);
    enterApp(APP_PLAY);
    return;
  }

  updatePulse();
  uint16_t cyan  = themeCyan(pulse);
  uint16_t green = themeGreen(pulse);
  uint16_t pink  = themePink(pulse);
  uint16_t subC  = themeSub(pulse);

  if (M5.BtnB.wasPressed()) {
    ls_diff = (ls_diff + 1) % 3;
    uiBeep(BEEP_NAV);
  }
  if (M5.BtnA.wasPressed()) {
    lsReset();
    uiBeep(BEEP_OK);
  }

  uint32_t now = millis();

  if (!ls_dead) {
    float ax=0, ay=0, az=0;
    if (M5.Imu.isEnabled()) {
      if (M5.Imu.getAccel(&ax, &ay, &az)) {
        float tiltLR = ay;
        float tiltUD = -ax;
        ay = tiltLR;
        ax = tiltUD;
      } else { ax=0; ay=0; }
    }

    ls_vx += ax * 0.36f;
    ls_vy += ay * 0.36f;

    ls_vx *= 0.93f;
    ls_vy *= 0.93f;

    ls_x += ls_vx;
    ls_y += ls_vy;

    ls_x = clampf(ls_x, PF_X0 + ls_r, PF_X1 - ls_r);
    ls_y = clampf(ls_y, PF_Y0 + ls_r, PF_Y1 - ls_r);

    if (now > ls_nextSpawnMs) {
      lsSpawnOne(now);
      uint16_t base = ls_spawnBaseMs[ls_diff];

      float tsec = (ls_startMs > 0) ? ((now - ls_startMs) / 1000.0f) : 0.0f;
      float spawnMul = 1.0f - clampf(tsec / 28.0f, 0.0f, 0.55f);
      uint16_t interval = (uint16_t)(base * spawnMul);
      if (interval < 260) interval = 260;

      ls_nextSpawnMs = now + interval;
    }

    for (int i = 0; i < LS_MAX; i++) {
      if (!ls_l[i].active) continue;

      ls_l[i].pos += ls_l[i].speed * (float)ls_l[i].dir;

      if (ls_l[i].vertical) {
        if (ls_l[i].dir > 0 && ls_l[i].pos > PF_X1 + 3) ls_l[i].active = false;
        if (ls_l[i].dir < 0 && ls_l[i].pos < PF_X0 - ls_l[i].thick - 3) ls_l[i].active = false;
      } else {
        if (ls_l[i].dir > 0 && ls_l[i].pos > PF_Y1 + 3) ls_l[i].active = false;
        if (ls_l[i].dir < 0 && ls_l[i].pos < PF_Y0 - ls_l[i].thick - 3) ls_l[i].active = false;
      }
    }

    for (int i = 0; i < LS_MAX; i++) {
      if (lsCircleHitsLaser(ls_x, ls_y, ls_r, ls_l[i])) {
  ls_dead = true;
  ls_deadMs = now;

  // ---- scoring ----
  uint32_t runMs = (ls_startMs > 0 && ls_deadMs > ls_startMs) ? (ls_deadMs - ls_startMs) : 0;
  if (runMs > laserBestMs) {
    laserBestMs = runMs;
    scorePrefs.begin(SCORE_PREFS_NS, false);
    scorePrefs.putUChar("ver", SCORE_PREFS_VER);
    scorePrefs.putUInt("tiltBest", tiltBestCs);   // keep existing
    scorePrefs.putUInt("laserBest", laserBestMs); // new
    scorePrefs.end();
  }

  uiBeep(BEEP_BACK);
  break;
}
    }
  }

  // ---- Draw ----
  appCanvas.fillScreen(TFT_BLACK);
  appCanvas.fillRect(0, 0, W, 2, cyan);
  drawCornerBrackets(appCanvas, 6, 26, W - 12, H - 40, green);

  appCanvas.setTextSize(1);
  appCanvas.setTextColor(subC);
  appCanvas.setCursor(8, 8);
  appCanvas.println("LASER SWEEP  (hold B to exit)");
  // BEST survival
if (laserBestMs > 0) {
  uint32_t bestSec = laserBestMs / 1000;
  appCanvas.setTextSize(1);
  appCanvas.setTextColor(themeSub(pulse));
  appCanvas.setCursor(150, 32);
  appCanvas.print("BEST ");
  appCanvas.print(bestSec);
  appCanvas.print("s");
}

  uint32_t tms = 0;
  if (ls_startMs > 0) {
    uint32_t endMs = ls_dead ? ls_deadMs : millis();
    tms = (endMs > ls_startMs) ? (endMs - ls_startMs) : 0;
  }
  uint32_t sec = tms / 1000;
  uint32_t cs  = (tms % 1000) / 10;

  const char* dlab = (ls_diff==0) ? "EASY" : (ls_diff==1) ? "MID" : "HARD";

  appCanvas.setCursor(8, 20);
  appCanvas.setTextColor(subC);
  appCanvas.printf("TIME:%lu.%02lu   B:%s   A:Restart",
                   (unsigned long)sec, (unsigned long)cs, dlab);

  // Draw short lasers
  for (int i = 0; i < LS_MAX; i++) {
    if (!ls_l[i].active) continue;

    if (ls_l[i].vertical) {
      int x = (int)ls_l[i].pos;
      int w = ls_l[i].thick;
      int y0 = ls_l[i].span0;
      int h  = ls_l[i].spanLen;

      appCanvas.fillRect(x, y0, w, h, themeSub(210));
      int coreW = (w >= 6) ? (w - 4) : max(1, w - 2);
      appCanvas.fillRect(x + (w - coreW)/2, y0, coreW, h, themePink(220));
    } else {
      int y = (int)ls_l[i].pos;
      int h = ls_l[i].thick;
      int x0 = ls_l[i].span0;
      int w  = ls_l[i].spanLen;

      appCanvas.fillRect(x0, y, w, h, themeSub(210));
      int coreH = (h >= 6) ? (h - 4) : max(1, h - 2);
      appCanvas.fillRect(x0, y + (h - coreH)/2, w, coreH, themePink(220));
    }
  }

  appCanvas.fillCircle((int)ls_x, (int)ls_y, ls_r, cyan);
  appCanvas.drawCircle((int)ls_x, (int)ls_y, ls_r + 2, themeSub(200));

  if (ls_dead) {
    appCanvas.setTextSize(2);
    appCanvas.setTextColor(themePink(220));
    appCanvas.setCursor(64, 56);
    appCanvas.println("HIT!");
    appCanvas.setTextSize(1);
    appCanvas.setTextColor(subC);
    appCanvas.setCursor(46, 78);
    appCanvas.println("Press A to restart");
  }
}

// =======================================================
// PARTICLE LAB 
// =======================================================
static Rect pl_box = { 168, 54, 46, 34 };

// Walls
static const int PL_WALL_MAX = 8;
static Rect pl_walls[PL_WALL_MAX];
static int  pl_wallCount = 0;

// Particles
static const int PL_PR = 2;
static const int PL_SLOT = 5;

static const int PL_MAXP = 26;
struct PLParticle {
  float x, y, vx, vy;
  bool deposited = false;
};
static PLParticle pl_p[PL_MAXP];

static int pl_need = 18;
static int pl_got  = 0;
static bool pl_win = false;
static uint32_t pl_winMs = 0;

// Generate *few* random walls (2..4), no "maze"
static void plGenerateWalls() {
  pl_wallCount = 0;

  Rect boxZone   = { pl_box.x - 10, pl_box.y - 10, pl_box.w + 20, pl_box.h + 20 };
  const int want = random(2, 5); // 2..4
  const int triesMax = 140;

  for (int tries = 0; tries < triesMax && pl_wallCount < want; tries++) {
    bool vertical = (random(0, 100) < 50);

    int w = vertical ? 6 : random(34, 64);
    int h = vertical ? random(22, 48) : 6;

    int x = random(PF_X0 + 8, PF_X1 - 8 - w);
    int y = random(PF_Y0 + 8, PF_Y1 - 8 - h);

    Rect cand = { x, y, w, h };

    if (rectOverlap(cand, boxZone)) continue;

    bool conflict = false;
    for (int i = 0; i < pl_wallCount; i++) {
      Rect pad = { pl_walls[i].x - 12, pl_walls[i].y - 12, pl_walls[i].w + 24, pl_walls[i].h + 24 };
      if (rectOverlap(cand, pad)) { conflict = true; break; }
    }
    if (conflict) continue;

    pl_walls[pl_wallCount++] = cand;
  }
}

static void plBounceParticleRect(float &x, float &y, float &vx, float &vy, int pr, const Rect& rc) {
  if (!circleIntersectsRect(x, y, pr, rc)) return;

  float left   = (x + pr) - rc.x;
  float right  = (rc.x + rc.w) - (x - pr);
  float top    = (y + pr) - rc.y;
  float bottom = (rc.y + rc.h) - (y - pr);

  float minX = (left < right) ? left : right;
  float minY = (top  < bottom) ? top  : bottom;

  if (minX < minY) {
    if (left < right) x = rc.x - pr - 0.5f;
    else              x = rc.x + rc.w + pr + 0.5f;
    vx = -vx * 0.78f;
  } else {
    if (top < bottom) y = rc.y - pr - 0.5f;
    else              y = rc.y + rc.h + pr + 0.5f;
    vy = -vy * 0.78f;
  }
}

static void particleReset() {
  pl_got = 0;
  pl_win = false;
  pl_winMs = 0;
  particleStartMs = millis();

  plGenerateWalls();

  // init particles
  for (int i = 0; i < PL_MAXP; i++) {
    pl_p[i].deposited = false;

    // spawn away from box
    for (int t = 0; t < 80; t++) {
      int x = random(PF_X0 + 10, PF_X1 - 10);
      int y = random(PF_Y0 + 10, PF_Y1 - 10);
      Rect pt = { x-3, y-3, 6, 6 };
      Rect boxZone   = { pl_box.x - 10, pl_box.y - 10, pl_box.w + 20, pl_box.h + 20 };

      bool ok = !rectOverlap(pt, boxZone);
      for (int w = 0; w < pl_wallCount && ok; w++) {
        if (rectOverlap(pt, pl_walls[w])) ok = false;
      }
      if (!ok) continue;

      pl_p[i].x = (float)x;
      pl_p[i].y = (float)y;
      break;
    }

    pl_p[i].vx = (random(-100, 101) / 100.0f) * 0.45f;
    pl_p[i].vy = (random(-100, 101) / 100.0f) * 0.45f;
  }
}

static void particleEnter() {
  irToolsActive = false;
  speakerEnsureOn();
  particleReset();
}

static void particleLoop() {
  if (btnBHoldExit()) {
    uiBeep(BEEP_BACK);
    enterApp(APP_PLAY);
    return;
  }

  updatePulse();
  uint16_t cyan  = themeCyan(pulse);
  uint16_t green = themeGreen(pulse);
  uint16_t pink  = themePink(pulse);
  uint16_t subC  = themeSub(pulse);

  if (M5.BtnA.wasPressed()) { particleReset(); uiBeep(BEEP_OK); }

  if (!pl_win) {
    // global tilt flow applied to particles
    float ax=0, ay=0, az=0;
    if (M5.Imu.isEnabled()) {
      if (M5.Imu.getAccel(&ax, &ay, &az)) {
        float tiltLR = ay;
        float tiltUD = -ax;
        ay = tiltLR;
        ax = tiltUD;
      } else { ax=0; ay=0; }
    }

    // inner box for deposit
    Rect inner = { pl_box.x + 2, pl_box.y + 2, pl_box.w - 4, pl_box.h - 4 };

    for (int i = 0; i < pl_need; i++) {
      if (pl_p[i].deposited) continue;

      // tilt "wind"
      pl_p[i].vx += ax * 0.08f;
      pl_p[i].vy += ay * 0.08f;

      // mild damping
      pl_p[i].vx *= 0.993f;
      pl_p[i].vy *= 0.993f;

      // move
      pl_p[i].x += pl_p[i].vx;
      pl_p[i].y += pl_p[i].vy;

      // bounds bounce
      if (pl_p[i].x < PF_X0 + PL_PR) { pl_p[i].x = PF_X0 + PL_PR; pl_p[i].vx = -pl_p[i].vx * 0.9f; }
      if (pl_p[i].x > PF_X1 - PL_PR) { pl_p[i].x = PF_X1 - PL_PR; pl_p[i].vx = -pl_p[i].vx * 0.9f; }
      if (pl_p[i].y < PF_Y0 + PL_PR) { pl_p[i].y = PF_Y0 + PL_PR; pl_p[i].vy = -pl_p[i].vy * 0.9f; }
      if (pl_p[i].y > PF_Y1 - PL_PR) { pl_p[i].y = PF_Y1 - PL_PR; pl_p[i].vy = -pl_p[i].vy * 0.9f; }

      // wall bounces
      for (int w = 0; w < pl_wallCount; w++) {
        plBounceParticleRect(pl_p[i].x, pl_p[i].y, pl_p[i].vx, pl_p[i].vy, PL_PR, pl_walls[w]);
      }

      // deposit if enters box
      if (circleIntersectsRect(pl_p[i].x, pl_p[i].y, PL_PR, inner)) {
        pl_p[i].deposited = true;
        pl_got++;
        uiBeep(BEEP_OK);

        // snap into a slot in the box so it fills up
        int innerW = pl_box.w - 6;
        int cols = innerW / PL_SLOT;
        if (cols < 1) cols = 1;

        int slot = pl_got - 1;
        int sx = pl_box.x + 3 + (slot % cols) * PL_SLOT;
        int sy = pl_box.y + 3 + (slot / cols) * PL_SLOT;

        if (sx > pl_box.x + pl_box.w - 4) sx = pl_box.x + pl_box.w - 4;
        if (sy > pl_box.y + pl_box.h - 4) sy = pl_box.y + pl_box.h - 4;

        pl_p[i].x = (float)sx;
        pl_p[i].y = (float)sy;
        pl_p[i].vx = pl_p[i].vy = 0;

        if (pl_got >= pl_need) {
  pl_win = true;
  pl_winMs = millis();

  // ---- scoring ----
  uint32_t runMs = (particleStartMs > 0 && pl_winMs > particleStartMs) ? (pl_winMs - particleStartMs) : 0;
  if (runMs > 0 && (particleBestMs == 0 || runMs < particleBestMs)) {
    particleBestMs = runMs;

    scorePrefs.begin(SCORE_PREFS_NS, false);
    scorePrefs.putUChar("ver", SCORE_PREFS_VER);
    scorePrefs.putUInt("tiltBest", tiltBestCs);
    scorePrefs.putUInt("laserBest", laserBestMs);
    scorePrefs.putUInt("particleBest", particleBestMs);
    scorePrefs.end();
  }
}
      }
    }
  } else {
    if (millis() - pl_winMs > 900) particleReset();
  }

  // ---- Draw ----
  appCanvas.fillScreen(TFT_BLACK);
  appCanvas.fillRect(0, 0, W, 2, cyan);

  appCanvas.setTextSize(1);
  appCanvas.setTextColor(subC);
  appCanvas.setCursor(8, 8);
  appCanvas.println("PARTICLE LAB  (hold B to exit)");

  appCanvas.setCursor(8, 20);
  appCanvas.setTextColor(subC);
 // Live timer (runs until SEALED)
uint32_t endMs = pl_win ? pl_winMs : millis();
uint32_t tms = (particleStartMs > 0 && endMs > particleStartMs) ? (endMs - particleStartMs) : 0;
uint32_t sec = tms / 1000;
uint32_t cs  = (tms % 1000) / 10;

appCanvas.setTextColor(subC);
appCanvas.setCursor(8, 20);
appCanvas.printf("TIME:%lu.%02lu  Stored:%d/%d  A:Reset",
                 (unsigned long)sec, (unsigned long)cs, pl_got, pl_need);

// BEST time (fastest SEALED)
if (particleBestMs > 0) {
  uint32_t bsec = particleBestMs / 1000;
  uint32_t bcs  = (particleBestMs % 1000) / 10;
  appCanvas.setCursor(150, 32);
  appCanvas.printf("BEST:%lu.%02lu", (unsigned long)bsec, (unsigned long)bcs);
}

  drawCornerBrackets(appCanvas, 6, 26, W - 12, H - 40, green);

  // walls
  for (int i = 0; i < pl_wallCount; i++) {
    appCanvas.fillRect(pl_walls[i].x, pl_walls[i].y, pl_walls[i].w, pl_walls[i].h, themeGreen(220));
  }

  // box
  appCanvas.drawRect(pl_box.x, pl_box.y, pl_box.w, pl_box.h, themePink(220));
  appCanvas.drawRect(pl_box.x + 1, pl_box.y + 1, pl_box.w - 2, pl_box.h - 2, themePink(220));

  // fill indicator (energy rising)
  float frac = (pl_need > 0) ? ((float)pl_got / (float)pl_need) : 0.0f;
  frac = clampf(frac, 0.0f, 1.0f);
  int fillH = (int)((pl_box.h - 4) * frac);
  if (fillH > 0) {
    int fx = pl_box.x + 2;
    int fy = pl_box.y + pl_box.h - 2 - fillH;
    int fw = pl_box.w - 4;
    appCanvas.fillRect(fx, fy, fw, fillH, themeCyan(180));
  }

  // particles
  for (int i = 0; i < pl_need; i++) {
    int x = (int)pl_p[i].x;
    int y = (int)pl_p[i].y;

    if (pl_p[i].deposited) {
      uint16_t cc = ((i & 1) == 0) ? themePink(220) : themeCyan(220);
      appCanvas.fillCircle(x, y, PL_PR, cc);
      appCanvas.drawCircle(x, y, PL_PR + 1, themeSub(200));
      continue;
    }

    float sp = fabsf(pl_p[i].vx) + fabsf(pl_p[i].vy);
    uint16_t pc = (sp < 0.30f) ? themeSub(200) : (sp < 0.70f) ? themeCyan(220) : themePink(220);

    appCanvas.fillCircle(x, y, PL_PR, pc);
    if (sp > 0.65f) appCanvas.drawCircle(x, y, PL_PR + 2, themeSub(210));
  }

  if (pl_win) {
    appCanvas.setTextSize(2);
    appCanvas.setTextColor(themePink(220));
    appCanvas.setCursor(64, 56);
    appCanvas.println("SEALED!");
  }
}

// =======================================================
// VISUALIZER 
// =======================================================
static constexpr size_t V_N = 240;
static constexpr int    V_BARS = 12;
static constexpr int    V_SR = 18000;

static constexpr int V_GAP = 2;
static constexpr int V_SLICE = 4;

static constexpr int V_PEAK_DROP_MS = 30;
static constexpr int V_BASE_DIM = 120;
static constexpr int V_TOP_BOOST_DIV = 2;
static constexpr int V_START_RAINBOW_AT = 50;

static int16_t v_samples[V_N];
static int v_barSmooth[V_BARS] = {0};
static int v_peakHold[V_BARS]  = {0};
static uint32_t v_lastDrop = 0;

static uint8_t v_theme = 3;

static int v_sensLevel = 1;
static float v_sensMult[3] = {0.70f, 1.00f, 1.50f};
static uint32_t v_sensMsgUntil = 0;

static bool v_micActive = false;

static uint16_t v_solidThemeColor(uint8_t pulseNow) {
  if (v_theme == 0) return themeGreen(pulseNow);
  return rgb565(255, 40, 40);
}

static void visualizerEnter() {
  irToolsActive = false;
  speakerPauseForMic();
  M5.Mic.begin();
  v_micActive = true;

  v_sensMsgUntil = 0;
  v_lastDrop = millis();
}

static void visualizerStopAudio() {
  if (!v_micActive) return;
  M5.Mic.end();
  v_micActive = false;
}

static void visualizerLoop() {
  if (btnBHoldExit()) {
    uiBeep(BEEP_BACK);
    enterApp(APP_MENU); // dispatcher stops mic + restores speaker
    return;
  }

  updatePulse();
  uint16_t cyan  = themeCyan(pulse);
  uint16_t green = themeGreen(pulse);
  uint16_t pink  = themePink(pulse);
  uint16_t subC  = themeSub(pulse);

  if (M5.BtnA.wasPressed()) v_theme = (v_theme + 1) % 4;
  if (M5.BtnB.wasPressed()) {
    v_sensLevel = (v_sensLevel + 1) % 3;
    v_sensMsgUntil = millis() + 1000;
  }

  if (!v_micActive) {
    speakerPauseForMic();
    M5.Mic.begin();
    v_micActive = true;
  }

  if (!M5.Mic.record(v_samples, V_N, V_SR, false)) {
    appCanvas.fillScreen(TFT_BLACK);
    appCanvas.fillRect(0, 0, W, 2, cyan);
    drawCornerBrackets(appCanvas, 6, 26, W - 12, H - 40, green);

    appCanvas.setTextSize(2);
    appCanvas.setTextColor(pink);
    appCanvas.setCursor(12, 48);
    appCanvas.println("Visualizer");

    appCanvas.setTextSize(1);
    appCanvas.setTextColor(subC);
    appCanvas.setCursor(12, 76);
    appCanvas.println("Mic init...");
    appCanvas.setCursor(12, 92);
    appCanvas.println("hold B to exit");
    return;
  }

  const int chunk = (int)(V_N / V_BARS);
  const int playY0 = 34;
  const int playH  = 114 - playY0;
  const int playY1 = playY0 + playH;

  int barW = (W - (V_BARS - 1) * V_GAP) / V_BARS;
  if (barW < 4) barW = 4;

  for (int b = 0; b < V_BARS; b++) {
    int32_t peak = 0;
    int start = b * chunk;
    int end = start + chunk;

    for (int i = start; i < end; i++) {
      int32_t v = v_samples[i];
      if (v < 0) v = -v;
      if (v > peak) peak = v;
    }

    int32_t shaped = (int32_t)sqrt((double)peak) * 181;
    int h = (int)((shaped * v_sensMult[v_sensLevel] * playH) / 32767.0f);
    if (h < 0) h = 0;
    if (h > playH) h = playH;

    v_barSmooth[b] = (v_barSmooth[b] * 6 + h * 2) / 8;
    if (v_barSmooth[b] > v_peakHold[b]) v_peakHold[b] = v_barSmooth[b];
  }

  uint32_t now = millis();
  if (now - v_lastDrop > (uint32_t)V_PEAK_DROP_MS) {
    for (int b = 0; b < V_BARS; b++) if (v_peakHold[b] > 0) v_peakHold[b]--;
    v_lastDrop = now;
  }

  appCanvas.fillScreen(TFT_BLACK);
  appCanvas.fillRect(0, 0, W, 2, cyan);

  appCanvas.setTextSize(1);
  appCanvas.setTextColor(subC);
  appCanvas.setCursor(8, 8);
  appCanvas.println("VISUALIZER  (hold B to exit)");

  appCanvas.setTextSize(1);
  appCanvas.setTextColor(subC);
  appCanvas.setCursor(8, 20);
  appCanvas.println("A:Theme  B:Sens");

  drawCornerBrackets(appCanvas, 6, 26, W - 12, H - 40, green);

  for (int b = 0; b < V_BARS; b++) {
    int x = b * (barW + V_GAP);
    int h = v_barSmooth[b];
    int yTop = playY1 - h;

    appCanvas.fillRect(x, playY0, barW, playH, TFT_BLACK);

    if (v_theme == 2) {
      for (int yy = 0; yy < h; yy += V_SLICE) {
        int sliceH = (yy + V_SLICE <= h) ? V_SLICE : (h - yy);
        int t = (h > 1) ? ((h - 1 - yy) * 255) / (h - 1) : 0;
        uint8_t vv = (uint8_t)(V_BASE_DIM + (t / V_TOP_BOOST_DIV));
        uint8_t hue = (uint8_t)(b * (255 / V_BARS));
        uint16_t c = hsvTo565(hue, 255, vv);
        appCanvas.fillRect(x, yTop + yy, barW, sliceH, c);
      }
    } else if (v_theme == 3) {
      for (int yy = 0; yy < h; yy += V_SLICE) {
        int sliceH = (yy + V_SLICE <= h) ? V_SLICE : (h - yy);
        int t = (h > 1) ? ((h - 1 - yy) * 255) / (h - 1) : 0;

        uint8_t hue;
        if (t < V_START_RAINBOW_AT) hue = 85;
        else {
          int tt = (t - V_START_RAINBOW_AT) * 255 / (255 - V_START_RAINBOW_AT);
          hue = (uint8_t)(85 - (tt * 85 / 255));
        }

        uint8_t vv = (uint8_t)(V_BASE_DIM + (t / V_TOP_BOOST_DIV));
        uint16_t c = hsvTo565(hue, 255, vv);
        appCanvas.fillRect(x, yTop + yy, barW, sliceH, c);
      }
    } else {
      uint16_t c = v_solidThemeColor(pulse);
      appCanvas.fillRect(x, yTop, barW, h, c);
    }

    int py = playY1 - v_peakHold[b];
    appCanvas.fillRect(x, py, barW, 2, TFT_WHITE);
  }

  if (millis() < v_sensMsgUntil) {
    appCanvas.setTextSize(1);
    appCanvas.setTextColor(TFT_WHITE);
    appCanvas.setCursor(150, 20);

    const char* label =
      (v_sensLevel == 0) ? "LOW" :
      (v_sensLevel == 1) ? "MID" : "HIGH";

    appCanvas.printf("S:%s", label);
  }
}

// =======================================================
// IR TOOLS — NEC-only learn/send
// =======================================================
static constexpr uint8_t IR_RECEIVE_PIN = 42;
static constexpr uint8_t IR_SEND_PIN    = 46;

static rmt_channel_handle_t rx_chan = NULL;
static rmt_channel_handle_t tx_chan = NULL;
static rmt_encoder_handle_t copy_encoder = NULL;

static volatile bool     rx_done = false;
static volatile uint32_t rx_seq  = 0;
static size_t            rx_symbol_num = 0;
static rmt_symbol_word_t rx_raw_symbols[RC_SYMBOL_MAX];

static constexpr int   IR_CARRIER_FREQ_HZ = 38000;
static constexpr float IR_DUTY_CYCLE      = 0.33f;

static uint32_t irLastHitMs    = 0;
static uint16_t irLastSymCount = 0;

static constexpr int RC_PROFILES = 4;
enum RcFn : uint8_t { RC_POWER=0, RC_VOL_UP, RC_VOL_DN, RC_MUTE, RC_CH_UP, RC_CH_DN, RC_FN_COUNT };
static const char* rcFnName[RC_FN_COUNT] = { "POWER", "VOL+", "VOL-", "MUTE", "CH+", "CH-" };

static Preferences rcPrefs;
static const char* RC_PREFS_NS  = "pulseos_rc";
static const uint8_t RC_STORE_VER = 3;

static RcCode rcDB[RC_PROFILES][RC_FN_COUNT];
static int rcProfile = 0;
static int rcSelFn   = 0;

static bool     rcLearning        = false;
static uint32_t rcLearnUntilMs    = 0;
static const uint32_t RC_LEARN_TIMEOUT_MS = 6000;

static uint32_t rcLearnArmSeq        = 0;
static uint32_t rcLearnIgnoreUntilMs = 0;

static const uint32_t RC_A_HOLD_MS = 450;

static uint32_t rcToastUntil = 0;
static char     rcToastMsg[44] = {0};

static void rcToast(const char* msg, uint32_t ms = 1400) {
  strncpy(rcToastMsg, msg, sizeof(rcToastMsg)-1);
  rcToastMsg[sizeof(rcToastMsg)-1] = 0;
  rcToastUntil = millis() + ms;
}

static bool rmt_rx_done_callback(rmt_channel_handle_t, const rmt_rx_done_event_data_t *edata, void *) {
  if (edata && edata->num_symbols > 0) {
    rx_symbol_num = edata->num_symbols;
    rx_seq++;
    rx_done = true;
    return true;
  }
  return false;
}

static void rmtSetupRx() {
  if (rx_chan != NULL) return;

  rmt_rx_channel_config_t rx_cfg = {};
  rx_cfg.gpio_num          = (gpio_num_t)IR_RECEIVE_PIN;
  rx_cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
  rx_cfg.resolution_hz     = 1000000;
  rx_cfg.mem_block_symbols = 128;

  if (rmt_new_rx_channel(&rx_cfg, &rx_chan) != ESP_OK) {
    rx_chan = NULL;
    return;
  }

  rmt_rx_event_callbacks_t cbs = { .on_recv_done = rmt_rx_done_callback };
  rmt_rx_register_event_callbacks(rx_chan, &cbs, NULL);
  rmt_enable(rx_chan);
}

static void rmtStartReceive() {
  if (rx_chan == NULL) return;

  rx_done = false;

  rmt_receive_config_t cfg = {};
  cfg.signal_range_min_ns = 1000;
  cfg.signal_range_max_ns = 20000000;

  rmt_receive(rx_chan, rx_raw_symbols, sizeof(rx_raw_symbols), &cfg);
}

static void rmtSetupTx() {
  if (tx_chan != NULL) return;

  rmt_tx_channel_config_t tx_cfg = {};
  tx_cfg.gpio_num          = (gpio_num_t)IR_SEND_PIN;
  tx_cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
  tx_cfg.resolution_hz     = 1000000;
  tx_cfg.mem_block_symbols = 64;
  tx_cfg.trans_queue_depth = 4;

  if (rmt_new_tx_channel(&tx_cfg, &tx_chan) != ESP_OK) {
    tx_chan = NULL;
    return;
  }

  rmt_carrier_config_t carrier_cfg = {};
  carrier_cfg.frequency_hz = (uint32_t)IR_CARRIER_FREQ_HZ;
  carrier_cfg.duty_cycle   = IR_DUTY_CYCLE;
  carrier_cfg.flags.polarity_active_low = false;
  carrier_cfg.flags.always_on = false;
  rmt_apply_carrier(tx_chan, &carrier_cfg);

  rmt_copy_encoder_config_t enc_cfg = {};
  rmt_new_copy_encoder(&enc_cfg, &copy_encoder);

  rmt_enable(tx_chan);
}

static uint32_t rcNECRaw(uint16_t address, uint8_t command) {
  uint8_t addrLo = (uint8_t)(address & 0xFF);
  uint8_t addrHi = (uint8_t)((address >> 8) & 0xFF);
  uint8_t cmdInv = (uint8_t)~command;
  return ((uint32_t)addrLo) |
         ((uint32_t)addrHi << 8) |
         ((uint32_t)command << 16) |
         ((uint32_t)cmdInv << 24);
}

static bool rcDecodeNECFromRx(uint16_t &address, uint8_t &command) {
  if (rx_symbol_num < 33 || rx_symbol_num > RC_SYMBOL_MAX) return false;

  auto durNear = [](uint32_t v, uint32_t target, uint32_t tol) -> bool {
    return (v >= (target > tol ? target - tol : 0)) && (v <= target + tol);
  };

  auto symbolMark = [](const rmt_symbol_word_t &s) -> uint32_t {
    return (s.level0 == 0) ? s.duration0 : ((s.level1 == 0) ? s.duration1 : 0);
  };

  auto symbolSpace = [](const rmt_symbol_word_t &s) -> uint32_t {
    return (s.level0 == 1) ? s.duration0 : ((s.level1 == 1) ? s.duration1 : 0);
  };

  uint32_t hdrMark  = symbolMark(rx_raw_symbols[0]);
  uint32_t hdrSpace = symbolSpace(rx_raw_symbols[0]);

  if (!durNear(hdrMark, 9000, 2200)) return false;
  if (!durNear(hdrSpace, 4500, 1400)) return false;

  uint32_t raw = 0;

  for (int i = 0; i < 32; i++) {
    const rmt_symbol_word_t &s = rx_raw_symbols[i + 1];
    uint32_t mark  = symbolMark(s);
    uint32_t space = symbolSpace(s);

    if (!durNear(mark, 560, 300)) return false;

    if (durNear(space, 560, 350)) {
      // bit 0
    } else if (durNear(space, 1690, 500)) {
      raw |= (1UL << i);
    } else {
      return false;
    }
  }

  uint8_t addrLo = (uint8_t)(raw & 0xFF);
  uint8_t addrHi = (uint8_t)((raw >> 8) & 0xFF);
  uint8_t cmd    = (uint8_t)((raw >> 16) & 0xFF);
  uint8_t cmdInv = (uint8_t)((raw >> 24) & 0xFF);

  if ((uint8_t)~cmd != cmdInv) return false;

  address = (uint16_t)addrLo | ((uint16_t)addrHi << 8);
  command = cmd;
  return true;
}

static size_t rcEncodeNEC(uint16_t address, uint8_t command, rmt_symbol_word_t *symbols, size_t maxSymbols) {
  if (maxSymbols < 35) return 0;

  const uint32_t raw = rcNECRaw(address, command);

  size_t idx = 0;

  symbols[idx].level0 = 1;
  symbols[idx].duration0 = 9000;
  symbols[idx].level1 = 0;
  symbols[idx].duration1 = 4500;
  idx++;

  for (int i = 0; i < 32; i++) {
    bool bit = (raw >> i) & 0x1;
    symbols[idx].level0 = 1;
    symbols[idx].duration0 = 560;
    symbols[idx].level1 = 0;
    symbols[idx].duration1 = bit ? 1690 : 560;
    idx++;
  }

  symbols[idx].level0 = 1;
  symbols[idx].duration0 = 560;
  symbols[idx].level1 = 0;
  symbols[idx].duration1 = 30000;
  idx++;

  return idx;
}

static void rcMakeKeyBase(char* out, size_t outLen, int p, int f) {
  snprintf(out, outLen, "p%df%d", p, f);
}

static void rcSaveOne(int p, int f) {
  RcCode &c = rcDB[p][f];
  char key[12];

  rcMakeKeyBase(key, sizeof(key), p, f);
  rcPrefs.putBool((String(key) + "v").c_str(), c.valid);
  rcPrefs.putBool((String(key) + "n").c_str(), c.isNEC);
  rcPrefs.putUShort((String(key) + "a").c_str(), c.address);
  rcPrefs.putUChar((String(key) + "c").c_str(), c.command);
  rcPrefs.putUChar((String(key) + "r").c_str(), c.repeats);
}

static void rcLoadOne(int p, int f) {
  RcCode &c = rcDB[p][f];
  char key[12];

  rcMakeKeyBase(key, sizeof(key), p, f);
  c.valid   = rcPrefs.getBool((String(key) + "v").c_str(), false);
  c.isNEC   = rcPrefs.getBool((String(key) + "n").c_str(), false);
  c.address = rcPrefs.getUShort((String(key) + "a").c_str(), 0);
  c.command = rcPrefs.getUChar((String(key) + "c").c_str(), 0);
  c.repeats = rcPrefs.getUChar((String(key) + "r").c_str(), 0);

  if (!c.valid || !c.isNEC) {
    c.valid = false;
    c.isNEC = false;
    c.address = 0;
    c.command = 0;
    c.repeats = 0;
  }
}

static void rcLoadAll() {
  for (int p = 0; p < RC_PROFILES; p++) {
    for (int f = 0; f < RC_FN_COUNT; f++) {
      rcDB[p][f].valid = false;
      rcDB[p][f].isNEC = false;
      rcDB[p][f].address = 0;
      rcDB[p][f].command = 0;
      rcDB[p][f].repeats = 0;
    }
  }

  uint8_t ver = rcPrefs.getUChar("ver", 0);
  if (ver != RC_STORE_VER) return;

  for (int p = 0; p < RC_PROFILES; p++) {
    for (int f = 0; f < RC_FN_COUNT; f++) {
      rcLoadOne(p, f);
    }
  }
}

static void rcSendSelected() {
  RcCode &c = rcDB[rcProfile][rcSelFn];
  if (!c.valid || !c.isNEC) {
    rcToast("No NEC code stored");
    return;
  }

  rmt_symbol_word_t symbols[40];
  size_t count = rcEncodeNEC(c.address, c.command, symbols, 40);
  if (count == 0) {
    rcToast("Encode fail");
    return;
  }

  M5.Power.setExtOutput(true);
  delay(2);

  if (rx_chan != NULL) rmt_disable(rx_chan);

  rmt_transmit_config_t cfg = {};
  cfg.loop_count = 0;

  esp_err_t err = ESP_OK;
  if (copy_encoder) rmt_encoder_reset(copy_encoder);

  err = rmt_transmit(
    tx_chan,
    copy_encoder,
    symbols,
    count * sizeof(rmt_symbol_word_t),
    &cfg
  );

  if (err == ESP_OK) err = rmt_tx_wait_all_done(tx_chan, 2000);

  if (rx_chan != NULL) {
    rmt_enable(rx_chan);
    rmtStartReceive();
  }

  if (err != ESP_OK) rcToast("Send error!");
}

static void rcClearSelectedFn() {
  RcCode &c = rcDB[rcProfile][rcSelFn];
  c.valid = false;
  c.isNEC = false;
  c.address = 0;
  c.command = 0;
  c.repeats = 0;
  rcSaveOne(rcProfile, rcSelFn);
}

static void rcClearProfile(int p) {
  for (int f = 0; f < RC_FN_COUNT; f++) {
    rcDB[p][f].valid = false;
    rcDB[p][f].isNEC = false;
    rcDB[p][f].address = 0;
    rcDB[p][f].command = 0;
    rcDB[p][f].repeats = 0;
    rcSaveOne(p, f);
  }
}

static void irEnter() {
  irToolsActive = true;
  M5.Power.setExtOutput(true);
  delay(10);
  speakerEnsureOn();

  rmtSetupRx();
  rmtSetupTx();

  rcPrefs.begin(RC_PREFS_NS, false);
  if (rcPrefs.getUChar("ver", 0) != RC_STORE_VER) {
    rcPrefs.putUChar("ver", RC_STORE_VER);
  }
  rcLoadAll();

  rcLearning     = false;
  rx_done        = false;
  irLastHitMs    = 0;
  irLastSymCount = 0;

  rmtStartReceive();

  rcToastUntil  = 0;
  rcToastMsg[0] = 0;
}

static void irExit() {
  irToolsActive = false;
  rcLearning    = false;
  rcPrefs.end();
}

static void irLoop() {
  updatePulse();
  uint16_t cyan  = themeCyan(pulse);
  uint16_t green = themeGreen(pulse);
  uint16_t pink  = themePink(pulse);
  uint16_t subC  = themeSub(pulse);

  const bool aPress = M5.BtnA.wasPressed();
  const bool bPress = M5.BtnB.wasPressed();
  const bool aNow   = M5.BtnA.isPressed();
  const bool bNow   = M5.BtnB.isPressed();

  if (!aNow && btnBHoldExit()) {
    uiBeepIR(BEEP_BACK);
    irExit();
    currentApp = APP_MENU;
    return;
  }

  static bool chordArmed = false;
  static uint32_t chordArmMs = 0;
  static bool profCleared = false;

  const bool bothNow = aNow && bNow;

  if (bothNow && !chordArmed) {
    chordArmed  = true;
    chordArmMs  = millis();
    profCleared = false;
  }

  if (chordArmed && bothNow) {
    uint32_t held = millis() - chordArmMs;
    if (!profCleared && held >= 1200) {
      rcClearProfile(rcProfile);
      uiBeepIR(BEEP_OK);
      rcToast("Cleared entire profile");
      profCleared = true;
    }
  }

  if (chordArmed && !bothNow) {
    uint32_t dt = millis() - chordArmMs;
    if (dt < 600 && !profCleared) {
      rcProfile = (rcProfile + 1) % RC_PROFILES;
      uiBeepIR(BEEP_NAV);
      rcToast("Profile -> next");
    }
    chordArmed = false;
  }

  const bool chordActive = bothNow || chordArmed;
  if (!chordActive) {
    static uint32_t lastBTapMs = 0;
    static bool pendingBTap = false;

    if (bPress) {
      uint32_t now = millis();
      if (pendingBTap && (now - lastBTapMs) < 380) {
        rcClearSelectedFn();
        uiBeepIR(BEEP_BACK);
        rcToast("Cleared selected button");
        pendingBTap = false;
        lastBTapMs = 0;
      } else {
        pendingBTap = true;
        lastBTapMs = now;
      }
    }

    if (pendingBTap && (millis() - lastBTapMs) > 380) {
      rcSelFn = (rcSelFn + 1) % RC_FN_COUNT;
      uiBeepIR(BEEP_NAV);
      pendingBTap = false;
    }

    static bool aHoldArmed = false;
    static uint32_t aHoldStart = 0;
    static bool aHoldFired = false;

    if (aNow && !bNow && !aHoldArmed) {
      aHoldArmed = true;
      aHoldStart = millis();
      aHoldFired = false;
    }
    if (!aNow) {
      aHoldArmed = false;
      aHoldFired = false;
    }

    if (aNow && !bNow && aHoldArmed && !aHoldFired && !rcLearning &&
        (millis() - aHoldStart) > RC_A_HOLD_MS) {
      aHoldFired = true;
      rcLearning           = true;
      rcLearnUntilMs       = millis() + RC_LEARN_TIMEOUT_MS;
      rcLearnArmSeq        = rx_seq;
      rcLearnIgnoreUntilMs = millis() + 80;

      rx_done = false;
      uiBeepIR(BEEP_OK);
      rcToast("Learning NEC...");
    }

    if (aPress && !rcLearning) {
      RcCode &c = rcDB[rcProfile][rcSelFn];
      if (c.valid && c.isNEC) {
        rcSendSelected();
        uiBeepIR(BEEP_OK);
        rcToast("Sent NEC");
      } else {
        uiBeepIR(BEEP_BACK);
        rcToast("No NEC code stored");
      }
    }
  }

  if (rx_done) {
    rx_done = false;
    irLastHitMs    = millis();
    irLastSymCount = (uint16_t)rx_symbol_num;

    if (rcLearning) {
      if (rx_seq <= rcLearnArmSeq || millis() < rcLearnIgnoreUntilMs) {
        rmtStartReceive();
      } else {
        uint16_t addr = 0;
        uint8_t cmd = 0;
        RcCode &dst = rcDB[rcProfile][rcSelFn];

        if (rcDecodeNECFromRx(addr, cmd)) {
          dst.valid = true;
          dst.isNEC = true;
          dst.address = addr;
          dst.command = cmd;
          dst.repeats = 0;
          rcSaveOne(rcProfile, rcSelFn);
          uiBeepIR(BEEP_OK);
          rcToast("NEC learned + saved");
        } else {
          dst.valid = false;
          dst.isNEC = false;
          dst.address = 0;
          dst.command = 0;
          dst.repeats = 0;
          uiBeepIR(BEEP_BACK);
          rcToast("Not NEC / decode fail");
        }

        rcLearning = false;
        rmtStartReceive();
      }
    } else {
      rmtStartReceive();
    }
  }

  if (rcLearning && millis() > rcLearnUntilMs) {
    rcLearning = false;
    uiBeepIR(BEEP_BACK);
    rcToast("Learn timeout");
  }

  appCanvas.fillScreen(TFT_BLACK);
  appCanvas.fillRect(0, 0, W, 2, cyan);
  drawCornerBrackets(appCanvas, 6, 26, W - 12, H - 40, green);

  appCanvas.setTextSize(1);
  appCanvas.setTextColor(subC);
  appCanvas.setCursor(8, 8);
  appCanvas.println("IR TOOLS NEC  (hold B to exit)");

  appCanvas.setTextColor(green);
  appCanvas.setCursor(12, 36);
  appCanvas.printf("Profile: %d", rcProfile + 1);

  appCanvas.setCursor(12, 52);
  appCanvas.printf("Fn: %s", rcFnName[rcSelFn]);

  RcCode &c = rcDB[rcProfile][rcSelFn];
  appCanvas.setCursor(12, 68);
  appCanvas.setTextColor(c.valid ? pink : subC);
  appCanvas.printf("Stored: %s", c.valid ? "YES" : "NO");

  appCanvas.setCursor(12, 84);
  appCanvas.setTextColor(subC);
  if (c.valid && c.isNEC) {
    appCanvas.printf("Addr:%04X Cmd:%02X", c.address, c.command);
  } else {
    appCanvas.printf("Addr:---- Cmd:--");
  }

  appCanvas.setCursor(12, 100);
  appCanvas.setTextColor(subC);
  if (rcLearning) {
    uint32_t sec = (rcLearnUntilMs > millis()) ? (uint32_t)((rcLearnUntilMs - millis()) / 1000) : 0;
    appCanvas.setTextColor(pink);
    appCanvas.printf("Learning: %lus", (unsigned long)sec);
  } else if (irLastHitMs) {
    appCanvas.printf("Last RX: %u sym", irLastSymCount);
  } else {
    appCanvas.printf("Last RX: none");
  }

  const int footerY = 114;
  appCanvas.fillRect(0, footerY, W, H - footerY, TFT_BLACK);

  appCanvas.setTextSize(1);
  if (millis() < rcToastUntil && rcToastMsg[0]) {
    drawTrimmedText(appCanvas, 8, footerY + 2, W - 16, rcToastMsg, TFT_WHITE);
  } else {
    appCanvas.setTextColor(subC);
    appCanvas.setCursor(8, footerY + 0);
    appCanvas.println("B:NextFn  BB:ClrBtn  A:Send");
    appCanvas.setCursor(8, footerY + 10);
    appCanvas.println("HoldA:Learn  A+B:Profile");
  }
}


// =======================================================
// SETTINGS APP 
// =======================================================
static void drawBatteryBar(M5Canvas& c, int x, int y, int w, int h, int pct, uint16_t col, uint16_t dim) {
  pct = clampi(pct, 0, 100);
  c.drawRect(x, y, w, h, dim);
  int fill = (w - 2) * pct / 100;
  c.fillRect(x + 1, y + 1, fill, h - 2, col);
}

static int readBatteryPercent() {
  int pct = (int)M5.Power.getBatteryLevel();
  if (pct < 0 || pct > 100) pct = -1;
  return pct;
}

static float readBatteryVoltage() {
  float raw = (float)M5.Power.getBatteryVoltage();
  float v = raw / 1000.0f;
  if (v < 2.5f || v > 5.5f) {
    v = raw;
    if (v < 2.5f || v > 5.5f) return 0.0f;
  }
  return v;
}

static String formatHMSDots(uint32_t ms) {
  uint32_t total = ms / 1000;
  uint32_t hh = total / 3600;
  uint32_t mm = (total % 3600) / 60;
  uint32_t ss = total % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu.%02lu.%02lu",
           (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
  return String(buf);
}

static void settingsEnter() {
  irToolsActive = false;
  speakerEnsureOn();
  settingsPage = SET_PAGE_LIST;
  settingsSel  = 0;
}

static void settingsLoop() {
  updatePulse();
  uint16_t cyan  = themeCyan(pulse);
  uint16_t green = themeGreen(pulse);
  uint16_t pink  = themePink(pulse);
  uint16_t subC  = themeSub(pulse);

  if (btnBHoldExit()) {
    uiBeep(BEEP_BACK);
    currentApp = APP_MENU;
    return;
  }

  if (settingsPage == SET_PAGE_LIST) {
    if (M5.BtnB.wasPressed()) { settingsSel = (settingsSel + 1) % 4; uiBeep(BEEP_NAV); }
    if (M5.BtnA.wasPressed()) {
      uiBeep(BEEP_OK);
      settingsPage = (settingsSel == 0) ? SET_PAGE_ROTATION :
                     (settingsSel == 1) ? SET_PAGE_TIMER :
                     (settingsSel == 2) ? SET_PAGE_BATTERY :
                                          SET_PAGE_THEME;
    }

    appCanvas.fillScreen(TFT_BLACK);
    appCanvas.fillRect(0, 0, W, 2, cyan);
    appCanvas.setTextSize(1);
    appCanvas.setTextColor(subC);
    appCanvas.setCursor(8, 8);
    appCanvas.println("SETTINGS  (hold B to exit)");

    drawCornerBrackets(appCanvas, 6, 26, W - 12, H - 40, green);

    const char* items[4] = { "ROTATION", "TIMER", "BATTERY", "THEME" };
    int y0 = 44;
    for (int i = 0; i < 4; i++) {
      bool sel = (i == settingsSel);
      if (sel) {
        appCanvas.drawRect(11, y0 + i*16 - 3, W - 22, 14, pink);
        appCanvas.drawRect(12, y0 + i*16 - 2, W - 24, 12, pink);
      }
      appCanvas.setTextColor(sel ? pink : green);
      appCanvas.setCursor(18, y0 + i*16);
      appCanvas.println(items[i]);
    }

    appCanvas.setTextColor(subC);
    appCanvas.setCursor(8, H - 14);
    appCanvas.println("B: Next   A: Select");
    return;
  }

  if (settingsPage == SET_PAGE_ROTATION) {
    if (M5.BtnA.wasPressed()) {
      ui_flipRotation = !ui_flipRotation;
      uiApplyRotation();
      uiSave();
      uiBeep(BEEP_OK);
    }
    if (M5.BtnB.wasPressed()) { uiBeep(BEEP_BACK); settingsPage = SET_PAGE_LIST; }

    appCanvas.fillScreen(TFT_BLACK);
    appCanvas.fillRect(0, 0, W, 2, cyan);
    drawCornerBrackets(appCanvas, 6, 26, W - 12, H - 40, green);

    appCanvas.setTextSize(2);
    appCanvas.setTextColor(pink);
    appCanvas.setCursor(12, 40);
    appCanvas.println("Rotation");

    appCanvas.setTextSize(1);
    appCanvas.setTextColor(subC);
    appCanvas.setCursor(12, 72);
    appCanvas.print("Blue button: ");
    appCanvas.println(ui_flipRotation ? "LEFT" : "RIGHT");

    appCanvas.setCursor(12, 92);
    appCanvas.println("A: Toggle");
    appCanvas.setCursor(12, 108);
    appCanvas.println("B: Back");
    return;
  }

  if (settingsPage == SET_PAGE_BATTERY) {
    if (M5.BtnB.wasPressed()) { uiBeep(BEEP_BACK); settingsPage = SET_PAGE_LIST; }

    int pct = readBatteryPercent();
    float v = readBatteryVoltage();

    appCanvas.fillScreen(TFT_BLACK);
    appCanvas.fillRect(0, 0, W, 2, cyan);
    drawCornerBrackets(appCanvas, 6, 26, W - 12, H - 40, green);

    appCanvas.setTextSize(2);
    appCanvas.setTextColor(pink);
    appCanvas.setCursor(12, 40);
    appCanvas.println("Battery");

    if (pct >= 0) {
      appCanvas.setTextSize(3);
      appCanvas.setTextColor(themeGreen(220));

      String pctStr = String(pct) + "%";
      int charWidth = 6 * 3;
      int textWidth = (int)pctStr.length() * charWidth;
      int x = (W - textWidth) / 2;
      if (x < 0) x = 0;

      appCanvas.setCursor(x, 66);
      appCanvas.print(pctStr);

      drawBatteryBar(appCanvas, 20, 96, W - 40, 14, pct,
                     themeGreen(220), themeSub(180));

      if (v > 0.0f) {
        appCanvas.fillRect(12, 114, W - 24, 12, TFT_BLACK);
        appCanvas.setTextSize(1);
        appCanvas.setTextColor(subC);
        appCanvas.setCursor(12, 116);
        appCanvas.printf("V: %.2f", v);
      }
    } else {
      appCanvas.setTextSize(1);
      appCanvas.setTextColor(subC);
      appCanvas.setCursor(12, 78);
      appCanvas.println("N/A on this build");
    }

    appCanvas.setTextSize(1);
    appCanvas.setTextColor(subC);
    appCanvas.setCursor(12, 120);
    appCanvas.println("B: Back");
    return;
  }

  if (settingsPage == SET_PAGE_THEME) {
    if (M5.BtnA.wasPressed()) {
      ui_theme = (ui_theme + 1) % 3;
      uiSave();
      uiBeep(BEEP_OK);
    }
    if (M5.BtnB.wasPressed()) { uiBeep(BEEP_BACK); settingsPage = SET_PAGE_LIST; }

    appCanvas.fillScreen(TFT_BLACK);
    appCanvas.fillRect(0, 0, W, 2, cyan);
    drawCornerBrackets(appCanvas, 6, 26, W - 12, H - 40, green);

    appCanvas.setTextSize(2);
    appCanvas.setTextColor(pink);
    appCanvas.setCursor(12, 40);
    appCanvas.println("Theme");

    const char* tname[3] = { "CYAN/GREEN", "PURPLE/TEAL", "ORANGE/BLUE" };

    appCanvas.setTextSize(1);
    appCanvas.setTextColor(subC);
    appCanvas.setCursor(12, 72);
    appCanvas.println("A: Next theme");
    appCanvas.setCursor(12, 88);
    appCanvas.print("Now: ");
    appCanvas.println(tname[ui_theme]);

    appCanvas.fillRect(12, 104, 60, 8, themeCyan(220));
    appCanvas.fillRect(78, 104, 60, 8, themeGreen(220));
    appCanvas.fillRect(144,104, 60, 8, themePink(220));

    appCanvas.setCursor(12, 118);
    appCanvas.println("B: Back");
    return;
  }

  if (settingsPage == SET_PAGE_TIMER) {
    if (M5.BtnA.wasPressed()) {
      timerRunning = !timerRunning;
      if (timerRunning) timerStartMs = millis();
      else timerAccumMs += (millis() - timerStartMs);
      uiBeep(BEEP_OK);
    }
    if (M5.BtnB.wasPressed()) {
      timerRunning = false;
      timerAccumMs = 0;
      timerStartMs = millis();
      uiBeep(BEEP_BACK);
    }

    uint32_t elapsed = timerAccumMs + (timerRunning ? (millis() - timerStartMs) : 0);

    appCanvas.fillScreen(TFT_BLACK);
    appCanvas.fillRect(0, 0, W, 2, cyan);
    drawCornerBrackets(appCanvas, 6, 26, W - 12, H - 40, green);

    appCanvas.setTextSize(2);
    appCanvas.setTextColor(pink);
    appCanvas.setCursor(12, 38);
    appCanvas.println("Timer");

    String t = formatHMSDots(elapsed);

    appCanvas.setTextSize(4);
    appCanvas.setTextColor(themeGreen(220));

    int charW = 6 * 4;
    int textW = (int)t.length() * charW;
    int x = (W - textW) / 2;
    if (x < 0) x = 0;

    appCanvas.fillRect(0, 56, W, 44, TFT_BLACK);

    appCanvas.setCursor(x, 64);
    appCanvas.print(t);

    appCanvas.setTextSize(1);
    appCanvas.setTextColor(subC);
    appCanvas.setCursor(12, 106);
    appCanvas.println("A: Start/Stop");
    appCanvas.setCursor(12, 120);
    appCanvas.println("B: Reset   (hold B: exit)");
    return;
  }
}

// =======================================================
// App dispatcher
// =======================================================
static void enterApp(AppId id) {
  lastApp = currentApp;
  currentApp = id;

  if (lastApp == APP_VISUALIZER && id != APP_VISUALIZER) {
    visualizerStopAudio();
    speakerEnsureOn();
    M5.Speaker.setVolume(90);
  }

  if (apps[id].onEnter) apps[id].onEnter();
}

static void drawBootSplash(uint32_t showMs = 1200) {
  // Some builds need RGB565 byte swapping when pushing raw arrays
  M5.Display.setSwapBytes(true);

  M5.Display.pushImage(0, 0, PULSEOS_BOOT_W, PULSEOS_BOOT_H,
                       (uint16_t*)pulseos_boot_img);

  M5.Display.setSwapBytes(false);
  delay(showMs);
}

// =======================================================
// Setup / Loop
// =======================================================
void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);

  M5.Power.setExtOutput(true);
  delay(20);

  M5.Speaker.begin();
  M5.Speaker.setVolume(90);

  randomSeed((uint32_t)esp_random());

  uiLoad();                 // rotation/theme loaded here
  scoreLoad();
  drawBootSplash(1300);     // <-- add this (1.3s feels nice)

  menuCanvas.setColorDepth(16);
  appCanvas.setColorDepth(16);

  menuSpriteOK = menuCanvas.createSprite(W, H);
  appSpriteOK  = appCanvas.createSprite(W, H);

  M5.Imu.begin();

  currentApp = APP_MENU;
  selectedIndex = 1;
}

void loop() {
  M5.update();

  if (currentApp == APP_MENU) {
    if (M5.BtnB.wasPressed()) {
      selectedIndex++;
      if (selectedIndex >= MAIN_COUNT) selectedIndex = 1;
      uiBeep(BEEP_NAV);
    }
    if (M5.BtnA.wasPressed()) {
      uiBeep(BEEP_OK);

      // Map main menu selection -> AppId
      if (selectedIndex == 1) enterApp(APP_PLAY);
      else if (selectedIndex == 2) enterApp(APP_VISUALIZER);
      else if (selectedIndex == 3) enterApp(APP_IRTOOLS);
      else if (selectedIndex == 4) enterApp(APP_SETTINGS);
    }

    drawMenuSprite();
    menuCanvas.pushSprite(0, 0);
    return;
  }

  if (apps[currentApp].onLoop) apps[currentApp].onLoop();
  appCanvas.pushSprite(0, 0);
}