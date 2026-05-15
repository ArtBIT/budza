#include <Arduino.h>
#include <Wire.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <JPEGDEC.h>
#include <CST816S.h>
#include <esp_sleep.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// ── Pin assignments ────────────────────────────────────────────────────────────
#define TOUCH_SDA  6
#define TOUCH_SCL  7
#define TOUCH_INT  5
#define TOUCH_RST  13

// ── Timing ────────────────────────────────────────────────────────────────────
#define TARGET_FPS  24
#define FRAME_MS    (1000 / TARGET_FPS)

// ── Media catalogue ───────────────────────────────────────────────────────────
#define MAX_ITEMS   64

enum MediaType : uint8_t { STILL, VIDEO };

struct Item {
    char      path[64];
    MediaType type;
};

static Item items[MAX_ITEMS];
static int  itemCount    = 0;
static int  currentIndex = 0;

// ── Peripherals ───────────────────────────────────────────────────────────────
static TFT_eSPI tft;
static JPEGDEC  jpeg;
static CST816S  touch(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);

// ── WiFi / upload state ───────────────────────────────────────────────────────
static bool      wifiEnabled  = false;
static WebServer server(80);
static char      wifiSSID[20];
static bool      uploadAborted = false;
static File      uploadFile;
static char      uploadFileName[64];
static bool      uploadIsAvi   = false;
static uint8_t   aviHdrBuf[72];
static uint8_t   aviHdrLen     = 0;
static bool      aviHdrChecked = false;
static bool      pendingNav    = false;

// ── Display / brightness ──────────────────────────────────────────────────────
static uint8_t   brightness    = 200;
static Preferences prefs;

// ── Config strip ──────────────────────────────────────────────────────────────
static int       configPage    = 1;   // 0=WiFi, 1=App Info, 2=Brightness

// ── AVI RIFF structs ──────────────────────────────────────────────────────────
struct __attribute__((packed)) RiffChunk   { char fcc[4]; uint32_t size; };
struct __attribute__((packed)) AviMainHdr  {
    uint32_t dwMicroSecPerFrame, dwMaxBytesPerSec, dwPaddingGranularity,
             dwFlags, dwTotalFrames;
};
struct __attribute__((packed)) Idx1Entry   {
    char chunkId[4]; uint32_t flags, offset, size;
};

// ── Colors ────────────────────────────────────────────────────────────────────
#define COL_DIM   0x2104
#define COL_MID   0x4208
#define COL_RED   0xF800
#define COL_GREEN 0x07E0
#define COL_CYAN  0x07FF

// ── Helpers ───────────────────────────────────────────────────────────────────
static void showMessage(const char* line1, const char* line2 = nullptr) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    if (line2) { tft.drawString(line1, 120, 105); tft.drawString(line2, 120, 135); }
    else        { tft.drawString(line1, 120, 120); }
}

static void setBrightness(uint8_t val) {
    brightness = val;
    analogWrite(TFT_BL, val);
    prefs.putUChar("bright", val);
}

