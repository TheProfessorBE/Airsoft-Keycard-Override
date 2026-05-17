/**
 * Keycard Override — ESP32 RFID objective box firmware
 *
 * Hardware:
 *   ESP32-WROOM-32 (30 pin), RC522 RFID (SPI),
 *   WS2812B LED strip, TM1637 6-digit display,
 *   push-pull piezo buzzer, DFPlayer Mini (HardwareSerial2)
 *
 * Card types (12 total):
 *   0  Red CLAIM       — hold → sweep LEDs to red
 *   1  Red CAPTURE     — hold → bar fills → +1 red
 *   2  Blue CLAIM      — hold → sweep LEDs to blue
 *   3  Blue CAPTURE    — hold → bar fills → +1 blue
 *   4  Admin RESET     — tap  → scores reset to 0
 *   5  Admin RED+      — hold 3 s → red score +1
 *   6  Admin RED-      — hold 3 s → red score −1
 *   7  Admin BLUE+     — hold 3 s → blue score +1
 *   8  Admin BLUE-     — hold 3 s → blue score −1
 *   9  Admin DUR-2s    — hold 3 s → game hold duration = 2 s
 *  10  Admin DUR-5s    — hold 3 s → game hold duration = 5 s
 *  11  Admin DUR-8s    — hold 3 s → game hold duration = 8 s
 *
 * Config: send 'M' via Serial Monitor (115200 baud, Newline).
 * First boot loads defaults from cards.h; all changes saved to NVS.
 */

#include <SPI.h>
#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <FastLED.h>
#include <TM1637TinyDisplay6.h>
#include <Preferences.h>
#include <DFRobotDFPlayerMini.h>
#include "audio_library.h"

// ─── Compile-time defaults (first boot / factory reset) ──────
#define DEF_HOLD_SECS         5     // CLAIMING & CAPTURING duration — 2, 5, or 8 only
#define DEF_POST_SCORE_SECS   8
#define DEF_NUM_LEDS         24
#define DEF_BRIGHTNESS      100
#define DEF_CARD_TIMEOUT_MS 700
#define DEF_POLL_MS         100

// ─── Pins ─────────────────────────────────────────────────────
#define PIN_RC522_SS   5
#define PIN_RC522_RST 22
#define PIN_LED        4
#define PIN_DISP_CLK  26
#define PIN_DISP_DIO  27
#define PIN_BUZZER_A  32   // buzzer + terminal
#define PIN_BUZZER_B  33   // buzzer − terminal (push-pull; NOT to GND)
#define PIN_DFP_RX    17   // ESP32 RX ← DFPlayer TX
#define PIN_DFP_TX    16   // ESP32 TX → DFPlayer RX (via 1 kΩ)
#define DFPLAYER_VOL  25   // 0–30

// ─── Capacity ─────────────────────────────────────────────────
#define MAX_LEDS           60
#define MAX_CARDS_PER_TYPE 10
#define NUM_CARD_TYPES     12

// ─── Card type indices ────────────────────────────────────────
#define CT_RED_CLAIM        0
#define CT_RED_CAPTURE      1
#define CT_BLUE_CLAIM       2
#define CT_BLUE_CAPTURE     3
#define CT_ADMIN_RESET      4   // tap  → immediate game reset
#define CT_ADMIN_RED_PLUS   5   // hold 3 s → red score +1
#define CT_ADMIN_RED_MINUS  6   // hold 3 s → red score −1
#define CT_ADMIN_BLUE_PLUS  7   // hold 3 s → blue score +1
#define CT_ADMIN_BLUE_MINUS 8   // hold 3 s → blue score −1
#define CT_ADMIN_DUR_2      9   // hold 3 s → set hold duration to 2 s
#define CT_ADMIN_DUR_5     10   // hold 3 s → set hold duration to 5 s
#define CT_ADMIN_DUR_8     11   // hold 3 s → set hold duration to 8 s

// cards.h defines CardDef struct + default arrays (first-boot values)
#include "cards.h"

// ─── Hardware objects ─────────────────────────────────────────
MFRC522DriverPinSimple ss_pin(PIN_RC522_SS);
MFRC522DriverSPI       rfidDriver{ss_pin, SPI, SPISettings(1000000u, MSBFIRST, SPI_MODE0)};
MFRC522                rfid{rfidDriver};
CRGB                   leds[MAX_LEDS];
TM1637TinyDisplay6     disp(PIN_DISP_CLK, PIN_DISP_DIO);
Preferences            prefs;
HardwareSerial         dfSerial(2);
DFRobotDFPlayerMini    dfPlayer;
AirsoftAudio           audio(dfPlayer);

// ─── Runtime settings ─────────────────────────────────────────
uint8_t  g_hold_secs       = DEF_HOLD_SECS;   // duration for CLAIMING and CAPTURING
uint8_t  g_post_score_secs = DEF_POST_SCORE_SECS;
uint8_t  g_num_leds        = DEF_NUM_LEDS;
uint8_t  g_brightness      = DEF_BRIGHTNESS;
uint16_t g_card_timeout_ms = DEF_CARD_TIMEOUT_MS;
uint16_t g_poll_ms         = DEF_POLL_MS;

