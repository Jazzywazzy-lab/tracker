// Workout Monitor: ESP32 version
// Live Firebase data over the ESP32's own Wi-Fi on the OLED, with the pot,
// 3 buttons and 2 LEDs. No Yun Shield and no fetch_live.py needed: the ESP32
// downloads /live from Firebase itself and does the same filtering/shortening
// that fetch_live.py used to do (active people only, ASCII text, same limits).
//
// Board:     ESP32 Dev Module (ESP32 DevKit V1), Arduino-ESP32 core 2.x or 3.x
// Libraries (Library Manager):
//   U8g2         by oliver
//   ArduinoJson  by Benoit Blanchon (version 7.x)
//   (WiFi, HTTPClient, WiFiClientSecure come with the ESP32 core)
//
// FILL IN: WIFI_SSID, WIFI_PASS below (2.4 GHz network; the ESP32 can't do 5 GHz).
//
// SET USE_WIFI TO 0 to run the screen on built-in fake people instead (no Wi-Fi
// needed). Handy for testing the buttons, pot and LEDs on the bench.
//
// WIRING (everything is 3.3 V. Do NOT use the 5V/VIN pin for the pot or OLED):
//   OLED   GND->GND, VCC->3V3, SDA->GPIO21, SCL->GPIO22
//   Pot    left->3V3, middle->GPIO34, right->GND
//   Left button   GPIO25 -> GND
//   Middle button GPIO26 -> GND
//   Right button  GPIO27 -> GND
//   LED1 GPIO18 -> 220 ohm -> LED -> GND
//   LED2 GPIO19 -> 220 ohm -> LED -> GND
//
// UPLOADING: plug the ESP32 in over USB, pick "ESP32 Dev Module" and the port,
// upload. If it hangs on "Connecting...", hold the BOOT button until it starts.
//
// BUTTONS (tap = quick press, hold = about 0.6 s):
//   LEFT    tap: previous page        hold: jump home to the Overview
//   RIGHT   tap: next page            hold: jump to the next person doing it WRONG
//   MIDDLE  tap: refresh now          hold: freeze / unfreeze auto refresh
//   POT     scrolls the rows of the page
//
// LEDS (solid, nothing blinks):
//   nobody doing it wrong -> both off
//   1 person wrong        -> LED1 on
//   2+ people wrong       -> both on
//
// SCREEN (nothing flashes on refresh):
//   * Header turns white              = this person's last rep was wrong
//   * Bar on a row's left edge        = a problem (wrong rep, form issue, attention)
//   * "+2" in the header              = 2 OTHER pages changed since you looked
//   * White rows on the Overview      = that person changed since you looked
//   * Boxed "NEW" in the header       = this page changed since your last visit; the
//     rows that changed are white until you leave the page
//   * Boxed "HOLD" / "OFFLINE"        = refresh frozen / last download failed

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <atomic>

// ---------- Settings ----------
#define USE_WIFI   1                       // 1 = live data from Firebase, 0 = built-in demo people
#define TEXT_SIZE  3                       // 3 = huge (2 rows), 2 = large (3 rows), 1 = medium (4 rows)
#define POT_INVERT 0                       // set to 1 if scrolling feels backwards

// iPhones use a curly apostrophe (’) in the hotspot name, so that's tried first;
// if it doesn't connect, the sketch tries the plain apostrophe (') version.
const char* WIFI_SSID     = "Nila\xE2\x80\x99s iPhone";
const char* WIFI_SSID_ALT = "Nila's iPhone";
const char* WIFI_PASS     = "hanischubby";
const char* DB_URL    = "https://cccare-ac2e3-default-rtdb.firebaseio.com";   // no trailing slash
const char* AUTH      = "";                // optional: token appended as ?auth=... if your rules need it

const int      POT_DEADBAND     = 6;
const uint32_t AUTO_REFRESH_MS  = 3000;    // time between downloads
const uint32_t FETCH_TIMEOUT_MS = 15000;   // show OFFLINE if a download takes longer than this
const uint16_t HTTP_TIMEOUT_MS  = 8000;    // same as curl -m 8 in the old script
const uint32_t DEBOUNCE_MS      = 30;
const uint32_t LONG_PRESS_MS    = 600;
const uint32_t TOAST_MS         = 700;
const uint32_t DRAW_EVERY_MS    = 100;

#if USE_WIFI
  #include <WiFi.h>
  #include <WiFiClientSecure.h>
  #include <HTTPClient.h>
  #include <ArduinoJson.h>
#endif

// ---------- Pins ----------
const uint8_t PIN_SDA         = 21;
const uint8_t PIN_SCL         = 22;
const uint8_t PIN_POT         = 34;        // ADC1 pin: still works while Wi-Fi is on
const uint8_t PIN_BTN_PREV    = 25;
const uint8_t PIN_BTN_REFRESH = 26;
const uint8_t PIN_BTN_NEXT    = 27;
const uint8_t PIN_LED1        = 18;
const uint8_t PIN_LED2        = 19;

// Full-buffer mode ("_F_"): the ESP32 has plenty of RAM.
// If the screen stays blank or looks shifted, use the SH1106 line instead
// (common on 1.3" modules).
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);
// U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);

// ---------- Layout ----------
#if TEXT_SIZE == 3
  #define BODY_FONT u8g2_font_10x20_tr
  const uint8_t PITCH = 26, BASE_OFF = 19, VISIBLE_LINES = 2, LINE_CHARS = 11;
#elif TEXT_SIZE == 2
  #define BODY_FONT u8g2_font_9x15B_tr
  const uint8_t PITCH = 15, BASE_OFF = 12, VISIBLE_LINES = 3, LINE_CHARS = 13;
#else
  #define BODY_FONT u8g2_font_7x13B_tr
  const uint8_t PITCH = 13, BASE_OFF = 10, VISIBLE_LINES = 4, LINE_CHARS = 16;
