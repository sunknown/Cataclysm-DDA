#pragma once
#ifndef CATA_SRC_MP_MP_IMGUI_SERVER_H
#define CATA_SRC_MP_MP_IMGUI_SERVER_H

#include <cstdint>

// The server's third imtui blitter. The game renders imgui through the imtui
// text backend, which draws into ImTui::TScreen; this module puts that screen
// into the server grid so the connected client sees every imgui window (craft,
// inventory, popups) instead of an empty frame. Declared unconditionally so
// this header compiles in every build; the definitions live in
// mp_imgui_server.cpp under MP_SERVER only.

void mp_imgui_draw_screen();

void mp_test_fill_imtui_screen( int w, int h, char ch, uint8_t fg, uint8_t bg );

#endif // CATA_SRC_MP_MP_IMGUI_SERVER_H
