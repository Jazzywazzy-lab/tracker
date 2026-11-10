// Workout Monitor: UI demo v3 (huge text, no periodic flashing, more buttons)
// Board:   Arduino UNO R4 Minima
// Library: U8g2 (install "U8g2" by oliver from the Library Manager)
//
// WIRING (unchanged):
//   OLED   GND->GND, VCC->5V (or 3.3V if your module needs it), SCL->A5, SDA->A4
//   Pot    left->5V, middle->A0, right->GND
//   Left button   D2 -> GND
//   Middle button D3 -> GND
//   Right button  D4 -> GND
//   LED1 D6 -> 220 ohm -> LED -> GND
//   LED2 D7 -> 220 ohm -> LED -> GND
//
// BUTTONS (tap = quick press, hold = press for about 0.6 s):
//   LEFT    tap: previous page          hold: jump home to the Overview
//   RIGHT   tap: next page              hold: jump to the next person doing it WRONG
//   MIDDLE  tap: refresh now            hold: freeze / unfreeze auto refresh (HOLD)
//   POT     scrolls the rows of the current page
//
// LEDS (based on the last refresh):
//   nobody doing it wrong  -> both off
//   1 person doing it wrong -> LED1 on
//   2+ people doing it wrong -> both on
//
// THE SCREEN NO LONGER FLASHES ON REFRESH. Indicators are all steady:
//   * Header turns white              = this person's last rep was wrong
//   * Solid bar on a row's left edge  = a problem (wrong rep, form issue, attention)
//   * "+2" in the header              = 2 OTHER pages updated since you looked
//   * Overview rows shown in white    = that person updated since you looked
//   * Boxed "NEW" in the header       = this page changed since your last visit,
//     and the rows that changed are white until you leave the page
//   * Boxed "HOLD" in the header      = auto refresh is frozen
// A short message pops up for 0.7 s after a button action (REFRESHED, HOME...).

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// If the screen stays blank or looks shifted, comment out the SSD1306 line
// and use the SH1106 one instead (common on 1.3" modules).
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
// U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ---------- Pins ----------
const uint8_t PIN_POT         = A0;
const uint8_t PIN_BTN_PREV    = 2;
const uint8_t PIN_BTN_REFRESH = 3;
const uint8_t PIN_BTN_NEXT    = 4;
const uint8_t PIN_LED1        = 6;
const uint8_t PIN_LED2        = 7;

// ---------- Settings ----------
#define TEXT_SIZE 3                        // 3 = huge (2 rows), 2 = large (3 rows), 1 = medium (4 rows)
#define POT_INVERT 0                       // set to 1 if scrolling feels backwards
#define LED_BLINK_ON_DATA 0                // 1 = LED1 also blinks briefly when new reps arrive (and nobody is wrong)
const int      POT_DEADBAND     = 6;
const uint32_t SIM_TICK_MS      = 1000;    // fake server does one thing per second
const uint32_t AUTO_REFRESH_MS  = 3000;
const uint32_t DEBOUNCE_MS      = 30;
const uint32_t LONG_PRESS_MS    = 600;
const uint32_t TOAST_MS         = 700;
const uint32_t BLINK_MS         = 120;

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
const uint8_t HEADER_H = 10;               // header bar height
const uint8_t BODY_TOP = 11;               // first body row starts here
const uint8_t TEXT_X   = 5;                // text indent (room for the alert bar)
const uint8_t MAX_LINES = 28;

// ---------- Data model (mirrors /live in Firebase) ----------
const uint8_t MAX_USERS = 6;

enum Exercise : uint8_t { SQUATS = 0, JUMPING_JACKS = 1 };

struct User {
  char     name[14];
  Exercise exercise;
  uint16_t correct;
  uint16_t incorrect;
  char     lastRep;            // 'C' correct, 'I' incorrect, 0 none yet
  char     lastIssue[44];
  uint16_t issueCount[2];
  bool     needsAttention;     // true once incorrect >= 5
  uint32_t updated;            // millis of the last rep
  bool     active;
  uint16_t target;             // demo only: reps before this person finishes
};

const char* const NAMES[MAX_USERS] = { "alex", "maya", "sam_k", "jordan", "priya", "chen" };
const char* const ISSUES[2][2] = {
  { "go deeper", "keep chest up" },    // squats
  { "arms higher", "feet wider" }      // jumping jacks
};

User srv[MAX_USERS];     // the fake "server"
User view[MAX_USERS];    // what the display last downloaded (active users only)
uint8_t viewCount = 0;