#endif
const uint8_t HEADER_H = 10;
const uint8_t BODY_TOP = 11;
const uint8_t TEXT_X   = 5;

// ---------- Data ----------
const uint8_t MAX_USERS = 8;               // people kept on screen (more can be active)

struct User {
  char     name[11];                       // 10 chars, same as fetch_live.py
  char     ex[15];                         // exercise label, 14 chars, e.g. "SQUATS"
  uint16_t correct;
  uint16_t incorrect;
  uint8_t  pct;
  char     lastRep;                        // 'C' correct, 'I' incorrect, 0 none
  bool     needs;                          // needs attention
  char     issue[25];                      // 24 chars
  char     reason[25];                     // 24 chars
  uint8_t  mask;                           // fields changed while you were elsewhere
  uint8_t  shown;                          // fields highlighted right now
  bool     unseen;                         // changed while you were elsewhere
  bool     seen;                           // present in the download being read
};

User    view[MAX_USERS];
uint8_t viewCount = 0;
uint8_t totalActive = 0;                   // from the "OK|n" line (can exceed viewCount)

// Per-person field bits
const uint8_t F_CORRECT = 1, F_WRONG = 2, F_PCT = 4, F_LAST = 8, F_ISSUE = 16, F_ATTN = 32;
const uint8_t F_ALL = 63;
// Overview bits
const uint8_t O_ACTIVE = 1, O_ATTN = 2, O_WRONG = 4, O_CORR = 8, O_WRNG = 16, O_AVG = 32;
const uint8_t O_STRUCTURAL = O_ACTIVE | O_ATTN | O_WRONG;

struct Agg {
  uint8_t  n, attn, wrong;
  uint16_t c, w;
};

uint8_t ovMask = 0, ovShown = 0;
bool    ovUnseen = false;

// Row styles
const uint8_t ST_CHANGED = 1;              // white row
const uint8_t ST_ALERT   = 2;              // bar on the left edge

// Only the rows currently on screen are kept.
struct Row {
  char    text[LINE_CHARS + 1];
  uint8_t style;
};
Row     vis[VISIBLE_LINES];
uint8_t rowCursor = 0;
uint8_t rowFrom = 0;

// ---------- UI state ----------
uint8_t  page = 0;                         // 0 = overview, 1..viewCount = people
bool     holdMode = false;
bool     netOk = true;
char     netMsg[11] = "";
bool     firstLoad = true;
uint32_t lastFetchDone = 0;
uint32_t lastDraw = 0;
int      potAccepted = 0;
uint32_t lastPotRead = 0;
char     toastText[14] = "";
uint32_t toastUntil = 0;

// Parse state (one download). Each person becomes one line in the same
// "name|exercise|right|wrong|percent|last_rep|needs|issue|reason" format
// fetch_live.py printed, so the rest of the sketch works exactly as before.
char     rx[160];
uint8_t  rxLen = 0;
bool     gotHeader = false, gotErr = false;
char     prevName[11] = "";
Agg      oldAgg;

// ---------- Buttons (tap and hold) ----------
enum BtnEvent : uint8_t { EV_NONE, EV_TAP, EV_HOLD };

struct Button {
  uint8_t  pin;
  bool     stable;
  bool     lastRead;
  uint32_t changedAt;
  uint32_t pressedAt;
  bool     holdFired;
};
Button btnPrev    = { PIN_BTN_PREV,    HIGH, HIGH, 0, 0, false };
Button btnRefresh = { PIN_BTN_REFRESH, HIGH, HIGH, 0, 0, false };
Button btnNext    = { PIN_BTN_NEXT,    HIGH, HIGH, 0, 0, false };

BtnEvent pollButton(Button& b, uint32_t now) {
  bool r = digitalRead(b.pin);
  if (r != b.lastRead) { b.lastRead = r; b.changedAt = now; }
  BtnEvent ev = EV_NONE;
  if (now - b.changedAt > DEBOUNCE_MS && r != b.stable) {
    b.stable = r;
    if (r == LOW) { b.pressedAt = now; b.holdFired = false; }   // pull-up: LOW = pressed
    else if (!b.holdFired) ev = EV_TAP;                         // released before a hold
  }
  if (b.stable == LOW && !b.holdFired && now - b.pressedAt >= LONG_PRESS_MS) {
    b.holdFired = true;
    ev = EV_HOLD;
  }
  return ev;
}

// ---------- Small helpers ----------
void copyStr(char* dst, const char* src, size_t size) {
  size_t i = 0;
  while (i + 1 < size && src[i]) { dst[i] = src[i]; i++; }
  dst[i] = 0;
}

void showToast(uint32_t now, const char* text) {
  copyStr(toastText, text, sizeof(toastText));
  toastUntil = now + TOAST_MS;
}

// Adds one row. Only rows inside the visible window are stored.
void addLine(uint8_t style, const char* fmt, ...) {
  uint8_t idx = rowCursor++;
  if (idx < rowFrom || idx >= (uint8_t)(rowFrom + VISIBLE_LINES)) return;
  Row& r = vis[idx - rowFrom];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r.text, LINE_CHARS + 1, fmt, ap);
  va_end(ap);
  r.style = style;
}
#define ADD(style, fmt, ...) addLine(style, fmt, ##__VA_ARGS__)

// Adds text as several rows, wrapping at spaces.
void addWrapped(const char* text, uint8_t indent, uint8_t style) {
  const size_t width = LINE_CHARS - indent;
  const size_t len = strlen(text);
  size_t pos = 0;
  while (pos < len) {
    size_t chunk = len - pos;
    if (chunk > width) {
      chunk = width;
      size_t k = chunk;
      while (k > 0 && text[pos + k] != ' ') k--;
      if (k > 0) chunk = k;
    }
    char buf[LINE_CHARS + 1];
    uint8_t n = 0;
    while (n < indent) buf[n++] = ' ';
    for (size_t i = 0; i < chunk && n < LINE_CHARS; i++) buf[n++] = text[pos + i];
    buf[n] = 0;
    ADD(style, "%s", buf);
    pos += chunk;
    while (text[pos] == ' ') pos++;
  }
}