static int jpegDrawCallback(JPEGDRAW* pDraw) {
    tft.pushImage(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
    return 1;
}

static void decodeJpeg(uint8_t* buf, uint32_t len, bool clearFirst = false) {
    if (!jpeg.openRAM(buf, (int)len, jpegDrawCallback)) {
        Serial.printf("JPEG open failed (err %d) len=%u\n", jpeg.getLastError(), len);
        showMessage("JPEG open", "failed");
        return;
    }
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    int w = jpeg.getWidth(), h = jpeg.getHeight(), maxDim = max(w, h);
    int scaleFlag, scaledW, scaledH;
    if      (maxDim > 960) { scaleFlag = JPEG_SCALE_EIGHTH;  scaledW = w/8; scaledH = h/8; }
    else if (maxDim > 480) { scaleFlag = JPEG_SCALE_QUARTER; scaledW = w/4; scaledH = h/4; }
    else if (maxDim > 240) { scaleFlag = JPEG_SCALE_HALF;    scaledW = w/2; scaledH = h/2; }
    else                   { scaleFlag = 0;                   scaledW = w;   scaledH = h;   }
    if (clearFirst) tft.fillScreen(TFT_BLACK);
    int rc = jpeg.decode(max(0,(240-scaledW)/2), max(0,(240-scaledH)/2), scaleFlag);
    if (rc != 1) { Serial.printf("JPEG decode error: %d\n", rc); showMessage("JPEG err", nullptr); }
    jpeg.close();
}

static uint32_t lastGestureMs = 0;
#define SWIPE_DEBOUNCE_MS 500

static uint8_t readGesture() {
    if (!touch.available()) return NONE;
    uint8_t g = touch.data.gestureID;
    if (g == NONE) return NONE;
    // Non-swipe events (tap, double-tap) bypass the debounce
    bool isSwipe = (g == SWIPE_LEFT || g == SWIPE_RIGHT ||
                    g == SWIPE_UP   || g == SWIPE_DOWN);
    uint32_t now = millis();
    if (isSwipe && now - lastGestureMs < SWIPE_DEBOUNCE_MS) return NONE;
    if (isSwipe) lastGestureMs = now;
    return g;
}

static void drainTouch() {
    delay(50);
    while (touch.available()) {}
}

static bool endsWith(const char* str, const char* suf) {
    size_t sl = strlen(str), ul = strlen(suf);
    return sl >= ul && strcasecmp(str + sl - ul, suf) == 0;
}


// ── Media enumeration ─────────────────────────────────────────────────────────
static void sortItems() {
    for (int i = 1; i < itemCount; i++) {
        Item key = items[i]; int j = i - 1;
        while (j >= 0 && strcmp(items[j].path, key.path) > 0) { items[j+1] = items[j]; j--; }
        items[j+1] = key;
    }
}

static void enumerateMedia() {
    itemCount = 0;
    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) { Serial.println("LittleFS root open failed"); return; }
    File f = root.openNextFile();
    while (f && itemCount < MAX_ITEMS) {
        if (!f.isDirectory()) {
            const char* n = f.name();
            Item& it = items[itemCount];
            if (endsWith(n, ".jpg") || endsWith(n, ".jpeg")) {
                snprintf(it.path, sizeof(it.path), "/%s", n); it.type = STILL; itemCount++;
            } else if (endsWith(n, ".avi")) {
                snprintf(it.path, sizeof(it.path), "/%s", n); it.type = VIDEO; itemCount++;
            }
        }
        f = root.openNextFile();
    }
    sortItems();
    Serial.printf("Found %d media items\n", itemCount);
}

// ── Still display ─────────────────────────────────────────────────────────────
static void showStill(const char* path) {
    File f = LittleFS.open(path, "r");
    if (!f) { showMessage("Open failed", path); return; }
    size_t sz = f.size();
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) { showMessage("malloc fail", nullptr); f.close(); return; }
    f.read(buf, sz); f.close();
    decodeJpeg(buf, sz, true);
    free(buf);
}

// ── AVI MJPEG engine ──────────────────────────────────────────────────────────
static bool aviParseHeader(File& f, uint32_t* moviStart, uint32_t* totalFrames) {
    RiffChunk riff; char form[4];
    if (f.read((uint8_t*)&riff, 8) != 8 || memcmp(riff.fcc, "RIFF", 4)) return false;
    if (f.read((uint8_t*)form, 4) != 4 || memcmp(form, "AVI ", 4))       return false;
    *totalFrames = 0; *moviStart = 0;
    uint32_t pos = 12;
    while (pos < 8 + riff.size) {
        f.seek(pos); RiffChunk ch;
        if (f.read((uint8_t*)&ch, 8) != 8) break;
        if (!memcmp(ch.fcc, "LIST", 4)) {
            char lt[4]; f.read((uint8_t*)lt, 4);
            if (!memcmp(lt, "hdrl", 4)) {
                RiffChunk av; f.read((uint8_t*)&av, 8);
                if (!memcmp(av.fcc, "avih", 4)) {
                    AviMainHdr h; f.read((uint8_t*)&h, sizeof(h)); *totalFrames = h.dwTotalFrames;
                }
            } else if (!memcmp(lt, "movi", 4)) {
                *moviStart = pos + 12;
            }
        }
        pos += 8 + ch.size + (ch.size & 1);
    }
    return *moviStart != 0;
}

