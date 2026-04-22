#pragma once

// Joke strings displayed on line 1 of the LCD during the idle state.
// They cycle every JOKE_INTERVAL_MS milliseconds.
// Anton: add/edit entries here before demo. Keep each string <= 16 chars
// so it fits on a single LCD line without truncation.

static const char *JOKE_STRINGS[] = {
    "Place ur bets!  ",
    "House always wins",
    "Steve is shaky",
    "Bob is ready",
    "Who will triumph?",
    "May odds favor u ",
    "Speed is fake",
    "Vroom vroom...  ",
};

static const int JOKE_STRINGS_COUNT =
    sizeof(JOKE_STRINGS) / sizeof(JOKE_STRINGS[0]);
