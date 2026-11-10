// Dragino Yun Shield + Arduino UNO R4 Minima: link test
//
// TEST_MODE 1 = Bridge + curl (the real test)
// TEST_MODE 2 = raw UART probe on Serial1 (fallback if Mode 1 won't work)
//
// Open Serial Monitor at 115200 baud (that's the USB port, separate from the shield).
// Optional: LED on D6 through a 220 ohm resistor to GND as a "not hung" indicator.

#define TEST_MODE 1

const int LED_PIN = 6;

#if TEST_MODE == 1

#include <Bridge.h>
#include <Process.h>

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) {}

  pinMode(LED_PIN, OUTPUT);

  Serial.println(F("[1] Bridge.begin() - blocks until the Linux side is up."));
  Serial.println(F("    The shield can take ~60 s to boot after power-on."));
  digitalWrite(LED_PIN, HIGH);   // solid = waiting for Linux
  Bridge.begin();
  digitalWrite(LED_PIN, LOW);
  Serial.println(F("[2] Bridge is up."));
}

void loop() {
  Serial.println(F("[3] curl test to https://example.com ..."));

  Process p;
  p.begin("curl");
  p.addParameter("-s");
  p.addParameter("-m");  p.addParameter("10");          // 10 s timeout
  p.addParameter("-o");  p.addParameter("/dev/null");   // discard body
  p.addParameter("-w");  p.addParameter("%{http_code}"); // print status only
  p.addParameter("https://example.com");
  int rc = p.run();                                      // blocking

  Serial.print(F("    exit code: "));
  Serial.println(rc);
  Serial.print(F("    HTTP status: "));
  while (p.available()) Serial.write(p.read());
  Serial.println();
  // Good result: exit code 0, HTTP status 200.
  // Status 000 = the shield isn't online (check its WiFi/hotspot settings).

  digitalWrite(LED_PIN, HIGH);   // short blink = loop completed
  delay(100);
  digitalWrite(LED_PIN, LOW);
  delay(5000);
}

#else  // TEST_MODE 2: raw UART probe

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) {}

  Serial1.begin(250000);         // Serial1 = D0 (RX) / D1 (TX) on the R4
  Serial.println(F("Listening on Serial1 (D0/D1) at 250000 baud."));
  Serial.println(F("Now power-cycle the shield and watch for ANY bytes."));
}

void loop() {
  while (Serial1.available()) {
    Serial.write(Serial1.read());
  }
}

#endif