// ─── Runtime cards ────────────────────────────────────────────
CardDef  g_cards[NUM_CARD_TYPES][MAX_CARDS_PER_TYPE];
uint8_t  g_card_count[NUM_CARD_TYPES] = {};

const char* TYPE_NAMES[] = {
  "Red CLAIM", "Red CAPTURE", "Blue CLAIM", "Blue CAPTURE",
  "Admin RESET",
  "Admin RED+", "Admin RED-", "Admin BLUE+", "Admin BLUE-",
  "Admin DUR-2s", "Admin DUR-5s", "Admin DUR-8s"
};

// NVS key prefixes — 2 chars each, must be unique
const char* NVS_PREFIXES[] = {
  "rc", "rx", "bc", "bx",          // game cards
  "as", "rp", "rm", "bp", "bm",    // admin tap + score
  "d3", "d6", "d9"                  // admin duration
};

// ─── Colors ───────────────────────────────────────────────────
const CRGB COL_YELLOW = CRGB(255, 160,   0);
const CRGB COL_RED    = CRGB(255,   0,   0);
const CRGB COL_BLUE   = CRGB(  0,   0, 255);
const CRGB COL_PURPLE = CRGB(128,   0, 255);

// ─── Game state ───────────────────────────────────────────────
enum State {
  NEUTRAL, CLAIMED_RED, CLAIMED_BLUE,
  RED_CAPTURING, BLUE_CAPTURING,
  CLAIMING, POST_SCORE,
  ADMIN_HOLD   // processing a hold-type admin card
};

State         gameState        = NEUTRAL;
uint8_t       redScore         = 0;
uint8_t       blueScore        = 0;
uint8_t       lastScorer       = 0;
uint8_t       claimingToTeam   = 0;
uint8_t       claimingFromTeam = 0;
unsigned long phaseStart       = 0;
unsigned long lastCardMillis   = 0;
unsigned long lastPollTime     = 0;
byte          heldUID[10]      = {};
byte          heldUIDLen       = 0;
unsigned long lastCardSeen     = 0;

bool          inMenu           = false;
bool          dfPlayerReady    = false;
unsigned long lastVoiceMs      = 0;

// ─── Admin hold state ─────────────────────────────────────────
State         savedState        = NEUTRAL;
unsigned long savedPhaseStart   = 0;
uint8_t       savedClaimingTo   = 0;
uint8_t       savedClaimingFrom = 0;
byte          savedHeldUID[10]  = {};
byte          savedHeldUIDLen   = 0;
uint8_t       adminCardType     = 0;
bool          adminActionDone   = false;

// ════════════════════════════════════════════════════════════════
//  NVS
// ════════════════════════════════════════════════════════════════

void nvsSaveSettings() {
  prefs.begin("ko", false);
  prefs.putUChar("hld",  g_hold_secs);
  prefs.putUChar("pst",  g_post_score_secs);
  prefs.putUChar("leds", g_num_leds);
  prefs.putUChar("bri",  g_brightness);
  prefs.putUShort("ctmo", g_card_timeout_ms);
  prefs.putUShort("poll", g_poll_ms);
  prefs.end();
}

void nvsLoadSettings() {
  prefs.begin("ko", true);
  g_hold_secs       = prefs.getUChar("hld",  DEF_HOLD_SECS);
  g_post_score_secs = prefs.getUChar("pst",  DEF_POST_SCORE_SECS);
  g_num_leds        = prefs.getUChar("leds", DEF_NUM_LEDS);
  g_brightness      = prefs.getUChar("bri",  DEF_BRIGHTNESS);
  g_card_timeout_ms = prefs.getUShort("ctmo", DEF_CARD_TIMEOUT_MS);
  g_poll_ms         = prefs.getUShort("poll", DEF_POLL_MS);
  prefs.end();
}

void nvsSaveCards() {
  prefs.begin("ko", false);
  char key[8];
  for (uint8_t t = 0; t < NUM_CARD_TYPES; t++) {
    snprintf(key, sizeof(key), "%sn", NVS_PREFIXES[t]);
    prefs.putUChar(key, g_card_count[t]);
    for (uint8_t i = 0; i < g_card_count[t]; i++) {
      snprintf(key, sizeof(key), "%s%du", NVS_PREFIXES[t], i);
      prefs.putBytes(key, g_cards[t][i].uid, g_cards[t][i].len);
      snprintf(key, sizeof(key), "%s%dl", NVS_PREFIXES[t], i);
      prefs.putUChar(key, g_cards[t][i].len);
    }
  }
  prefs.end();
}

void nvsLoadCards() {
  prefs.begin("ko", true);
  char key[8];
  for (uint8_t t = 0; t < NUM_CARD_TYPES; t++) {
    snprintf(key, sizeof(key), "%sn", NVS_PREFIXES[t]);
    g_card_count[t] = prefs.getUChar(key, 0);
    for (uint8_t i = 0; i < g_card_count[t]; i++) {
      snprintf(key, sizeof(key), "%s%du", NVS_PREFIXES[t], i);
      prefs.getBytes(key, g_cards[t][i].uid, 10);
      snprintf(key, sizeof(key), "%s%dl", NVS_PREFIXES[t], i);
      g_cards[t][i].len = prefs.getUChar(key, 4);
    }
  }
  prefs.end();
}

