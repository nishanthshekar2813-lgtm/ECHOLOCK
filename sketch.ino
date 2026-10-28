/*
 ==========================================================
   ECHOLOCK - ACCESSIBLE SMART LOCK (fixed)

   INDEPENDENT ACCESS METHODS (any ONE opens the lock):
     1. RFID card        -> unlock
     2. Normal PIN       -> unlock         (1234#)
     3. Distress PIN     -> unlock + alert (8888#)
     4. Knock pattern    -> unlock         (click, ~2s pause, click-click)

   RED LED = silent distress alert ONLY (stays off on normal unlock)
 ==========================================================
*/

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>

// ---------------- OBJECTS ----------------
LiquidCrystal_I2C display(0x27, 16, 2);
Servo lockServo;

// ---------------- PINS ----------------
#define RFID_SS_PIN    10
#define RFID_RST_PIN   5
#define KNOCK_BTN_PIN  7
#define SERVO_PIN      8
#define BUZZER_PIN     9
#define LED_PIN        A3

// ---------------- RFID ----------------
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);

const byte NUM_USERS = 3;
// These match Wokwi's built-in preset cards (Blue / Green / Yellow)
byte AUTHORIZED_UIDS[NUM_USERS][4] = {
  {0x01, 0x02, 0x03, 0x04},   // Blue card
  {0x11, 0x22, 0x33, 0x44},   // Green card
  {0x55, 0x66, 0x77, 0x88}    // Yellow card
};

// ---------------- KEYPAD ----------------
const byte ROWS = 4;
const byte COLS = 3;
char keys[ROWS][COLS] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};
byte rowPins[ROWS] = {2, 3, 4, 6};
byte colPins[COLS] = {A0, A1, A2};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

const char NORMAL_PIN[]   = "1234";
const char DISTRESS_PIN[] = "8888";
String enteredPIN = "";
unsigned long lastKeyTime = 0;
const unsigned long PIN_IDLE_TIMEOUT = 10000;   // clear half-typed PIN after 10s

// ---------------- LOCK ----------------
const int LOCKED_ANGLE   = 0;
const int UNLOCKED_ANGLE = 90;
const unsigned long AUTO_LOCK_TIME = 5000;

// ---------------- SECURITY ----------------
int failedAttempts = 0;
const int MAX_FAILED_ATTEMPTS = 3;
const unsigned long LOCKOUT_TIME = 30000;
bool lockedOut = false;
unsigned long lockoutStart = 0;

// ---------------- KNOCK PATTERN ----------------
// click ... ~2000ms ... click ... ~300ms ... click
const unsigned long KNOCK_GAP_1 = 2000;
const unsigned long KNOCK_GAP_2 = 300;
const unsigned long KNOCK_TOL_1 = 500;    // gap 1 allowed: 1500-2500 ms
const unsigned long KNOCK_TOL_2 = 250;    // gap 2 allowed:   50-550 ms
const unsigned long KNOCK_DEBOUNCE = 150;
const unsigned long KNOCK_TIMEOUT  = 6000;

// ======================================================
// DISPLAY HELPERS
// ======================================================
void showMessage(const char *line1, const char *line2) {
  display.clear();
  display.setCursor(0, 0);
  display.print(line1);
  display.setCursor(0, 1);
  display.print(line2);
}

void showReady() {
  showMessage("ECHOLOCK READY", "Card/PIN/Knock");
}