// ---------- Change tracking ----------
// Per-user field bits (which values changed)
const uint8_t F_CORRECT = 1, F_WRONG = 2, F_PCT = 4, F_LAST = 8, F_ISSUE = 16, F_ATTN = 32;
const uint8_t F_ALL = 63;
// Overview aggregate bits
const uint8_t O_ACTIVE = 1, O_ATTN = 2, O_WRONG = 4, O_SQ = 8, O_JJ = 16, O_CORR = 32, O_WRNG = 64, O_AVG = 128;
const uint8_t O_STRUCTURAL = O_ACTIVE | O_ATTN | O_WRONG;   // these count as "unseen" updates

struct UserUi {
  uint8_t mask;       // fields that changed while you were on another page
  uint8_t shown;      // fields highlighted right now (changes since your last visit)
  bool    unseen;     // this page changed while you were elsewhere
};
UserUi  viewUi[MAX_USERS];
uint8_t ovMask = 0, ovShown = 0;
bool    ovUnseen = false;

struct Agg {
  uint8_t  n, sq, jj, attn, wrong;
  uint16_t c, w;
};

// Row styles
const uint8_t ST_CHANGED = 1;   // white row
const uint8_t ST_ALERT   = 2;   // solid bar on the left edge

struct Line {
  char    text[LINE_CHARS + 1];
  uint8_t style;
};
Line    lines[MAX_LINES];
uint8_t lineCount = 0;

