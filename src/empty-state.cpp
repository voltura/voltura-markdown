#include "empty-state.h"

#include <algorithm>
#include <wrl/client.h>

namespace vm {
namespace {
using Microsoft::WRL::ComPtr;

ComPtr<ID2D1LinearGradientBrush> gradient(ID2D1RenderTarget *target, unsigned first, unsigned last,
                                          D2D1_POINT_2F from, D2D1_POINT_2F to) {
    D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(first)}, {1, D2D1::ColorF(last)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> result;
    if (SUCCEEDED(target->CreateGradientStopCollection(stops, 2, &collection)))
        target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to), collection.Get(),
                                          &result);
    return result;
}
} // namespace

void drawEmptyState(ID2D1RenderTarget *target, const D2D1_RECT_F &bounds, bool dark) {
    if (!target || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return;

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0xffffff), &brush)))
        return;
    auto paper = gradient(target, dark ? 0x26354c : 0xffffff, dark ? 0x1a2336 : 0xedf2fc,
                          D2D1::Point2F(32, 20), D2D1::Point2F(134, 162));
    auto accent = gradient(target, 0x428eff, 0x6551ed, D2D1::Point2F(101, 119), D2D1::Point2F(144, 161));
    if (!paper || !accent)
        return;

    ComPtr<ID2D1Factory> factory;
    target->GetFactory(&factory);
    ComPtr<ID2D1PathGeometry> sheet, fold, bolt;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(&sheet)) || FAILED(sheet->Open(&sink)))
        return;
    sink->BeginFigure(D2D1::Point2F(46, 18), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(105, 18));
    sink->AddLine(D2D1::Point2F(136, 49));
    sink->AddLine(D2D1::Point2F(136, 146));
    sink->AddBezier(
        D2D1::BezierSegment(D2D1::Point2F(136, 154), D2D1::Point2F(130, 160), D2D1::Point2F(122, 160)));
    sink->AddLine(D2D1::Point2F(46, 160));
    sink->AddBezier(
        D2D1::BezierSegment(D2D1::Point2F(38, 160), D2D1::Point2F(32, 154), D2D1::Point2F(32, 146)));
    sink->AddLine(D2D1::Point2F(32, 32));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(32, 24), D2D1::Point2F(38, 18), D2D1::Point2F(46, 18)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close()))
        return;
    sink.Reset();
    if (FAILED(factory->CreatePathGeometry(&fold)) || FAILED(fold->Open(&sink)))
        return;
    sink->BeginFigure(D2D1::Point2F(105, 18), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(136, 49));
    sink->AddLine(D2D1::Point2F(118, 49));
    sink->AddBezier(
        D2D1::BezierSegment(D2D1::Point2F(110, 49), D2D1::Point2F(105, 44), D2D1::Point2F(105, 36)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close()))
        return;
    sink.Reset();
    if (FAILED(factory->CreatePathGeometry(&bolt)) || FAILED(bolt->Open(&sink)))
        return;
    sink->BeginFigure(D2D1::Point2F(128, 124), D2D1_FIGURE_BEGIN_FILLED);
    const D2D1_POINT_2F points[] = {{114, 141}, {123, 141}, {117, 154}, {133, 136}, {124, 136}};
    sink->AddLines(points, ARRAYSIZE(points));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close()))
        return;

    // Fixed-size DIP artwork scales down for small windows; it never grows into a splash screen.
    const float scale =
        std::min({1.0f, (bounds.right - bounds.left) / 184.0f, (bounds.bottom - bounds.top) / 200.0f});
    const float x = (bounds.left + bounds.right) * .5f - 84 * scale;
    const float y = (bounds.top + bounds.bottom) * .5f - 90 * scale;
    D2D1_MATRIX_3X2_F saved;
    target->GetTransform(&saved);
    target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(x, y) * saved);
    auto color = [&](unsigned rgb, float alpha = 1) {
        brush->SetColor(D2D1::ColorF(rgb, alpha));
        return brush.Get();
    };

    // Restrained depth, a second sheet, and a folded paper corner keep the motif recognizable.
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(87, 169), 61, 8), color(0x000000, dark ? .14f : .035f));
    auto back = D2D1::RoundedRect(D2D1::RectF(23, 31, 124, 164), 14, 14);
    target->FillRoundedRectangle(back, color(dark ? 0x192230 : 0xe4ebf6));
    target->DrawRoundedRectangle(back, color(dark ? 0x33415a : 0xcdd9ec, .65f), 1);
    target->FillGeometry(sheet.Get(), paper.Get());
    target->DrawGeometry(sheet.Get(), color(dark ? 0x4b6388 : 0xc6d5eb, .62f), 1);
    target->FillGeometry(fold.Get(), color(dark ? 0x354867 : 0xdbe7fc));
    target->DrawLine(D2D1::Point2F(118, 49.5f), D2D1::Point2F(135, 49.5f),
                     color(dark ? 0x52698a : 0xb8cbed, .5f), 1);

    // Abstract typesetting instead of a font-dependent Markdown letter or arrow.
    target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(49, 51, 88, 57), 3, 3),
                                 color(dark ? 0x83adff : 0x4c7bdc));
    const float lengths[] = {67, 60, 67, 43};
    for (unsigned i = 0; i < ARRAYSIZE(lengths); ++i) {
        float lineY = 75 + i * 12.0f;
        target->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(49, lineY, 49 + lengths[i], lineY + 3), 1.5f, 1.5f),
            color(dark ? 0x768aa8 : 0xa4b5cf, i == 3 ? .52f : .72f));
    }
    target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(49, 130, 78, 133), 1.5f, 1.5f),
                                 color(dark ? 0x768aa8 : 0xa4b5cf, .4f));

    auto badge = D2D1::RoundedRect(D2D1::RectF(101, 118, 146, 163), 14, 14);
    target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(99, 116, 148, 165), 16, 16),
                                 color(dark ? 0x101319 : 0xfafbfd));
    target->FillRoundedRectangle(badge, accent.Get());
    target->DrawRoundedRectangle(badge, color(0xadcaff, .35f), 1);
    target->FillGeometry(bolt.Get(), color(0xa4f5ff));
    target->SetTransform(saved);
}
} // namespace vm