int percentCorrect(uint32_t c, uint32_t i) {
  return (c + i) ? (int)(c * 100UL / (c + i)) : 0;
}

Agg aggregate() {
  Agg a;
  memset(&a, 0, sizeof(a));
  a.n = totalActive;
  for (uint8_t i = 0; i < viewCount; i++) {
    if (view[i].needs) a.attn++;
    if (view[i].lastRep == 'I') a.wrong++;
    a.c += view[i].correct;
    a.w += view[i].incorrect;
  }
  return a;
}

uint8_t diffAgg(const Agg& a, const Agg& b) {
  uint8_t m = 0;
  if (a.n != b.n)         m |= O_ACTIVE;
  if (a.attn != b.attn)   m |= O_ATTN;
  if (a.wrong != b.wrong) m |= O_WRONG;
  if (a.c != b.c)         m |= O_CORR;
  if (a.w != b.w)         m |= O_WRNG;
  if (percentCorrect(a.c, a.w) != percentCorrect(b.c, b.w)) m |= O_AVG;
  return m;
}

uint8_t diffUser(const User& a, const User& b) {
  uint8_t m = 0;
  if (a.correct != b.correct)     m |= F_CORRECT | F_PCT | F_LAST;   // a new rep happened
  if (a.incorrect != b.incorrect) m |= F_WRONG | F_PCT | F_LAST;
  if (a.lastRep != b.lastRep)     m |= F_LAST;
  if (strcmp(a.issue, b.issue) != 0) m |= F_ISSUE;
  if (a.needs != b.needs || strcmp(a.reason, b.reason) != 0) m |= F_ATTN;
  return m;
}

uint8_t wrongNow() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < viewCount; i++) if (view[i].lastRep == 'I') n++;
  return n;
}

// ---------- Page switching ----------
void leavePage() {
  if (page == 0) ovShown = 0;
  else if (page <= viewCount) view[page - 1].shown = 0;
}

// Highlight what changed since you were last on this page.
void enterPage() {
  if (page == 0) {
    ovShown = ovMask;
    ovMask = 0;
    ovUnseen = false;
  } else if (page <= viewCount) {
    User& u = view[page - 1];
    u.shown = u.mask;
    u.mask = 0;
    u.unseen = false;
  }
}

void goToPage(uint8_t newPage) {
  leavePage();
  page = newPage;
  enterPage();
}

// Right button hold: jump to the next person whose last rep was wrong.
void jumpToNextAlert(uint32_t now) {
  uint8_t start = (page == 0) ? 0 : page;      // 0-based index of the person after the current one
  for (uint8_t step = 0; step < viewCount; step++) {
    uint8_t idx = (start + step) % viewCount;
    if (view[idx].lastRep == 'I') {
      goToPage(idx + 1);
      char t[14];
      char nm[9];
      copyStr(nm, view[idx].name, sizeof(nm));
      snprintf(t, sizeof(t), "> %s", nm);
      showToast(now, t);
      return;
    }
  }
  showToast(now, "NO ALERTS");
}

// ---------- Reading a download ----------
void beginParse() {
  for (uint8_t i = 0; i < viewCount; i++) view[i].seen = false;
  prevName[0] = 0;
  if (page > 0 && page <= viewCount) copyStr(prevName, view[page - 1].name, sizeof(prevName));
  oldAgg = aggregate();
  rxLen = 0;
  gotHeader = false;
  gotErr = false;
}

void handleLine() {
  rx[rxLen] = 0;
  rxLen = 0;
  if (rx[0] == 0) return;

  if (strncmp(rx, "OK|", 3) == 0) {
    int n = atoi(rx + 3);
    totalActive = (uint8_t)constrain(n, 0, 99);
    gotHeader = true;
    return;
  }
  if (strncmp(rx, "ERR|", 4) == 0) {
    copyStr(netMsg, rx + 4, sizeof(netMsg));
    gotErr = true;
    return;
  }

  // name|exercise|right|wrong|percent|last_rep|needs|issue|reason
  char* f[9];
  uint8_t nf = 0;
  char* p = rx;
  f[nf++] = p;
  while (*p && nf < 9) {
    if (*p == '|') { *p = 0; f[nf++] = p + 1; }
    p++;
  }
  while (nf < 9) f[nf++] = p;                  // missing fields read as empty text

  User t;
  memset(&t, 0, sizeof(t));
  copyStr(t.name, f[0], sizeof(t.name));
  copyStr(t.ex, f[1], sizeof(t.ex));
  t.correct   = (uint16_t)constrain(atoi(f[2]), 0, 65535);
  t.incorrect = (uint16_t)constrain(atoi(f[3]), 0, 65535);
  t.pct       = (uint8_t)constrain(atoi(f[4]), 0, 100);
  t.lastRep   = (f[5][0] == 'I') ? 'I' : ((f[5][0] == 'C') ? 'C' : 0);
  t.needs     = (f[6][0] == '1');
  copyStr(t.issue, f[7], sizeof(t.issue));
  copyStr(t.reason, f[8], sizeof(t.reason));
  t.seen = true;

  int j = -1;
  for (uint8_t q = 0; q < viewCount; q++) {
    if (strcmp(view[q].name, t.name) == 0) { j = q; break; }
  }
  if (j >= 0) {
    uint8_t m = diffUser(view[j], t);
    t.mask = view[j].mask;
    t.shown = view[j].shown;
    t.unseen = view[j].unseen;
    bool isCurrent = (prevName[0] != 0 && strcmp(prevName, t.name) == 0);
    if (m && !isCurrent) {                     // changes on the page you're on just update quietly
      t.mask |= m;
      t.unseen = true;
    }
    view[j] = t;
  } else if (viewCount < MAX_USERS) {          // new person
    t.mask = F_ALL;
    t.unseen = true;
    view[viewCount++] = t;
  }
}