void copyDefaultCards(uint8_t t, const CardDef* src, uint8_t count) {
  uint8_t cnt = count < MAX_CARDS_PER_TYPE ? count : (uint8_t)MAX_CARDS_PER_TYPE;
  g_card_count[t] = cnt;
  for (uint8_t i = 0; i < cnt; i++) {
    memcpy(g_cards[t][i].uid, src[i].uid, src[i].len);
    g_cards[t][i].len = src[i].len;
  }
}

void nvsFactoryReset() {
  g_hold_secs       = DEF_HOLD_SECS;
  g_post_score_secs = DEF_POST_SCORE_SECS;
  g_num_leds        = DEF_NUM_LEDS;
  g_brightness      = DEF_BRIGHTNESS;
  g_card_timeout_ms = DEF_CARD_TIMEOUT_MS;
  g_poll_ms         = DEF_POLL_MS;
  nvsSaveSettings();

  copyDefaultCards(CT_RED_CLAIM,    RED_CLAIM_CARDS,    sizeof(RED_CLAIM_CARDS)    / sizeof(CardDef));
  copyDefaultCards(CT_RED_CAPTURE,  RED_CAPTURE_CARDS,  sizeof(RED_CAPTURE_CARDS)  / sizeof(CardDef));
  copyDefaultCards(CT_BLUE_CLAIM,   BLUE_CLAIM_CARDS,   sizeof(BLUE_CLAIM_CARDS)   / sizeof(CardDef));
  copyDefaultCards(CT_BLUE_CAPTURE, BLUE_CAPTURE_CARDS, sizeof(BLUE_CAPTURE_CARDS) / sizeof(CardDef));
  copyDefaultCards(CT_ADMIN_RESET,  ADMIN_RESET_CARDS,  sizeof(ADMIN_RESET_CARDS)  / sizeof(CardDef));
  for (uint8_t t = CT_ADMIN_RED_PLUS; t < NUM_CARD_TYPES; t++)
    g_card_count[t] = 0;
  nvsSaveCards();

  prefs.begin("ko", false);
  prefs.putBool("init", true);
  prefs.end();

  FastLED.setBrightness(g_brightness);
}

// ════════════════════════════════════════════════════════════════
//  CARD HELPERS
// ════════════════════════════════════════════════════════════════

bool cardMatchAny(uint8_t typeIdx) {
  for (uint8_t i = 0; i < g_card_count[typeIdx]; i++) {
    const CardDef& c = g_cards[typeIdx][i];
    if (rfid.uid.size == c.len && memcmp(rfid.uid.uidByte, c.uid, c.len) == 0)
      return true;
  }
  return false;
}

int findCard(const byte* uid, byte len) {
  for (uint8_t t = 0; t < NUM_CARD_TYPES; t++)
    for (uint8_t i = 0; i < g_card_count[t]; i++)
      if (g_cards[t][i].len == len && memcmp(g_cards[t][i].uid, uid, len) == 0)
        return t;
  return -1;
}

