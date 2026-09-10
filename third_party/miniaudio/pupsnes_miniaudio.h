#pragma once

// PupSNES only uses the low-level playback device API. Disabling the null
// backend ensures an unavailable physical output is reported as a failure.
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#define MA_NO_NULL
#include "miniaudio.h"
