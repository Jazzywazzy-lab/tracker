// Workout Monitor: full UI demo (fake data, no WiFi)
// Board:   Arduino UNO R4 Minima
// Library: U8g2 (install "U8g2" by oliver from the Library Manager)
//
// WIRING (same as the plan):
//   OLED   GND->GND, VCC->5V (or 3.3V if your module needs it), SCL->A5, SDA->A4
//   Pot    left->5V, middle->A0, right->GND
//   Left button   D2 -> GND   (previous user)
//   Middle button D3 -> GND   (refresh now)
//   Right button  D4 -> GND   (next user)
//   LED1 D6 -> 220 ohm -> LED -> GND
//   LED2 D7 -> 220 ohm -> LED -> GND
//
// WHAT THE DEMO DOES:
//   Once per second a fake "server" does something: a person joins, does a
//   correct or incorrect rep, or finishes their workout. The screen only
//   updates when it "refreshes" (every 3 s, or when you press the middle
//   button), just like it will with real Firebase data.
//
// CONTROLS:
//   Pot            scroll up/down the current page
//   Right button   next page: Overview -> user 1 -> user 2 -> ... -> Overview
//   Left button    previous page
//   Middle button  refresh now
//
// LEDS:
//   nobody active                    both off
//   everyone fine, new data arrived  LED1 blinks
//   1 person's last rep INCORRECT    LED1 solid
//   2+ people's last rep INCORRECT   both solid

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
#define POT_INVERT 0                       // set to 1 if scrolling feels backwards
const int      POT_DEADBAND     = 6;       // ignore pot jitter smaller than this
const uint32_t SIM_TICK_MS      = 1000;    // fake server does one thing per second
const uint32_t AUTO_REFRESH_MS  = 3000;    // screen refresh interval
const uint32_t DEBOUNCE_MS      = 30;
const uint32_t BLINK_MS         = 120;

// ---------- Layout ----------
const uint8_t LINE_CHARS    = 23;          // characters that fit across the body
const uint8_t MAX_LINES     = 24;
const uint8_t VISIBLE_LINES = 6;           // body lines shown at once

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
  uint16_t issueCount[2];      // how often each issue type happened
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
uint8_t  viewCount = 0;

// ---------- UI state ----------
uint8_t  page = 0;                 // 0 = overview, 1..viewCount = users
uint32_t lastSim = 0;
uint32_t lastRefresh = 0;
uint32_t refreshFlashUntil = 0;
uint32_t blinkUntil = 0;
uint32_t lastSumReps = 0;
uint32_t lastDraw = 0;
int      potAccepted = 0;
uint32_t lastPotRead = 0;

char    lines[MAX_LINES][LINE_CHARS + 1];
uint8_t lineCount = 0;

// ---------- Buttons ----------
struct Button {
  uint8_t  pin;
  bool     stable;
  bool     lastRead;
  uint32_t changedAt;
};
Button btnPrev    = { PIN_BTN_PREV,    HIGH, HIGH, 0 };
Button btnRefresh = { PIN_BTN_REFRESH, HIGH, HIGH, 0 };
Button btnNext    = { PIN_BTN_NEXT,    HIGH, HIGH, 0 };