// ---------- UI state ----------
uint8_t  page = 0;                 // 0 = overview, 1..viewCount = users
bool     holdMode = false;         // true = auto refresh frozen
uint32_t lastSim = 0;
uint32_t lastRefresh = 0;
uint32_t blinkUntil = 0;
uint32_t lastSumReps = 0;
uint32_t lastDraw = 0;
int      potAccepted = 0;
uint32_t lastPotRead = 0;
char     toastText[14] = "";
uint32_t toastUntil = 0;

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
void logLine(const char* fmt, ...) {
  char buf[80];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

void showToast(uint32_t now, const char* text) {
  snprintf(toastText, sizeof(toastText), "%s", text);
  toastUntil = now + TOAST_MS;
}

void addLine(uint8_t style, const char* fmt, ...) {
  if (lineCount >= MAX_LINES) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(lines[lineCount].text, LINE_CHARS + 1, fmt, ap);
  va_end(ap);
  lines[lineCount].style = style;
  lineCount++;
}

// Adds text as several rows, wrapping at spaces.
void addWrapped(const char* text, uint8_t indent, uint8_t style) {
  const size_t width = LINE_CHARS - indent;
  const size_t len = strlen(text);
  size_t pos = 0;
  while (pos < len && lineCount < MAX_LINES) {
    size_t chunk = len - pos;
    if (chunk > width) {
      chunk = width;
      size_t k = chunk;
      while (k > 0 && text[pos + k] != ' ') k--;
      if (k > 0) chunk = k;
    }
    snprintf(lines[lineCount].text, LINE_CHARS + 1, "%*s%.*s", (int)indent, "", (int)chunk, text + pos);
    lines[lineCount].style = style;
    lineCount++;
    pos += chunk;
    while (text[pos] == ' ') pos++;
  }
}

void buildReason(const User& u, char* out, size_t n) {
  out[0] = 0;
  for (uint8_t k = 0; k < 2; k++) {
    if (u.issueCount[k]) {
      size_t used = strlen(out);
      snprintf(out + used, n - used, "%s%s x%d", used ? " / " : "", ISSUES[u.exercise][k], (int)u.issueCount[k]);
    }
  }
}

int percentCorrect(uint32_t c, uint32_t i) {
  return (c + i) ? (int)(c * 100UL / (c + i)) : 0;
}

Agg aggregate(const User* v, uint8_t n) {
  Agg a = {};
  a.n = n;
  for (uint8_t i = 0; i < n; i++) {
    if (v[i].exercise == SQUATS) a.sq++; else a.jj++;
    if (v[i].needsAttention) a.attn++;
    if (v[i].lastRep == 'I') a.wrong++;
    a.c += v[i].correct;
    a.w += v[i].incorrect;
  }
  return a;
}

uint8_t diffAgg(const Agg& a, const Agg& b) {
  uint8_t m = 0;
  if (a.n != b.n)         m |= O_ACTIVE;
  if (a.attn != b.attn)   m |= O_ATTN;
  if (a.wrong != b.wrong) m |= O_WRONG;
  if (a.sq != b.sq)       m |= O_SQ;
  if (a.jj != b.jj)       m |= O_JJ;
  if (a.c != b.c)         m |= O_CORR;
  if (a.w != b.w)         m |= O_WRNG;
  if (percentCorrect(a.c, a.w) != percentCorrect(b.c, b.w)) m |= O_AVG;
  return m;
}

uint8_t diffUser(const User& a, const User& b) {
  uint8_t m = 0;
  if (a.correct != b.correct)     m |= F_CORRECT | F_PCT;
  if (a.incorrect != b.incorrect) m |= F_WRONG | F_PCT;
  if (a.lastRep != b.lastRep || a.updated != b.updated) m |= F_LAST;   // any new rep
  if (strcmp(a.lastIssue, b.lastIssue) != 0) m |= F_ISSUE;
  if (a.needsAttention != b.needsAttention ||
      a.issueCount[0] != b.issueCount[0] || a.issueCount[1] != b.issueCount[1]) m |= F_ATTN;
  return m;
}

// ---------- Fake server (stands in for Firebase) ----------
void startUser(User& u, uint8_t idx) {
  memset(&u, 0, sizeof(u));
  snprintf(u.name, sizeof(u.name), "%s", NAMES[idx]);
  u.exercise = (Exercise)random(2);
  u.target   = random(14, 23);
  u.active   = true;
  u.updated  = millis();
  logLine("[sim] %s joined (%s)", u.name, u.exercise == SQUATS ? "squats" : "jumping jacks");
}

void doRep(User& u) {
  bool wrong = random(100) < 35;
  if (wrong) {
    u.incorrect++;
    u.lastRep = 'I';
    uint8_t k = random(2);
    u.issueCount[k]++;
    if (u.exercise == SQUATS && k == 0) {
      snprintf(u.lastIssue, sizeof(u.lastIssue), "go deeper (reached %d deg, aim <= 95)", (int)random(98, 125));
    } else {
      snprintf(u.lastIssue, sizeof(u.lastIssue), "%s", ISSUES[u.exercise][k]);
    }
    logLine("[sim] %s INCORRECT: %s", u.name, u.lastIssue);
  } else {
    u.correct++;
    u.lastRep = 'C';
    u.lastIssue[0] = 0;
    logLine("[sim] %s correct", u.name);
  }
  u.needsAttention = (u.incorrect >= 5);
  u.updated = millis();

  if (u.correct + u.incorrect >= u.target) {
    u.active = false;
    logLine("[sim] %s finished", u.name);
  }
}

void simTick() {
  uint8_t activeN = 0;
  for (uint8_t i = 0; i < MAX_USERS; i++) if (srv[i].active) activeN++;

  // Someone joins when it's quiet, or occasionally at random.
  if (activeN < 2 || (activeN < MAX_USERS && random(100) < 10)) {
    for (uint8_t i = 0; i < MAX_USERS; i++) {
      if (!srv[i].active) { startUser(srv[i], i); return; }
    }
  }
  if (activeN == 0) return;

  uint8_t pick = random(activeN);
  for (uint8_t i = 0; i < MAX_USERS; i++) {
    if (!srv[i].active) continue;
    if (pick == 0) { doRep(srv[i]); return; }
    pick--;
  }
}

// ---------- Page switching ----------
void leavePage() {
  if (page == 0) ovShown = 0;
  else if (page <= viewCount) viewUi[page - 1].shown = 0;
}

// Highlight what changed since you were last on this page.
void enterPage() {
  if (page == 0) {
    ovShown = ovMask;
    ovMask = 0;
    ovUnseen = false;
  } else if (page <= viewCount) {
    UserUi& ui = viewUi[page - 1];
    ui.shown = ui.mask;
    ui.mask = 0;
    ui.unseen = false;
  }
}

void goToPage(uint8_t newPage) {
  leavePage();
  page = newPage;
  enterPage();
}

// ---------- "Download" from the fake server ----------
void refreshView(uint32_t now) {
  User   oldView[MAX_USERS];
  UserUi oldUi[MAX_USERS];
  uint8_t oldCount = viewCount;
  memcpy(oldView, view, sizeof(view));
  memcpy(oldUi, viewUi, sizeof(viewUi));
  Agg oldAgg = aggregate(oldView, oldCount);

  char prevName[14] = "";
  if (page > 0 && page <= oldCount) snprintf(prevName, sizeof(prevName), "%s", oldView[page - 1].name);

  viewCount = 0;
  uint32_t sum = 0;
  for (uint8_t i = 0; i < MAX_USERS; i++) {
    if (!srv[i].active) continue;
    uint8_t k = viewCount++;
    view[k] = srv[i];
    sum += srv[i].correct + srv[i].incorrect;

    int j = -1;
    for (uint8_t q = 0; q < oldCount; q++) {
      if (strcmp(oldView[q].name, view[k].name) == 0) { j = q; break; }
    }
    if (j < 0) {                                   // brand new person: everything is new
      viewUi[k].mask = F_ALL;
      viewUi[k].shown = 0;
      viewUi[k].unseen = true;
    } else {
      viewUi[k] = oldUi[j];
      uint8_t m = diffUser(oldView[j], view[k]);
      bool isCurrent = (prevName[0] != 0 && strcmp(prevName, view[k].name) == 0);
      if (m && !isCurrent) {                       // changes on the page you're on just update quietly
        viewUi[k].mask |= m;
        viewUi[k].unseen = true;
      }
    }
  }

  Agg newAgg = aggregate(view, viewCount);
  uint8_t om = diffAgg(oldAgg, newAgg);
  if (om && page != 0) {
    ovMask |= om;
    if (om & O_STRUCTURAL) ovUnseen = true;
  }

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

  if (sum > lastSumReps) blinkUntil = now + BLINK_MS;   // only used if LED_BLINK_ON_DATA is 1
  lastSumReps = sum;
  lastRefresh = now;
}

// Right button hold: jump to the next person whose last rep was wrong.
void jumpToNextAlert(uint32_t now) {
  uint8_t start = (page == 0) ? 0 : page;       // 0-based index of the person after the current one
  for (uint8_t step = 0; step < viewCount; step++) {
    uint8_t idx = (start + step) % viewCount;
    if (view[idx].lastRep == 'I') {
      goToPage(idx + 1);
      char t[14];
      snprintf(t, sizeof(t), "> %.8s", view[idx].name);
      showToast(now, t);
      return;
    }
  }
  showToast(now, "NO ALERTS");
}

// ---------- LEDs ----------
uint8_t wrongNow() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < viewCount; i++) if (view[i].lastRep == 'I') n++;
  return n;
}

