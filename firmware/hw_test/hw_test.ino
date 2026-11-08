// EMBER hardware test — non-blocking, no delay() in loop.
// Prints every 500 ms: HW,gas=<raw>,temp=<t>,hum=<h>,ir=<0/1>,boot=<0/1>
// LEDs blink in turn. Every 3 s: buzzer 2 kHz tone 200 ms (pattern A), then 1 s later plain DC on 200 ms (pattern B).
// Each BOOT press: servo 0 -> 90 -> 0, then detach.
#include <Arduino.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"

#define PIN_GAS    33
#define PIN_DHT     4
#define PIN_IR     23
#define PIN_BOOT    0
#define PIN_LED_G  26
#define PIN_LED_Y  27
#define PIN_LED_R  14
#define PIN_BUZ    25
#define PIN_SERVO  18
#define BUZ_CH      6      // LEDC channel 6 -> timer 3 (servo uses timer 0)

DHT dht(PIN_DHT, DHT11);
Servo valve;

const int LEDS[3] = {PIN_LED_G, PIN_LED_Y, PIN_LED_R};
float tC = NAN, hum = NAN;
uint32_t tTick = 0, tDht = 0, tBuz = 0, tServo = 0;
int ledIdx = 0, buzStep = 0, servoStep = 0;
bool bootPrev = false; uint32_t tBootChg = 0;

void buzToneOn(uint32_t f) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttachChannel(PIN_BUZ, f, 8, BUZ_CH);
  ledcWriteTone(PIN_BUZ, f);
#else
  ledcSetup(BUZ_CH, f, 8);
  ledcAttachPin(PIN_BUZ, BUZ_CH);
  ledcWriteTone(BUZ_CH, f);
#endif
  gpio_set_drive_capability((gpio_num_t)PIN_BUZ, GPIO_DRIVE_CAP_1);
}
void buzToneOff() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIN_BUZ, 0);
  ledcDetach(PIN_BUZ);
#else
  ledcWriteTone(BUZ_CH, 0);
  ledcDetachPin(PIN_BUZ);
#endif
  pinMode(PIN_BUZ, OUTPUT);
  digitalWrite(PIN_BUZ, LOW);
  gpio_set_drive_capability((gpio_num_t)PIN_BUZ, GPIO_DRIVE_CAP_1);
}

int readGas() {
  uint32_t s = 0;
  for (int i = 0; i < 8; i++) s += analogRead(PIN_GAS);
  return s / 8;
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 3; i++) {
    pinMode(LEDS[i], OUTPUT); digitalWrite(LEDS[i], LOW);
    gpio_set_drive_capability((gpio_num_t)LEDS[i], GPIO_DRIVE_CAP_0);
  }
  pinMode(PIN_BUZ, OUTPUT); digitalWrite(PIN_BUZ, LOW);
  gpio_set_drive_capability((gpio_num_t)PIN_BUZ, GPIO_DRIVE_CAP_1);
  pinMode(PIN_IR, INPUT);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  analogReadResolution(12);
  analogRead(PIN_GAS);                         // configures the pin as ADC first
  rtc_gpio_pulldown_en(GPIO_NUM_33);           // unplugged -> ~0
  rtc_gpio_pullup_dis(GPIO_NUM_33);
  dht.begin();
  ESP32PWM::allocateTimer(0);
  valve.setPeriodHertz(50);
  Serial.println("# hw_test start");
}

void loop() {
  uint32_t now = millis();

  // BOOT button, debounced edge
  bool boot = digitalRead(PIN_BOOT) == LOW;
  if (boot != bootPrev && now - tBootChg > 40) {
    bootPrev = boot; tBootChg = now;
    if (boot && servoStep == 0) {
      valve.attach(PIN_SERVO, 500, 2400); valve.write(90);
      servoStep = 1; tServo = now;
      Serial.println("# servo -> 90");
    }
  }
  if (servoStep == 1 && now - tServo >= 800) { valve.write(0); servoStep = 2; tServo = now; Serial.println("# servo -> 0"); }
  if (servoStep == 2 && now - tServo >= 800) { valve.detach(); servoStep = 0; }

  // DHT11 every 2 s
  if (now - tDht >= 2000) {
    tDht = now;
    float t = dht.readTemperature(), h = dht.readHumidity();
    tC = t; hum = h;                           // keep NaN visible in the test
  }

  // Buzzer cycle every 3 s
  uint32_t b = now - tBuz;
  if (buzStep == 0 && b >= 3000) { tBuz = now; buzToneOn(2000); buzStep = 1; Serial.println("# buzzer A: 2 kHz tone"); }
  else if (buzStep == 1 && b >= 200) { buzToneOff(); buzStep = 2; }
  else if (buzStep == 2 && b >= 1200) { digitalWrite(PIN_BUZ, HIGH); buzStep = 3; Serial.println("# buzzer B: plain DC on"); }
  else if (buzStep == 3 && b >= 1400) { digitalWrite(PIN_BUZ, LOW); buzStep = 0; }

  // 500 ms tick: LEDs in turn + HW line
  if (now - tTick >= 500) {
    tTick = now;
    for (int i = 0; i < 3; i++) digitalWrite(LEDS[i], i == ledIdx ? HIGH : LOW);
    ledIdx = (ledIdx + 1) % 3;
    Serial.printf("HW,gas=%d,temp=%.1f,hum=%.1f,ir=%d,boot=%d\n",
                  readGas(), tC, hum, digitalRead(PIN_IR), boot ? 1 : 0);
  }
}