static bool aviReadIdx1(File& f, uint32_t moviStart,
                        uint32_t** outOff, uint32_t* outCnt) {
    f.seek(12);
    while (f.position() < f.size() - 8) {
        RiffChunk ch;
        if (f.read((uint8_t*)&ch, 8) != 8) break;
        if (!memcmp(ch.fcc, "idx1", 4)) {
            uint32_t n = ch.size / sizeof(Idx1Entry);
            uint32_t* off = (uint32_t*)malloc(n * sizeof(uint32_t));
            if (!off) return false;
            uint32_t cnt = 0;
            for (uint32_t i = 0; i < n; i++) {
                Idx1Entry e; f.read((uint8_t*)&e, sizeof(e));
                if (e.chunkId[2] == 'd' || e.chunkId[2] == 'D')
                    off[cnt++] = moviStart - 4 + e.offset;
            }
            *outOff = off; *outCnt = cnt; return cnt > 0;
        }
        if (!f.seek(f.position() + ch.size + (ch.size & 1))) break;
    }
    return false;
}

static uint8_t playVideo(const char* path) {
    lastGestureMs = 0;   // entering new content — don't carry over swipe debounce
    File f = LittleFS.open(path, "r");
    if (!f) { showMessage("Open failed", path); return NONE; }
    uint32_t moviStart, totalFrames;
    if (!aviParseHeader(f, &moviStart, &totalFrames)) {
        showMessage("AVI parse", "failed"); f.close(); return NONE;
    }
    uint32_t *frameOff = nullptr, frameCnt = 0;
    if (!aviReadIdx1(f, moviStart, &frameOff, &frameCnt)) {
        showMessage("AVI index", "missing"); f.close(); return NONE;
    }
    Serial.printf("Playing %s — %u frames\n", path, frameCnt);
    uint8_t* fb = (uint8_t*)malloc(64 * 1024);
    if (!fb) { free(frameOff); f.close(); return NONE; }

    uint8_t exit = NONE; uint32_t fi = 0;
    while (exit == NONE) {
        uint8_t g = readGesture();
        if (g == SWIPE_LEFT || g == SWIPE_RIGHT || g == SWIPE_UP ||
            g == SWIPE_DOWN || g == DOUBLE_CLICK) { exit = g; break; }
        uint32_t t0 = millis();
        f.seek(frameOff[fi]); RiffChunk ch; f.read((uint8_t*)&ch, 8);
        if (ch.size <= 64*1024) { f.read(fb, ch.size); decodeJpeg(fb, ch.size); }
        if (++fi >= frameCnt) fi = 0;
        uint32_t el = millis() - t0;
        while (el < FRAME_MS) {
            g = readGesture();
            if (g == SWIPE_LEFT || g == SWIPE_RIGHT || g == SWIPE_UP ||
                g == SWIPE_DOWN || g == DOUBLE_CLICK) { exit = g; break; }
            if (wifiEnabled) server.handleClient();
            delay(5); el = millis() - t0;
        }
    }
    free(fb); free(frameOff); f.close();
    return exit;
}

// ── Sleep ─────────────────────────────────────────────────────────────────────
static void enterLightSleep() {
    while (touch.available()) {}
    esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH_INT, 0);
    esp_light_sleep_start();
    delay(20);
}

static void enterSleep() {
    analogWrite(TFT_BL, 0);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH_INT, 0);
    esp_light_sleep_start();
    analogWrite(TFT_BL, brightness);
}

// ── File management ───────────────────────────────────────────────────────────
static void deleteCurrentItem() {
    LittleFS.remove(items[currentIndex].path);
    for (int i = currentIndex; i < itemCount - 1; i++) items[i] = items[i+1];
    itemCount--;
    if (itemCount == 0) { currentIndex = 0; return; }
    if (currentIndex >= itemCount) currentIndex = itemCount - 1;
}

