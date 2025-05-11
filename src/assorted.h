
#pragma once

#define DIV_UP(X, Y) ((X + Y - 1) / Y)

// see https://en.wikipedia.org/wiki/ANSI_escape_code#8-bit
#define FG(COLOR) std::format("\33[38:5:{}m", COLOR)
#define BG(COLOR) std::format("\33[48:5:{}m", COLOR)
#define ANSI_RESET "\33[0m"
#define DEFAULT_FG "\33[39m"
#define DEFAULT_BG "\33[49m"
#define HAS_FLAG(BITS, FLAG) ((BITS & FLAG) == FLAG)
