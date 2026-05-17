#pragma once

struct CardDef { byte uid[10]; byte len; };

const CardDef RED_CLAIM_CARDS[] = {
  {{ 0xCA,0x8A,0x4F,0x35 }, 4},
};

const CardDef RED_CAPTURE_CARDS[] = {
  {{ 0x3A,0xC7,0x4C,0x35 }, 4},
};

const CardDef BLUE_CLAIM_CARDS[] = {
  {{ 0x97,0xC6,0x7A,0x33 }, 4},
};

const CardDef BLUE_CAPTURE_CARDS[] = {
  {{ 0x3A,0x65,0x0B,0x35 }, 4},
};

// Was ADMIN_CARDS — now only the reset card lives here.
// Score and duration admin cards have no defaults; teach-in via serial menu.
const CardDef ADMIN_RESET_CARDS[] = {
  {{ 0xEA,0x0E,0xCA,0x35 }, 4},
};
