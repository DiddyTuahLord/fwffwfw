#pragma once

// Keep <windows.h> (pulled in by the SDK) from defining min/max macros.
// GDI+ expects unqualified min/max, so it gets the std:: versions below.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <SDK/foobar2000.h>

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

namespace Gdiplus {
    using std::min;
    using std::max;
}
#include <gdiplus.h>
