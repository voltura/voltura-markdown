#pragma once
// NanoSVG's private parser API is used only here, after its implementation is
// included by images.cpp. Convert simple SVG text to ordinary vector paths so
// fonts, transforms, paint order and alpha use the existing bounded rasterizer.
#include "document.h"
#include <d2d1.h>
#include <dwrite.h>
#include <string_view>

namespace vm {
namespace {
void svgTextRequire(bool ok) {
    if (!ok)
        throw std::runtime_error("SVG text unavailable");
}
class SvgGlyphPaths final : public IDWriteTextRenderer, public IDWriteGeometrySink {
    NSVGparser *parser;
    size_t &points;
    float x, y, stretch, baseline, glyphX = 0, glyphY = 0;
    bool failed = false;
    D2D1_POINT_2F point(D2D1_POINT_2F p) {
        if (++points > 200000 || !std::isfinite(p.x) || !std::isfinite(p.y))
            failed = true;
        return {x + (glyphX + p.x) * stretch, y + glyphY - baseline + p.y};
    }

  public:
    SvgGlyphPaths(NSVGparser *p, size_t &count, float x, float y, float stretch, float baseline)
        : parser(p), points(count), x(x), y(y), stretch(stretch), baseline(baseline) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteTextRenderer) ||
            iid == __uuidof(IDWritePixelSnapping))
            *out = static_cast<IDWriteTextRenderer *>(this);
        else if (iid == __uuidof(IDWriteGeometrySink))
            *out = static_cast<IDWriteGeometrySink *>(this);
        else
            return E_NOINTERFACE;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return 1;
    }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void *, BOOL *disabled) override {
        *disabled = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void *, DWRITE_MATRIX *matrix) override {
        *matrix = {1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *value) override {
        *value = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void *, FLOAT bx, FLOAT by, DWRITE_MEASURING_MODE,
                                           const DWRITE_GLYPH_RUN *run, const DWRITE_GLYPH_RUN_DESCRIPTION *,
                                           IUnknown *) override {
        glyphX = bx;
        glyphY = by;
        return run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances,
                                                 run->glyphOffsets, run->glyphCount, run->isSideways,
                                                 run->bidiLevel % 2, this);
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void *, FLOAT, FLOAT, const DWRITE_UNDERLINE *,
                                            IUnknown *) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void *, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH *,
                                                IUnknown *) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void *, FLOAT, FLOAT, IDWriteInlineObject *, BOOL, BOOL,
                                               IUnknown *) override {
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE) override {}
    void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) override {}
    void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F p, D2D1_FIGURE_BEGIN) override {
        p = point(p);
        if (!failed) {
            nsvg__resetPath(parser);
            nsvg__moveTo(parser, p.x, p.y);
        }
    }
    void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F *p, UINT32 count) override {
        for (UINT32 i = 0; i < count; ++i) {
            auto v = point(p[i]);
            if (!failed)
                nsvg__lineTo(parser, v.x, v.y);
        }
    }
    void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT *p, UINT32 count) override {
        for (UINT32 i = 0; i < count; ++i) {
            auto a = point(p[i].point1), b = point(p[i].point2), c = point(p[i].point3);
            if (!failed)
                nsvg__cubicBezTo(parser, a.x, a.y, b.x, b.y, c.x, c.y);
        }
    }
    void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END end) override {
        if (!failed)
            nsvg__addPath(parser, end == D2D1_FIGURE_END_CLOSED);
    }
    HRESULT STDMETHODCALLTYPE Close() override {
        return failed ? E_FAIL : S_OK;
    }
};

