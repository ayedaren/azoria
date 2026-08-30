#pragma once

// Reuse VIEWE's LCD and backlight definition. The bundled generic FT5x06
// initializer aborts Board::begin() on this FT6336U revision, so the controller
// bus is initialized separately with VIEWE's published four-register sequence.
#include "board/supported/viewe/BOARD_VIEWE_UEDX48480040E_WB_A.h"

#undef ESP_PANEL_BOARD_USE_TOUCH
#define ESP_PANEL_BOARD_USE_TOUCH (0)

#undef ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_MAJOR
#undef ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_MINOR
#undef ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_PATCH
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_MAJOR 1
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_MINOR 4
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_PATCH 0
