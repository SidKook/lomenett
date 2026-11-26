#pragma once
#include <cstdint>
struct rosc_hw_t { uint32_t randombit; };
static rosc_hw_t rosc_hw_inst = {0};
#define rosc_hw (&rosc_hw_inst)
