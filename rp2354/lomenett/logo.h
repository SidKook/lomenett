#pragma once
// Nulenke 'NL' monogram, single bold stroke, drawn for the 128x32 panel (unit-height box, y down). N first, then L.
#define LOGO_ASPECT 1.2500f
static const float LOGO_PTS[][2] = { {0.0000f,1.0000f}, {0.0000f,0.0600f}, {0.0023f,0.0370f}, {0.0092f,0.0281f}, {0.0206f,0.0332f}, {0.0366f,0.0524f}, {0.7000f,1.0000f}, {0.7000f,0.0000f}, {0.7000f,0.9400f}, {0.7047f,0.9662f}, {0.7188f,0.9850f}, {0.7422f,0.9962f}, {0.7750f,1.0000f}, {1.2500f,1.0000f} };
static const int LOGO_PATHS[][2] = { {0,7}, {7,7} };
#define LOGO_NPATHS 2
static const float LOGO_VIAS[][2] = { {0.0000f,1.0000f}, {0.7000f,0.0000f}, {1.2500f,1.0000f} };
#define LOGO_NVIAS 3