// ── Info strip ────────────────────────────────────────────────────────────────
static bool showInfoStrip() {
    const char* path = items[currentIndex].path;
    const char* name = (path[0] == '/') ? path + 1 : path;
    bool isAvi = (items[currentIndex].type == VIDEO);

    File f = LittleFS.open(path, "r");
    size_t fsz = f ? f.size() : 0; if (f) f.close();
    char szStr[20];
    if (fsz >= 1024*1024) snprintf(szStr, sizeof(szStr), "%.1f MB", fsz/(1024.0f*1024.0f));
    else                   snprintf(szStr, sizeof(szStr), "%u KB",   (unsigned)(fsz/1024));
    char posStr[16];
    snprintf(posStr, sizeof(posStr), "%d / %d", currentIndex + 1, itemCount);

    char shortName[28]; strncpy(shortName, name, 27); shortName[27] = 0;

    auto draw = [&](bool confirm) {
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);

        // Type pill
        uint16_t pc = isAvi ? 0x001F : 0x039F;   // AVI=navy, JPEG=blue
        tft.fillRoundRect(85, 42, 70, 22, 6, pc);
        tft.setTextColor(TFT_WHITE, pc); tft.setTextSize(1);
        tft.drawString(isAvi ? "AVI" : "JPEG", 120, 53);

        // Metadata
        tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(1);
        tft.drawString(shortName, 120, 80);
        tft.setTextColor(COL_MID,  TFT_BLACK);
        tft.drawString(szStr,  120, 103);
        tft.drawString(posStr, 120, 123);

        // Delete / Confirm button
        uint16_t bc = confirm ? COL_RED : 0x6000;
        tft.fillRoundRect(55, 155, 130, 36, 8, bc);
        tft.setTextColor(TFT_WHITE, bc); tft.setTextSize(2);
        tft.drawString(confirm ? "CONFIRM" : "DELETE", 120, 173);

        tft.setTextSize(1); tft.setTextColor(COL_MID, TFT_BLACK);
        tft.drawString("swipe down to return", 120, 217);
    };

    draw(false);
    drainTouch();

    uint32_t lastTouch = millis();
    bool     awaitConfirm = false;
    uint32_t confirmAt    = 0;

    while (true) {
        if (millis() - lastTouch > 8000) return false;
        if (awaitConfirm && millis() - confirmAt > 3000) {
            awaitConfirm = false; draw(false);
        }
        if (touch.available()) {
            lastTouch = millis();
            uint8_t g = touch.data.gestureID;
            int     ty = touch.data.y;

            if (g == DOUBLE_CLICK) { enterSleep(); drainTouch(); return false; }
            if (g != NONE && g != SINGLE_CLICK) { drainTouch(); return false; }

            if (g == SINGLE_CLICK) {
                if (ty >= 155 && ty <= 191) {
                    if (!awaitConfirm) { awaitConfirm = true; confirmAt = millis(); draw(true); }
                    else               { drainTouch(); deleteCurrentItem(); return true; }
                } else {
                    if (awaitConfirm) { awaitConfirm = false; draw(false); }
                }
            }
        }
        delay(10);
    }
}

// ── WiFi upload ───────────────────────────────────────────────────────────────
static void addCorsHeaders() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

static void handleCorsPrelight() {
    addCorsHeaders();
    server.send(204);
}

static void serveUploadForm() {
    addCorsHeaders();
    server.send(200, "text/html",
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Badge</title>"
        "<style>body{font-family:sans-serif;padding:20px;background:#111;color:#eee}"
        "h2{color:#4af}input[type=file]{display:block;margin:16px 0;font-size:16px}"
        "button{background:#4af;color:#000;border:none;padding:14px 28px;"
        "font-size:18px;border-radius:8px;cursor:pointer}"
        "small{color:#888}</style></head><body>"
        "<h2>Badge Upload</h2>"
        "<form method='POST' action='/upload' enctype='multipart/form-data'>"
        "<input type='file' name='file' accept='.jpg,.jpeg,.avi'>"
        "<br><br><button type='submit'>Upload</button></form>"
        "<p><small>JPEG: any size — auto-scaled to 240×240<br>"
        "AVI: must be exactly 240×240 MJPEG</small></p>"
        "</body></html>");
}

