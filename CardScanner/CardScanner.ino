/**
 * CardScanner — interactive card registration tool for Keycard Override
 *
 * 1. Open Serial Monitor at 115200 baud (line ending: Newline)
 * 2. Use the menu to scan cards into each category (up to 10 per type)
 * 3. Press P to print the header — copy it into cards.h in the KeycardOverride folder
 */

#include <SPI.h>
#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>

#define PIN_RC522_SS    5
#define PIN_RC522_RST  22
#define MAX_PER_TYPE   10

MFRC522DriverPinSimple ss_pin(PIN_RC522_SS);
MFRC522DriverSPI       driver{ss_pin, SPI, SPISettings(1000000u, MSBFIRST, SPI_MODE0)};
MFRC522                rfid{driver};

// ── storage ─────────────────────────────────────────────────

struct StoredCard { byte uid[10]; byte len; };

const uint8_t NUM_TYPES = 5;

const char* TYPE_NAMES[]  = {
  "Red CLAIM", "Red CAPTURE", "Blue CLAIM", "Blue CAPTURE", "Admin"
};
const char* ARRAY_NAMES[] = {
  "RED_CLAIM_CARDS", "RED_CAPTURE_CARDS", "BLUE_CLAIM_CARDS", "BLUE_CAPTURE_CARDS", "ADMIN_CARDS"
};

StoredCard cards[NUM_TYPES][MAX_PER_TYPE];
uint8_t    cardCount[NUM_TYPES] = {};

enum AppState { MAIN_MENU, SCANNING };
AppState appState = MAIN_MENU;
uint8_t  scanType = 0;

// ── helpers ──────────────────────────────────────────────────

void hr()  { Serial.println(F("───────────────────────────────────────")); }
void dhr() { Serial.println(F("═══════════════════════════════════════")); }

void printUID(const byte* uid, byte len) {
  for (byte i = 0; i < len; i++) {
    if (uid[i] < 0x10) Serial.print('0');
    Serial.print(uid[i], HEX);
    if (i < len - 1) Serial.print(' ');
  }
}

int findCard(const byte* uid, byte len) {
  for (uint8_t t = 0; t < NUM_TYPES; t++)
    for (uint8_t i = 0; i < cardCount[t]; i++)
      if (cards[t][i].len == len && memcmp(cards[t][i].uid, uid, len) == 0)
        return t;
  return -1;
}

// ── screens ──────────────────────────────────────────────────

void showMainMenu() {
  Serial.println();
  dhr();
  Serial.println(F("  KEYCARD OVERRIDE  —  Card Scanner"));
  dhr();
  for (uint8_t i = 0; i < NUM_TYPES; i++) {
    Serial.print(F("  "));
    Serial.print(i + 1);
    Serial.print(F("  "));
    Serial.print(TYPE_NAMES[i]);
    Serial.print(F("  ("));
    Serial.print(cardCount[i]);
    Serial.println(F(" cards)"));
  }
  hr();
  Serial.println(F("  P  Print cards.h"));
  Serial.println(F("  X  Clear all"));
  hr();
  Serial.print(F("  Select: "));
}

void showScanScreen() {
  Serial.println();
  dhr();
  Serial.print(F("  Scanning: "));
  Serial.print(TYPE_NAMES[scanType]);
  Serial.print(F("  ["));
  Serial.print(cardCount[scanType]);
  Serial.print('/');
  Serial.print(MAX_PER_TYPE);
  Serial.println(']');
  dhr();
  if (cardCount[scanType] == 0) {
    Serial.println(F("  No cards stored yet."));
  } else {
    for (uint8_t i = 0; i < cardCount[scanType]; i++) {
      Serial.print(F("  "));
      Serial.print(i + 1);
      Serial.print(F(":  "));
      printUID(cards[scanType][i].uid, cards[scanType][i].len);
      Serial.println();
    }
  }
  hr();
  if (cardCount[scanType] < MAX_PER_TYPE)
    Serial.println(F("  Hold card to reader to add it."));
  else
    Serial.println(F("  Slot full (10/10). Use Z to remove last."));
  Serial.println(F("  Z  Remove last      D  Done"));
  hr();
}

void printHeader() {
  Serial.println();
  dhr();
  Serial.println(F("  cards.h  —  copy everything between the lines"));
  dhr();
  Serial.println();
  Serial.println(F("#pragma once"));
  Serial.println();
  Serial.println(F("struct CardDef { byte uid[10]; byte len; };"));
  Serial.println();

  for (uint8_t t = 0; t < NUM_TYPES; t++) {
    Serial.print(F("const CardDef "));
    Serial.print(ARRAY_NAMES[t]);
    Serial.println(F("[] = {"));
    for (uint8_t i = 0; i < cardCount[t]; i++) {
      Serial.print(F("  {{ "));
      for (byte b = 0; b < cards[t][i].len; b++) {
        Serial.print(F("0x"));
        if (cards[t][i].uid[b] < 0x10) Serial.print('0');
        Serial.print(cards[t][i].uid[b], HEX);
        if (b < cards[t][i].len - 1) Serial.print(',');
      }
      Serial.print(F(" }, "));
      Serial.print(cards[t][i].len);
      Serial.println(F("},"));
    }
    Serial.println(F("};"));
    Serial.println();
  }

  dhr();
  Serial.println(F("  Save as cards.h in the KeycardOverride folder."));
  dhr();
  Serial.println();
  showMainMenu();
}

// ── input ────────────────────────────────────────────────────

void handleInput(char c) {
  c = toupper(c);

  if (c == 'M') {
    appState == MAIN_MENU ? showMainMenu() : showScanScreen();
    return;
  }

  if (appState == MAIN_MENU) {
    if (c >= '1' && c <= '0' + NUM_TYPES) {
      scanType = c - '1';
      appState = SCANNING;
      showScanScreen();
    } else if (c == 'P') {
      printHeader();
    } else if (c == 'X') {
      memset(cardCount, 0, sizeof(cardCount));
      Serial.println(F("\n  All cards cleared."));
      showMainMenu();
    }

  } else {  // SCANNING
    if (c == 'D') {
      appState = MAIN_MENU;
      showMainMenu();
    } else if (c == 'Z') {
      if (cardCount[scanType] > 0) {
        cardCount[scanType]--;
        Serial.println(F("  Last card removed."));
      }
      showScanScreen();
    }
  }
}

// ── card scan ────────────────────────────────────────────────

void handleCard() {
  if (appState != SCANNING) return;

  byte* uid = rfid.uid.uidByte;
  byte  len = rfid.uid.size;

  int existing = findCard(uid, len);
  if (existing >= 0) {
    Serial.print(F("  Already in "));
    Serial.print(TYPE_NAMES[existing]);
    Serial.print(F(": "));
    printUID(uid, len);
    Serial.println();
    return;
  }
  if (cardCount[scanType] >= MAX_PER_TYPE) {
    Serial.println(F("  Slot full — remove a card first (Z)."));
    return;
  }

  memcpy(cards[scanType][cardCount[scanType]].uid, uid, len);
  cards[scanType][cardCount[scanType]].len = len;
  cardCount[scanType]++;

  Serial.print(F("  + Added ["));
  Serial.print(cardCount[scanType]);
  Serial.print(F("]: "));
  printUID(uid, len);
  Serial.println();
}

// ── setup & loop ─────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  SPI.begin();
  pinMode(PIN_RC522_RST, OUTPUT);
  digitalWrite(PIN_RC522_RST, HIGH);
  delay(10);
  rfid.PCD_Init();
  showMainMenu();
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') continue;
    handleInput(c);
  }

  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    handleCard();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    delay(800);
  }
}