void updateLeds(uint32_t now) {
  bool l1 = false, l2 = false;
  uint8_t wrong = wrongNow();
  if (wrong >= 2)      { l1 = true; l2 = true; }   // several people wrong: both on
  else if (wrong == 1) { l1 = true; }              // one person wrong: one on
#if LED_BLINK_ON_DATA
  else if (viewCount > 0 && now < blinkUntil) { l1 = true; }
#endif
  digitalWrite(PIN_LED1, l1 ? HIGH : LOW);
  digitalWrite(PIN_LED2, l2 ? HIGH : LOW);
}

// ---------- Pot ----------
void readPot(uint32_t now) {
  if (now - lastPotRead < 10) return;
  lastPotRead = now;
  long sum = 0;
  for (uint8_t i = 0; i < 8; i++) sum += analogRead(PIN_POT);
  int raw = (int)(sum / 8);
#if POT_INVERT
  raw = 1023 - raw;
#endif
  if (abs(raw - potAccepted) > POT_DEADBAND) potAccepted = raw;
}

// ---------- Page content ----------
uint8_t ovStyle(uint8_t bit, bool alert) {
  uint8_t s = 0;
  if (ovShown & bit) s |= ST_CHANGED;
  if (alert) s |= ST_ALERT;
  return s;
}

uint8_t usrStyle(const UserUi& ui, uint8_t bit, bool alert) {
  uint8_t s = 0;
  if (ui.shown & bit) s |= ST_CHANGED;
  if (alert) s |= ST_ALERT;
  return s;
}