bool buttonPressed(Button& b, uint32_t now) {
  bool r = digitalRead(b.pin);
  if (r != b.lastRead) { b.lastRead = r; b.changedAt = now; }
  if (now - b.changedAt > DEBOUNCE_MS && r != b.stable) {
    b.stable = r;
    if (r == LOW) return true;     // pressed (pull-up, so LOW = pressed)
  }
  return false;
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

void addLine(const char* fmt, ...) {
  if (lineCount >= MAX_LINES) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(lines[lineCount], LINE_CHARS + 1, fmt, ap);
  va_end(ap);
  lineCount++;
}

// Adds text as several lines, wrapping at spaces.
void addWrapped(const char* text, uint8_t indent) {
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
    snprintf(lines[lineCount], LINE_CHARS + 1, "%*s%.*s", (int)indent, "", (int)chunk, text + pos);
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

// ---------- "Download" from the fake server ----------
void refreshView(uint32_t now) {
  char prevName[14] = "";
  if (page > 0 && page <= viewCount) snprintf(prevName, sizeof(prevName), "%s", view[page - 1].name);

  viewCount = 0;
  uint32_t sum = 0;
  for (uint8_t i = 0; i < MAX_USERS; i++) {
    if (!srv[i].active) continue;
    view[viewCount++] = srv[i];
    sum += srv[i].correct + srv[i].incorrect;
  }

  // Keep following the same person if they're still active.
  if (page > 0) {
    bool found = false;
    for (uint8_t i = 0; i < viewCount; i++) {
      if (strcmp(view[i].name, prevName) == 0) { page = i + 1; found = true; break; }
    }
    if (!found && page > viewCount) page = viewCount;
  }

  if (sum > lastSumReps) blinkUntil = now + BLINK_MS;   // new data arrived
  lastSumReps = sum;
  lastRefresh = now;
  refreshFlashUntil = now + 400;
}

// ---------- LEDs ----------
uint8_t wrongNow() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < viewCount; i++) if (view[i].lastRep == 'I') n++;
  return n;
}

void updateLeds(uint32_t now) {
  bool l1 = false, l2 = false;
  if (viewCount > 0) {
    uint8_t wrong = wrongNow();
    if (wrong >= 2)      { l1 = true; l2 = true; }
    else if (wrong == 1) { l1 = true; }
    else if (now < blinkUntil) { l1 = true; }
  }
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
void buildOverview(uint32_t now) {
  uint8_t squats = 0, jacks = 0, attn = 0;
  uint32_t c = 0, w = 0;
  for (uint8_t i = 0; i < viewCount; i++) {
    if (view[i].exercise == SQUATS) squats++; else jacks++;
    if (view[i].needsAttention) attn++;
    c += view[i].correct;
    w += view[i].incorrect;
  }
  addLine("Active users: %d", (int)viewCount);
  if (viewCount == 0) addLine("Waiting for people...");
  addLine("Need attention: %d", (int)attn);
  addLine("Wrong right now: %d", (int)wrongNow());
  addLine("Squats: %d", (int)squats);
  addLine("Jumping jacks: %d", (int)jacks);
  addLine("Total correct: %d", (int)c);
  addLine("Total wrong: %d", (int)w);
  addLine("Avg correct: %d%%", percentCorrect(c, w));
  addLine("Refreshed: %ds ago", (int)((now - lastRefresh) / 1000));
  addLine("Mode: DEMO (no WiFi)");
  addLine("--- controls ---");
  addLine("Pot: scroll lines");
  addLine("L/R: prev/next user");
  addLine("Mid: refresh now");
}

void buildUserPage(uint32_t now, const User& u) {
  addLine("%s", u.exercise == SQUATS ? "Exercise: SQUATS" : "Exercise: JUMP. JACKS");
  addLine("Correct: %d", (int)u.correct);
  addLine("Wrong: %d", (int)u.incorrect);
  addLine("Percent: %d%%", percentCorrect(u.correct, u.incorrect));
  addLine("Last rep: %s", u.lastRep == 'C' ? "CORRECT" : (u.lastRep == 'I' ? "INCORRECT" : "-"));
  if (u.lastIssue[0]) {
    addLine("Last issue:");
    addWrapped(u.lastIssue, 1);
  }
  addLine("Attention: %s", u.needsAttention ? "YES" : "no");
  if (u.needsAttention) {
    char r[64];
    buildReason(u, r, sizeof(r));
    addWrapped(r, 1);
  }
  addLine("Updated: %ds ago", (int)((now - u.updated) / 1000));
}

// ---------- Drawing ----------
void drawScreen(uint32_t now) {
  lineCount = 0;
  char left[20];
  bool invert;
  if (page == 0 || page > viewCount) {
    page = 0;
    buildOverview(now);
    snprintf(left, sizeof(left), "OVERVIEW");
    invert = (wrongNow() > 0);
  } else {
    const User& u = view[page - 1];
    buildUserPage(now, u);
    snprintf(left, sizeof(left), "%s %s", u.name, u.exercise == SQUATS ? "SQ" : "JJ");
    invert = (u.lastRep == 'I');
  }

  char right[12];
  snprintf(right, sizeof(right), "%s%d/%d", (now < refreshFlashUntil) ? "~" : "", (int)page, (int)viewCount);

  uint8_t maxOffset = (lineCount > VISIBLE_LINES) ? (lineCount - VISIBLE_LINES) : 0;
  uint8_t offset = (uint8_t)(((uint32_t)potAccepted * (maxOffset + 1)) / 1024);
  if (offset > maxOffset) offset = maxOffset;

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x8_tr);

  // Header (inverted while someone is doing it wrong)
  if (invert) {
    u8g2.drawBox(0, 0, 128, 10);
    u8g2.setDrawColor(0);
  }
  u8g2.drawStr(1, 8, left);
  u8g2.drawStr(128 - u8g2.getStrWidth(right) - 1, 8, right);
  u8g2.setDrawColor(1);
  if (!invert) u8g2.drawHLine(0, 10, 128);

  // Body
  for (uint8_t i = 0; i < VISIBLE_LINES; i++) {
    uint8_t idx = offset + i;
    if (idx >= lineCount) break;
    u8g2.drawStr(1, 18 + i * 8, lines[idx]);
  }

  // Scroll bar
  if (lineCount > VISIBLE_LINES) {
    const int top = 12, h = 50;
    int thumbH = (h * VISIBLE_LINES) / lineCount;
    if (thumbH < 4) thumbH = 4;
    int thumbY = top + ((h - thumbH) * offset) / maxOffset;
    u8g2.drawVLine(126, top, h);
    u8g2.drawBox(125, thumbY, 3, thumbH);
  }

  u8g2.sendBuffer();
}

