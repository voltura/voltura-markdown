#pragma once
#include "document.h"
#include <algorithm>
#include <utility>

namespace vm {
// HTML dimensions are logical pixels. A missing dimension follows the source aspect ratio.
inline std::pair<float, float> imageDisplaySize(const Block &block, unsigned intrinsicWidth,
                                                unsigned intrinsicHeight, float available, float zoom) {
    if (!intrinsicWidth || !intrinsicHeight)
        return {0.f, 0.f};
    float w = intrinsicWidth * zoom, h = intrinsicHeight * zoom;
    if (block.image && block.image->width)
        w = block.image->width * zoom;
    if (block.image && block.image->height)
        h = block.image->height * zoom;
    if (block.image && block.image->width && !block.image->height)
        h = w * intrinsicHeight / intrinsicWidth;
    else if (block.image && block.image->height && !block.image->width)
        w = h * intrinsicWidth / intrinsicHeight;
    float scale = std::min(1.f, available / w);
    return {w * scale, h * scale};
}
} // namespace vm