void buildOverview(uint32_t now) {
  Agg a = aggregate(view, viewCount);
  addLine(ovStyle(O_ACTIVE, false), "Active: %d", (int)a.n);
  if (viewCount == 0) addLine(0, "Waiting...");

  // Roster: one row per person. White row = they updated since you looked.
  for (uint8_t i = 0; i < viewCount; i++) {
    const User& u = view[i];
    uint8_t s = 0;
    if (viewUi[i].unseen) s |= ST_CHANGED;
    if (u.lastRep == 'I') s |= ST_ALERT;
    addLine(s, "%-6.6s %d%%", u.name, percentCorrect(u.correct, u.incorrect));
  }

  addLine(ovStyle(O_ATTN,  a.attn > 0),  "Attn: %d", (int)a.attn);
  addLine(ovStyle(O_WRONG, a.wrong > 0), "Wrong now %d", (int)a.wrong);
  addLine(ovStyle(O_SQ,   false), "Squats: %d", (int)a.sq);
  addLine(ovStyle(O_JJ,   false), "Jacks: %d", (int)a.jj);
  addLine(ovStyle(O_CORR, false), "Right: %d", (int)a.c);
  addLine(ovStyle(O_WRNG, false), "Wrong: %d", (int)a.w);
  addLine(ovStyle(O_AVG,  false), "Avg: %d%%", percentCorrect(a.c, a.w));
  addLine(0, "Refresh %ds", (int)((now - lastRefresh) / 1000));
  addLine(0, "Mode: DEMO");
  addLine(0, "-Controls-");
  addLine(0, "Pot: scroll");
  addLine(0, "L tap: prev");
  addLine(0, "R tap: next");
  addLine(0, "M tap: sync");
  addLine(0, "L hold:");
  addLine(0, " go home");
  addLine(0, "R hold:");
  addLine(0, " next alert");
  addLine(0, "M hold:");
  addLine(0, " freeze");
}

void buildUserPage(uint32_t now, const User& u, const UserUi& ui) {
  bool wrong = (u.lastRep == 'I');
  addLine(0, "%s", u.exercise == SQUATS ? "SQUATS" : "JUMP.JACKS");
  addLine(usrStyle(ui, F_CORRECT, false), "Right: %d", (int)u.correct);
  addLine(usrStyle(ui, F_WRONG,   false), "Wrong: %d", (int)u.incorrect);
  addLine(usrStyle(ui, F_PCT,     false), "Pct: %d%%", percentCorrect(u.correct, u.incorrect));
  addLine(usrStyle(ui, F_LAST, wrong),
          "Last: %s", u.lastRep == 'C' ? "OK" : (wrong ? "WRONG" : "-"));
  if (u.lastIssue[0]) {
    addLine(0, "Issue:");
    addWrapped(u.lastIssue, 1, usrStyle(ui, F_ISSUE, true));
  }
  addLine(usrStyle(ui, F_ATTN, u.needsAttention), "Attn: %s", u.needsAttention ? "YES" : "no");
  if (u.needsAttention) {
    char r[64];
    buildReason(u, r, sizeof(r));
    addWrapped(r, 1, usrStyle(ui, F_ATTN, true));
  }
  addLine(0, "Upd: %ds", (int)((now - u.updated) / 1000));
}