// Feeds one complete line (without newline) to the parser.
void feedLine(const char* line) {
  copyStr(rx, line, sizeof(rx));
  rxLen = strlen(rx);
  handleLine();
}

void endParse(uint32_t now) {
  if (rxLen > 0) handleLine();

  if (gotErr || !gotHeader) {                  // failed download: keep showing the last data
    netOk = false;
    if (!gotErr) copyStr(netMsg, "no reply", sizeof(netMsg));
    lastFetchDone = now;
    return;
  }
  netOk = true;
  netMsg[0] = 0;

  // Drop people who are no longer active.
  uint8_t w = 0;
  for (uint8_t i = 0; i < viewCount; i++) {
    if (view[i].seen) {
      if (w != i) view[w] = view[i];
      w++;
    }
  }
  viewCount = w;

  // Keep following the same person if they're still active.
  if (page > 0) {
    bool found = false;
    for (uint8_t i = 0; i < viewCount; i++) {
      if (strcmp(view[i].name, prevName) == 0) { page = i + 1; found = true; break; }
    }
    if (!found) {
      if (page > viewCount) page = viewCount;
      enterPage();
    }
  }

  if (firstLoad) {                             // nothing to flag on the very first load
    for (uint8_t i = 0; i < viewCount; i++) { view[i].mask = 0; view[i].shown = 0; view[i].unseen = false; }
    ovMask = ovShown = 0;
    ovUnseen = false;
    firstLoad = false;
  } else {
    uint8_t om = diffAgg(oldAgg, aggregate());
    if (om && page != 0) {
      ovMask |= om;
      if (om & O_STRUCTURAL) ovUnseen = true;
    }
  }
  lastFetchDone = now;
}

// ---------- Text cleanup (same rules as fetch_live.py) ----------
// UTF-8 characters that get swapped for plain ASCII. Any other non-ASCII
// character becomes '?', like Python's encode("ascii", "replace").
struct Translit { const char* from; const char* to; };
const Translit TRANSLIT[] = {
  { "\xC2\xB0",     ""   },   // degree sign
  { "\xE2\x89\xA4", "<=" },   // less-than-or-equal
  { "\xE2\x89\xA5", ">=" },   // greater-than-or-equal
  { "\xC3\x97",     "x"  },   // multiplication sign
  { "\xC2\xB7",     "/"  },   // middle dot
  { "\xE2\x80\xA2", "/"  },   // bullet
  { "\xE2\x80\x93", "-"  },   // en dash
  { "\xE2\x80\x94", "-"  },   // em dash
  { "\xE2\x80\x99", "'"  },   // curly apostrophe
};

// Plain ASCII, no '|' or newlines, at most `limit` characters (0 = no limit).
void cleanText(const char* in, char* out, size_t outSize, size_t limit) {
  size_t maxLen = outSize - 1;
  if (limit && limit < maxLen) maxLen = limit;
  size_t n = 0;
  const uint8_t* p = (const uint8_t*)in;
  while (*p && n < maxLen) {
    uint8_t c = *p;
    if (c < 0x80) {
      if (c == '|') out[n++] = '/';
      else if (c == '\n' || c == '\r') out[n++] = ' ';
      else out[n++] = (char)c;
      p++;
      continue;
    }
    size_t len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    bool swapped = false;
    for (const Translit& t : TRANSLIT) {
      if (strlen(t.from) == len && strncmp((const char*)p, t.from, len) == 0) {
        for (const char* q = t.to; *q && n < maxLen; q++) out[n++] = *q;
        swapped = true;
        break;
      }
    }
    if (!swapped) out[n++] = '?';
    for (size_t k = 0; k < len && *p; k++) p++;
  }
  out[n] = 0;
}

// "squats" -> SQUATS, "jumping_jacks" -> JUMP.JACKS, else upper case, _ -> space, 14 chars.
void exerciseLabel(const char* raw, char* out, size_t outSize) {
  if (strcmp(raw, "squats") == 0)        { copyStr(out, "SQUATS", outSize); return; }
  if (strcmp(raw, "jumping_jacks") == 0) { copyStr(out, "JUMP.JACKS", outSize); return; }
  char tmp[64];
  size_t i = 0;
  for (; raw[i] && i < sizeof(tmp) - 1; i++) {
    char c = raw[i];
    if (c == '_') c = ' ';
    tmp[i] = (char)toupper((unsigned char)c);
  }
  tmp[i] = 0;
  cleanText(tmp, out, outSize, 14);
}

// ---------- Getting the data ----------
#if USE_WIFI

// Firebase value -> text (strings as-is, numbers printed, missing = empty).
void variantText(JsonVariantConst v, char* buf, size_t size) {
  if (v.is<const char*>())     copyStr(buf, v.as<const char*>(), size);
  else if (v.is<bool>())       copyStr(buf, v.as<bool>() ? "True" : "False", size);
  else if (v.is<long long>())  snprintf(buf, size, "%lld", v.as<long long>());
  else if (v.is<double>())     snprintf(buf, size, "%g", v.as<double>());
  else                         buf[0] = 0;
}

// Number fields may arrive as numbers or as text. Rounded to a whole number; bad = 0.
long numVal(JsonVariantConst v) {
  double d;
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    char* end;
    d = strtod(s, &end);
    if (end == s) return 0;
  } else if (v.is<bool>()) {
    d = v.as<bool>() ? 1 : 0;
  } else if (v.is<double>()) {
    d = v.as<double>();
  } else {
    return 0;
  }
  if (isnan(d) || isinf(d)) return 0;
  return lround(d);
}