static void handleFileUpload() {
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        uploadAborted = false; aviHdrLen = 0; aviHdrChecked = false;
        String safe = "/";
        for (char c : up.filename) if (isalnum(c) || c=='.' || c=='-' || c=='_') safe += c;
        if (safe.length() <= 1) safe = "/upload.jpg";
        strlcpy(uploadFileName, safe.c_str(), sizeof(uploadFileName));
        uploadIsAvi = safe.endsWith(".avi") || safe.endsWith(".AVI");
        uploadFile  = LittleFS.open(uploadFileName, "w");
        if (!uploadFile) uploadAborted = true;
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (uploadAborted) return;
        if (uploadIsAvi && !aviHdrChecked) {
            size_t take = min((size_t)(72 - aviHdrLen), (size_t)up.currentSize);
            memcpy(aviHdrBuf + aviHdrLen, up.buf, take); aviHdrLen += take;
            if (aviHdrLen >= 72) {
                aviHdrChecked = true;
                uint32_t w, h; memcpy(&w, aviHdrBuf+64, 4); memcpy(&h, aviHdrBuf+68, 4);
                if (w != 240 || h != 240) {
                    uploadFile.close(); LittleFS.remove(uploadFileName);
                    uploadAborted = true; return;
                }
                uploadFile.write(aviHdrBuf, aviHdrLen);
                if (up.currentSize > take) uploadFile.write(up.buf+take, up.currentSize-take);
            }
        } else { uploadFile.write(up.buf, up.currentSize); }
    } else if (up.status == UPLOAD_FILE_END) {
        if (uploadFile) uploadFile.close();
    }
}

static void handleUploadComplete() {
    addCorsHeaders();
    if (uploadAborted) {
        server.send(400, "text/plain", "Rejected: AVI must be exactly 240x240 pixels");
        return;
    }
    enumerateMedia();
    for (int i = 0; i < itemCount; i++)
        if (strcmp(items[i].path, uploadFileName) == 0) { currentIndex = i; break; }
    server.send(200, "text/html",
        "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:sans-serif;padding:20px;background:#111;color:#eee}"
        "a{color:#4af}</style></head><body>"
        "<h2 style='color:#4f4'>&#10003; Uploaded!</h2>"
        "<p><a href='/'>Upload another</a></p></body></html>");
    pendingNav = true;
}

static void startWifi() {
    WiFi.softAP(wifiSSID);
    server.on("/", HTTP_GET, serveUploadForm);
    server.on("/upload", HTTP_OPTIONS, handleCorsPrelight);
    server.on("/upload", HTTP_POST, handleUploadComplete, handleFileUpload);
    server.begin(); wifiEnabled = true;
    Serial.printf("AP: %s — 192.168.4.1\n", wifiSSID);
}
static void stopWifi() {
    server.stop(); WiFi.softAPdisconnect(true); wifiEnabled = false;
}

// ── Config pages ──────────────────────────────────────────────────────────────
static void drawConfigDots() {
    for (int i = 0; i < 3; i++)
        tft.fillCircle(100 + i*20, 220, 4, (i == configPage) ? TFT_WHITE : COL_DIM);
}