// ---------- Drawing ----------
void drawHeader(const char* left, bool invert, bool badgeNew) {
  // Count other pages with updates you haven't looked at yet.
  uint8_t unseenOthers = 0;
  if (page != 0 && ovUnseen) unseenOthers++;
  for (uint8_t i = 0; i < viewCount; i++) {
    if ((uint8_t)(i + 1) != page && viewUi[i].unseen) unseenOthers++;
  }

  u8g2.setFont(u8g2_font_6x10_tr);
  if (invert) {
    u8g2.drawBox(0, 0, 128, HEADER_H);
    u8g2.setDrawColor(0);
  }
  u8g2.drawStr(1, 8, left);

  char pg[10];
  snprintf(pg, sizeof(pg), "%d/%d", (int)page, (int)viewCount);
  int x = 127 - u8g2.getStrWidth(pg);
  u8g2.drawStr(x, 8, pg);

  if (unseenOthers > 0) {
    char un[6];
    snprintf(un, sizeof(un), "+%d", (int)unseenOthers);
    x -= u8g2.getStrWidth(un) + 5;
    u8g2.drawStr(x, 8, un);
  }

  const char* badge = holdMode ? "HOLD" : (badgeNew ? "NEW" : nullptr);
  if (badge) {
    int w = u8g2.getStrWidth(badge);
    x -= w + 7;
    u8g2.drawFrame(x - 2, 0, w + 4, HEADER_H);
    u8g2.drawStr(x, 8, badge);
  }

  u8g2.setDrawColor(1);
  if (!invert) u8g2.drawHLine(0, HEADER_H, 128);
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

void drawScreen(uint32_t now) {
  lineCount = 0;
  char left[20];
  bool invert, badgeNew;

  if (page == 0 || page > viewCount) {
    page = 0;
    buildOverview(now);
    snprintf(left, sizeof(left), "OVERVIEW");
    invert = (wrongNow() > 0);
    badgeNew = (ovShown != 0);
  } else {
    const User& u = view[page - 1];
    const UserUi& ui = viewUi[page - 1];
    buildUserPage(now, u, ui);
    snprintf(left, sizeof(left), "%.6s %s", u.name, u.exercise == SQUATS ? "SQ" : "JJ");
    invert = (u.lastRep == 'I');
    badgeNew = (ui.shown != 0);
  }

  uint8_t maxOffset = (lineCount > VISIBLE_LINES) ? (lineCount - VISIBLE_LINES) : 0;
  uint8_t offset = (uint8_t)(((uint32_t)potAccepted * (maxOffset + 1)) / 1024);
  if (offset > maxOffset) offset = maxOffset;

  u8g2.clearBuffer();
  drawHeader(left, invert, badgeNew);

  // Body rows
  u8g2.setFont(BODY_FONT);
  for (uint8_t i = 0; i < VISIBLE_LINES; i++) {
    uint8_t idx = offset + i;
    if (idx >= lineCount) break;
    int top = BODY_TOP + i * PITCH;
    bool changed = lines[idx].style & ST_CHANGED;
    bool alert   = lines[idx].style & ST_ALERT;

    if (changed) {
      u8g2.setDrawColor(1);
      u8g2.drawBox(0, top, 124, PITCH);
    }
    u8g2.setDrawColor(changed ? 0 : 1);
    if (alert) u8g2.drawBox(0, top, 3, PITCH);       // left-edge alert bar
    u8g2.drawStr(TEXT_X, top + BASE_OFF, lines[idx].text);
  }
  u8g2.setDrawColor(1);

  // Scroll bar
  if (lineCount > VISIBLE_LINES) {
    const int sbTop = 12, sbH = 48;
    int thumbH = (sbH * VISIBLE_LINES) / lineCount;
    if (thumbH < 4) thumbH = 4;
    int thumbY = sbTop + ((sbH - thumbH) * offset) / maxOffset;
    u8g2.drawVLine(126, sbTop, sbH);
    u8g2.drawBox(125, thumbY, 3, thumbH);
  }

  if (now < toastUntil) drawToast();

  u8g2.sendBuffer();
}

void splash() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_10x20_tr);
  u8g2.drawStr(29, 22, "WORKOUT");
  u8g2.drawStr(29, 46, "MONITOR");
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(28, 61, "demo / LED test");
  u8g2.sendBuffer();
  digitalWrite(PIN_LED1, HIGH); delay(350); digitalWrite(PIN_LED1, LOW);
  digitalWrite(PIN_LED2, HIGH); delay(350); digitalWrite(PIN_LED2, LOW);
  digitalWrite(PIN_LED1, HIGH); digitalWrite(PIN_LED2, HIGH); delay(350);
  digitalWrite(PIN_LED1, LOW);  digitalWrite(PIN_LED2, LOW);
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(115200);              // doesn't wait for a PC; runs fine without one
  pinMode(PIN_BTN_PREV, INPUT_PULLUP);
  pinMode(PIN_BTN_REFRESH, INPUT_PULLUP);
  pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
  pinMode(PIN_LED1, OUTPUT);
  pinMode(PIN_LED2, OUTPUT);
  analogReadResolution(10);
  randomSeed(analogRead(A1));        // A1 left unconnected: noisy, so the demo differs each run

  u8g2.begin();
  u8g2.setBusClock(400000);

  splash();

  potAccepted = analogRead(PIN_POT);
  uint32_t now = millis();
  lastSim = now;
  refreshView(now);
  // Nothing to flag on the very first load.
  ovMask = ovShown = 0;
  ovUnseen = false;
  for (uint8_t i = 0; i < MAX_USERS; i++) viewUi[i] = { 0, 0, false };
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
    refreshView(now);
    showToast(now, "REFRESHED");
  } else if (e == EV_HOLD) {
    holdMode = !holdMode;
    showToast(now, holdMode ? "HOLD ON" : "HOLD OFF");
  }

  // Fake server does something once per second
  if (now - lastSim >= SIM_TICK_MS) {
    lastSim += SIM_TICK_MS;
    simTick();
  }

  // Auto refresh (paused while frozen)
  if (!holdMode && now - lastRefresh >= AUTO_REFRESH_MS) refreshView(now);

  readPot(now);
  updateLeds(now);

  if (now - lastDraw >= 50) {
    lastDraw = now;
    drawScreen(now);
  }
}