// Same idea as Python's "if value:".
bool truthy(JsonVariantConst v) {
  if (v.isNull())                return false;
  if (v.is<bool>())              return v.as<bool>();
  if (v.is<const char*>())       return v.as<const char*>()[0] != 0;
  if (v.is<double>())            return v.as<double>() != 0;
  if (v.is<JsonObjectConst>())   return v.as<JsonObjectConst>().size() > 0;
  if (v.is<JsonArrayConst>())    return v.as<JsonArrayConst>().size() > 0;
  return false;
}

// "INCORRECT" -> 'I', "CORRECT" -> 'C', anything else -> none.
const char* repCode(JsonVariantConst v) {
  if (!v.is<const char*>()) return "";
  char c = (char)toupper((unsigned char)v.as<const char*>()[0]);
  return c == 'I' ? "I" : (c == 'C' ? "C" : "");
}

int cmpNames(const void* a, const void* b) {
  return strcmp(*(const char* const*)a, *(const char* const*)b);
}

// Turns the /live JSON into the same lines fetch_live.py printed and feeds them in.
void processPayload(const String& body, uint32_t now) {
  beginParse();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    feedLine("ERR|bad reply");
    endParse(now);
    return;
  }

  JsonVariantConst root = doc.as<JsonVariantConst>();
  char line[160];

  // Firebase answers {"error": "..."} when something is wrong (e.g. rules).
  if (root.is<JsonObjectConst>()) {
    JsonObjectConst obj = root.as<JsonObjectConst>();
    JsonVariantConst e = obj["error"];
    if (obj.size() == 1 && !e.isNull() && !e.is<JsonObjectConst>()) {
      char raw[64], msg[64];
      variantText(e, raw, sizeof(raw));
      cleanText(raw, msg, sizeof(msg), 0);
      snprintf(line, sizeof(line), "ERR|%s", msg);
      feedLine(line);
      endParse(now);
      return;
    }
  }

  // Active people, sorted by name.
  const uint8_t MAX_SORT = 64;
  const char* names[MAX_SORT];
  uint8_t nNames = 0;
  int total = 0;
  if (root.is<JsonObjectConst>()) {
    for (JsonPairConst kv : root.as<JsonObjectConst>()) {
      JsonVariantConst rec = kv.value();
      if (!rec.is<JsonObjectConst>()) continue;
      JsonVariantConst st = rec["status"];
      if (!st.is<const char*>() || strcmp(st.as<const char*>(), "active") != 0) continue;
      total++;
      if (nNames < MAX_SORT) names[nNames++] = kv.key().c_str();
    }
  }
  qsort(names, nNames, sizeof(names[0]), cmpNames);

  snprintf(line, sizeof(line), "OK|%d", total);
  feedLine(line);

  JsonObjectConst people = root.as<JsonObjectConst>();
  for (uint8_t i = 0; i < nNames; i++) {
    JsonObjectConst rec = people[names[i]].as<JsonObjectConst>();
    char raw[96], nm[12], ex[16], issue[26], reason[26];

    cleanText(names[i], nm, sizeof(nm), 10);
    variantText(rec["exercise"], raw, sizeof(raw));
    exerciseLabel(raw, ex, sizeof(ex));
    variantText(rec["last_issue"], raw, sizeof(raw));
    cleanText(raw, issue, sizeof(issue), 24);
    variantText(rec["attention_reason"], raw, sizeof(raw));
    cleanText(raw, reason, sizeof(reason), 24);

    snprintf(line, sizeof(line), "%s|%s|%ld|%ld|%ld|%s|%d|%s|%s",
             nm, ex,
             numVal(rec["correct"]), numVal(rec["incorrect"]), numVal(rec["percent_correct"]),
             repCode(rec["last_rep"]),
             truthy(rec["needs_attention"]) ? 1 : 0,
             issue, reason);
    feedLine(line);
  }
  endParse(now);
}

void processError(const char* msg, uint32_t now) {
  char line[24];
  snprintf(line, sizeof(line), "ERR|%s", msg);
  beginParse();
  feedLine(line);
  endParse(now);
}

// The download runs in its own task (on the Wi-Fi core) so the screen,
// buttons and pot keep working while it waits on the network, just like
// runAsynchronously() did on the Yun.
enum : uint8_t { FS_IDLE, FS_REQUESTED, FS_RUNNING, FS_DONE };
std::atomic<uint8_t> fetchState(FS_IDLE);
String   fetchBody;                        // written by the task, read by loop() once FS_DONE
char     fetchErr[16] = "";
bool     fetchOk = false;
uint32_t fetchStarted = 0;
bool     timeoutShown = false;

void fetchTask(void*) {
  WiFiClientSecure client;
  client.setInsecure();                    // skip certificate checks (simplest; fine for a class project)
  HTTPClient http;
  http.setReuse(true);

  String url = String(DB_URL) + "/live.json";
  if (AUTH[0]) url += String("?auth=") + AUTH;

  for (;;) {
    if (fetchState.load() != FS_REQUESTED) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    fetchState.store(FS_RUNNING);

    fetchOk = false;
    fetchBody = "";
    fetchErr[0] = 0;

    if (WiFi.status() != WL_CONNECTED) {
      copyStr(fetchErr, "no wifi", sizeof(fetchErr));
    } else {
      http.setConnectTimeout(HTTP_TIMEOUT_MS);
      http.setTimeout(HTTP_TIMEOUT_MS);
      if (!http.begin(client, url)) {
        copyStr(fetchErr, "bad url", sizeof(fetchErr));
      } else {
        int code = http.GET();
        if (code > 0) {                    // any HTTP answer: Firebase errors come back as JSON
          fetchBody = http.getString();
          fetchOk = true;
        } else {
          copyStr(fetchErr, "no conn", sizeof(fetchErr));
        }
        http.end();
      }
    }
    fetchState.store(FS_DONE);
  }
}