void saveHeld() {
  memcpy(heldUID, rfid.uid.uidByte, rfid.uid.size);
  heldUIDLen   = rfid.uid.size;
  lastCardSeen = millis();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

bool pollHeldCard() {
  byte atqa[2]; byte sz = sizeof(atqa);
  MFRC522::StatusCode r = rfid.PICC_WakeupA(atqa, &sz);
  if (r != MFRC522Constants::STATUS_OK && r != MFRC522Constants::STATUS_COLLISION) return false;
  if (!rfid.PICC_ReadCardSerial()) return false;
  bool match = (rfid.uid.size == heldUIDLen &&
                memcmp(rfid.uid.uidByte, heldUID, heldUIDLen) == 0);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return match;
}

// ════════════════════════════════════════════════════════════════
//  LEDs
// ════════════════════════════════════════════════════════════════

void updateLEDs() {
  unsigned long elapsed = millis() - phaseStart;

  if (inMenu) {
    uint8_t bri = beatsin8(15, 10, 60);
    fill_solid(leds, g_num_leds, CRGB(bri, bri, bri));
    FastLED.show();
    return;
  }

  switch (gameState) {

    case NEUTRAL: {
      uint8_t bri = beatsin8(20, 5, 220);
      fill_solid(leds, g_num_leds, CRGB(
        (uint16_t)COL_YELLOW.r * bri / 255,
        (uint16_t)COL_YELLOW.g * bri / 255,
        0));
      break;
    }

    case CLAIMED_RED: {
      uint8_t bri = beatsin8(30, 5, 220);
      fill_solid(leds, g_num_leds, CRGB(bri, 0, 0));
      break;
    }

    case CLAIMED_BLUE: {
      uint8_t bri = beatsin8(30, 5, 220);
      fill_solid(leds, g_num_leds, CRGB(0, 0, bri));
      break;
    }

    case RED_CAPTURING:
    case BLUE_CAPTURING: {
      CRGB teamCol = (gameState == RED_CAPTURING) ? COL_RED : COL_BLUE;
      uint8_t lit  = (uint8_t)((unsigned long)elapsed * g_num_leds
                               / ((unsigned long)g_hold_secs * 1000));
      if (lit > g_num_leds) lit = g_num_leds;
      uint8_t sparkBri = beatsin8(90, 0, 255);
      for (int i = 0; i < g_num_leds; i++) {
        if (i < lit)        leds[i] = teamCol;
        else if (i == lit)  leds[i] = teamCol.scale8(sparkBri);
        else                leds[i] = CRGB::Black;
      }
      break;
    }

    case CLAIMING: {
      CRGB newCol = (claimingToTeam   == 1) ? COL_RED  : COL_BLUE;
      CRGB oldCol = (claimingFromTeam == 1) ? COL_RED  :
                    (claimingFromTeam == 2) ? COL_BLUE : COL_YELLOW;
      uint8_t lit = (uint8_t)((unsigned long)elapsed * g_num_leds
                              / ((unsigned long)g_hold_secs * 1000));
      if (lit > g_num_leds) lit = g_num_leds;
      for (int i = 0; i < g_num_leds; i++)
        leds[i] = (i < lit) ? newCol : oldCol;
      break;
    }

    case POST_SCORE: {
      CRGB col = (lastScorer == 1) ? COL_RED : COL_BLUE;
      bool on  = (elapsed / 150) % 2 == 0;
      fill_solid(leds, g_num_leds, on ? col : CRGB::Black);
      break;
    }

    case ADMIN_HOLD: {
      // Purple progress bar for 3 s hold; full purple while waiting for card removal
      uint8_t lit = adminActionDone
        ? g_num_leds
        : (uint8_t)((unsigned long)elapsed * g_num_leds / 3000UL);
      if (lit > g_num_leds) lit = g_num_leds;
      for (int i = 0; i < g_num_leds; i++)
        leds[i] = (i < lit) ? COL_PURPLE : CRGB::Black;
      break;
    }
  }

  FastLED.show();
}

// ════════════════════════════════════════════════════════════════
//  DISPLAY
// ════════════════════════════════════════════════════════════════

void updateDisplays() {
  disp.showNumber(redScore,  true, 2, 0);
  disp.showString("  ", 2, 2);
  disp.showNumber(blueScore, true, 2, 4);
}

// ════════════════════════════════════════════════════════════════
//  BUZZER
// ════════════════════════════════════════════════════════════════

#define BEEP_HZ 2000

static void tonePlay(uint32_t ms) {
  const uint32_t half = 500000UL / BEEP_HZ;
  const uint32_t end  = millis() + ms;
  while (millis() < end) {
    digitalWrite(PIN_BUZZER_A, HIGH); digitalWrite(PIN_BUZZER_B, LOW);
    delayMicroseconds(half);
    digitalWrite(PIN_BUZZER_A, LOW);  digitalWrite(PIN_BUZZER_B, HIGH);
    delayMicroseconds(half);
  }
  digitalWrite(PIN_BUZZER_A, LOW); digitalWrite(PIN_BUZZER_B, LOW);
}

void beepCardAccepted() { tonePlay(100); delay(50); tonePlay(100); }
void beepCardRejected() { tonePlay(60); delay(30); tonePlay(60); delay(30); tonePlay(60); }
void beepClaimed()      { tonePlay(80); delay(40); tonePlay(120); delay(40); tonePlay(250); }
void beepScored()       { for (int i=0;i<4;i++){tonePlay(80);delay(30);} tonePlay(400); }
void beepAborted()      { tonePlay(200); delay(40); tonePlay(80); }
void beepReset()        { for (int i=0;i<6;i++){tonePlay(60);delay(30);} }

// ════════════════════════════════════════════════════════════════
//  AUDIO VOICE
// ════════════════════════════════════════════════════════════════

void playStatusVoice() {
  if (!dfPlayerReady) return;
  lastVoiceMs = millis();
  switch (gameState) {
    case NEUTRAL:      audio.playRandom(AudioClip::THE_TERMINAL_IS_NEUTRAL);                    break;
    case CLAIMED_RED:  audio.playRandom(AudioClip::THE_TERMINAL_IS_CLAIMED_BY_THE_RED_TEAM);    break;
    case CLAIMED_BLUE: audio.playRandom(AudioClip::THE_TERMINAL_IS_CLAIMED_BY_THE_BLUE_TEAM);   break;
    case POST_SCORE:
      if (lastScorer == 1) audio.playRandom(AudioClip::THE_TERMINAL_IS_CAPTURED_BY_THE_RED_TEAM);
      else                 audio.playRandom(AudioClip::THE_TERMINAL_IS_CAPTURED_BY_THE_BLUE_TEAM);
      break;
    default: break;
  }
}

void playLeadVoice() {
  if (!dfPlayerReady) return;
  lastVoiceMs = millis();
  switch (gameState) {
    case CLAIMED_RED:
    case RED_CAPTURING:
      audio.playRandom(AudioClip::THE_TERMINAL_IS_CLAIMED_BY_THE_RED_TEAM);  break;
    case CLAIMED_BLUE:
    case BLUE_CAPTURING:
      audio.playRandom(AudioClip::THE_TERMINAL_IS_CLAIMED_BY_THE_BLUE_TEAM); break;
    case NEUTRAL:
      audio.playRandom(AudioClip::THE_TERMINAL_IS_NEUTRAL); break;
    default: break;
  }
}

// ════════════════════════════════════════════════════════════════
//  GAME LOGIC
// ════════════════════════════════════════════════════════════════

// Blocking purple-blink confirmation used after setting hold duration
void blinkDurationConfirm(uint8_t n) {
  for (int r = 0; r < 4; r++) {
    fill_solid(leds, g_num_leds, CRGB::Black);
    for (int i = 0; i < n && i < g_num_leds; i++) leds[i] = COL_PURPLE;
    FastLED.show(); delay(300);
    fill_solid(leds, g_num_leds, CRGB::Black); FastLED.show(); delay(200);
  }
}

void scorePoint(uint8_t team) {
  if (team == 1) redScore++;
  else           blueScore++;
  lastScorer = team;
  heldUIDLen = 0;
  phaseStart = millis();
  gameState  = POST_SCORE;
  beepScored();
  playStatusVoice();
}

void startClaim(uint8_t toTeam) {
  if      (gameState == CLAIMED_RED   || gameState == RED_CAPTURING)  claimingFromTeam = 1;
  else if (gameState == CLAIMED_BLUE  || gameState == BLUE_CAPTURING) claimingFromTeam = 2;
  else                                                                 claimingFromTeam = 0;
  claimingToTeam = toTeam;
  phaseStart     = millis();
  gameState      = CLAIMING;
  saveHeld();
}

void resetGame() {
  redScore   = 0;
  blueScore  = 0;
  lastScorer = 0;
  heldUIDLen = 0;
  phaseStart = millis();
  gameState  = NEUTRAL;
}

void executeAdminHold() {
  switch (adminCardType) {
    case CT_ADMIN_RED_PLUS:
      redScore++;
      Serial.print(F("Admin: red score → ")); Serial.println(redScore);
      beepScored();
      break;
    case CT_ADMIN_RED_MINUS:
      if (redScore > 0) redScore--;
      Serial.print(F("Admin: red score → ")); Serial.println(redScore);
      beepAborted();
      break;
    case CT_ADMIN_BLUE_PLUS:
      blueScore++;
      Serial.print(F("Admin: blue score → ")); Serial.println(blueScore);
      beepScored();
      break;
    case CT_ADMIN_BLUE_MINUS:
      if (blueScore > 0) blueScore--;
      Serial.print(F("Admin: blue score → ")); Serial.println(blueScore);
      beepAborted();
      break;
    case CT_ADMIN_DUR_2:
      g_hold_secs = 2; nvsSaveSettings();
      Serial.println(F("Admin: hold duration → 2 s"));
      blinkDurationConfirm(2);
      break;
    case CT_ADMIN_DUR_5:
      g_hold_secs = 5; nvsSaveSettings();
      Serial.println(F("Admin: hold duration → 5 s"));
      blinkDurationConfirm(5);
      break;
    case CT_ADMIN_DUR_8:
      g_hold_secs = 8; nvsSaveSettings();
      Serial.println(F("Admin: hold duration → 8 s"));
      blinkDurationConfirm(8);
      break;
  }
  updateDisplays();
}

void saveGameState() {
  savedState        = gameState;
  savedPhaseStart   = phaseStart;
  savedClaimingTo   = claimingToTeam;
  savedClaimingFrom = claimingFromTeam;
  memcpy(savedHeldUID, heldUID, sizeof(heldUID));
  savedHeldUIDLen   = heldUIDLen;
}

void restoreGameState() {
  gameState        = savedState;
  phaseStart       = savedPhaseStart;
  claimingToTeam   = savedClaimingTo;
  claimingFromTeam = savedClaimingFrom;
  memcpy(heldUID, savedHeldUID, sizeof(heldUID));
  heldUIDLen       = savedHeldUIDLen;
}

void handleCard() {
  unsigned long now = millis();
  if (now - lastCardMillis < 500) return;
  lastCardMillis = now;

  // Admin RESET — immediate tap, works in any non-hold state
  if (cardMatchAny(CT_ADMIN_RESET)) {
    resetGame(); beepReset(); playStatusVoice(); return;
  }

  // Admin hold cards — save game state, enter ADMIN_HOLD for 3 s
  static const uint8_t adminHoldTypes[] = {
    CT_ADMIN_RED_PLUS, CT_ADMIN_RED_MINUS,
    CT_ADMIN_BLUE_PLUS, CT_ADMIN_BLUE_MINUS,
    CT_ADMIN_DUR_2, CT_ADMIN_DUR_5, CT_ADMIN_DUR_8
  };
  for (uint8_t i = 0; i < 7; i++) {
    if (cardMatchAny(adminHoldTypes[i])) {
      saveGameState();
      adminCardType   = adminHoldTypes[i];
      adminActionDone = false;
      phaseStart      = now;
      gameState       = ADMIN_HOLD;
      saveHeld();
      beepCardAccepted();
      return;
    }
  }

  // Regular game card handling
  bool actionTaken = false;

  switch (gameState) {
    case NEUTRAL:
      if      (cardMatchAny(CT_RED_CLAIM))  { startClaim(1); actionTaken = true; }
      else if (cardMatchAny(CT_BLUE_CLAIM)) { startClaim(2); actionTaken = true; }
      break;

    case CLAIMED_RED:
      if      (cardMatchAny(CT_RED_CAPTURE)) { gameState = RED_CAPTURING;  phaseStart = now; saveHeld(); actionTaken = true; }
      else if (cardMatchAny(CT_BLUE_CLAIM))  { startClaim(2); actionTaken = true; }
      break;

    case CLAIMED_BLUE:
      if      (cardMatchAny(CT_BLUE_CAPTURE)) { gameState = BLUE_CAPTURING; phaseStart = now; saveHeld(); actionTaken = true; }
      else if (cardMatchAny(CT_RED_CLAIM))    { startClaim(1); actionTaken = true; }
      break;

    case RED_CAPTURING:
      if (cardMatchAny(CT_BLUE_CLAIM)) { startClaim(2); actionTaken = true; }
      break;

    case BLUE_CAPTURING:
      if (cardMatchAny(CT_RED_CLAIM)) { startClaim(1); actionTaken = true; }
      break;

    default: break;
  }

  if (actionTaken) {
    beepCardAccepted();
  } else if (findCard(rfid.uid.uidByte, rfid.uid.size) >= 0) {
    beepCardRejected();
  }
}

// ════════════════════════════════════════════════════════════════
//  MENU
// ════════════════════════════════════════════════════════════════

enum MenuState { MS_MAIN, MS_SETTINGS, MS_SETTINGS_EDIT, MS_CARDS, MS_CARDS_SCAN };
MenuState     menuState      = MS_MAIN;
uint8_t       menuParam      = 0;
String        inputBuf       = "";
unsigned long lastMenuCardMs = 0;

void hr()  { Serial.println(F("───────────────────────────────────────")); }
void dhr() { Serial.println(F("═══════════════════════════════════════")); }

void printUID(const byte* uid, byte len) {
  for (byte i = 0; i < len; i++) {
    if (uid[i] < 0x10) Serial.print('0');
    Serial.print(uid[i], HEX);
    if (i < len - 1) Serial.print(' ');
  }
}

void showMainMenu() {
  menuState = MS_MAIN;
  Serial.println();
  dhr();
  Serial.println(F("  KEYCARD OVERRIDE — Config"));
  dhr();
  Serial.println(F("  1  Settings"));
  Serial.println(F("  2  Cards"));
  Serial.println(F("  3  Factory reset (reload cards.h defaults)"));
  Serial.println(F("  0  Resume game"));
  hr();
  Serial.print(F("  > "));
}

void showSettingsMenu() {
  menuState = MS_SETTINGS;
  Serial.println();
  dhr();
  Serial.println(F("  Settings"));
  dhr();
  Serial.print(F("  Hold duration        ")); Serial.print(g_hold_secs);       Serial.println(F(" s  (set by admin card)"));
  Serial.print(F("  1  Post-score lock   ")); Serial.print(g_post_score_secs); Serial.println(F(" s"));
  Serial.print(F("  2  LED count         ")); Serial.println(g_num_leds);
  Serial.print(F("  3  LED brightness    ")); Serial.println(g_brightness);
  Serial.print(F("  4  Card timeout      ")); Serial.print(g_card_timeout_ms); Serial.println(F(" ms"));
  Serial.print(F("  5  Poll interval     ")); Serial.print(g_poll_ms);         Serial.println(F(" ms"));
  Serial.println(F("  0  Back"));
  hr();
  Serial.print(F("  > "));
}

void showCardsMenu() {
  menuState = MS_CARDS;
  inputBuf  = "";
  Serial.println();
  dhr();
  Serial.println(F("  Cards  (enter number + Enter, 0 = back)"));
  dhr();
  for (uint8_t i = 0; i < NUM_CARD_TYPES; i++) {
    Serial.print(F("  "));
    if (i + 1 < 10) Serial.print(' ');
    Serial.print(i + 1);
    Serial.print(F("  "));
    Serial.print(TYPE_NAMES[i]);
    Serial.print(F("  ("));
    Serial.print(g_card_count[i]);
    Serial.println(F(" cards)"));
  }
  Serial.println(F("   0  Back"));
  hr();
  Serial.print(F("  > "));
}

void showScanMenu(uint8_t t) {
  menuState = MS_CARDS_SCAN;
  menuParam = t;
  Serial.println();
  dhr();
  Serial.print(F("  "));
  Serial.print(TYPE_NAMES[t]);
  Serial.print(F("  ["));
  Serial.print(g_card_count[t]);
  Serial.print('/');
  Serial.print(MAX_CARDS_PER_TYPE);
  Serial.println(']');
  dhr();
  if (g_card_count[t] == 0) {
    Serial.println(F("  No cards stored."));
  } else {
    for (uint8_t i = 0; i < g_card_count[t]; i++) {
      Serial.print(F("  "));
      Serial.print(i + 1);
      Serial.print(F(":  "));
      printUID(g_cards[t][i].uid, g_cards[t][i].len);
      Serial.println();
    }
  }
  hr();
  if (g_card_count[t] < MAX_CARDS_PER_TYPE)
    Serial.println(F("  Hold card to reader to add."));
  else
    Serial.println(F("  Slot full. Z to remove last."));
  Serial.println(F("  Z  Remove last      D  Done"));
  hr();
}

void menuHandleInput(char c) {
  c = toupper(c);

  switch (menuState) {

    case MS_MAIN:
      if      (c == '1') showSettingsMenu();
      else if (c == '2') showCardsMenu();
      else if (c == '3') {
        nvsFactoryReset();
        Serial.println(F("\n  Factory reset done."));
        showMainMenu();
      }
      else if (c == '0') {
        inMenu = false;
        Serial.println(F("\n  Game resumed."));
      }
      break;

    case MS_SETTINGS:
      if (c >= '1' && c <= '5') {
        menuParam = c - '0';
        menuState = MS_SETTINGS_EDIT;
        inputBuf  = "";
        while (Serial.available()) Serial.read();
        const char* labels[] = {
          "Post-score secs", "LED count", "LED brightness",
          "Card timeout ms", "Poll interval ms"
        };
        Serial.print(F("\n  "));
        Serial.print(labels[menuParam - 1]);
        Serial.print(F(" = "));
      } else if (c == '0') {
        showMainMenu();
      }
      break;

    case MS_SETTINGS_EDIT:
      if (c == '\n' || c == '\r') {
        if (inputBuf.length() > 0) {
          uint16_t v = (uint16_t)inputBuf.toInt();
          switch (menuParam) {
            case 1: g_post_score_secs = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v); break;
            case 2: g_num_leds        = (uint8_t)(v < 1 ? 1 : v > MAX_LEDS ? MAX_LEDS : v); break;
            case 3: g_brightness      = (uint8_t)(v > 255 ? 255 : v);
                    FastLED.setBrightness(g_brightness); break;
            case 4: g_card_timeout_ms = (v < 100 ? 100 : v > 9999 ? 9999 : v); break;
            case 5: g_poll_ms         = (v < 20  ? 20  : v > 1000 ? 1000 : v); break;
          }
          nvsSaveSettings();
          Serial.println();
          Serial.println(F("  Saved."));
          showSettingsMenu();
        }
      } else if (isDigit(c) && inputBuf.length() < 5) {
        inputBuf += c;
        Serial.print(c);
      }
      break;

    case MS_CARDS:
      if (c == '0' && inputBuf.length() == 0) {
        showMainMenu();
      } else if (isDigit(c) && inputBuf.length() < 2) {
        inputBuf += c;
        Serial.print(c);
      } else if ((c == '\n' || c == '\r') && inputBuf.length() > 0) {
        uint8_t sel = (uint8_t)inputBuf.toInt();
        inputBuf = "";
        if (sel >= 1 && sel <= NUM_CARD_TYPES) {
          showScanMenu(sel - 1);
        } else {
          Serial.println(F("\n  Invalid — enter 1–12."));
          showCardsMenu();
        }
      }
      break;

    case MS_CARDS_SCAN:
      if (c == 'D') {
        showCardsMenu();
      } else if (c == 'Z') {
        if (g_card_count[menuParam] > 0) {
          g_card_count[menuParam]--;
          nvsSaveCards();
          Serial.println(F("  Removed."));
        }
        showScanMenu(menuParam);
      }
      break;
  }
}

