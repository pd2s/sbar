#if !defined(CONFIG_H)
#define CONFIG_H

#include <stdint.h>

static const char FONT[] = "monospace:size=20";

static const int32_t STATUS_MARGIN_LEFT = 3;
static const int32_t STATUS_MARGIN_RIGHT = 3;
static const uint32_t STATUS_ERROR_TEXT_COLOR = 0xFFFF0000;
static const int32_t STATUS_SEPARATOR_WIDTH = 2;

static const int32_t WORKSPACE_BORDER_WIDTH = 1;
static const int32_t WORKSPACE_MARGIN_LEFT = 5;
static const int32_t WORKSPACE_MARGIN_RIGHT = 5;
static const int32_t WORKSPACE_MARGIN_BOTTOM = 1;
static const int32_t WORKSPACE_MARGIN_TOP = 1;

static const int32_t BINDING_MODE_INDICATOR_BORDER_WIDTH = 1;
static const int32_t BINDING_MODE_INDICATOR_MARGIN_LEFT = 5;
static const int32_t BINDING_MODE_INDICATOR_MARGIN_RIGHT = 5;
static const int32_t BINDING_MODE_INDICATOR_MARGIN_BOTTOM = 1;
static const int32_t BINDING_MODE_INDICATOR_MARGIN_TOP = 1;

#endif // CONFIG_H