void startFetch(uint32_t now) {
  fetchStarted = now;
  timeoutShown = false;
  fetchState.store(FS_REQUESTED);
}

void serviceFetch(uint32_t now) {
  uint8_t s = fetchState.load();
  if (s == FS_DONE) {
    if (fetchOk) processPayload(fetchBody, now);
    else         processError(fetchErr, now);
    fetchBody = "";
    fetchState.store(FS_IDLE);
  } else if (s == FS_REQUESTED || s == FS_RUNNING) {
    if (!timeoutShown && now - fetchStarted > FETCH_TIMEOUT_MS) {
      netOk = false;
      copyStr(netMsg, "timeout", sizeof(netMsg));
      timeoutShown = true;
    }
  } else if (!holdMode && now - lastFetchDone >= AUTO_REFRESH_MS) {
    startFetch(now);
  }
}

void requestRefresh(uint32_t now) {
  if (fetchState.load() == FS_IDLE) startFetch(now);
}

#else  // ----- built-in demo people (no Wi-Fi needed) -----

struct Fake {
  bool    on;
  uint8_t who, ex, c, w, issue, target;
  char    last;
};
Fake fk[4];

const char* const DEMO_NAMES[] = { "alex", "maya", "sam_k", "jordan", "priya", "chen" };
const char* const DEMO_EX[]    = { "SQUATS", "JUMP.JACKS", "SHOULDER PRESS" };
const char* const DEMO_ISSUE[] = { "go deeper (reached 106, aim <= 95)", "keep chest up",
                                   "arms higher", "feet wider", "elbows in" };

void demoEvent() {
  uint8_t activeN = 0;
  for (uint8_t i = 0; i < 4; i++) if (fk[i].on) activeN++;

  if (activeN < 2 || (activeN < 4 && random(100) < 15)) {      // someone joins
    for (uint8_t i = 0; i < 4; i++) {
      if (!fk[i].on) {
        fk[i].on = true;
        fk[i].who = random(6);
        fk[i].ex = random(3);
        fk[i].c = 0; fk[i].w = 0; fk[i].last = 0; fk[i].issue = 0;
        fk[i].target = random(12, 22);
        return;
      }
    }
  }
  if (activeN == 0) return;

  uint8_t pick = random(activeN);
  for (uint8_t i = 0; i < 4; i++) {
    if (!fk[i].on) continue;
    if (pick-- > 0) continue;
    if (random(100) < 35) { fk[i].w++; fk[i].last = 'I'; fk[i].issue = random(5); }
    else                  { fk[i].c++; fk[i].last = 'C'; }
    if (fk[i].c + fk[i].w >= fk[i].target) fk[i].on = false;
    return;
  }
}

void demoFetch(uint32_t now) {
  for (uint8_t k = 0; k < 3; k++) demoEvent();                 // a few reps happen per download

  beginParse();
  uint8_t n = 0;
  for (uint8_t i = 0; i < 4; i++) if (fk[i].on) n++;
  char line[160];
  snprintf(line, sizeof(line), "OK|%d", (int)n);
  feedLine(line);
  for (uint8_t i = 0; i < 4; i++) {
    if (!fk[i].on) continue;
    bool wrong = (fk[i].last == 'I');
    bool needs = (fk[i].w >= 5);
    char issue[25];
    copyStr(issue, wrong ? DEMO_ISSUE[fk[i].issue] : "", sizeof(issue));   // 24 chars, like the live data
    snprintf(line, sizeof(line), "%s%d|%s|%d|%d|%d|%s|%d|%s|%s",
             DEMO_NAMES[fk[i].who], (int)i, DEMO_EX[fk[i].ex], (int)fk[i].c, (int)fk[i].w,
             percentCorrect(fk[i].c, fk[i].w),
             fk[i].last == 'I' ? "I" : (fk[i].last == 'C' ? "C" : ""),
             needs ? 1 : 0, issue, needs ? "form issues" : "");
    feedLine(line);
  }
  endParse(now);
}

void serviceFetch(uint32_t now) {
  if (!holdMode && now - lastFetchDone >= AUTO_REFRESH_MS) demoFetch(now);
}

void requestRefresh(uint32_t now) {
  demoFetch(now);
}

#endif

// ---------- LEDs ----------
void updateLeds() {
  uint8_t wrong = wrongNow();
  digitalWrite(PIN_LED1, wrong >= 1 ? HIGH : LOW);   // one person wrong: one LED
  digitalWrite(PIN_LED2, wrong >= 2 ? HIGH : LOW);   // several people wrong: both
}

// ---------- Pot ----------
void readPot(uint32_t now) {
  if (now - lastPotRead < 10) return;
  lastPotRead = now;
  long sum = 0;
  for (uint8_t i = 0; i < 8; i++) sum += analogRead(PIN_POT);
  int raw = (int)(sum / 8);                   // 0..1023 (resolution set to 10 bits in setup)
#if POT_INVERT
  raw = 1023 - raw;
#endif
  if (abs(raw - potAccepted) > POT_DEADBAND) potAccepted = raw;
}

// ---------- Page content ----------
uint8_t ovStyle(uint8_t bit, bool alert) {
  return ((ovShown & bit) ? ST_CHANGED : 0) | (alert ? ST_ALERT : 0);
}

uint8_t usrStyle(const User& u, uint8_t bit, bool alert) {
  return ((u.shown & bit) ? ST_CHANGED : 0) | (alert ? ST_ALERT : 0);
}

