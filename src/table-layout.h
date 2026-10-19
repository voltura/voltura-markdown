#pragma once
#include "document.h"
#include <algorithm>
#include <numeric>

namespace vm {
// Native width hints fit the viewport when possible, preserving minimum cell
// space. Freezing constrained tracks avoids shrinking a narrow column below its
// padding/content minimum when another column has a very large width hint.
inline std::vector<float> tableColumnOffsets(const TableModel &table, float available, float zoom) {
    size_t count = table.columns;
    std::vector<double> minimum(count), desired(count), resolved(count);
    double required = 0;
    for (size_t col = 0; col < count; ++col) {
        minimum[col] = 40 * zoom * (table.tracks ? (*table.tracks)[col].minimum : 1);
        required += minimum[col];
    }
    double width = std::max<double>(available, required), weight = 0;
    std::vector<size_t> order(count);
    std::iota(order.begin(), order.end(), 0);
    size_t automatic = 0;
    double specified = 0;
    for (size_t col = 0; col < count; ++col) {
        auto track = table.tracks ? (*table.tracks)[col] : TableColumn{};
        if (track.pixels || track.percent) {
            desired[col] = std::max({minimum[col], (double)track.pixels * zoom, width * track.percent / 100});
            specified += desired[col];
        } else
            ++automatic;
    }
    double remainder = automatic ? std::max(0., width - specified) / automatic : 0;
    for (size_t col = 0; col < count; ++col) {
        if (!desired[col])
            desired[col] = std::max(minimum[col], remainder);
        weight += desired[col];
    }
    auto ratio = [&](size_t col) { return desired[col] ? minimum[col] / desired[col] : 0; };
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ratio(a) > ratio(b); });
    double remaining = width;
    size_t fixed = 0;
    for (; fixed < count; ++fixed) {
        size_t col = order[fixed];
        if (!weight || remaining * desired[col] / weight >= minimum[col])
            break;
        resolved[col] = minimum[col];
        remaining -= minimum[col];
        weight -= desired[col];
    }
    for (size_t i = fixed; i < count; ++i)
        resolved[order[i]] = weight ? remaining * desired[order[i]] / weight : remaining / (count - fixed);
    std::vector<float> offsets(count + 1, 0);
    double x = 0;
    for (size_t col = 0; col < count; ++col) {
        x += resolved[col];
        offsets[col + 1] = (float)x;
    }
    return offsets;
}

// CSS table height constraints: single-row cells establish minimum row heights;
// a spanning cell must fit within the sum of the rows it covers. CSS does not
// prescribe how an implementation distributes a spanning cell's extra height.
// Equal distribution is deterministic; range updates keep long spans bounded.
inline std::vector<float> tableRowOffsets(const Block &table, const std::vector<float> &natural) {
    if (!table.table)
        return {0, natural.empty() ? 0 : *std::max_element(natural.begin(), natural.end())};
    unsigned count = table.table->rows;
    std::vector<float> heights(count, 0);
    for (size_t i = 0; i < table.cells.size(); ++i) {
        auto &cell = table.cells[i];
        if (!table.table || cell.rows == 1)
            heights[table.table ? cell.row : 0] = std::max(heights[table.table ? cell.row : 0], natural[i]);
    }
    // Range-add / range-sum Fenwick indexes avoid scanning 65,534 rows for
    // every long rowspan. Storage is proportional to rows, never grid area.
    std::vector<double> difference(count + 1), sums(count + 1), weighted(count + 1);
    for (unsigned row = 0; row < count; ++row)
        difference[row] = heights[row] - (row ? heights[row - 1] : 0);
    for (unsigned i = 1; i <= count; ++i) {
        sums[i] += difference[i - 1];
        weighted[i] += difference[i - 1] * (i - 1);
        unsigned parent = i + (i & (~i + 1));
        if (parent <= count) {
            sums[parent] += sums[i];
            weighted[parent] += weighted[i];
        }
    }
    auto prefix = [&](unsigned end) {
        double sum = 0, weight = 0;
        for (unsigned i = end; i; i -= i & (~i + 1)) {
            sum += sums[i];
            weight += weighted[i];
        }
        return end * sum - weight;
    };
    auto update = [&](unsigned at, double amount) {
        difference[at] += amount;
        for (unsigned i = at + 1; i <= count; i += i & (~i + 1)) {
            sums[i] += amount;
            weighted[i] += amount * at;
        }
    };
    if (table.table) {
        for (size_t i = 0; i < table.cells.size(); ++i) {
            auto &cell = table.cells[i];
            if (cell.rows == 1)
                continue;
            double used = prefix(cell.row + cell.rows) - prefix(cell.row);
            double extra = std::max(0., natural[i] - used) / cell.rows;
            if (extra) {
                update(cell.row, extra);
                update(cell.row + cell.rows, -extra);
            }
        }
    }
    std::vector<float> offsets(count + 1, 0);
    double height = 0, total = 0;
    for (unsigned row = 0; row < count; ++row) {
        height += difference[row];
        total += std::max(0., height);
        offsets[row + 1] = (float)total;
    }
    return offsets;
}
} // namespace vm
