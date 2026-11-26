#pragma once
#include <cstring>
#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8
static inline void pico_get_unique_board_id_string(char*b,int n){ for(int i=0;i<n;i++) b[i]= i<n-1?'0':0; }