void buildOverview(uint32_t now) {
  Agg a = aggregate();
  ADD(ovStyle(O_ACTIVE, false), "Active: %d", (int)a.n);
  if (a.n == 0) ADD(0, "Waiting...");

  // One row per person. White row = they changed since you last looked.
  for (uint8_t i = 0; i < viewCount; i++) {
    const User& u = view[i];
    char nm[7];
    copyStr(nm, u.name, sizeof(nm));
    ADD((u.unseen ? ST_CHANGED : 0) | (u.lastRep == 'I' ? ST_ALERT : 0),
        "%s %d%%", nm, (int)u.pct);
  }
  if (totalActive > viewCount) ADD(0, "+%d more", (int)(totalActive - viewCount));

  ADD(ovStyle(O_ATTN,  a.attn > 0),  "Attn: %d", (int)a.attn);
  ADD(ovStyle(O_WRONG, a.wrong > 0), "Wrong now %d", (int)a.wrong);
  ADD(ovStyle(O_CORR, false), "Right: %d", (int)a.c);
  ADD(ovStyle(O_WRNG, false), "Wrong: %d", (int)a.w);
  ADD(ovStyle(O_AVG,  false), "Avg: %d%%", percentCorrect(a.c, a.w));
  if (netOk) {
    ADD(0, "Net: OK");
  } else {
    ADD(ST_ALERT, "Net: OFF");
    ADD(ST_ALERT, " %s", netMsg);
  }
#if USE_WIFI
  if (WiFi.status() == WL_CONNECTED) ADD(0, "WiFi %ddBm", (int)WiFi.RSSI());
  else                               ADD(ST_ALERT, "WiFi: none");
#endif
  ADD(0, "Sync: %ds", (int)((now - lastFetchDone) / 1000));
#if USE_WIFI
  ADD(0, "Mode: LIVE");
#else
  ADD(0, "Mode: DEMO");
#endif
  ADD(0, "-Controls-");
  ADD(0, "Pot: scroll");
  ADD(0, "L tap: prev");
  ADD(0, "R tap: next");
  ADD(0, "M tap: sync");
  ADD(0, "L hold:");
  ADD(0, " go home");
  ADD(0, "R hold:");
  ADD(0, " next alert");
  ADD(0, "M hold:");
  ADD(0, " freeze");
}

void buildUserPage(uint32_t now, const User& u) {
  bool wrong = (u.lastRep == 'I');
  addWrapped(u.ex, 0, 0);
  ADD(usrStyle(u, F_CORRECT, false), "Right: %d", (int)u.correct);
  ADD(usrStyle(u, F_WRONG,   false), "Wrong: %d", (int)u.incorrect);
  ADD(usrStyle(u, F_PCT,     false), "Pct: %d%%", (int)u.pct);
  if (u.lastRep == 'C')   ADD(usrStyle(u, F_LAST, false), "Last: OK");
  else if (wrong)         ADD(usrStyle(u, F_LAST, true),  "Last: WRONG");
  else                    ADD(usrStyle(u, F_LAST, false), "Last: -");
  if (u.issue[0]) {
    ADD(0, "Issue:");
    addWrapped(u.issue, 1, usrStyle(u, F_ISSUE, true));
  }
  ADD(usrStyle(u, F_ATTN, u.needs), "Attn: %s", u.needs ? "YES" : "no");
  if (u.needs && u.reason[0]) addWrapped(u.reason, 1, usrStyle(u, F_ATTN, true));
  ADD(0, "Sync: %ds", (int)((now - lastFetchDone) / 1000));
}

// ---------- Drawing ----------
char    hdrLeft[16];
bool    hdrInvert = false;
bool    hdrNew = false;
uint8_t hdrUnseen = 0;
uint8_t bodyOffset = 0;
uint8_t bodyTotal = 0;

void drawHeader() {
  u8g2.setFont(u8g2_font_6x10_tr);
  if (hdrInvert) {
    u8g2.drawBox(0, 0, 128, HEADER_H);
    u8g2.setDrawColor(0);
  }
  u8g2.drawStr(1, 8, hdrLeft);

  char pg[10];
  snprintf(pg, sizeof(pg), "%d/%d", (int)page, (int)viewCount);
  int x = 127 - u8g2.getStrWidth(pg);
  u8g2.drawStr(x, 8, pg);

  if (hdrUnseen > 0) {
    char un[6];
    snprintf(un, sizeof(un), "+%d", (int)hdrUnseen);
    x -= u8g2.getStrWidth(un) + 5;
    u8g2.drawStr(x, 8, un);
  }

  const char* badge = holdMode ? "HOLD" : (!netOk ? "OFFLINE" : (hdrNew ? "NEW" : nullptr));
  if (badge) {
    int w = u8g2.getStrWidth(badge);
    x -= w + 7;
    u8g2.drawFrame(x - 2, 0, w + 4, HEADER_H);
    u8g2.drawStr(x, 8, badge);
  }

  u8g2.setDrawColor(1);
  if (!hdrInvert) u8g2.drawHLine(0, HEADER_H, 128);
}

void drawBody() {
  u8g2.setFont(BODY_FONT);
  for (uint8_t i = 0; i < VISIBLE_LINES; i++) {
    if (vis[i].text[0] == 0) continue;
    int top = BODY_TOP + i * PITCH;
    bool changed = vis[i].style & ST_CHANGED;
    bool alert   = vis[i].style & ST_ALERT;

    if (changed) {
      u8g2.setDrawColor(1);
      u8g2.drawBox(0, top, 124, PITCH);
    }
    u8g2.setDrawColor(changed ? 0 : 1);
    if (alert) u8g2.drawBox(0, top, 3, PITCH);       // left-edge alert bar
    u8g2.drawStr(TEXT_X, top + BASE_OFF, vis[i].text);
  }
  u8g2.setDrawColor(1);

  if (bodyTotal > VISIBLE_LINES) {                   // scroll bar
    const int sbTop = 12, sbH = 48;
    uint8_t maxOffset = bodyTotal - VISIBLE_LINES;
    int thumbH = (sbH * VISIBLE_LINES) / bodyTotal;
    if (thumbH < 4) thumbH = 4;
    int thumbY = sbTop + ((sbH - thumbH) * bodyOffset) / maxOffset;
    u8g2.drawVLine(126, sbTop, sbH);
    u8g2.drawBox(125, thumbY, 3, thumbH);
  }
}