void menuHandleCard() {
  if (menuState != MS_CARDS_SCAN) return;
  unsigned long now = millis();
  if (now - lastMenuCardMs < 800) return;
  lastMenuCardMs = now;

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
  if (g_card_count[menuParam] >= MAX_CARDS_PER_TYPE) {
    Serial.println(F("  Slot full — Z to remove."));
    return;
  }
  memcpy(g_cards[menuParam][g_card_count[menuParam]].uid, uid, len);
  g_cards[menuParam][g_card_count[menuParam]].len = len;
  g_card_count[menuParam]++;
  nvsSaveCards();

  Serial.print(F("  + ["));
  Serial.print(g_card_count[menuParam]);
  Serial.print(F("]  "));
  printUID(uid, len);
  Serial.println();
}

// ════════════════════════════════════════════════════════════════
//  SETUP & LOOP
// ════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);

  dfSerial.begin(9600, SERIAL_8N1, PIN_DFP_RX, PIN_DFP_TX);
  delay(1000);
  if (dfPlayer.begin(dfSerial, false)) {
    dfPlayer.volume(DFPLAYER_VOL);
    dfPlayer.outputDevice(DFPLAYER_DEVICE_SD);
    dfPlayerReady = true;
    Serial.println(F("DFPlayer OK."));
  } else {
    Serial.println(F("DFPlayer not found — audio disabled."));
  }

  SPI.begin();
  pinMode(PIN_RC522_RST, OUTPUT);
  pinMode(PIN_BUZZER_A, OUTPUT);
  pinMode(PIN_BUZZER_B, OUTPUT);
  digitalWrite(PIN_BUZZER_A, LOW);
  digitalWrite(PIN_BUZZER_B, LOW);
  digitalWrite(PIN_RC522_RST, HIGH);
  delay(10);
  rfid.PCD_Init();

  FastLED.addLeds<WS2812, PIN_LED, GRB>(leds, MAX_LEDS);

  disp.begin();
  disp.setBrightness(BRIGHT_HIGH);

  prefs.begin("ko", true);
  bool initialized = prefs.getBool("init", false);
  prefs.end();

  if (!initialized) {
    Serial.println(F("First boot — loading defaults from cards.h"));
    nvsFactoryReset();
  } else {
    nvsLoadSettings();
    nvsLoadCards();
  }

  FastLED.setBrightness(g_brightness);

  Serial.println(F("Hardware test..."));
  disp.showNumber(888888);
  fill_solid(leds, g_num_leds, CRGB::Red);   FastLED.show(); delay(500);
  fill_solid(leds, g_num_leds, CRGB::Green); FastLED.show(); delay(500);
  fill_solid(leds, g_num_leds, CRGB::Blue);  FastLED.show(); delay(500);
  fill_solid(leds, g_num_leds, CRGB::Black); FastLED.show();
  disp.clear();
  Serial.println(F("Ready. Send 'M' to open config menu."));

  resetGame();
}

