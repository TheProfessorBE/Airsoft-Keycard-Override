# Keycard Override

ESP32-based RFID objective box for airsoft. Two teams compete to claim and capture a single objective using physical RFID cards.

---

## Photos

![Prop side view — LED strip showing neutral yellow](Photos/WhatsApp%20Image%202026-05-14%20at%2012.38.44.jpeg)

*Ammo can prop with 3D-printed faceplate. LED strip showing neutral (yellow breathing), 6-digit score display on the right.*

![Prop top view — RFID reader, LED strip, and dual score displays](Photos/WhatsApp%20Image%202026-05-14%20at%2014.09.17.jpeg)

*Top view showing the RFID reader zone, LED strip, and separate red/blue score displays.*

---

## Game Flow

1. **Neutral** — objective glows yellow. Either team can act.
2. **Claim** — hold your team's *claim card* to the reader for 5 s. LEDs sweep to your team color. Objective is now claimed by you.
3. **Capture** — hold your team's *capture card* to the reader for 5 s while claimed. A progress bar fills, then **+1 point** is awarded and the objective resets to neutral.
4. **Counter-claim** — while the objective is claimed or being captured by the enemy, hold your team's *claim card* for 5 s to sweep it back to your color.
5. **Abort** — remove the card at any time to cancel the current action and revert to the previous state.
6. **Admin reset** — hold the admin card to reset both scores to 0 and return to neutral.

### LED states

| State | Visual |
|-------|--------|
| Neutral | Yellow breathing |
| Claimed Red | Red breathing |
| Claimed Blue | Blue breathing |
| Capturing | Team-color bar fills left to right |
| Claiming / Counter-claim | LED sweep from old color to new color |
| Post-score | Team color fast-blink for 8 s |
| Menu open | Dim white breathing |

### Scoreboard (6-digit TM1637)

```
RR  ──  BB
```
Red score on the left two digits, blue on the right two, middle two blank.

---

## Hardware

| Component | Part |
|-----------|------|
| Microcontroller | ESP32-WROOM-32 (30-pin) |
| RFID reader | RC522 (SPI) |
| LEDs | WS2812B strip |
| Display | TM1637 6-digit (AKJ7 module) |
| Buzzer | Passive piezo (push-pull driven) |

---

## Wiring

### RC522 RFID reader

| RC522 | ESP32 |
|-------|-------|
| SDA (SS) | D5 |
| SCK | D18 |
| MOSI | D23 |
| MISO | D19 |
| RST | D22 |
| 3.3V | 3.3V |
| GND | GND |

### WS2812B LED strip

| LED | ESP32 |
|-----|-------|
| DIN | D4 |
| 5V | 5V (external supply recommended) |
| GND | GND |

### TM1637 6-digit display

| Display | ESP32 |
|---------|-------|
| CLK | D26 |
| DIO | D27 |
| VCC | 3.3V |
| GND | GND |

### Passive piezo buzzer (push-pull — do **not** connect buzzer to GND)

| Buzzer terminal | ESP32 |
|-----------------|-------|
| + / signal | D32 |
| − / return | D33 |

> The buzzer is driven push-pull between D32 and D33, doubling the voltage swing across the element for maximum volume. Do not connect either buzzer terminal to GND.

---

## Libraries

Install via Arduino Library Manager:

- **RFID_MFRC522v2** by GithubCommunity
- **FastLED** by Daniel Garcia
- **TM1637TinyDisplay** by AKJ7
- **Preferences** — built into ESP32 Arduino core

---

## Files

| File | Purpose |
|------|---------|
| `KeycardOverride/KeycardOverride.ino` | Main firmware |
| `KeycardOverride/cards.h` | Default RFID card UIDs (loaded on first boot only) |

---

## First Boot & NVS

On first boot all settings and card UIDs are copied from `cards.h` into NVS (ESP32 non-volatile storage). After that, `cards.h` is ignored — all changes made through the serial menu are persisted in NVS across power cycles.

To force a reload of `cards.h` defaults, use **Factory Reset** from the serial menu.

---

## Serial Menu

Open Serial Monitor at **115200 baud, Newline** line ending. Send `M` at any time to pause the game and open the config menu.

```
  KEYCARD OVERRIDE — Config
  1  Settings
  2  Cards
  3  Factory reset (reload cards.h defaults)
  0  Resume game
```

### Settings

| # | Setting | Default |
|---|---------|---------|
| 1 | Capture time | 5 s |
| 2 | Claim time | 5 s |
| 3 | Post-score lock | 8 s |
| 4 | LED count | 24 |
| 5 | LED brightness | 100 |
| 6 | Card timeout | 700 ms |
| 7 | Poll interval | 100 ms |

### Cards

Each card type holds up to 10 UIDs. Select a type from the Cards menu, hold a card to the reader to register it. Use **Z** to remove the last card, **D** when done. Changes save to NVS immediately.

| Type | Role |
|------|------|
| Red CLAIM | Starts/retakes claim for red |
| Red CAPTURE | Captures for red (requires red claim) |
| Blue CLAIM | Starts/retakes claim for blue |
| Blue CAPTURE | Captures for blue (requires blue claim) |
| Admin | Resets scores and returns to neutral |

---

## Card UID format (`cards.h`)

```cpp
const CardDef RED_CLAIM_CARDS[] = {
  {{ 0xCA, 0x8A, 0x4F, 0x35 }, 4},
};
```

Each entry is `{ { byte0, byte1, ... }, length }`. UIDs can be 4 or 7 bytes depending on the card.