void drawToast() {
  u8g2.setFont(u8g2_font_10x20_tr);
  int w = u8g2.getStrWidth(toastText);
  int x = (128 - w) / 2;
  u8g2.setDrawColor(1);
  u8g2.drawBox(x - 6, 18, w + 12, 28);
  u8g2.setDrawColor(0);
  u8g2.drawBox(x - 4, 20, w + 8, 24);
  u8g2.setDrawColor(1);
  u8g2.drawStr(x, 38, toastText);
}

void clearRows() {
  for (uint8_t i = 0; i < VISIBLE_LINES; i++) { vis[i].text[0] = 0; vis[i].style = 0; }
  rowCursor = 0;
}

void buildCurrentPage(uint32_t now) {
  clearRows();
  if (page == 0 || page > viewCount) buildOverview(now);
  else buildUserPage(now, view[page - 1]);
}

void drawScreen(uint32_t now) {
  if (page > viewCount) page = 0;

  // Header facts
  hdrUnseen = 0;
  if (page != 0 && ovUnseen) hdrUnseen++;
  for (uint8_t i = 0; i < viewCount; i++) {
    if ((uint8_t)(i + 1) != page && view[i].unseen) hdrUnseen++;
  }
  if (page == 0) {
    snprintf(hdrLeft, sizeof(hdrLeft), "OVERVIEW");
    hdrInvert = (wrongNow() > 0);
    hdrNew = (ovShown != 0);
  } else {
    const User& u = view[page - 1];
    char nm[7];
    copyStr(nm, u.name, sizeof(nm));
    snprintf(hdrLeft, sizeof(hdrLeft), "%s %c%c", nm, u.ex[0] ? u.ex[0] : '?', u.ex[1] ? u.ex[1] : ' ');
    hdrInvert = (u.lastRep == 'I');
    hdrNew = (u.shown != 0);
  }

  // Pass 1 counts the rows; pass 2 captures the ones the pot is pointing at.
  rowFrom = 0;
  buildCurrentPage(now);
  bodyTotal = rowCursor;
  uint8_t maxOffset = (bodyTotal > VISIBLE_LINES) ? (bodyTotal - VISIBLE_LINES) : 0;
  bodyOffset = (uint8_t)(((uint32_t)potAccepted * (maxOffset + 1)) / 1024);
  if (bodyOffset > maxOffset) bodyOffset = maxOffset;
  if (bodyOffset != 0) {
    rowFrom = bodyOffset;
    buildCurrentPage(now);
  }

  u8g2.firstPage();
  do {
    drawHeader();
    drawBody();
    if ((int32_t)(toastUntil - now) > 0) drawToast();
  } while (u8g2.nextPage());
}

void drawMessage(const char* line) {
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_10x20_tr);
    u8g2.drawStr(29, 22, "WORKOUT");
    u8g2.drawStr(29, 46, "MONITOR");
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(4, 61, line);
  } while (u8g2.nextPage());
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_BTN_PREV, INPUT_PULLUP);
  pinMode(PIN_BTN_REFRESH, INPUT_PULLUP);
  pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
  pinMode(PIN_LED1, OUTPUT);
  pinMode(PIN_LED2, OUTPUT);

  analogReadResolution(10);                  // 0..1023, same range the scroll math expects
  analogSetPinAttenuation(PIN_POT, ADC_11db);// full 0..3.3 V range on the pot
  randomSeed(esp_random());                  // hardware random seed for the demo mode

  u8g2.begin();
  u8g2.setBusClock(400000);

#if USE_WIFI
  drawMessage("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);               // reconnects by itself if the network drops
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {         // try the plain-apostrophe name
    drawMessage("Trying other name...");
    WiFi.disconnect();
    delay(200);
    WiFi.begin(WIFI_SSID_ALT, WIFI_PASS);
    t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK, IP ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi not connected yet (will keep trying)");
  }
  // Network work runs on core 0 next to the Wi-Fi stack; loop() stays on core 1.
  xTaskCreatePinnedToCore(fetchTask, "fetch", 16384, nullptr, 1, nullptr, 0);
#endif
  drawMessage("Loading data...");

  potAccepted = analogRead(PIN_POT);
  lastFetchDone = millis() - AUTO_REFRESH_MS;  // fetch right away
}

void loop() {
  uint32_t now = millis();

  // RIGHT: tap = next page, hold = jump to next person doing it wrong
  BtnEvent e = pollButton(btnNext, now);
  if (e == EV_TAP)       goToPage((page + 1) % (viewCount + 1));
  else if (e == EV_HOLD) jumpToNextAlert(now);

  // LEFT: tap = previous page, hold = home (Overview)
  e = pollButton(btnPrev, now);
  if (e == EV_TAP)       goToPage((page + viewCount) % (viewCount + 1));
  else if (e == EV_HOLD) { goToPage(0); showToast(now, "HOME"); }

  // MIDDLE: tap = refresh now, hold = freeze / unfreeze auto refresh
  e = pollButton(btnRefresh, now);
  if (e == EV_TAP) {
    requestRefresh(now);
    showToast(now, "REFRESHING");
  } else if (e == EV_HOLD) {
    holdMode = !holdMode;
    showToast(now, holdMode ? "HOLD ON" : "HOLD OFF");
  }

  serviceFetch(now);
  readPot(now);
  updateLeds();

  if (now - lastDraw >= DRAW_EVERY_MS) {
    lastDraw = now;
    drawScreen(now);
  }
}