struct SvgTextParser {
    NSVGparser *parser;
    ComPtr<IDWriteFactory> write;
    ComPtr<IDWriteFontCollection> fonts;
    struct Font {
        std::wstring family = L"Arial";
        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
        DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
        float anchor = 0;
    };
    std::vector<Font> stack{Font{}};
    unsigned skip = 0;
    bool inText = false;
    float x = 0, y = 0, textLength = 0;
    size_t characters = 0, points = 0;
    std::string content;
    static std::string_view trim(std::string_view value) {
        auto first = value.find_first_not_of(" \t\r\n\"'");
        if (first == value.npos)
            return {};
        return value.substr(first, value.find_last_not_of(" \t\r\n\"'") - first + 1);
    }
    void initializeFonts() {
        if (write)
            return;
        svgTextRequire(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
                                                     reinterpret_cast<IUnknown **>(write.GetAddressOf()))));
        svgTextRequire(SUCCEEDED(write->GetSystemFontCollection(&fonts)));
    }
    void property(std::string_view name, std::string_view value) {
        name = trim(name);
        value = trim(value);
        auto &font = stack.back();
        if (name == "text-anchor")
            font.anchor = value == "middle" ? .5f : value == "end" ? 1.f : 0.f;
        else if (name == "font-weight") {
            auto number = atoi(std::string(value).c_str());
            font.weight = value == "bold" ? DWRITE_FONT_WEIGHT_BOLD
                                          : (number >= 1 && number <= 999 ? (DWRITE_FONT_WEIGHT)number
                                                                          : DWRITE_FONT_WEIGHT_NORMAL);
        } else if (name == "font-style")
            font.style = value == "italic" ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
        else if (name == "font-family") {
            initializeFonts();
            while (!value.empty()) {
                auto comma = value.find(',');
                auto family = fromUtf8(trim(value.substr(0, comma)));
                if (family == L"sans-serif")
                    family = L"Arial";
                if (family == L"serif")
                    family = L"Times New Roman";
                if (family == L"monospace")
                    family = L"Consolas";
                UINT32 index = 0;
                BOOL found = FALSE;
                if (SUCCEEDED(fonts->FindFamilyName(family.c_str(), &index, &found)) && found) {
                    font.family = std::move(family);
                    break;
                }
                if (comma == value.npos)
                    break;
                value.remove_prefix(comma + 1);
            }
        }
    }
    void attributes(const char **attr) {
        for (int i = 0; attr[i]; i += 2)
            if (std::string_view(attr[i]) != "style")
                property(attr[i], attr[i + 1]);
        for (int i = 0; attr[i]; i += 2) {
            if (std::string_view(attr[i]) != "style")
                continue;
            std::string_view style(attr[i + 1]);
            while (!style.empty()) {
                auto semi = style.find(';');
                auto item = style.substr(0, semi);
                auto colon = item.find(':');
                if (colon != item.npos)
                    property(item.substr(0, colon), item.substr(colon + 1));
                if (semi == style.npos)
                    break;
                style.remove_prefix(semi + 1);
            }
        }
    }
    void renderText() {
        if (content.empty())
            return;
        characters += content.size();
        svgTextRequire(content.size() <= 4096 && characters <= 65536);
        auto text = entity(fromUtf8(content));
        auto size = nsvg__getAttr(parser)->fontSize;
        if (!size)
            size = 16;
        svgTextRequire(std::isfinite(size) && size > 0 && size <= 4096 && std::isfinite(x) &&
                       std::isfinite(y));
        initializeFonts();
        auto &font = stack.back();
        ComPtr<IDWriteTextFormat> format;
        svgTextRequire(
            SUCCEEDED(write->CreateTextFormat(font.family.c_str(), fonts.Get(), font.weight, font.style,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"", &format)));
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteTextLayout> layout;
        svgTextRequire(SUCCEEDED(write->CreateTextLayout(text.data(), (UINT32)text.size(), format.Get(),
                                                         10000000, 10000000, &layout)));
        DWRITE_TEXT_METRICS metrics{};
        DWRITE_LINE_METRICS line{};
        UINT32 lines = 0;
        svgTextRequire(SUCCEEDED(layout->GetMetrics(&metrics)) &&
                       SUCCEEDED(layout->GetLineMetrics(&line, 1, &lines)));
        float width = metrics.widthIncludingTrailingWhitespace;
        float stretch = textLength > 0 && width > 0 ? textLength / width : 1;
        svgTextRequire(std::isfinite(stretch) && stretch > 0 && stretch <= 1000);
        SvgGlyphPaths paths(parser, points, x - width * stretch * font.anchor, y, stretch, line.baseline);
        svgTextRequire(SUCCEEDED(layout->Draw(nullptr, &paths, 0, 0)) && SUCCEEDED(paths.Close()));
        nsvg__addShape(parser);
        content.clear();
    }
    void embeddedImage(const char **attr) {
        std::string_view data;
        float left = 0, top = 0, width = 0, height = 0;
        for (int i = 0; attr[i]; i += 2) {
            std::string_view key(attr[i]);
            if (key == "href" || key == "xlink:href")
                data = attr[i + 1];
            if (key == "x")
                left = (float)nsvg__atof(attr[i + 1]);
            if (key == "y")
                top = (float)nsvg__atof(attr[i + 1]);
            if (key == "width")
                width = (float)nsvg__atof(attr[i + 1]);
            if (key == "height")
                height = (float)nsvg__atof(attr[i + 1]);
        }
        constexpr std::string_view prefix = "data:image/svg+xml;base64,";
        if (!data.starts_with(prefix) || data.size() > 65536 || width <= 0 || height <= 0)
            return;
        data.remove_prefix(prefix.size());
        std::string decoded;
        unsigned bits = 0, count = 0;
        for (char c : data) {
            if (c == '=')
                break;
            unsigned value = c >= 'A' && c <= 'Z'   ? c - 'A'
                             : c >= 'a' && c <= 'z' ? c - 'a' + 26
                             : c >= '0' && c <= '9' ? c - '0' + 52
                             : c == '+'             ? 62
                             : c == '/'             ? 63
                                                    : 64;
            if (value == 64)
                return;
            bits = (bits << 6) | value;
            count += 6;
            if (count >= 8) {
                count -= 8;
                decoded.push_back((char)(bits >> count));
            }
        }
        // Embedded icons are restricted to NanoSVG's existing vector subset;
        // nested image resources are ignored and never fetched or opened.
        std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> icon(nsvgParse(decoded.data(), "px", 96),
                                                               nsvgDelete);
        if (!icon || icon->width <= 0 || icon->height <= 0)
            return;
        float scale = std::min(width / icon->width, height / icon->height);
        svgTextRequire(std::isfinite(scale) && scale > 0 && std::isfinite(left) && std::isfinite(top));
        left += (width - icon->width * scale) / 2;
        top += (height - icon->height * scale) / 2;
        nsvg__pushAttr(parser);
        nsvg__parseAttribs(parser, attr);
        auto inherited = nsvg__getAttr(parser);
        for (auto shape = icon->shapes; shape; shape = shape->next) {
            // Gradient images require their own paint-coordinate mapping.
            svgTextRequire(shape->fill.type != NSVG_PAINT_LINEAR_GRADIENT &&
                           shape->fill.type != NSVG_PAINT_RADIAL_GRADIENT &&
                           shape->stroke.type != NSVG_PAINT_LINEAR_GRADIENT &&
                           shape->stroke.type != NSVG_PAINT_RADIAL_GRADIENT);
            shape->bounds[0] = shape->bounds[1] = FLT_MAX;
            shape->bounds[2] = shape->bounds[3] = -FLT_MAX;
            shape->opacity *= inherited->opacity;
            shape->strokeWidth *= scale * nsvg__getAverageScale(inherited->xform);
            if (!inherited->visible)
                shape->flags = 0;
            for (auto path = shape->paths; path; path = path->next) {
                path->bounds[0] = path->bounds[1] = FLT_MAX;
                path->bounds[2] = path->bounds[3] = -FLT_MAX;
                for (int i = 0; i < path->npts; ++i) {
                    svgTextRequire(++points <= 200000);
                    float px = left + path->pts[i * 2] * scale, py = top + path->pts[i * 2 + 1] * scale;
                    nsvg__xformPoint(&px, &py, px, py, inherited->xform);
                    svgTextRequire(std::isfinite(px) && std::isfinite(py));
                    path->pts[i * 2] = px;
                    path->pts[i * 2 + 1] = py;
                    path->bounds[0] = std::min(path->bounds[0], px);
                    path->bounds[1] = std::min(path->bounds[1], py);
                    path->bounds[2] = std::max(path->bounds[2], px);
                    path->bounds[3] = std::max(path->bounds[3], py);
                }
                for (int i = 0; i < 2; ++i) {
                    shape->bounds[i] = std::min(shape->bounds[i], path->bounds[i]);
                    shape->bounds[i + 2] = std::max(shape->bounds[i + 2], path->bounds[i + 2]);
                }
            }
        }
        if (icon->shapes) {
            if (parser->shapesTail)
                parser->shapesTail->next = icon->shapes;
            else
                parser->image->shapes = icon->shapes;
            parser->shapesTail = icon->shapes;
            while (parser->shapesTail->next)
                parser->shapesTail = parser->shapesTail->next;
            icon->shapes = nullptr;
        }
        nsvg__popAttr(parser);
    }
    static void start(void *ud, const char *name, const char **attr) {
        auto &self = *(SvgTextParser *)ud;
        if (self.skip) {
            ++self.skip;
            return;
        }
        std::string_view tag(name);
        if (tag == "clipPath" || tag == "mask" || tag == "filter" || tag == "title" || tag == "desc" ||
            tag == "symbol") {
            self.skip = 1;
            return;
        }
        svgTextRequire(self.stack.size() < NSVG_MAX_ATTR - 1);
        self.stack.push_back(self.stack.back());
        self.attributes(attr);
        // Badge links use fully transparent RGBA rectangles as hit areas.
        // NanoSVG handles RGB but would otherwise paint these opaque gray.
        std::vector<const char *> adjusted;
        for (int i = 0; attr[i]; i += 2) {
            adjusted.push_back(attr[i]);
            const char *value = attr[i + 1];
            if (std::string_view(attr[i]) == "fill" || std::string_view(attr[i]) == "stroke") {
                float r = 0, g = 0, b = 0, a = 1;
                if (sscanf_s(value, "rgba(%f,%f,%f,%f)", &r, &g, &b, &a) == 4 && a == 0)
                    value = "none";
            }
            adjusted.push_back(value);
        }
        adjusted.push_back(nullptr);
        attr = adjusted.data();
        if (tag == "image" && !self.parser->defsFlag) {
            self.embeddedImage(attr);
            return;
        }
        if (tag == "text" && !self.parser->defsFlag) {
            svgTextRequire(!self.inText);
            self.inText = true;
            self.x = self.y = self.textLength = 0;
            self.content.clear();
            nsvg__pushAttr(self.parser);
            nsvg__parseAttribs(self.parser, attr);
            for (int i = 0; attr[i]; i += 2) {
                auto key = std::string_view(attr[i]);
                if (key == "x")
                    self.x = (float)nsvg__atof(attr[i + 1]);
                if (key == "y")
                    self.y = (float)nsvg__atof(attr[i + 1]);
                if (key == "textLength")
                    self.textLength = (float)nsvg__atof(attr[i + 1]);
            }
        } else
            nsvg__startElement(self.parser, name, attr);
    }
    static void end(void *ud, const char *name) {
        auto &self = *(SvgTextParser *)ud;
        if (self.skip) {
            --self.skip;
            return;
        }
        if (std::string_view(name) == "text" && self.inText) {
            self.renderText();
            self.inText = false;
            nsvg__popAttr(self.parser);
        } else
            nsvg__endElement(self.parser, name);
        if (self.stack.size() > 1)
            self.stack.pop_back();
    }
    static void text(void *ud, const char *text) {
        auto &self = *(SvgTextParser *)ud;
        if (self.skip)
            return;
        if (self.inText) {
            svgTextRequire(self.content.size() + strlen(text) <= 4096);
            self.content += text;
        } else
            nsvg__content(self.parser, text);
    }
};
NSVGimage *parseSvgWithText(char *source) {
    std::unique_ptr<NSVGparser, decltype(&nsvg__deleteParser)> parser(nsvg__createParser(),
                                                                      nsvg__deleteParser);
    svgTextRequire(parser != nullptr);
    parser->dpi = 96;
    SvgTextParser context{parser.get()};
    nsvg__parseXML(source, SvgTextParser::start, SvgTextParser::end, SvgTextParser::text, &context);
    nsvg__createGradients(parser.get());
    nsvg__scaleToViewbox(parser.get(), "px");
    auto image = parser->image;
    parser->image = nullptr;
    return image;
}
} // namespace
} // namespace vm