// ======================================================
// RFID  -> 0 = no card, 1 = authorized, 2 = unauthorized
// ======================================================
int checkRFID() {
  if (!rfid.PICC_IsNewCardPresent()) return 0;
  if (!rfid.PICC_ReadCardSerial())   return 0;

  bool authorized = false;
  for (byte u = 0; u < NUM_USERS; u++) {
    bool match = true;
    for (byte i = 0; i < 4; i++) {
      if (rfid.uid.uidByte[i] != AUTHORIZED_UIDS[u][i]) {
        match = false;
        break;
      }
    }
    if (match) { authorized = true; break; }
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return authorized ? 1 : 2;
}

// ======================================================
// KEYPAD -> 0 = nothing yet, 1 = normal PIN,
//           2 = distress PIN, 3 = wrong PIN
// ======================================================
int checkKeypad() {
  // Clear a half-typed PIN if nobody finishes it
  if (enteredPIN.length() > 0 && millis() - lastKeyTime > PIN_IDLE_TIMEOUT) {
    enteredPIN = "";
    showReady();
  }

  char key = keypad.getKey();
  if (!key) return 0;

  lastKeyTime = millis();

  if (key == '#') {
    if (enteredPIN.length() == 0) return 0;   // ignore empty submit

    int result = 3;
    if (enteredPIN == NORMAL_PIN)        result = 1;
    else if (enteredPIN == DISTRESS_PIN) result = 2;

    enteredPIN = "";
    return result;
  }

  if (key == '*') {
    enteredPIN = "";
    showMessage("PIN CLEARED", "");
    delay(500);
    showReady();
    return 0;
  }

  // digit
  if (enteredPIN.length() < 8) enteredPIN += key;

  display.clear();
  display.setCursor(0, 0);
  display.print("ENTER PIN:");
  display.setCursor(0, 1);
  for (unsigned int i = 0; i < enteredPIN.length(); i++) display.print("*");

  return 0;
}

// ======================================================
// KNOCK PATTERN (called once the button is already pressed)
// ======================================================
bool checkKnockPattern() {
  unsigned long knockTimes[3];
  int knockCount = 0;
  unsigned long startTime = millis();
  unsigned long lastKnockTime = 0;

  showMessage("KNOCK MODE", "Keep knocking...");

  while (millis() - startTime < KNOCK_TIMEOUT) {
    if (digitalRead(KNOCK_BTN_PIN) == LOW) {
      unsigned long now = millis();

      if (now - lastKnockTime > KNOCK_DEBOUNCE) {
        knockTimes[knockCount] = now;
        knockCount++;
        lastKnockTime = now;
        tone(BUZZER_PIN, 1000, 60);

        display.clear();
        display.setCursor(0, 0);
        display.print("KNOCK ");
        display.print(knockCount);
        display.print("/3");

        if (knockCount == 3) {
          long gap1 = (long)(knockTimes[1] - knockTimes[0]);
          long gap2 = (long)(knockTimes[2] - knockTimes[1]);

          bool ok1 = abs(gap1 - (long)KNOCK_GAP_1) <= (long)KNOCK_TOL_1;
          bool ok2 = abs(gap2 - (long)KNOCK_GAP_2) <= (long)KNOCK_TOL_2;
          return ok1 && ok2;
        }
      }

      // wait for release so one long press isn't counted twice
      while (digitalRead(KNOCK_BTN_PIN) == LOW) delay(5);
    }
    delay(5);
  }
  return false;   // timed out before 3 knocks
}

// ======================================================
// UNLOCK / DENY
// ======================================================
void unlockDoor(bool distress) {
  failedAttempts = 0;

  lockServo.write(UNLOCKED_ANGLE);
  tone(BUZZER_PIN, 1500, 200);

  if (distress) {
    showMessage("ACCESS GRANTED", "(Alert Sent)");
    // flash, then hold the red LED on while the door is open
    for (int i = 0; i < 8; i++) {
      digitalWrite(LED_PIN, HIGH);
      delay(100);
      digitalWrite(LED_PIN, LOW);
      delay(100);
    }
    digitalWrite(LED_PIN, HIGH);
  } else {
    digitalWrite(LED_PIN, LOW);   // normal unlock: alert LED stays OFF
    showMessage("ACCESS GRANTED", "DOOR UNLOCKED");
  }

  delay(AUTO_LOCK_TIME);

  lockServo.write(LOCKED_ANGLE);
  digitalWrite(LED_PIN, LOW);
  showReady();
}

void accessDenied() {
  failedAttempts++;
  digitalWrite(LED_PIN, LOW);
  tone(BUZZER_PIN, 400, 300);

  if (failedAttempts >= MAX_FAILED_ATTEMPTS) {
    lockedOut = true;
    lockoutStart = millis();
    showMessage("TOO MANY FAILS", "LOCKED 30 SEC");
    return;
  }

  showMessage("ACCESS DENIED", "TRY AGAIN");
  delay(1500);
  showReady();
}

// ======================================================
// SETUP
// ======================================================
void setup() {
  Serial.begin(9600);

  SPI.begin();
  rfid.PCD_Init();

  pinMode(KNOCK_BTN_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(BUZZER_PIN, OUTPUT);

  lockServo.attach(SERVO_PIN);
  lockServo.write(LOCKED_ANGLE);

  display.init();
  display.backlight();

  showMessage("ECHOLOCK", "SYSTEM READY");
  delay(1500);
  showReady();
}

// ======================================================
// MAIN LOOP
// ======================================================
void loop() {
  // ---- lockout: ignore everything until time is up ----
  if (lockedOut) {
    if (millis() - lockoutStart >= LOCKOUT_TIME) {
      lockedOut = false;
      failedAttempts = 0;
      showReady();
    } else {
      return;
    }
  }

  // ---- method 1: RFID ----
  int card = checkRFID();
  if (card == 1) {
    showMessage("CARD VERIFIED", "ACCESS GRANTED");
    delay(300);
    unlockDoor(false);
    return;
  }
  if (card == 2) {
    showMessage("UNKNOWN CARD", "");
    accessDenied();
    return;
  }

  // ---- methods 2 & 3: keypad ----
  int pin = checkKeypad();
  if (pin == 1) {
    showMessage("PIN VERIFIED", "ACCESS GRANTED");
    delay(300);
    unlockDoor(false);
    return;
  }
  if (pin == 2) {
    unlockDoor(true);        // silent distress unlock
    return;
  }
  if (pin == 3) {
    accessDenied();
    return;
  }

  // ---- method 4: knock pattern ----
  if (digitalRead(KNOCK_BTN_PIN) == LOW) {
    if (checkKnockPattern()) {
      showMessage("KNOCK VERIFIED", "ACCESS GRANTED");
      delay(300);
      unlockDoor(false);
    } else {
      accessDenied();        // wrong/incomplete knock now counts toward lockout
    }
  }
}