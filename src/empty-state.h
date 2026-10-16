#pragma once

#include <d2d1.h>

namespace vm {
void drawEmptyState(ID2D1RenderTarget *target, const D2D1_RECT_F &bounds, bool dark);
}
