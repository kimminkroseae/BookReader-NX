#ifndef WOOK_READER_COMMON_H
#define WOOK_READER_COMMON_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

extern SDL_Window *WINDOW;
extern SDL_Renderer *RENDERER;

// UI fonts (Switch system font when available, Roboto fallback).
extern TTF_Font *FONT_14, *FONT_16, *FONT_18, *FONT_20, *FONT_22, *FONT_25,
    *FONT_28, *FONT_32, *FONT_40;

#endif