void splash() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(14, 24, "WORKOUT MONITOR");
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(34, 40, "UI demo mode");
  u8g2.drawStr(22, 54, "LED test...");
  u8g2.sendBuffer();
  digitalWrite(PIN_LED1, HIGH); delay(350); digitalWrite(PIN_LED1, LOW);
  digitalWrite(PIN_LED2, HIGH); delay(350); digitalWrite(PIN_LED2, LOW);
  digitalWrite(PIN_LED1, HIGH); digitalWrite(PIN_LED2, HIGH); delay(350);
  digitalWrite(PIN_LED1, LOW);  digitalWrite(PIN_LED2, LOW);
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(115200);              // don't wait for the monitor; runs fine without a PC
  pinMode(PIN_BTN_PREV, INPUT_PULLUP);
  pinMode(PIN_BTN_REFRESH, INPUT_PULLUP);
  pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
  pinMode(PIN_LED1, OUTPUT);
  pinMode(PIN_LED2, OUTPUT);
  analogReadResolution(10);
  randomSeed(analogRead(A1));        // A1 left unconnected: noisy, so a different demo each time

  u8g2.begin();
  u8g2.setBusClock(400000);

  splash();

  potAccepted = analogRead(PIN_POT);
  uint32_t now = millis();
  lastSim = now;
  refreshView(now);
}

void loop() {
  uint32_t now = millis();
  uint8_t pages = viewCount + 1;

  // Buttons
  if (buttonPressed(btnNext, now))    { page = (page + 1) % pages; }
  if (buttonPressed(btnPrev, now))    { page = (page + pages - 1) % pages; }
  if (buttonPressed(btnRefresh, now)) { refreshView(now); }

  // Fake server does something once per second
  if (now - lastSim >= SIM_TICK_MS) {
    lastSim += SIM_TICK_MS;
    simTick();
  }

  // Auto refresh
  if (now - lastRefresh >= AUTO_REFRESH_MS) refreshView(now);

  readPot(now);
  updateLeds(now);

  if (now - lastDraw >= 50) {
    lastDraw = now;
    drawScreen(now);
  }
}