static void drawWifiPage() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(2);
    tft.drawString("WiFi Upload", 120, 55);
    tft.setTextDatum(ML_DATUM); tft.drawString("WiFi", 20, 110);
    if (wifiEnabled) {
        tft.fillRoundRect(155, 97, 65, 26, 6, COL_GREEN);
        tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_BLACK, COL_GREEN);
        tft.drawString("ON", 187, 110);
        tft.setTextSize(1); tft.setTextColor(COL_CYAN, TFT_BLACK); tft.setTextDatum(MC_DATUM);
        tft.drawString(wifiSSID,       120, 148);
        tft.drawString("192.168.4.1",  120, 163);
    } else {
        tft.fillRoundRect(155, 97, 65, 26, 6, COL_DIM);
        tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE, COL_DIM);
        tft.drawString("OFF", 187, 110);
        tft.fillRect(0, 138, 240, 34, TFT_BLACK);
    }
    tft.setTextSize(1); tft.setTextColor(COL_MID, TFT_BLACK); tft.setTextDatum(MC_DATUM);
    tft.drawString("swipe up to return", 120, 198);
}

static void drawBrightnessPage() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(2);
    tft.drawString("Brightness", 120, 55);
    // Bar
    tft.drawRect(20, 100, 200, 24, COL_MID);
    int fw = (int)map(brightness, 30, 255, 0, 196);
    if (fw > 0) tft.fillRect(22, 102, fw, 20, TFT_WHITE);
    // Percentage
    char pct[8]; snprintf(pct, sizeof(pct), "%d%%", (int)map(brightness, 30, 255, 0, 100));
    tft.setTextSize(2); tft.drawString(pct, 120, 148);
    // Tap zone labels
    tft.setTextSize(1); tft.setTextColor(COL_MID, TFT_BLACK);
    tft.setTextDatum(ML_DATUM); tft.drawString("- tap", 28, 178);
    tft.setTextDatum(MR_DATUM); tft.drawString("tap +", 212, 178);
    tft.setTextDatum(MC_DATUM); tft.drawString("swipe up to return", 120, 198);
}

static void drawAppInfoPage() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(2);
    tft.drawString("Budz v1.0", 120, 55);
    tft.setTextSize(1); tft.setTextColor(COL_MID, TFT_BLACK);
    char buf[40];
    snprintf(buf, sizeof(buf), "Built: %s", __DATE__); tft.drawString(buf, 120, 92);
    snprintf(buf, sizeof(buf), "Media: %d items", itemCount);  tft.drawString(buf, 120, 115);
    size_t used = LittleFS.usedBytes(), total = LittleFS.totalBytes();
    snprintf(buf, sizeof(buf), "Used:  %.1f MB", used/(1024.0f*1024)); tft.drawString(buf, 120, 138);
    snprintf(buf, sizeof(buf), "Free:  %.1f MB", (total-used)/(1024.0f*1024)); tft.drawString(buf, 120, 158);
    snprintf(buf, sizeof(buf), "Total: %.1f MB", total/(1024.0f*1024)); tft.drawString(buf, 120, 178);
    tft.setTextColor(COL_MID, TFT_BLACK); tft.drawString("swipe up to return", 120, 200);
}

// ── Config strip (blocking) ───────────────────────────────────────────────────
static void showConfigStrip() {
    auto drawPage = [&]() {
        switch (configPage) {
            case 0: drawWifiPage();       break;
            case 1: drawAppInfoPage();    break;
            case 2: drawBrightnessPage(); break;
        }
        drawConfigDots();
    };
    drawPage();

    while (true) {
        if (pendingNav) { pendingNav = false; return; }
        if (!touch.available()) {
            if (wifiEnabled) server.handleClient();
            delay(10); continue;
        }
        uint8_t g  = touch.data.gestureID;
        int     tx = touch.data.x, ty = touch.data.y;

        if (g == SWIPE_UP)    { drainTouch(); return; }
        if (g == DOUBLE_CLICK){ enterSleep(); drawPage(); continue; }
        if (g == SWIPE_LEFT)  { if (configPage < 2) { configPage++; drawPage(); } drainTouch(); }
        else if (g == SWIPE_RIGHT) { if (configPage > 0) { configPage--; drawPage(); } drainTouch(); }
        else if (g == SINGLE_CLICK) {
            if (configPage == 0 && tx > 140 && ty > 90 && ty < 130) {
                wifiEnabled ? stopWifi() : startWifi(); drawPage();
            } else if (configPage == 2) {
                setBrightness(tx < 120 ? max(30, (int)brightness - 32)
                                       : min(255, (int)brightness + 32));
                drawPage();
            }
        }
    }
}