void loop() {
  unsigned long now = millis();

  // ── Serial input ──────────────────────────────────────────
  while (Serial.available()) {
    char c = Serial.read();
    if (inMenu) {
      menuHandleInput(c);
    } else if (c == 'M' || c == 'm') {
      inMenu = true;
      showMainMenu();
    }
  }

  // ── Menu mode ─────────────────────────────────────────────
  if (inMenu) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      menuHandleCard();
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
    updateLEDs();
    return;
  }

  // ── Game mode ─────────────────────────────────────────────
  bool holdState = (gameState == CLAIMING      ||
                    gameState == RED_CAPTURING  ||
                    gameState == BLUE_CAPTURING);

  // Admin hold: poll admin card; after 3 s execute action; wait for card to leave
  if (gameState == ADMIN_HOLD && now - lastPollTime >= g_poll_ms) {
    lastPollTime = now;
    bool here = pollHeldCard();
    if (adminActionDone) {
      if (here) {
        lastCardSeen = now;
      } else if (now - lastCardSeen > g_card_timeout_ms) {
        restoreGameState();  // card is gone — resume game
      }
    } else {
      if (here) {
        lastCardSeen = now;
        if (now - phaseStart >= 3000UL) {
          executeAdminHold();
          adminActionDone = true;
          lastCardSeen    = now;
        }
      } else if (now - lastCardSeen > g_card_timeout_ms) {
        restoreGameState();  // card removed before 3 s — cancel
        beepAborted();
      }
    }
  }

  // Team card hold tracking (CLAIMING / CAPTURING)
  if (holdState && now - lastPollTime >= g_poll_ms) {
    lastPollTime = now;
    if (pollHeldCard()) {
      lastCardSeen = now;
    } else if (now - lastCardSeen > g_card_timeout_ms) {
      if      (gameState == CLAIMING && claimingFromTeam == 1) gameState = CLAIMED_RED;
      else if (gameState == CLAIMING && claimingFromTeam == 2) gameState = CLAIMED_BLUE;
      else if (gameState == CLAIMING)                          gameState = NEUTRAL;
      else if (gameState == RED_CAPTURING)                     gameState = CLAIMED_RED;
      else                                                     gameState = CLAIMED_BLUE;
      phaseStart = now;
      heldUIDLen = 0;
      beepAborted();
      playStatusVoice();
    }
  }

  // Claim complete → claimed
  if (gameState == CLAIMING &&
      now - phaseStart >= (unsigned long)g_hold_secs * 1000) {
    gameState  = (claimingToTeam == 1) ? CLAIMED_RED : CLAIMED_BLUE;
    phaseStart = now;
    heldUIDLen = 0;
    beepClaimed();
    playStatusVoice();
  }

  // Capture complete → score
  if ((gameState == RED_CAPTURING || gameState == BLUE_CAPTURING) &&
      now - phaseStart >= (unsigned long)g_hold_secs * 1000)
    scorePoint(gameState == RED_CAPTURING ? 1 : 2);

  // Post-score → neutral
  if (gameState == POST_SCORE &&
      now - phaseStart >= (unsigned long)g_post_score_secs * 1000) {
    gameState   = NEUTRAL;
    phaseStart  = now;
    lastVoiceMs = now;
  }

  // New card scan (not while any card is being held)
  if (!holdState && gameState != ADMIN_HOLD) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      handleCard();
      if (heldUIDLen == 0) {
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
      }
    }
  }

  // Periodic status announcement every 30 s
  if (!inMenu && now - lastVoiceMs >= 30000UL)
    playLeadVoice();

  updateLEDs();
  updateDisplays();
}