// ── showItem (slideshow only) ─────────────────────────────────────────────────
static uint8_t showItem(int index) {
    Serial.printf("Showing item %d: %s\n", index, items[index].path);
    if (items[index].type == VIDEO) return playVideo(items[index].path);
    showStill(items[index].path); return NONE;
}

// ── Help / splash screen ─────────────────────────────────────────────────────
static void showHelpScreen() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    // ▲ Info (top)
    tft.fillTriangle(120,18, 108,36, 132,36, COL_MID);
    tft.setTextSize(1);
    tft.drawString("Info", 120, 48);

    // ◄ Next (left)
    tft.fillTriangle(20,120, 38,108, 38,132, COL_MID);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("Next", 44, 120);

    // ► Prev (right)
    tft.fillTriangle(220,120, 202,108, 202,132, COL_MID);
    tft.setTextDatum(MR_DATUM);
    tft.drawString("Prev", 196, 120);

    // "Config" label + ▼ (bottom)
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Config", 120, 186);
    tft.fillTriangle(120,210, 108,194, 132,194, COL_MID);

    // Centre title
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("Budz", 120, 112);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(COL_MID, TFT_BLACK);
    tft.drawString("v1.0.0", 116, 132);

    // Dismiss hint
    tft.setTextColor(COL_DIM, TFT_BLACK);
    tft.drawString("tap to start", 120, 218);

    drainTouch();
    while (!touch.available()) { delay(10); }
    drainTouch();
}

// ── Arduino entry points ──────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    prefs.begin("badge", false);
    brightness = prefs.getUChar("bright", 200);

    tft.init();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);

    analogWrite(TFT_BL, brightness);

    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    touch.begin(RISING);
    touch.enable_double_click();

    uint64_t mac = ESP.getEfuseMac();
    snprintf(wifiSSID, sizeof(wifiSSID), "Budz-%04X", (uint16_t)(mac >> 32));

    if (!LittleFS.begin(true)) {
        showMessage("LittleFS", "mount failed"); return;
    }

    enumerateMedia();
    if (itemCount == 0) {
        showMessage("No media", "swipe down for WiFi"); return;
    }
    currentIndex = 0;
    showHelpScreen();
    (void)showItem(currentIndex);
}

void loop() {
    if (wifiEnabled) server.handleClient();

    // No media: just wait for swipe-down to config strip
    if (itemCount == 0) {
        uint8_t g = readGesture();
        if (g == NONE)       { enterLightSleep(); return; }
        if (g == SWIPE_DOWN) { configPage = 2; showConfigStrip(); showMessage("No media", "swipe down for WiFi"); }
        return;
    }

    // Get gesture: video blocks, stills sleep
    uint8_t g = NONE;
    if (items[currentIndex].type == VIDEO) {
        g = playVideo(items[currentIndex].path);
    } else {
        g = readGesture();
        if (g == NONE) {
            if (wifiEnabled) { server.handleClient(); delay(10); }
            else enterLightSleep();
            return;
        }
    }

    if (g == SWIPE_UP) {
        showInfoStrip();
        if (itemCount > 0) (void)showItem(currentIndex);
        else               showMessage("No media", "swipe down for WiFi");
        drainTouch();
    } else if (g == SWIPE_DOWN) {
        configPage = 1;   // always enter config strip at App Info
        showConfigStrip();
        if (itemCount > 0) (void)showItem(currentIndex);
        drainTouch();
    } else if (g == SWIPE_LEFT) {
        currentIndex = (currentIndex + 1) % itemCount;
        (void)showItem(currentIndex); drainTouch();
    } else if (g == SWIPE_RIGHT) {
        currentIndex = (currentIndex - 1 + itemCount) % itemCount;
        (void)showItem(currentIndex); drainTouch();
    } else if (g == DOUBLE_CLICK) {
        enterSleep();
        if (itemCount > 0) (void)showItem(currentIndex);
    }
}
