#include "editor.h"
#include "dialog-theme.h"
#include <algorithm>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <fstream>
#include <richedit.h>
#include <richole.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <thread>
#include <tom.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>
#ifdef VMD_TESTING
#include <chrono>
#include <iostream>
#endif
#include <uxtheme.h>

namespace vm {
namespace {
// Present each small chrome surface only after its background and text are ready.
// Buffer the gutter/buttons rather than the Rich Edit document or preview.
class ChromePaint {
    HDC destination, memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    RECT bounds;

  public:
    ChromePaint(HDC dc, RECT rect) : destination(dc), bounds(rect) {
        if (rect.right <= rect.left || rect.bottom <= rect.top)
            return;
        memory = CreateCompatibleDC(dc);
        bitmap = CreateCompatibleBitmap(dc, rect.right - rect.left, rect.bottom - rect.top);
        if (memory && bitmap) {
            previous = SelectObject(memory, bitmap);
            SetViewportOrgEx(memory, -rect.left, -rect.top, nullptr);
        }
    }
    HDC dc() const { return previous ? memory : destination; }
    ~ChromePaint() {
        if (previous) {
            BitBlt(destination, bounds.left, bounds.top, bounds.right - bounds.left,
                   bounds.bottom - bounds.top, memory, bounds.left, bounds.top, SRCCOPY);
            SelectObject(memory, previous);
        }
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
    }
};
std::optional<CHARRANGE> sourceParagraph(HWND input, LONG at) {
    Microsoft::WRL::ComPtr<IRichEditOle> ole;
    SendMessageW(input, EM_GETOLEINTERFACE, 0, (LPARAM)ole.GetAddressOf());
    Microsoft::WRL::ComPtr<ITextDocument> document;
    Microsoft::WRL::ComPtr<ITextRange> range;
    if (!ole || FAILED(ole.As(&document)) || FAILED(document->Range(at, at, &range)) ||
        FAILED(range->Expand(tomParagraph, nullptr)))
        return {};
    CHARRANGE result{};
    if (FAILED(range->GetStart(&result.cpMin)) || FAILED(range->GetEnd(&result.cpMax)))
        return {};
    // TOM includes the paragraph mark, including the final implicit mark.
    result.cpMax = std::max(result.cpMin, result.cpMax - 1);
    return result;
}
constexpr unsigned Save = 1, Preview = 2, Finish = 3, Heading = 4, BoldAction = 5, ItalicAction = 6, Link = 7,
                   MoreInsert = 8, Warnings = 9, Overflow = 10, UnderlineAction = 11, Live = 12, Code = 13,
                   List = 14, ImageAction = 15;
constexpr size_t toolbarOrder[] = {0, 1, 11, 2, 3, 4, 5, 10, 6, 12, 13, 14, 7, 8};
const wchar_t *labels[] = {L"Save",      L"Preview", L"View mode", L"Heading",  L"Bold",
                           L"Italic",    L"Link",    L"Insert",    L"Warnings", L"More…",
                           L"Underline", L"Live",    L"Code",      L"List",     L"Image"};
const wchar_t *tips[] = {L"Save (Ctrl+S)",
                         L"Toggle source / preview (Ctrl+Shift+V)",
                         L"Switch to view mode",
                         L"Heading level (Ctrl+Shift+1–6; Ctrl+Shift+0 for normal text)",
                         L"Bold (Ctrl+B)",
                         L"Italic (Ctrl+I)",
                         L"Insert link (Ctrl+K)",
                         L"Insert Markdown",
                         L"Show validation warnings",
                         L"More editing actions",
                         L"Underline (Ctrl+U)",
                         L"Toggle live formatted preview (Ctrl+Shift+L)",
                         L"Insert fenced code block (Ctrl+Shift+C)",
                         L"Bulleted, numbered, or task list",
                         L"Insert image (Ctrl+Shift+I)"};
std::wstring windowText(HWND window) {
    int count = GetWindowTextLengthW(window);
    std::wstring result((size_t)count + 1, L'\0');
    GetWindowTextW(window, result.data(), count + 1);
    result.resize(count);
    return result;
}
struct Fields {
    std::wstring title, firstLabel, secondLabel, first, second;
    bool numbers = false, browse = false, dark = false;
    HBRUSH brush = nullptr;
    HBRUSH inputBrush = nullptr;
};
INT_PTR CALLBACK fieldProc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    auto fields = (Fields *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (msg == WM_INITDIALOG) {
        fields = (Fields *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetPropW(dialog, L"VolturaFieldDialog", (HANDLE)1);
        SetWindowTextW(dialog, fields->title.c_str());
        SetDlgItemTextW(dialog, 101, fields->firstLabel.c_str());
        SetDlgItemTextW(dialog, 102, fields->secondLabel.c_str());
        SetDlgItemTextW(dialog, 201, fields->first.c_str());
        SetDlgItemTextW(dialog, 202, fields->second.c_str());
        ShowWindow(GetDlgItem(dialog, 203), fields->browse ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(dialog, 204), fields->browse ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(dialog, 103), fields->numbers ? SW_SHOW : SW_HIDE);
        BOOL dark = fields->dark;
        DwmSetWindowAttribute(dialog, 20, &dark, sizeof(dark));
        return TRUE;
    }
    if (!fields)
        return FALSE;
    if (msg == WM_CTLCOLORDLG || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT) {
        SetTextColor((HDC)wp, fields->dark ? RGB(229, 233, 240) : RGB(32, 41, 56));
        bool input = msg == WM_CTLCOLOREDIT;
        SetBkColor((HDC)wp, input ? (fields->dark ? RGB(28, 34, 45) : RGB(255, 255, 255))
                                 : (fields->dark ? RGB(16, 19, 25) : RGB(250, 251, 253)));
        return (INT_PTR)(input ? fields->inputBrush : fields->brush);
    }
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        HDC dc = msg == WM_PAINT ? BeginPaint(dialog, &paint) : (HDC)wp;
        int saved = SaveDC(dc);
        RECT client{};
        GetClientRect(dialog, &client);
        FillRect(dc, &client, fields->brush);
        int dpi = GetDpiForWindow(dialog);
        auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
        SelectObject(dc, GetStockObject(DC_PEN));
        SelectObject(dc, fields->inputBrush);
        for (int id : {201, 202}) {
            RECT rect{};
            GetWindowRect(GetDlgItem(dialog, id), &rect);
            MapWindowPoints(nullptr, dialog, (POINT *)&rect, 2);
            InflateRect(&rect, px(10), px(7));
            SetDCPenColor(dc, GetFocus() == GetDlgItem(dialog, id)
                                 ? (fields->dark ? RGB(144, 185, 255) : RGB(39, 95, 199))
                                 : (fields->dark ? RGB(48, 58, 73) : RGB(220, 226, 235)));
            RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, px(8), px(8));
        }
        RestoreDC(dc, saved);
        if (msg == WM_PAINT)
            EndPaint(dialog, &paint);
        return TRUE;
    }
    if (msg == WM_NOTIFY && ((NMHDR *)lp)->code == NM_CUSTOMDRAW) {
        auto draw = (NMCUSTOMDRAW *)lp;
        if (draw->dwDrawStage != CDDS_PREPAINT)
            return FALSE;
        auto dc = draw->hdc;
        int saved = SaveDC(dc);
        auto rect = draw->rc;
        FillRect(dc, &rect, fields->brush);
        bool primary = draw->hdr.idFrom == IDOK;
        bool pressed = (draw->uItemState & CDIS_SELECTED) != 0;
        bool hot = (draw->uItemState & CDIS_HOT) != 0;
        bool focus = (draw->uItemState & CDIS_FOCUS) != 0;
        auto fill = primary ? (pressed ? RGB(32, 78, 163) : hot ? RGB(52, 111, 220) : RGB(39, 95, 199))
                            : fields->dark ? (pressed ? RGB(48, 65, 90) : hot ? RGB(46, 59, 78) : RGB(28, 34, 45))
                                           : (pressed ? RGB(209, 223, 248) : hot ? RGB(230, 237, 250) : RGB(255, 255, 255));
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, fill);
        SetDCPenColor(dc, focus ? (fields->dark ? RGB(144, 185, 255) : RGB(39, 95, 199))
                                : primary ? fill : fields->dark ? RGB(48, 58, 73) : RGB(220, 226, 235));
        int radius = MulDiv(8, GetDpiForWindow(dialog), 96);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
        SelectObject(dc, (HFONT)SendMessageW(draw->hdr.hwndFrom, WM_GETFONT, 0, 0));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, primary ? RGB(255, 255, 255) : fields->dark ? RGB(229, 233, 240) : RGB(32, 41, 56));
        auto text = windowText(draw->hdr.hwndFrom);
        DrawTextW(dc, text.c_str(), (int)text.size(), &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (focus && !(SendMessageW(draw->hdr.hwndFrom, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            InflateRect(&rect, -4, -4);
            DrawFocusRect(dc, &rect);
        }
        RestoreDC(dc, saved);
        SetWindowLongPtrW(dialog, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
        return TRUE;
    }
    if (msg == WM_COMMAND) {
        if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)
            InvalidateRect(dialog, nullptr, FALSE);
        if (LOWORD(wp) == 204 && fields->browse) {
            fields->first = windowText(GetDlgItem(dialog, 201));
            if (fields->first.empty())
                fields->first = L"Image description";
            fields->second = L"path/to/image.png";
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wp) == 203) {
            wchar_t path[32768]{};
            OPENFILENAMEW chooser{sizeof(chooser)};
            chooser.hwndOwner = dialog;
            chooser.lpstrFile = path;
            chooser.nMaxFile = 32768;
            chooser.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.gif;*.svg\0All files\0*.*\0";
            chooser.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&chooser))
                SetDlgItemTextW(dialog, 202, path);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK) {
            auto first = windowText(GetDlgItem(dialog, 201)), second = windowText(GetDlgItem(dialog, 202));
            if (fields->numbers) {
                auto valid = [](const std::wstring &s, unsigned limit) {
                    return !s.empty() && s.size() <= 3 && s.find_first_not_of(L"0123456789") == s.npos &&
                           std::stoul(s) >= 1 && std::stoul(s) <= limit;
                };
                if (!valid(first, 100) || !valid(second, 256)) {
                    themedMessageBox(dialog, L"Choose 1–100 body rows and 1–256 columns.", L"Insert table",
                                     MB_OK | MB_ICONINFORMATION);
                    return TRUE;
                }
            } else if (second.empty() || second.find_first_of(L"\r\n") != second.npos ||
                       first.find_first_of(L"\r\n") != first.npos) {
                themedMessageBox(dialog, L"Enter a destination and keep each field on one line.",
                                 fields->title.c_str(), MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            fields->first = std::move(first);
            fields->second = std::move(second);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
    }
    return FALSE;
}
bool prompt(HWND owner, Fields &fields) {
    // In-memory resource keeps the portable executable independent of external UI files.
    std::vector<WORD> words;
    auto word = [&](WORD value) { words.push_back(value); };
    auto dword = [&](DWORD value) {
        word((WORD)value);
        word((WORD)(value >> 16));
    };
    auto string = [&](const wchar_t *text) {
        do {
            word(*text);
        } while (*text++);
    };
    dword(WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | DS_MODALFRAME | DS_SETFONT | DS_CENTER);
    dword(0);
    word(9);
    word(0);
    word(0);
    word(300);
    word(162);
    word(0);
    word(0);
    string(L"");
    word(9);
    string(L"Segoe UI");
    auto control = [&](DWORD style, short x, short y, short width, short height, WORD id, WORD type,
                       const wchar_t *text) {
        if (words.size() % 2)
            word(0);
        dword(WS_CHILD | WS_VISIBLE | style);
        dword(0);
        word(x);
        word(y);
        word(width);
        word(height);
        word(id);
        word(0xffff);
        word(type);
        string(text);
        word(0);
    };
    control(0, 16, 12, 268, 12, 101, 0x82, L"");
    control(WS_TABSTOP | ES_AUTOHSCROLL, 21, 31, 258, 12, 201, 0x81, L"");
    control(0, 16, 56, 268, 12, 102, 0x82, L"");
    control(WS_TABSTOP | ES_AUTOHSCROLL, 21, 75, 258, 12, 202, 0x81, L"");
    control(WS_TABSTOP | BS_PUSHBUTTON, 16, 100, 65, 22, 203, 0x80, L"Browse…");
    control(WS_TABSTOP | BS_PUSHBUTTON, 88, 100, 76, 22, 204, 0x80, L"Placeholder");
    control(WS_TABSTOP | BS_DEFPUSHBUTTON, 148, 128, 64, 22, IDOK, 0x80, L"Insert");
    control(WS_TABSTOP | BS_PUSHBUTTON, 220, 128, 64, 22, IDCANCEL, 0x80, L"Cancel");
    // The last static supplies screen readers with the selected row-count convention.
    control(0, 16, 101, 268, 16, 103, 0x82, fields.numbers ? L"A header row is added automatically." : L"");
    fields.brush = CreateSolidBrush(fields.dark ? RGB(16, 19, 25) : RGB(250, 251, 253));
    fields.inputBrush = CreateSolidBrush(fields.dark ? RGB(28, 34, 45) : RGB(255, 255, 255));
    DialogThemeScope theme;
    auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), (DLGTEMPLATE *)words.data(), owner,
                                          fieldProc, (LPARAM)&fields);
    DeleteObject(fields.brush);
    DeleteObject(fields.inputBrush);
    return result == IDOK;
}
} // namespace
struct Editor::Job {
    enum Kind { Initialize, Validate, Write } kind = Validate;
    std::atomic_bool done = false, cancel = false;
    uint64_t revision = 0;
    std::shared_ptr<Document> document;
    std::vector<Diagnostic> diagnostics;
    FileBaseline baseline;
    SaveResult saved;
    std::wstring error;
    std::vector<HeadingEntry> headings;
    std::vector<FencePoint> fences;
    std::wstring fenceSource;
};
struct Editor::Auxiliary {
    enum Kind { Image, Export, Probe, Reload, Recover, Restore, ReplaceAll } kind;
    std::atomic_bool done = false, cancel = false;
    uint64_t revision = 0;
    std::filesystem::path path;
    std::wstring error;
    std::wstring text;
    std::shared_ptr<Document> document;
    bool changed = false, print = false;
    FileBaseline baseline;
    std::filesystem::file_time_type time{};
    uintmax_t size = 0;
    std::vector<DraftRecovery::Draft> drafts;
    std::mutex printerMutex;
    HDC printer = nullptr;
};
Editor::Editor(HWND parent, const Document &document, bool isDark, unsigned scale, bool newFile)
    : owner(parent), filePath(document.path) {
    try {
        dark = isDark;
        dpi = scale;
        if (document.source.find(L'\0') != document.source.npos)
            throw std::runtime_error("This document contains null characters and cannot be edited safely. It "
                                     "remains available in reading mode.");
        richEdit = LoadLibraryW(L"Msftedit.dll");
        if (!richEdit)
            throw std::runtime_error("Windows could not load its text editor.");
        WNDCLASSW cls{};
        cls.lpfnWndProc = windowProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"VolturaMarkdownEditor";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&cls);
        panel = CreateWindowExW(0, cls.lpszClassName, L"Markdown editor",
                                WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1, owner, nullptr,
                                cls.hInstance, this);
        input =
            CreateWindowExW(0, MSFTEDIT_CLASS, L"Markdown source",
                            WS_CHILD | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                                ES_AUTOHSCROLL | ES_NOHIDESEL | ES_WANTRETURN | ES_DISABLENOSCROLL,
                            0, 0, 1, 1, panel, (HMENU)100, cls.hInstance, nullptr);
        if (!panel || !input)
            throw std::runtime_error("Windows could not create the text editor.");
        SendMessageW(input, EM_SETTEXTMODE, TM_RICHTEXT | TM_MULTILEVELUNDO, 0);
        SendMessageW(input, EM_EXLIMITTEXT, 0, 500000000);
        SendMessageW(input, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SCROLL);
        RevokeDragDrop(input);
        DragAcceptFiles(input, TRUE);
        SetWindowSubclass(input, inputProc, 1, (DWORD_PTR)this);
        // Native scrollbar areas are outside the visible region. The parent
        // owns the themed tracks; Rich Edit supplies only scroll state.
        SetWindowRgn(input, CreateRectRgn(0, 0, 0, 0), FALSE);
        SetWindowTheme(input, L"", L"");
        tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0,
                                  0, panel, nullptr, cls.hInstance, nullptr);
        for (size_t i = 0; i < buttons.size(); ++i) {
            buttons[i] =
                CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0,
                              1, 1, panel, (HMENU)(i + 1), cls.hInstance, nullptr);
            SetWindowSubclass(buttons[i], inputProc, 1, (DWORD_PTR)this);
            TOOLINFOW info{sizeof(info)};
            info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            info.hwnd = panel;
            info.uId = (UINT_PTR)buttons[i];
            info.lpszText = const_cast<wchar_t *>(tips[i]);
            SendMessageW(tooltip, TTM_ADDTOOLW, 0, (LPARAM)&info);
        }
        warningList = CreateWindowExW(0, L"LISTBOX", L"Markdown validation warnings",
                                      WS_CHILD | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                                          LBS_DISABLENOSCROLL,
                                      0, 0, 1, 1, panel, (HMENU)101, cls.hInstance, nullptr);
        SetWindowSubclass(warningList, inputProc, 1, (DWORD_PTR)this);
        SetWindowRgn(warningList, CreateRectRgn(0, 0, 0, 0), FALSE);
        SetWindowTheme(warningList, L"", L"");
        cachedText = baselineText = editText(document.source);
        resetCapture();
        updating = true;
        SETTEXTEX set{ST_DEFAULT, 1200};
        SendMessageW(input, EM_SETTEXTEX, (WPARAM)&set, (LPARAM)cachedText.c_str());
        SendMessageW(input, EM_EMPTYUNDOBUFFER, 0, 0);
        updating = false;
        theme(isDark, scale);
        RECT client{};
        GetClientRect(owner, &client);
        live = client.right >= MulDiv(1000, dpi, 96);
        SetWindowTextW(buttons[11], live ? L"Live: on" : L"Live");
        SetTimer(panel, 1, 40, nullptr);
        SetTimer(panel, 4, 3000, nullptr);
        findMessage = RegisterWindowMessageW(FINDMSGSTRINGW);
        try {
            recovery = std::make_shared<DraftRecovery>();
        } catch (...) {
        }
        needsFirstSave = newFile && !filePath.empty();
        if (filePath.empty() || newFile) {
            ready = true;
            check();
        } else {
            SendMessageW(input, EM_SETREADONLY, TRUE, 0);
            job = std::make_shared<Job>();
            job->kind = Job::Initialize;
            auto result = job;
            auto path = filePath;
            auto expected = baselineText;
            std::thread([result, path, expected = std::move(expected)] {
                try {
                    result->baseline = readBaseline(path);
                    if (!result->baseline.exists || editText(decode(result->baseline.bytes)) != expected)
                        result->error = L"The file changed since it was opened. Reopen it before editing.";
                } catch (const std::exception &e) {
                    result->error = fromUtf8(e.what());
                }
                result->done.store(true, std::memory_order_release);
            }).detach();
        }
        title();
    } catch (...) {
        if (panel)
            DestroyWindow(panel);
        if (font)
            DeleteObject(font);
        if (mono)
            DeleteObject(mono);
        if (background)
            DeleteObject(background);
        if (richEdit)
            FreeLibrary(richEdit);
        throw;
    }
}
Editor::~Editor() {
    if (!recoveredSnapshot.empty()) {
        std::error_code ec;
        std::filesystem::remove(recoveredSnapshot, ec);
    }
    if (findDialog)
        DestroyWindow(findDialog);
    if (recovery) {
        recovery->clear(true);
    }
    for (auto &state : retiredRecovery)
        state->clear(true);
    if (auxiliary) {
        auxiliary->cancel = true;
        if (auxiliary->kind == Auxiliary::Image && auxiliary->done.load(std::memory_order_acquire) &&
            !auxiliary->path.empty()) {
            std::error_code ec;
            std::filesystem::remove(auxiliary->path, ec);
        }
    }
    if (job)
        job->cancel.store(true);
    if (panel)
        DestroyWindow(panel);
    if (font)
        DeleteObject(font);
    if (mono)
        DeleteObject(mono);
    if (background)
        DeleteObject(background);
    if (richEdit)
        FreeLibrary(richEdit);
}
void Editor::theme(bool isDark, unsigned scale) {
    bool previousUpdating = updating;
    updating = true;
    Microsoft::WRL::ComPtr<IRichEditOle> ole;
    SendMessageW(input, EM_GETOLEINTERFACE, 0, (LPARAM)ole.GetAddressOf());
    Microsoft::WRL::ComPtr<ITextDocument> tom;
    if (ole)
        ole.As(&tom);
    if (tom)
        tom->Undo(tomSuspend, nullptr);
    dark = isDark;
    dpi = scale;
    auto previousFont = font, previousMono = mono;
    font = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Segoe UI");
    mono = CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    for (auto button : buttons) {
        SendMessageW(button, WM_SETFONT, (WPARAM)font, TRUE);
        SetWindowTheme(button, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }
    SendMessageW(warningList, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(input, WM_SETFONT, (WPARAM)mono, TRUE);
    SendMessageW(input, EM_SETBKGNDCOLOR, 0, dark ? RGB(16, 19, 25) : RGB(250, 251, 253));
    CHARFORMAT2W format{sizeof(format)};
    format.dwMask = CFM_COLOR | CFM_BACKCOLOR;
    format.crBackColor = dark ? RGB(16, 19, 25) : RGB(250, 251, 253);
    format.crTextColor = dark ? RGB(229, 233, 240) : RGB(32, 41, 56);
    SendMessageW(input, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&format);
    if (previousFont)
        DeleteObject(previousFont);
    if (previousMono)
        DeleteObject(previousMono);
    if (background)
        DeleteObject(background);
    background = CreateSolidBrush(dark ? RGB(16, 19, 25) : RGB(250, 251, 253));
    dialogDark = dark;
    if (findDialog)
        themeDialog(findDialog, dark);
    layout();
    InvalidateRect(panel, nullptr, TRUE);
    updating = previousUpdating;
    if (tom)
        tom->Undo(tomResume, nullptr);
    SetTimer(panel, 5, 150, nullptr);
}
int Editor::headerHeight() const {
    return MulDiv(expanded ? 150 : 42, dpi, 96);
}
int Editor::splitPosition(int width, int height) const {
    bool vertical = width >= MulDiv(1000, dpi, 96);
    int length = vertical ? width : std::max(1, height - headerHeight());
    int minimum = std::min(length / 2, MulDiv(vertical ? 240 : 120, dpi, 96));
    return (vertical ? 0 : headerHeight()) +
           std::clamp((int)(length * (vertical ? splitWidth : splitHeight)), minimum, length - minimum);
}
RECT Editor::splitBounds() const {
    RECT r{};
    if (!live || preview) return r;
    GetClientRect(panel, &r);
    int middle = splitPosition(r.right, r.bottom), half = MulDiv(4, dpi, 96);
    if (r.right >= MulDiv(1000, dpi, 96))
        return {middle - half, headerHeight(), middle + half, r.bottom};
    return {0, middle - half, r.right, middle + half};
}
RECT Editor::splitMargin() const {
    auto margin = splitBounds();
    if (IsRectEmpty(&margin)) return margin;
    RECT source{}, panelBounds{};
    GetWindowRect(input, &source);
    MapWindowPoints(nullptr, panel, (POINT *)&source, 2);
    GetClientRect(panel, &panelBounds);
    if (panelBounds.right >= MulDiv(1000, dpi, 96))
        margin.left = std::min(margin.left, source.right);
    else
        margin.top = std::min(margin.top, source.bottom);
    return margin;
}
void Editor::paintSplit(HDC target) const {
    auto bounds = splitBounds();
    if (IsRectEmpty(&bounds)) return;
    RECT panelBounds{};
    GetClientRect(panel, &panelBounds);
    bool vertical = panelBounds.right >= MulDiv(1000, dpi, 96);
    int thickness = std::max(1, MulDiv(1, dpi, 96));
    auto track = scrollTrack(input, vertical ? SB_VERT : SB_HORZ);
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE};
    SCROLLBARINFO state{sizeof(state)};
    bool unavailable = GetScrollBarInfo(input, vertical ? OBJID_VSCROLL : OBJID_HSCROLL, &state) &&
                       (state.rgstate[0] & STATE_SYSTEM_UNAVAILABLE);
    bool scrollbar = !unavailable && (vertical ? track.right > track.left : track.bottom > track.top) &&
                     GetScrollInfo(input, vertical ? SB_VERT : SB_HORZ, &info) &&
                     info.nMax - info.nMin + 1 > (int)info.nPage;
    std::array<int64_t, 7> key{bounds.left, bounds.top, bounds.right, bounds.bottom, dark, scrollbar, dpi};
    if (!target && paintedSplit == key) return;
    HDC destination = target ? target : GetDC(panel);
    if (!destination) return;
    {
        // The native scrollbar ends before the grip's hit area. Own the whole
        // intervening margin so no document pixels survive a move/no-erase paint.
        auto margin = splitMargin();
        ChromePaint buffered(destination, margin);
        auto dc = buffered.dc();
        FillRect(dc, &margin, background);
        int x = (bounds.left + bounds.right) / 2, y = (bounds.top + bounds.bottom) / 2;
        auto border = CreateSolidBrush(dark ? RGB(48, 58, 73) : RGB(220, 226, 235));
        if (!scrollbar) {
            RECT line = vertical ? RECT{x, bounds.top, x + thickness, bounds.bottom}
                                 : RECT{bounds.left, y, bounds.right, y + thickness};
            FillRect(dc, &line, border);
        }
        DeleteObject(border);
        auto grip = CreateSolidBrush(dark ? RGB(146, 158, 175) : RGB(102, 117, 138));
        for (int i = -1; i <= 1; ++i) {
            int offset = MulDiv(i * 5, dpi, 96), size = std::max(2, MulDiv(2, dpi, 96));
            RECT dot = vertical ? RECT{x - size / 2, y + offset, x - size / 2 + size, y + offset + size}
                                : RECT{x + offset, y - size / 2, x + offset + size, y - size / 2 + size};
            FillRect(dc, &dot, grip);
        }
        DeleteObject(grip);
    }
    paintedSplit = key;
    if (!target) ReleaseDC(panel, destination);
}
RECT Editor::previewBounds(int top) const {
    RECT r;
    GetClientRect(owner, &r);
    r.top = top + headerHeight();
    if (live) {
        int gap = MulDiv(8, dpi, 96);
        if (r.right >= MulDiv(1000, dpi, 96))
            r.left = splitPosition(r.right, r.bottom - top) + gap / 2;
        else
            r.top = top + splitPosition(r.right, r.bottom - top) + gap / 2;
    }
    return r;
}
void Editor::position(int top) {
    RECT r;
    GetClientRect(owner, &r);
    RECT current;
    GetWindowRect(panel, &current);
    MapWindowPoints(nullptr, owner, (POINT *)&current, 2);
    RECT next{0, top, r.right, top + (preview ? headerHeight() : std::max(1L, r.bottom - top))};
    if (!EqualRect(&current, &next))
        MoveWindow(panel, next.left, next.top, next.right, next.bottom - next.top, TRUE);
    layout();
    if (!preview && restoreScroll) {
        SendMessageW(input, EM_SETSCROLLPOS, 0, (LPARAM)&sourceScroll);
        restoreScroll = false;
    }
}
void Editor::layout() {
    if (!input)
        return;
    RECT r;
    GetClientRect(panel, &r);
    auto px = [&](int n) { return MulDiv(n, dpi, 96); };
    // Preview refreshes also request layout. Keep unchanged controls visible and
    // in place so a content update cannot erase and repaint the whole toolbar.
    auto positionControl = [&](HWND control, int left, int top, int width, int height) {
        RECT current;
        GetWindowRect(control, &current);
        MapWindowPoints(nullptr, panel, (POINT *)&current, 2);
        RECT next{left, top, left + width, top + height};
        if (!EqualRect(&current, &next)) {
            bool scrollable = control == input || control == warningList;
            if (scrollable) {
                settingScrollRegion = true;
                SetWindowRgn(control, CreateRectRgn(0, 0, 0, 0), FALSE);
            }
            MoveWindow(control, left, top, width, height, TRUE);
            if (scrollable) {
                settingScrollRegion = false;
                updateScrollRegion(control);
            }
        }
    };
    int x = px(8), y = px(6), h = px(30), reserve = px(72);
    hiddenFrom = std::size(toolbarOrder);
    for (size_t order = 0; order < std::size(toolbarOrder); ++order) {
        size_t i = toolbarOrder[order];
        int width = px(i == 8 ? 130 : i == 2 ? 100 : i == 1 || i == 3 || i == 10 ? 78 : 60);
        if (x + width > r.right - reserve) {
            hiddenFrom = order;
            break;
        }
        positionControl(buttons[i], x, y, width, h);
        ShowWindow(buttons[i], SW_SHOWNA);
        x += width + px(4);
    }
    for (size_t order = hiddenFrom; order < std::size(toolbarOrder); ++order)
        ShowWindow(buttons[toolbarOrder[order]], SW_HIDE);
    ShowWindow(buttons[9], hiddenFrom < std::size(toolbarOrder) ? SW_SHOWNA : SW_HIDE);
    positionControl(buttons[9], std::min<LONG>(x, std::max(0L, r.right - reserve)), y, reserve - px(4), h);
    ShowWindow(warningList, expanded ? SW_SHOWNA : SW_HIDE);
    positionControl(warningList, px(8), px(42), std::max(1L, r.right - px(16)), px(102));
    int sourceWidth = std::max(1L, r.right - px(24));
    int sourceHeight = std::max(1L, r.bottom - headerHeight() - px(8));
    if (live) {
        if (r.right >= px(1000))
            sourceWidth = std::max(1, splitPosition(r.right, r.bottom) - px(20));
        else
            sourceHeight = std::max(1, splitPosition(r.right, r.bottom) - headerHeight() - px(8));
    }
    RECT sourceBounds{};
    GetWindowRect(input, &sourceBounds);
    MapWindowPoints(nullptr, panel, (POINT *)&sourceBounds, 2);
    // Reserve a stable gutter, with enough digits for even one-character lines.
    int gutter = px(12 + 8 * (int)std::max<size_t>(4, std::to_wstring(liveLength + 1).size()));
    sourceWidth = std::max(1, sourceWidth - gutter);
    RECT next{px(12) + gutter, headerHeight(), px(12) + gutter + sourceWidth,
              headerHeight() + sourceHeight};
    if (sourceWrapped != live) {
        // Wrap only the split source pane. Reflow is a view change, not an edit,
        // and must not run again on each live-preview refresh.
        LONG first = (LONG)SendMessageW(input, EM_GETFIRSTVISIBLELINE, 0, 0);
        LONG anchor = (LONG)SendMessageW(input, EM_LINEINDEX, first, 0);
        bool previousSyncing = syncing;
        syncing = true;
        SendMessageW(input, EM_SETTARGETDEVICE, 0, live ? 0 : 1);
        sourceWrapped = live;
        LONG line = (LONG)SendMessageW(input, EM_EXLINEFROMCHAR, 0, anchor);
        SendMessageW(input, EM_LINESCROLL, 0,
                     line - SendMessageW(input, EM_GETFIRSTVISIBLELINE, 0, 0));
        syncing = previousSyncing;
    }
    if (!EqualRect(&sourceBounds, &next)) {
        settingScrollRegion = true;
        SetWindowRgn(input, CreateRectRgn(0, 0, 0, 0), FALSE);
        MoveWindow(input, next.left, next.top, sourceWidth, sourceHeight, FALSE);
        settingScrollRegion = false;
        updateScrollRegion(input);
        RedrawWindow(input, nullptr, nullptr, RDW_INVALIDATE | RDW_NOFRAME);
    }
    ShowWindow(input, preview ? SW_HIDE : SW_SHOWNA);
    updateScrollRegion(input);
    updateScrollRegion(warningList);
    paintScrollbars(input);
    paintScrollbars(warningList);
}
void Editor::title() {
    auto name = filePath.empty() ? L"Untitled" : filePath.filename().wstring();
    SetWindowTextW(owner, (name + (modified() ? L" *" : L"") + L" — Voltura Markdown").c_str());
}
bool Editor::modified() const {
    return needsFirstSave || !textCurrent || cachedText != baselineText;
}
bool Editor::busy() const {
    return saving || !ready ||
           (auxiliary && (auxiliary->kind == Auxiliary::Image || auxiliary->kind == Auxiliary::Export ||
                          auxiliary->kind == Auxiliary::ReplaceAll || auxiliary->kind == Auxiliary::Restore));
}
void Editor::changed() {
    if (updating)
        return;
    searchResults.reset(); searchMarkerRows.clear(); activeSearch = UINT32_MAX;
    clearFindHighlights();
    textCurrent = false;
    if (!localMutation)
        fullCapture = true;
    checkDue = false;
    ++revision;
    dirtyFrom = std::min(dirtyFrom, changeFrom == SIZE_MAX ? 0 : changeFrom);
    if (job && job->kind == Job::Validate)
        job->cancel.store(true);
    SetTimer(panel, 2, 400, nullptr);
    if (!checkpointScheduled) {
        SetTimer(panel, 3, 2000, nullptr);
        checkpointScheduled = true;
    }
    SetTimer(panel, 5, 600, nullptr);
    SetWindowTextW(buttons[8], L"Checking…");
    title();
    SendMessageW(owner, EditorChanged, 0, 0);
}
void Editor::captureText() {
    if (textCurrent)
        return;
    if (!fullCapture && keptPrefix + keptSuffix <= cachedText.size() &&
        keptPrefix + keptSuffix <= liveLength) {
        size_t end = liveLength - keptSuffix;
        std::wstring changed(end - keptPrefix + 1, L'\0');
        TEXTRANGEW range{{(LONG)keptPrefix, (LONG)end}, changed.data()};
        auto count = (size_t)SendMessageW(input, EM_GETTEXTRANGE, 0, (LPARAM)&range);
        changed.resize(count);
        auto normalized = editText(changed);
        if (normalized.size() == end - keptPrefix) {
            cachedText.replace(keptPrefix, cachedText.size() - keptPrefix - keptSuffix, normalized);
            textCurrent = true;
            resetCapture();
            title();
            return;
        }
    }
    GETTEXTLENGTHEX length{GTL_NUMCHARS | GTL_PRECISE, 1200};
    auto count = (size_t)SendMessageW(input, EM_GETTEXTLENGTHEX, (WPARAM)&length, 0);
    std::wstring text(count + 2, L'\0');
    GETTEXTEX get{(DWORD)(text.size() * sizeof(wchar_t)), GT_DEFAULT, 1200, nullptr, nullptr};
    auto n = (size_t)SendMessageW(input, EM_GETTEXTEX, (WPARAM)&get, (LPARAM)text.data());
    text.resize(n);
    cachedText = editText(text);
    textCurrent = true;
    resetCapture();
    title();
}
void Editor::resetCapture() {
    liveLength = keptPrefix = keptSuffix = cachedText.size();
    fullCapture = false;
}
void Editor::trackCapture(size_t start, size_t oldEnd, size_t newEnd) {
    if (fullCapture)
        return;
    if (start > oldEnd || oldEnd > liveLength || newEnd < start) {
        fullCapture = true;
        return;
    }
    keptPrefix = std::min(keptPrefix, start);
    keptSuffix = std::min(keptSuffix, liveLength - oldEnd);
    liveLength = liveLength - (oldEnd - start) + (newEnd - start);
}
void Editor::check(bool forPreview) {
    if (!ready || saving)
        return;
    captureText();
    checkDue = true;
    previewPending = previewPending || forPreview;
    if (checkedRevision == revision && rendered) {
        if (previewPending) {
            previewPending = false;
            SendMessageW(input, EM_GETSCROLLPOS, 0, (LPARAM)&sourceScroll);
            preview = true;
            SetWindowTextW(buttons[1], L"Source");
            PostMessageW(owner, EditorPreview, 0, 0);
        } else if (live)
            PostMessageW(owner, EditorPreview, 0, 0);
        return;
    }
    if (job) {
        job->cancel.store(true);
        return;
    }
    checkDue = false;
    auto result = std::make_shared<Job>();
    result->revision = revision;
    job = result;
    auto source = cachedText;
    auto path = linkBase();
    std::thread([result, source = std::move(source), path]() mutable {
        try {
            result->document =
                std::make_shared<Document>(parse(std::move(source), &result->cancel, 0xffffffff, true));
            result->document->path = path;
            result->headings = sourceOutline(result->document->source);
            result->fences = sourceFences(result->document->source);
            result->fenceSource = result->document->source;
            if (!result->cancel.load())
                result->diagnostics = validateDraft(*result->document, result->cancel);
        } catch (const std::exception &e) {
            result->error = fromUtf8(e.what());
        }
        result->done.store(true, std::memory_order_release);
    }).detach();
}
void Editor::updateWarnings() {
    auto label = std::to_wstring(diagnostics.size()) + (diagnostics.size() == 1 ? L" warning" : L" warnings");
    SetWindowTextW(buttons[8], label.c_str());
    SendMessageW(warningList, LB_RESETCONTENT, 0, 0);
    for (auto &issue : diagnostics) {
        auto text = L"Line " + std::to_wstring(issue.line) + L": " + issue.message;
        SendMessageW(warningList, LB_ADDSTRING, 0, (LPARAM)text.c_str());
    }
    if (diagnostics.empty())
        SendMessageW(warningList, LB_ADDSTRING, 0, (LPARAM)L"No Markdown warnings.");
}
void Editor::poll() {
    if (!job || !job->done.load(std::memory_order_acquire))
        return;
    auto result = std::move(job);
    if (result->kind == Job::Initialize) {
        if (!result->error.empty()) {
            themedMessageBox(owner, result->error.c_str(), L"Edit Markdown", MB_OK | MB_ICONERROR);
            PostMessageW(owner, EditorFinish, 0, 0);
            return;
        }
        baseline = std::move(result->baseline);
        std::error_code ec;
        diskTime = std::filesystem::last_write_time(filePath, ec);
        diskSize = baseline.bytes.size();
        ready = true;
        SendMessageW(input, EM_SETREADONLY, FALSE, 0);
        check();
        focus();
        return;
    }
    if (result->kind == Job::Validate) {
        if (result->revision == revision && !result->cancel.load()) {
            checkedRevision = revision;
            rendered = std::move(result->document);
            headings = std::move(result->headings);
            outlineRevision = revision;
            fences = std::move(result->fences);
            fenceSource = std::move(result->fenceSource);
            dirtyFrom = SIZE_MAX;
            diagnostics = std::move(result->diagnostics);
            updateWarnings();
            if (live && rendered)
                PostMessageW(owner, EditorPreview, 0, 0);
            if (!result->error.empty()) {
                SetWindowTextW(buttons[8], L"Check failed");
                previewPending = false;
            }
        }
        if (previewPending || checkDue)
            check(previewPending);
        return;
    }
    saving = false;
    EnableWindow(panel, TRUE);
    SendMessageW(input, EM_SETREADONLY, FALSE, 0);
    if (result->document && result->saved.status == SaveResult::Saved) {
        rendered = result->document;
        diagnostics = std::move(result->diagnostics);
        checkedRevision = revision;
        headings = std::move(result->headings);
        fences = std::move(result->fences);
        fenceSource = std::move(result->fenceSource);
        outlineRevision = revision;
        dirtyFrom = SIZE_MAX;
        updateWarnings();
    }
    if (!result->error.empty())
        result->saved.error = result->error;
    switch (result->saved.status) {
    case SaveResult::Saved: {
        if (!recoveredSnapshot.empty()) {
            std::error_code ignored;
            std::filesystem::remove(recoveredSnapshot, ignored);
            recoveredSnapshot.clear();
        }
        needsFirstSave = false;
        baseline = std::move(result->saved.baseline);
        auto savedText = editText(result->document->source);
        if (savedText != cachedText) {
            CHARRANGE selection;
            SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&selection);
            CHARRANGE all{0, -1};
            updating = true;
            SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&all);
            SendMessageW(input, EM_STOPGROUPTYPING, 0, 0);
            SendMessageW(input, EM_REPLACESEL, TRUE, (LPARAM)savedText.c_str());
            SendMessageW(input, EM_STOPGROUPTYPING, 0, 0);
            selection.cpMin = std::min<LONG>(selection.cpMin, (LONG)savedText.size());
            selection.cpMax = std::min<LONG>(selection.cpMax, (LONG)savedText.size());
            SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&selection);
            updating = false;
            cachedText = std::move(savedText);
            resetCapture();
            textCurrent = true;
            ++revision;
            checkedRevision = outlineRevision = revision;
        }
        baselineText = cachedText;
        filePath = savePath;
        recoveredBase.clear();
        externalChanged = false;
        std::error_code ec;
        diskTime = std::filesystem::last_write_time(filePath, ec);
        diskSize = baseline.bytes.size();
        if (recovery) {
            auto old = recovery;
            retiredRecovery.push_back(old);
            recovery = std::make_shared<DraftRecovery>();
            std::thread([old] { old->clear(true); }).detach();
        }
        rendered = std::move(result->document);
        if (rendered)
            rendered->path = filePath;
        title();
        PostMessageW(owner, EditorSaved, 0, 0);
        break;
    }
    case SaveResult::Conflict: {
        int choice =
            themedMessageBox(owner,
                             L"The destination changed outside Voltura Markdown.\n\nYes: overwrite the "
                             L"current file\nNo: Save As another file\nCancel: keep editing",
                             L"File changed", MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON3);
        if (choice == IDYES)
            startSave(true);
        else if (choice == IDNO) {
            if (!save(true))
                PostMessageW(owner, EditorSaved, 1, 0);
        } else
            PostMessageW(owner, EditorSaved, 1, 0);
        break;
    }
    case SaveResult::NeedsUtf8:
        if (themedMessageBox(owner, L"This encoding cannot represent your changes. Save as UTF-8 instead?",
                             L"Save Markdown", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            convertUtf8 = true;
            startSave(overwrite);
        } else
            PostMessageW(owner, EditorSaved, 1, 0);
        break;
    default:
        themedMessageBox(owner,
                         result->saved.error.empty()
                             ? L"The file could not be saved. Your draft is still open."
                             : result->saved.error.c_str(),
                         L"Save Markdown", MB_OK | MB_ICONERROR);
        PostMessageW(owner, EditorSaved, 1, 0);
        break;
    }
}
void Editor::startSave(bool readExpected) {
    if (auxiliary && (auxiliary->kind == Auxiliary::Reload || auxiliary->kind == Auxiliary::Probe)) {
        auxiliary->cancel = true;
        auxiliary.reset();
    }
    if (job)
        job->cancel.store(true);
    auto result = std::make_shared<Job>();
    result->kind = Job::Write;
    job = result;
    saving = true;
    overwrite = readExpected;
    EnableWindow(panel, FALSE);
    SendMessageW(input, EM_SETREADONLY, TRUE, 0);
    auto text = cachedText;
    auto path = savePath;
    auto original = baseline;
    auto originalPath = linkBase();
    bool utf8 = convertUtf8;
    std::thread([result, text = std::move(text), path, originalPath, original = std::move(original),
                 readExpected, utf8]() mutable {
        try {
            if (path != originalPath)
                text = rebaseLocalLinks(text, originalPath, path);
            result->document = std::make_shared<Document>(parse(text, &result->cancel, 0xffffffff, true));
            result->document->path = path;
            result->headings = sourceOutline(text);
            result->fences = sourceFences(text);
            result->fenceSource = text;
            result->diagnostics = validateDraft(*result->document, result->cancel);
            auto expected = readExpected ? readBaseline(path) : original;
            result->saved = saveDraft(path, text, expected, original, utf8);
            if (result->saved.status == SaveResult::Saved)
                result->document->source = decode(result->saved.baseline.bytes, &result->document->encoding);
        } catch (const std::exception &e) {
            result->saved.error = fromUtf8(e.what());
        }
        result->done.store(true, std::memory_order_release);
    }).detach();
}
bool Editor::save(bool saveAs) {
    if (auxiliary && (auxiliary->kind == Auxiliary::Image || auxiliary->kind == Auxiliary::ReplaceAll ||
                      auxiliary->kind == Auxiliary::Restore)) {
        themedMessageBox(
            owner, L"An editing action is still running. Save again when its changes appear in the source.",
            L"Save Markdown", MB_OK);
        return false;
    }
    if (!ready || saving)
        return false;
    captureText();
    savePath = filePath;
    convertUtf8 = false;
    overwrite = false;
    if (saveAs || filePath.empty()) {
        std::array<wchar_t, 32768> path{};
        auto name = filePath.empty() ? L"Untitled.md" : filePath.wstring();
        wcsncpy_s(path.data(), path.size(), name.c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = owner;
        dialog.lpstrFile = path.data();
        dialog.nMaxFile = (DWORD)path.size();
        dialog.lpstrFilter = L"Markdown files\0*.md\0All files\0*.*\0";
        dialog.lpstrDefExt = L"md";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!themedSaveFile(&dialog))
            return false;
        savePath = path.data();
        overwrite = savePath != filePath;
    }
    KillTimer(panel, 2);
    startSave(overwrite);
    return true;
}
Editor::Leave Editor::leave() {
    if (auxiliary && auxiliary->kind == Auxiliary::Export) {
        themedMessageBox(owner,
                         L"The export or print job is still running. Finish this action after it completes.",
                         L"Edit Markdown", MB_OK);
        return Leave::Cancel;
    }
    if (auxiliary && (auxiliary->kind == Auxiliary::Image || auxiliary->kind == Auxiliary::ReplaceAll ||
                      auxiliary->kind == Auxiliary::Restore)) {
        themedMessageBox(
            owner,
            L"An editing action is still running. Finish this action when its changes appear in the source.",
            L"Edit Markdown", MB_OK);
        return Leave::Cancel;
    }
    if (saving)
        return Leave::Cancel;
    captureText();
    if (!modified())
        return Leave::Allow;
    int answer = themedMessageBox(owner, L"Save changes before leaving this draft?", L"Unsaved Markdown",
                                  MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDNO)
        return Leave::Allow;
    if (answer == IDYES && save())
        return Leave::Saving;
    return Leave::Cancel;
}
void Editor::replace(const TextEdit &edit) {
    if (!ready || saving || preview)
        return;
    if (textCurrent && edit.length == edit.replacement.size() &&
        std::wstring_view(cachedText).substr(edit.start, edit.length) == edit.replacement) {
        focus();
        return;
    }
    CHARRANGE range{(LONG)edit.start, (LONG)(edit.start + edit.length)};
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&range);
    SendMessageW(input, EM_STOPGROUPTYPING, 0, 0);
    changeFrom = edit.start;
    SendMessageW(input, EM_REPLACESEL, TRUE, (LPARAM)edit.replacement.c_str());
    changeFrom = SIZE_MAX;
    SendMessageW(input, EM_STOPGROUPTYPING, 0, 0);
    range.cpMin = (LONG)(edit.start + edit.selection);
    range.cpMax = range.cpMin + (LONG)edit.selected;
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&range);
    SendMessageW(input, EM_SCROLLCARET, 0, 0);
    focus();
}
void Editor::insert(Insert kind, unsigned level) {
    if (preview) {
        preview = false;
        SetWindowTextW(buttons[1], L"Preview");
        PostMessageW(owner, EditorLayout, 0, 0);
    }
    if (!ready || saving)
        return;
    captureText();
    CHARRANGE range;
    SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&range);
    if (kind == Insert::Table) {
        Fields fields{
            L"Insert table", L"Body rows (1–100)", L"Columns (1–256)", L"3", L"2", true, false, dark};
        if (prompt(owner, fields))
            replace(insertMarkdown(cachedText, range.cpMin, range.cpMax, kind, std::stoul(fields.first),
                                   std::stoul(fields.second)));
    } else if (kind == Insert::Link || kind == Insert::Image) {
        Fields fields{kind == Insert::Image ? L"Insert image" : L"Insert link",
                      kind == Insert::Image ? L"Alt text" : L"Link text",
                      L"Destination (URL or file path)",
                      cachedText.substr(range.cpMin, range.cpMax - range.cpMin),
                      L"",
                      false,
                      kind == Insert::Image,
                      dark};
        if (!prompt(owner, fields))
            return;
        std::filesystem::path destination(fields.second);
        if (destination.is_absolute() && fields.second.find(L"://") == fields.second.npos) {
            if (!linkBase().empty()) {
                auto relative =
                    destination.lexically_relative(std::filesystem::absolute(linkBase()).parent_path());
                if (!relative.empty())
                    destination = relative;
            }
            fields.second = markdownFileDestination(destination);
        }
        replace(
            insertMarkdown(cachedText, range.cpMin, range.cpMax, kind, 0, 0, fields.first, fields.second));
    } else
        replace(insertMarkdown(cachedText, range.cpMin, range.cpMax, kind, level));
}
unsigned Editor::trackMenu(HMENU menu, UINT flags, int x, int y) {
    // Keep strings and item data alive throughout the native menu's modal loop.
    // Prepare only on opening a menu, never on startup or while typing.
    std::vector<std::wstring> rows(GetMenuItemCount(menu));
    for (UINT i = 0; i < rows.size(); ++i) {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_FTYPE | MIIM_STRING;
        GetMenuItemInfoW(menu, i, TRUE, &item);
        rows[i].resize((size_t)item.cch + 1);
        item.dwTypeData = rows[i].data();
        ++item.cch;
        GetMenuItemInfoW(menu, i, TRUE, &item);
        rows[i].resize(item.cch);
        item.fMask = MIIM_FTYPE | MIIM_DATA;
        item.fType |= MFT_OWNERDRAW;
        item.dwItemData = (ULONG_PTR)&rows[i];
        SetMenuItemInfoW(menu, i, TRUE, &item);
    }
    auto brush = CreateSolidBrush(dark ? RGB(28, 34, 45) : RGB(255, 255, 255));
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_BACKGROUND | MIM_STYLE;
    info.hbrBack = brush;
    info.dwStyle = MNS_NOCHECK;
    SetMenuInfo(menu, &info);
    auto result = TrackPopupMenu(menu, flags | TPM_RETURNCMD, x, y, 0, panel, nullptr);
    // The caller destroys the menu; clear borrowed data before the rows go away.
    for (UINT i = 0; i < rows.size(); ++i) {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_DATA;
        SetMenuItemInfoW(menu, i, TRUE, &item);
    }
    info.fMask = MIM_BACKGROUND;
    info.hbrBack = nullptr;
    SetMenuInfo(menu, &info);
    DeleteObject(brush);
    return result;
}
void Editor::measureMenu(MEASUREITEMSTRUCT &item) const {
    auto &text = *reinterpret_cast<const std::wstring *>(item.itemData);
    auto dc = GetDC(panel);
    auto previous = SelectObject(dc, font);
    auto tab = text.find(L'\t');
    SIZE label{}, shortcut{};
    GetTextExtentPoint32W(dc, text.c_str(), (int)(tab == text.npos ? text.size() : tab), &label);
    if (tab != text.npos)
        GetTextExtentPoint32W(dc, text.c_str() + tab + 1, (int)(text.size() - tab - 1), &shortcut);
    SelectObject(dc, previous);
    ReleaseDC(panel, dc);
    item.itemWidth = label.cx + MulDiv(56, dpi, 96);
    if (tab != text.npos)
        item.itemWidth += shortcut.cx + MulDiv(40, dpi, 96);
    // Empty strings are separators in the editor menus.
    item.itemHeight = MulDiv(text.empty() ? 10 : 34, dpi, 96);
}
void Editor::drawMenu(const DRAWITEMSTRUCT &item) const {
    auto &text = *reinterpret_cast<const std::wstring *>(item.itemData);
    auto px = [this](int value) { return MulDiv(value, dpi, 96); };
    auto dc = item.hDC;
    auto saved = SaveDC(dc);
    auto r = item.rcItem;
    SetDCBrushColor(dc, dark ? RGB(28, 34, 45) : RGB(255, 255, 255));
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    bool disabled = (item.itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    if (text.empty()) {
        SetDCPenColor(dc, dark ? RGB(48, 58, 73) : RGB(220, 226, 235));
        MoveToEx(dc, r.left + px(12), (r.top + r.bottom) / 2, nullptr);
        LineTo(dc, r.right - px(12), (r.top + r.bottom) / 2);
    } else {
        if ((item.itemState & ODS_SELECTED) && !disabled) {
            SelectObject(dc, GetStockObject(NULL_PEN));
            SetDCBrushColor(dc, dark ? RGB(46, 59, 78) : RGB(230, 237, 250));
            RoundRect(dc, r.left + px(4), r.top + px(2), r.right - px(4), r.bottom - px(2), px(8), px(8));
            SelectObject(dc, GetStockObject(DC_PEN));
        }
        SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, disabled ? (dark ? RGB(108, 119, 136) : RGB(148, 157, 170))
                                  : (dark ? RGB(229, 233, 240) : RGB(32, 41, 56)));
        auto label = r;
        label.left += px(34);
        label.right -= px(16);
        auto tab = text.find(L'\t');
        if (tab != text.npos) {
            auto shortcut = text.c_str() + tab + 1;
            int length = (int)(text.size() - tab - 1);
            SIZE size{};
            GetTextExtentPoint32W(dc, shortcut, length, &size);
            RECT cap{label.right - size.cx - px(16), (r.top + r.bottom - size.cy) / 2 - px(2), label.right,
                     (r.top + r.bottom + size.cy) / 2 + px(2)};
            SetDCBrushColor(dc, dark ? RGB(36, 44, 57) : RGB(245, 247, 250));
            SetDCPenColor(dc, dark ? RGB(71, 83, 101) : RGB(190, 199, 212));
            RoundRect(dc, cap.left, cap.top, cap.right, cap.bottom, px(6), px(6));
            DrawTextW(dc, shortcut, length, &cap, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
            label.right = cap.left - px(24);
        }
        DrawTextW(dc, text.c_str(), (int)(tab == text.npos ? text.size() : tab), &label,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        if (item.itemState & ODS_CHECKED) {
            auto pen = CreatePen(PS_SOLID, std::max(1, px(2)), dark ? RGB(144, 185, 255) : RGB(39, 95, 199));
            auto previous = SelectObject(dc, pen);
            LONG x = r.left + px(14), y = (r.top + r.bottom) / 2;
            POINT points[] = {{x, y}, {x + px(3), y + px(3)}, {x + px(9), y - px(4)}};
            Polyline(dc, points, 3);
            SelectObject(dc, previous);
            DeleteObject(pen);
        }
    }
    RestoreDC(dc, saved);
}
void Editor::popup(bool overflow) {
    HMENU menu = CreatePopupMenu();
    if (overflow) {
        for (size_t order = hiddenFrom; order < std::size(toolbarOrder); ++order) {
            auto i = toolbarOrder[order];
            // The three direct list actions below replace the toolbar's chooser.
            if (i + 1 == List)
                continue;
            AppendMenuW(menu, MF_STRING, i + 1, labels[i]);
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, 20, L"Save As…\tCtrl+Shift+S");
    AppendMenuW(menu, MF_STRING, 50, L"Find…\tCtrl+F");
    AppendMenuW(menu, MF_STRING, 51, L"Find and replace…\tCtrl+H");
    AppendMenuW(menu, MF_STRING, 52, L"Document outline…\tCtrl+Shift+O");
    AppendMenuW(menu, MF_STRING, 53, L"Recover draft…");
    AppendMenuW(menu, MF_STRING, 54, L"Copy image into assets folder…");
    AppendMenuW(menu, MF_STRING, 55, L"Paste image from clipboard");
    AppendMenuW(menu, MF_STRING, 56, L"Edit table at caret…");
    AppendMenuW(menu, MF_STRING | (highlighting ? MF_CHECKED : 0), 57, L"Source and code highlighting");
    AppendMenuW(menu, MF_STRING | (synchronized ? MF_CHECKED : 0), 58, L"Synchronize live preview scrolling");
    AppendMenuW(menu, MF_STRING, 59, L"Export HTML…");
    AppendMenuW(menu, MF_STRING, 60, L"Print / PDF…");
    AppendMenuW(menu, MF_STRING, 61,
                externalChanged ? L"File changed — reload from disk…" : L"Reload from disk…");
    AppendMenuW(menu, MF_STRING, 62, L"Link to heading…");
    AppendMenuW(menu, MF_STRING | (auxiliary ? 0 : MF_GRAYED), 63, L"Cancel pending action");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (SendMessageW(input, EM_CANUNDO, 0, 0) ? 0 : MF_GRAYED), 24,
                L"Undo\tCtrl+Z");
    AppendMenuW(menu, MF_STRING | (SendMessageW(input, EM_CANREDO, 0, 0) ? 0 : MF_GRAYED), 25,
                L"Redo\tCtrl+Y");
    const wchar_t *items[] = {L"Table…",
                              L"Image…",
                              L"Bulleted list\tCtrl+Shift+8",
                              L"Numbered list\tCtrl+Shift+7",
                              L"Task list\tCtrl+Shift+9",
                              L"Quote",
                              L"Code block",
                              L"Horizontal rule",
                              L"Strikethrough",
                              L"Inline code"};
    for (unsigned i = 0; i < std::size(items); ++i)
        AppendMenuW(menu, MF_STRING, 30 + i, items[i]);
    auto button = buttons[overflow ? 9 : 7];
    RECT r;
    GetWindowRect(button, &r);
    unsigned action = trackMenu(menu, TPM_LEFTALIGN, r.left, r.bottom);
    DestroyMenu(menu);
    if (action)
        command(action);
}
void Editor::command(unsigned id) {
    if (saving)
        return;
    if (auxiliary && auxiliary->kind == Auxiliary::Probe && id >= 50) {
        auxiliary->cancel = true;
        auxiliary.reset();
    }
    if (id >= 50 && id <= 63) {
        switch (id) {
        case 50:
            findReplace(false);
            break;
        case 51:
            findReplace(true);
            break;
        case 52:
            outline();
            break;
        case 53:
            recover();
            break;
        case 54: {
            wchar_t path[32768]{};
            OPENFILENAMEW dialog{sizeof(dialog)};
            dialog.hwndOwner = owner;
            dialog.lpstrFile = path;
            dialog.nMaxFile = 32768;
            dialog.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.gif;*.svg;*.bmp;*.webp\0All files\0*.*\0";
            dialog.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&dialog))
                addImage(path);
            break;
        }
        case 55:
            if (!pasteImage())
                themedMessageBox(owner, L"Copy an image or an image file first.", L"Paste image", MB_OK);
            break;
        case 56: {
            HMENU menu = CreatePopupMenu();
            const wchar_t *items[] = {L"Add row below",     L"Delete row",        L"Add column after",
                                      L"Delete column",     L"Align column left", L"Align column center",
                                      L"Align column right"};
            for (unsigned i = 0; i < std::size(items); ++i)
                AppendMenuW(menu, MF_STRING, i + 1, items[i]);
            POINT point;
            GetCursorPos(&point);
            auto action = trackMenu(menu, 0, point.x, point.y);
            DestroyMenu(menu);
            if (action)
                tableAction((TableAction)(action - 1));
            break;
        }
        case 57:
            highlighting = !highlighting;
            highlight();
            break;
        case 58:
            synchronized = !synchronized;
            break;
        case 59:
            exportDocument(false);
            break;
        case 60:
            exportDocument(true);
            break;
        case 61: {
            if (auxiliary || filePath.empty() || !ready)
                break;
            captureText();
            if (modified() &&
                themedMessageBox(owner, L"Discard this draft and reload the file from disk?",
                                 L"Reload Markdown", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES)
                break;
            auto result = std::make_shared<Auxiliary>();
            result->kind = Auxiliary::Reload;
            result->path = filePath;
            result->revision = revision;
            auxiliary = result;
            std::thread([result] {
                try {
                    result->baseline = readBaseline(result->path);
                    if (!result->baseline.exists)
                        throw std::runtime_error("The file no longer exists.");
                    auto source = decode(result->baseline.bytes);
                    if (source.find(L'\0') != source.npos)
                        throw std::runtime_error(
                            "This file contains null characters and cannot be edited safely.");
                    result->document = std::make_shared<Document>(parse(std::move(source)));
                    result->document->path = result->path;
                    std::error_code ec;
                    result->time = std::filesystem::last_write_time(result->path, ec);
                } catch (const std::exception &e) {
                    result->error = fromUtf8(e.what());
                }
                result->done.store(true, std::memory_order_release);
            }).detach();
            break;
        }
        case 62: {
            if (!rendered || checkedRevision != revision) {
                check();
                themedMessageBox(owner, L"The heading list is updating. Try again after validation finishes.",
                                 L"Link to heading", MB_OK);
                break;
            }
            HMENU menu = CreatePopupMenu();
            std::vector<std::pair<std::wstring, std::wstring>> choices;
            visitBlocks(rendered->blocks, [&](const Block &b) {
                if (b.kind == Kind::Heading && b.anchor && choices.size() < 500) {
                    choices.emplace_back(rendered->text.substr(b.start, b.length), *b.anchor);
                    AppendMenuW(menu, MF_STRING, choices.size(), choices.back().first.c_str());
                }
                return choices.size() < 500;
            });
            POINT point;
            GetCursorPos(&point);
            auto selected = trackMenu(menu, 0, point.x, point.y);
            DestroyMenu(menu);
            if (selected) {
                captureText();
                CHARRANGE r;
                SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&r);
                replace(insertMarkdown(cachedText, r.cpMin, r.cpMax, Insert::Link, 0, 0,
                                       choices[selected - 1].first, L"#" + choices[selected - 1].second));
            }
            break;
        }
        case 63:
            if (auxiliary) {
                auxiliary->cancel = true;
                std::lock_guard lock(auxiliary->printerMutex);
                if (auxiliary->printer)
                    CancelDC(auxiliary->printer);
            }
            break;
        }
        return;
    }
    if (id == Save)
        save();
    else if (id == Preview)
        togglePreview();
    else if (id == Finish)
        PostMessageW(owner, EditorFinish, 0, 0);
    else if (id == BoldAction)
        insert(Insert::Bold);
    else if (id == ItalicAction)
        insert(Insert::Italic);
    else if (id == UnderlineAction)
        insert(Insert::Underline);
    else if (id == Code)
        insert(Insert::Code);
    else if (id == ImageAction)
        insert(Insert::Image);
    else if (id == List) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 21, L"Bulleted list\tCtrl+Shift+8");
        AppendMenuW(menu, MF_STRING, 22, L"Numbered list\tCtrl+Shift+7");
        AppendMenuW(menu, MF_STRING, 23, L"Task list\tCtrl+Shift+9");
        RECT r;
        GetWindowRect(buttons[13], &r);
        auto action = trackMenu(menu, 0, r.left, r.bottom);
        DestroyMenu(menu);
        if (action)
            command(action);
    } else if (id == Live) {
        live = !live;
        previewPending = false;
        if (preview) {
            preview = false;
            restoreScroll = true;
            SetWindowTextW(buttons[1], L"Preview");
        }
        SetWindowTextW(buttons[11], live ? L"Live: on" : L"Live");
        PostMessageW(owner, EditorLayout, 0, 0);
        if (live)
            check();
        focus();
    } else if (id == Link)
        insert(Insert::Link);
    else if (id == Heading) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 40, L"Normal text");
        for (unsigned i = 1; i <= 6; ++i)
            AppendMenuW(menu, MF_STRING, 40 + i, (L"Heading " + std::to_wstring(i)).c_str());
        RECT r;
        GetWindowRect(buttons[3], &r);
        auto action = trackMenu(menu, 0, r.left, r.bottom);
        DestroyMenu(menu);
        if (action)
            insert(Insert::Heading, action - 40);
    } else if (id == MoreInsert || id == Overflow)
        popup(id == Overflow);
    else if (id == Warnings) {
        expanded = !expanded;
        PostMessageW(owner, EditorLayout, 0, 0);
    } else if (id == 20)
        save(true);
    else if (id >= 21 && id <= 23)
        insert(id == 21 ? Insert::Bullet : id == 22 ? Insert::Numbered : Insert::Task);
    else if (id == 24 || id == 25) {
        if (preview)
            togglePreview();
        SendMessageW(input, id == 24 ? EM_UNDO : EM_REDO, 0, 0);
        focus();
    } else if (id >= 30 && id <= 39) {
        const Insert kinds[] = {Insert::Table,  Insert::Image,     Insert::Bullet, Insert::Numbered,
                                Insert::Task,   Insert::Quote,     Insert::Code,   Insert::Rule,
                                Insert::Strike, Insert::InlineCode};
        insert(kinds[id - 30]);
    }
}
void Editor::togglePreview() {
    if (live) {
        live = false;
        SetWindowTextW(buttons[11], L"Live");
        PostMessageW(owner, EditorLayout, 0, 0);
    }
    if (preview) {
        preview = false;
        restoreScroll = true;
        SetWindowTextW(buttons[1], L"Preview");
        PostMessageW(owner, EditorLayout, 0, 0);
        focus();
    } else
        check(true);
}
void Editor::focus() {
    if (!preview)
        SetFocus(input);
}
void Editor::setVisible(bool visible) {
    ShowWindow(panel, visible ? SW_SHOWNA : SW_HIDE);
    if (visible)
        title();
}
bool Editor::tab(bool backward) {
    if (GetFocus() == input && GetKeyState(VK_CONTROL) >= 0) {
        if (!ready || saving || preview)
            return true;
        CHARRANGE range;
        SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&range);
        if (range.cpMin == range.cpMax && !backward)
            return false;
        auto first = sourceParagraph(input, range.cpMin);
        auto last = sourceParagraph(input, range.cpMax > range.cpMin ? range.cpMax - 1 : range.cpMax);
        if (!first || !last)
            return true;
        LONG start = first->cpMin, end = last->cpMax;
        std::wstring text((size_t)(end - start) + 1, L'\0');
        TEXTRANGEW get{{start, end}, text.data()};
        text.resize((size_t)SendMessageW(input, EM_GETTEXTRANGE, 0, (LPARAM)&get));
        text = editText(text);
        auto edit = indentMarkdown(text, 0, text.size(), backward);
        if (range.cpMin == range.cpMax) {
            auto removed = text.size() - edit.replacement.size();
            auto caret = (size_t)(range.cpMin - start);
            edit.selection = caret > removed ? caret - removed : 0;
            edit.selected = 0;
        }
        edit.start += start;
        replace(edit);
        return true;
    }
    auto next = GetNextDlgTabItem(panel, GetFocus(), backward);
    if (next)
        SetFocus(next);
    return true;
}
void Editor::find() {
    SendMessageW(owner, EditorFind, 0, 0);
}
void Editor::prepareFind() {
    if (preview) togglePreview();
    if (findDialog) DestroyWindow(findDialog);
}
std::shared_ptr<const std::wstring> Editor::findTextSnapshot() {
    captureText();
    return std::make_shared<const std::wstring>(cachedText);
}
void Editor::setReplaceFind(const std::wstring &query, SearchOptions options) {
    wcsncpy_s(findBuffer.data(), findBuffer.size(), query.c_str(), _TRUNCATE);
    replaceFindFlags = FR_DOWN | (options.matchCase ? FR_MATCHCASE : 0) | (options.wholeWord ? FR_WHOLEWORD : 0);
}
void Editor::setFindResults(std::shared_ptr<const MatchIndex> results, uint32_t active) {
    clearFindHighlights();
    searchResults = std::move(results);
    activeSearch = active;
    SendMessageW(input, EM_HIDESELECTION, searchResults != nullptr, 0);
    projectFindMarkers();
    paintScrollbars(input);
}
void Editor::revealFind(uint32_t start, uint32_t length) {
    CHARRANGE selected{(LONG)start, (LONG)(start + length)};
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&selected);
    SendMessageW(input, EM_SCROLLCARET, 0, 0);
    if (live && synchronized) PostMessageW(owner, EditorScroll, 0, 0);
}
void Editor::presentFind() {
    paintFindHighlights();
    UpdateWindow(input);
}
// Temporary TOM background effects do not enter the document's undo stream.
static void temporaryFindBackground(ITextDocument *tom, LONG start, LONG end, LONG color) {
    if (end <= start) return;
    Microsoft::WRL::ComPtr<ITextRange> range;
    Microsoft::WRL::ComPtr<ITextFont> font;
    if (SUCCEEDED(tom->Range(start, end, &range)) && SUCCEEDED(range->GetFont(&font))) {
        font->Reset(tomApplyTmp);
        font->SetBackColor(color);
        font->Reset(tomApplyNow);
    }
}
void Editor::clearFindHighlights() {
    if (searchFormatTo <= searchFormatFrom) return;
    Microsoft::WRL::ComPtr<IRichEditOle> ole;
    SendMessageW(input, EM_GETOLEINTERFACE, 0, (LPARAM)ole.GetAddressOf());
    Microsoft::WRL::ComPtr<ITextDocument> tom;
    if (ole && SUCCEEDED(ole.As(&tom))) {
        bool old = updating; updating = true;
        auto modified = SendMessageW(input, EM_GETMODIFY, 0, 0);
        tom->Undo(tomSuspend, nullptr);
        temporaryFindBackground(tom.Get(), searchFormatFrom, searchFormatTo, dark ? RGB(16, 19, 25) : RGB(250, 251, 253));
        tom->Undo(tomResume, nullptr);
        SendMessageW(input, EM_SETMODIFY, modified, 0);
        updating = old;
    }
    searchFormatFrom = searchFormatTo = 0;
}
void Editor::projectFindMarkers() {
    searchMarkerRows.clear();
    searchActiveRow = -1;
    if (!searchResults) return;
    auto track = scrollTrack(input, SB_VERT);
    int extent = std::max(1L, track.bottom - track.top - 1), previous = -1;
    for (auto line : searchResults->lines) {
        int y = (int)((uint64_t)line * extent / std::max(1u, searchResults->totalLines));
        if (y != previous) searchMarkerRows.push_back(y);
        previous = y;
    }
    if (activeSearch != UINT32_MAX && !searchResults->lines.empty()) {
        auto line = SendMessageW(input, EM_EXLINEFROMCHAR, 0, activeSearch);
        searchActiveRow = (int)(line * extent / std::max(1u, searchResults->totalLines));
    }
}
void Editor::paintFindHighlights() {
    if (paintingFind || !searchResults || preview || !ready) return;
    paintingFind = true;
    clearFindHighlights();
    Microsoft::WRL::ComPtr<IRichEditOle> ole;
    SendMessageW(input, EM_GETOLEINTERFACE, 0, (LPARAM)ole.GetAddressOf());
    Microsoft::WRL::ComPtr<ITextDocument> tom;
    if (ole && SUCCEEDED(ole.As(&tom))) {
        RECT rect; GetClientRect(input, &rect);
        POINT a{0, 0}, b{rect.right, rect.bottom};
        auto from = (LONG)SendMessageW(input, EM_CHARFROMPOS, 0, (LPARAM)&a);
        auto to = (LONG)SendMessageW(input, EM_CHARFROMPOS, 0, (LPARAM)&b) + 1;
        // Keep the horizontal viewport's starting character: returning to the
        // line start would miss matches far to the right on very long lines.
        to = std::min(to + 1, from + 32768);
        bool old = updating; updating = true;
        auto modified = SendMessageW(input, EM_GETMODIFY, 0, 0);
        tom->Undo(tomSuspend, nullptr);
        auto &hits = searchResults->offsets;
        auto length = searchResults->length;
        auto it = std::lower_bound(hits.begin(), hits.end(), from >= (LONG)length ? from - length + 1 : 0);
        for (; it != hits.end() && *it < (uint32_t)std::max(0L, to); ++it) {
            LONG end = (LONG)(*it + length);
            temporaryFindBackground(tom.Get(), (LONG)*it, end,
                                    dark ? RGB(89, 72, 30) : RGB(255, 233, 151));
            searchFormatFrom = std::min(from, (LONG)*it);
            searchFormatTo = std::max(searchFormatTo, end);
        }
        if (activeSearch != UINT32_MAX && activeSearch < (uint32_t)std::max(0L, to) &&
            activeSearch + length > (uint32_t)std::max(0L, from))
            temporaryFindBackground(tom.Get(), (LONG)activeSearch, (LONG)(activeSearch + length),
                                    dark ? RGB(140, 85, 24) : RGB(255, 182, 75));
        tom->Undo(tomResume, nullptr);
        SendMessageW(input, EM_SETMODIFY, modified, 0);
        updating = old;
    }
    paintingFind = false;
    InvalidateRect(input, nullptr, FALSE);
}
bool Editor::dialogMessage(MSG &msg) {
    return findDialog && IsDialogMessageW(findDialog, &msg);
}
void Editor::findReplace(bool replacing) {
    if (!replacing) { find(); return; }
    SendMessageW(owner, EditorReplace, 0, 0);
    if (!ready || saving)
        return;
    if (preview)
        togglePreview();
    if (findDialog)
        DestroyWindow(findDialog);
    findState = {sizeof(findState)};
    findState.hwndOwner = panel;
    findState.Flags = replaceFindFlags;
    findState.lpstrFindWhat = findBuffer.data();
    findState.wFindWhatLen = (WORD)findBuffer.size();
    findState.lpstrReplaceWith = replaceBuffer.data();
    findState.wReplaceWithLen = (WORD)replaceBuffer.size();
    findDialog = replacing ? ReplaceTextW(&findState) : FindTextW(&findState);
    if (findDialog)
        themeDialog(findDialog, dark);
}
void Editor::findAction(DWORD flags) {
    if (flags & FR_DIALOGTERM) {
        findDialog = nullptr;
        return;
    }
    if (!ready || saving || !findBuffer[0])
        return;
    auto options = flags & (FR_DOWN | FR_MATCHCASE | FR_WHOLEWORD);
    if (flags & FR_REPLACEALL) {
        if (auxiliary)
            return;
        captureText();
        auto result = std::make_shared<Auxiliary>();
        result->kind = Auxiliary::ReplaceAll;
        result->revision = revision;
        auxiliary = result;
        auto source = cachedText;
        std::wstring query = findBuffer.data(), replacement = replaceBuffer.data();
        std::thread([result, source = std::move(source), query = std::move(query),
                     replacement = std::move(replacement), options] {
            try {
                result->text = replaceMatches(source, query, replacement, !!(options & FR_MATCHCASE),
                                              !!(options & FR_WHOLEWORD), result->cancel);
            } catch (const std::exception &e) {
                result->error = fromUtf8(e.what());
            }
            result->done.store(true, std::memory_order_release);
        }).detach();
        return;
    }
    CHARRANGE selection;
    SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&selection);
    FINDTEXTEXW find{};
    find.lpstrText = findBuffer.data();
    if (flags & FR_REPLACE) {
        find.chrg = selection;
        if (SendMessageW(input, EM_FINDTEXTEXW, options | FR_DOWN, (LPARAM)&find) >= 0 &&
            find.chrgText.cpMin == selection.cpMin && find.chrgText.cpMax == selection.cpMax) {
            SendMessageW(input, EM_REPLACESEL, TRUE, (LPARAM)replaceBuffer.data());
            SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&selection);
        }
    }
    bool backward = !(flags & FR_DOWN);
    find.chrg = {backward ? selection.cpMin : selection.cpMax, backward ? 0 : -1};
    auto result = SendMessageW(input, EM_FINDTEXTEXW, options, (LPARAM)&find);
    if (result < 0) {
        captureText();
        find.chrg = {backward ? (LONG)cachedText.size() : 0, backward ? 0 : -1};
        result = SendMessageW(input, EM_FINDTEXTEXW, options, (LPARAM)&find);
    }
    if (result >= 0) {
        SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&find.chrgText);
        SendMessageW(input, EM_SCROLLCARET, 0, 0);
    } else
        themedMessageBox(owner, L"No matches.", L"Find in source", MB_OK | MB_ICONINFORMATION);
    // Keep the modeless search dialog focused for repeated keyboard searches.
}
void Editor::outline() {
    if (outlineRevision != revision) {
        check();
        themedMessageBox(owner, L"The outline is updating. Try again after validation finishes.",
                         L"Document outline", MB_OK);
        return;
    }
    if (headings.empty()) {
        themedMessageBox(owner, L"This document has no headings.", L"Document outline", MB_OK);
        return;
    }
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < std::min<size_t>(headings.size(), 500); ++i) {
        auto label = std::wstring((headings[i].level - 1) * 2, L' ') +
                     (headings[i].title.empty() ? L"Untitled heading" : headings[i].title);
        AppendMenuW(menu, MF_STRING, i + 1, label.c_str());
    }
    POINT point;
    GetCursorPos(&point);
    auto choice = trackMenu(menu, 0, point.x, point.y);
    DestroyMenu(menu);
    if (choice)
        sourceAtHeading(choice - 1);
}
void Editor::sourceAtHeading(size_t index) {
    if (outlineRevision != revision || index >= headings.size())
        return;
    if (preview)
        togglePreview();
    CHARRANGE range{(LONG)headings[index].offset, (LONG)headings[index].offset};
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&range);
    SendMessageW(input, EM_SCROLLCARET, 0, 0);
    focus();
}
bool Editor::smartEnter() {
    if (!ready || saving || preview)
        return false;
    CHARRANGE range;
    SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&range);
    if (range.cpMin != range.cpMax)
        return false;
    auto paragraph = sourceParagraph(input, range.cpMin);
    if (!paragraph)
        return false;
    LONG start = paragraph->cpMin;
    auto state = fenceAt(start);
    if (!state)
        return false;
    auto length = (size_t)(paragraph->cpMax - start);
    if (length > 32766)
        return false;
    std::wstring text(length + 2, L'\0');
    TEXTRANGEW get{{start, paragraph->cpMax}, text.data()};
    auto n = SendMessageW(input, EM_GETTEXTRANGE, 0, (LPARAM)&get);
    text.resize((size_t)n);
    // Rich Edit includes its paragraph terminator, even on the final line.
    if (text.ends_with(L'\r'))
        text.pop_back();
    auto next = scanFenceLine(text, *state);
    if (next.marker || (state->marker && !state->quotes && !state->indent))
        return false;
    auto edit = continueMarkdown(text, (size_t)(range.cpMin - start));
    if (!edit)
        return false;
    // This bounded edit does not require capturing the document on the typing path.
    edit->start += start;
    CHARRANGE target{(LONG)edit->start, (LONG)(edit->start + edit->length)};
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&target);
    SendMessageW(input, EM_STOPGROUPTYPING, 0, 0);
    changeFrom = edit->start;
    SendMessageW(input, EM_REPLACESEL, TRUE, (LPARAM)edit->replacement.c_str());
    changeFrom = SIZE_MAX;
    SendMessageW(input, EM_STOPGROUPTYPING, 0, 0);
    return true;
}
std::optional<FenceState> Editor::fenceAt(LONG start) {
    if (start < 0)
        return {};
    size_t anchor = std::min<size_t>(dirtyFrom, (size_t)start);
    auto source = std::wstring_view(fenceSource);
    if ((size_t)start <= dirtyFrom && (size_t)start <= source.size()) {
        FenceState state;
        auto it =
            std::upper_bound(fences.begin(), fences.end(), (size_t)start,
                             [](size_t offset, const FencePoint &point) { return offset < point.offset; });
        if (it != fences.begin())
            state = (--it)->state;
        return state;
    }
    anchor = std::min(anchor, source.size());
    if (anchor) {
        auto previous = source.rfind(L'\n', anchor - 1);
        anchor = previous == source.npos ? 0 : previous + 1;
    }
    FenceState state;
    if (!fences.empty()) {
        auto it =
            std::upper_bound(fences.begin(), fences.end(), anchor,
                             [](size_t offset, const FencePoint &point) { return offset < point.offset; });
        if (it != fences.begin())
            state = (--it)->state;
    }
    if ((size_t)start - anchor > 32768)
        return {};
    std::wstring text((size_t)start - anchor + 1, L'\0');
    TEXTRANGEW range{{(LONG)anchor, start}, text.data()};
    text.resize((size_t)SendMessageW(input, EM_GETTEXTRANGE, 0, (LPARAM)&range));
    for (size_t at = 0; at < text.size();) {
        auto end = text.find_first_of(L"\r\n", at);
        if (end == text.npos)
            end = text.size();
        state = scanFenceLine(std::wstring_view(text).substr(at, end - at), state);
        at = end + 1;
        if (end < text.size() && text[end] == '\r' && at < text.size() && text[at] == '\n')
            ++at;
    }
    return state;
}
void Editor::tableAction(TableAction action) {
    if (preview)
        togglePreview();
    captureText();
    CHARRANGE range;
    SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&range);
    auto edit = editTable(cachedText, range.cpMin, action);
    if (edit)
        replace(*edit);
    else
        themedMessageBox(
            owner,
            L"Place the caret in a Markdown table. Header rows and the final column cannot be deleted.",
            L"Edit table", MB_OK);
}
double Editor::sourcePosition() const {
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    if (GetScrollInfo(input, SB_VERT, &info)) {
        int last = info.nMax - (int)info.nPage + 1;
        // The first visible line at the source's end can still precede tall
        // rendered tables/images. Match document ends at this boundary only.
        // Rich Edit can clamp one pixel below the inclusive scrollbar limit.
        if (last > info.nMin && info.nPos > info.nMin && info.nPos >= last - 1)
            return (double)liveLength;
    }
    LONG line = (LONG)SendMessageW(input, EM_GETFIRSTVISIBLELINE, 0, 0);
    LONG start = (LONG)SendMessageW(input, EM_LINEINDEX, line, 0);
    LONG next = (LONG)SendMessageW(input, EM_LINEINDEX, line + 1, 0);
    POINTL a{}, b{};
    SendMessageW(input, EM_POSFROMCHAR, (WPARAM)&a, start);
    if (next < 0)
        return std::max(0L, start);
    SendMessageW(input, EM_POSFROMCHAR, (WPARAM)&b, next);
    RECT rect{};
    SendMessageW(input, EM_GETRECT, 0, (LPARAM)&rect);
    return start + std::clamp((double)(rect.top - a.y) / std::max(1L, b.y - a.y), 0., 1.) * (next - start);
}
void Editor::previewScroll(double position) {
    if (!canSynchronize() || syncing)
        return;
    LONG at = (LONG)std::clamp(position, 0., (double)liveLength);
    LONG line = (LONG)SendMessageW(input, EM_EXLINEFROMCHAR, 0, at);
    LONG start = (LONG)SendMessageW(input, EM_LINEINDEX, line, 0);
    LONG next = (LONG)SendMessageW(input, EM_LINEINDEX, line + 1, 0);
    POINTL a{}, b{};
    SendMessageW(input, EM_POSFROMCHAR, (WPARAM)&a, start);
    if (next >= 0)
        SendMessageW(input, EM_POSFROMCHAR, (WPARAM)&b, next);
    RECT rect{};
    SendMessageW(input, EM_GETRECT, 0, (LPARAM)&rect);
    POINT point{};
    SendMessageW(input, EM_GETSCROLLPOS, 0, (LPARAM)&point);
    LONG previousY = point.y;
    point.y +=
        a.y - rect.top + (next > start ? (LONG)((position - start) / (next - start) * (b.y - a.y)) : 0);
    point.y = std::max(0L, point.y);
    if (point.y == previousY)
        return;
    syncing = true;
    SendMessageW(input, EM_SETSCROLLPOS, 0, (LPARAM)&point);
    syncing = false;
}
void Editor::highlight() {
    if (preview || !ready || saving)
        return;
    Microsoft::WRL::ComPtr<IRichEditOle> ole;
    SendMessageW(input, EM_GETOLEINTERFACE, 0, (LPARAM)ole.GetAddressOf());
    Microsoft::WRL::ComPtr<ITextDocument> tom;
    if (!ole || FAILED(ole.As(&tom)))
        return;
    RECT rect;
    GetClientRect(input, &rect);
    if (rect.right < 1 || rect.bottom < 1)
        return;
    LONG first = (LONG)SendMessageW(input, EM_GETFIRSTVISIBLELINE, 0, 0);
    auto atPoint = [&](LONG x, LONG y) {
        POINT point{x, y};
        ClientToScreen(input, &point);
        Microsoft::WRL::ComPtr<ITextRange> range;
        LONG at = 0;
        if (SUCCEEDED(tom->RangeFromPoint(point.x, point.y, &range)))
            range->GetStart(&at);
        return at;
    };
    LONG bottomChar = atPoint(rect.right - 1, rect.bottom - 1);
    LONG last = std::max(first, (LONG)SendMessageW(input, EM_EXLINEFROMCHAR, 0, bottomChar));
    last = std::min(last, first + 200);
    CHARRANGE selected;
    SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&selected);
    POINT scroll{};
    SendMessageW(input, EM_GETSCROLLPOS, 0, (LPARAM)&scroll);
    bool old = updating;
    updating = true;
    LRESULT modified = SendMessageW(input, EM_GETMODIFY, 0, 0);
    tom->Undo(tomSuspend, nullptr);
    SendMessageW(input, WM_SETREDRAW, FALSE, 0);
    auto color = [&](LONG a, LONG b, COLORREF c) {
        CHARRANGE r{a, b};
        SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&r);
        CHARFORMAT2W f{sizeof(f)};
        f.dwMask = CFM_COLOR;
        f.crTextColor = c;
        SendMessageW(input, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&f);
    };
    try {
        size_t budget = 32768;
        struct VisibleRange {
            LONG lineStart, start, end;
        };
        std::vector<VisibleRange> ranges;
        for (LONG line = first; line <= last && budget; ++line) {
            LONG lineStart = (LONG)SendMessageW(input, EM_LINEINDEX, line, 0);
            if (lineStart < 0)
                break;
            LONG lineEnd = lineStart + (LONG)SendMessageW(input, EM_LINELENGTH, lineStart, 0);
            if (lineEnd <= lineStart)
                continue;
            POINTL origin{};
            Microsoft::WRL::ComPtr<ITextRange> lineRange;
            if (FAILED(tom->Range(lineStart, lineStart, &lineRange)) ||
                FAILED(
                    lineRange->GetPoint(tomStart | tomClientCoord | tomAllowOffClient, &origin.x, &origin.y)))
                continue;
            LONG y = std::clamp(origin.y + 1, 0L, rect.bottom - 1);
            LONG start = std::clamp(atPoint(1, y) - 128, lineStart, lineEnd);
            LONG end = std::clamp(atPoint(rect.right - 1, y) + 128, start, lineEnd);
            end = std::min(end, start + (LONG)budget);
            if (end == start)
                continue;
            budget -= end - start;
            ranges.push_back({lineStart, start, end});
        }
        // Selection-based formatting can reveal text. Resolve the complete viewport first.
        for (auto [lineStart, start, end] : ranges) {
            std::wstring text((size_t)(end - start) + 1, L'\0');
            TEXTRANGEW get{{start, end}, text.data()};
            text.resize((size_t)SendMessageW(input, EM_GETTEXTRANGE, 0, (LPARAM)&get));
            color(start, end, dark ? RGB(229, 233, 240) : RGB(32, 41, 56));
            auto paragraph = sourceParagraph(input, lineStart);
            auto logicalStart = paragraph ? paragraph->cpMin : lineStart;
            auto state = fenceAt(logicalStart);

            if (highlighting && state)
                for (auto &span : syntaxSpans(text, *state, start != logicalStart)) {
                    COLORREF colors[] = {dark ? RGB(144, 185, 255) : RGB(39, 95, 199),
                                         dark ? RGB(220, 176, 132) : RGB(152, 74, 20),
                                         dark ? RGB(143, 170, 144) : RGB(72, 114, 76)};
                    color(start + (LONG)span.start, start + (LONG)(span.start + span.length),
                          colors[span.color - 1]);
                }
        }
    } catch (...) {
        SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&selected);
        SendMessageW(input, EM_SETSCROLLPOS, 0, (LPARAM)&scroll);
        SendMessageW(input, EM_SETMODIFY, modified, 0);
        SendMessageW(input, WM_SETREDRAW, TRUE, 0);
        tom->Undo(tomResume, nullptr);
        updating = old;
        throw;
    }
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&selected);
    SendMessageW(input, EM_SETSCROLLPOS, 0, (LPARAM)&scroll);
    SendMessageW(input, EM_SETMODIFY, modified, 0);
    SendMessageW(input, WM_SETREDRAW, TRUE, 0);
    tom->Undo(tomResume, nullptr);
    updating = old;
    paintFindHighlights();
    InvalidateRect(input, nullptr, FALSE);
}
void Editor::checkpoint() {
    if (!recovery)
        return;
    if (saving || !ready || recoveryJob) {
        SetTimer(panel, 3, 500, nullptr);
        checkpointScheduled = true;
        return;
    }
    captureText();
    auto state = recovery;
    auto result = std::make_shared<Auxiliary>();
    result->revision = revision;
    recoveryJob = result;
    auto text = cachedText;
    auto path = linkBase();
    bool dirty = modified();
    std::thread([state, result, text = std::move(text), path, dirty] {
        try {
            if (dirty)
                state->write(text, path);
            else
                state->clear();
        } catch (const std::exception &e) {
            result->error = fromUtf8(e.what());
        }
        result->done.store(true, std::memory_order_release);
    }).detach();
}
void Editor::recover() {
    if (auxiliary || !ready)
        return;
    captureText();
    if (modified()) {
        themedMessageBox(owner, L"Save or finish this draft, then open a new file to recover another draft.",
                         L"Recover draft", MB_OK);
        return;
    }
    auto result = std::make_shared<Auxiliary>();
    result->kind = Auxiliary::Recover;
    result->revision = revision;
    auxiliary = result;
    std::thread([result] {
        try {
            result->drafts = DraftRecovery::list();
        } catch (const std::exception &e) {
            result->error = fromUtf8(e.what());
        }
        result->done.store(true, std::memory_order_release);
    }).detach();
}
void Editor::exportDocument(bool print) {
    if (auxiliary || !ready || saving)
        return;
    captureText();
    if (print) {
        PRINTDLGW dialog{sizeof(dialog)};
        dialog.hwndOwner = owner;
        dialog.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE | PD_NOPAGENUMS | PD_NOSELECTION;
        if (!themedPrint(&dialog))
            return;
        bool pdf = false;
        if (dialog.hDevNames) {
            auto names = (DEVNAMES *)GlobalLock(dialog.hDevNames);
            if (names) {
                pdf = wcsstr((wchar_t *)names + names->wDeviceOffset, L"Microsoft Print to PDF") != nullptr;
                GlobalUnlock(dialog.hDevNames);
            }
        }
        if (dialog.hDevMode)
            GlobalFree(dialog.hDevMode);
        if (dialog.hDevNames)
            GlobalFree(dialog.hDevNames);
        std::filesystem::path destination;
        if (pdf) {
            wchar_t path[32768] = L"Document.pdf";
            OPENFILENAMEW chooser{sizeof(chooser)};
            chooser.hwndOwner = owner;
            chooser.lpstrFile = path;
            chooser.nMaxFile = 32768;
            chooser.lpstrFilter = L"PDF document\0*.pdf\0";
            chooser.lpstrDefExt = L"pdf";
            chooser.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (!themedSaveFile(&chooser)) {
                DeleteDC(dialog.hDC);
                return;
            }
            destination = path;
            auto extension = destination.extension().wstring();
            CharLowerBuffW(extension.data(), (DWORD)extension.size());
            if (extension != L".pdf") {
                DeleteDC(dialog.hDC);
                themedMessageBox(owner, L"Choose a filename ending in .pdf.", L"Print / PDF", MB_OK);
                return;
            }
            if (!filePath.empty()) {
                std::error_code ec;
                if (std::filesystem::equivalent(filePath, destination, ec)) {
                    DeleteDC(dialog.hDC);
                    themedMessageBox(owner, L"Choose a separate PDF file to keep the source intact.",
                                     L"Print / PDF", MB_OK);
                    return;
                }
            }
        }
        auto result = std::make_shared<Auxiliary>();
        result->kind = Auxiliary::Export;
        result->print = true;
        result->path = destination;
        auxiliary = result;
        auto text = cachedText;
        auto original = linkBase();
        HDC printer = dialog.hDC;
        result->printer = printer;
        std::thread([result, text = std::move(text), original, printer] {
            try {
                auto document = parse(text, &result->cancel);
                document.path = original;
                printDocument(document, printer, result->path, &result->cancel);
                result->changed = true;
            } catch (const std::exception &e) {
                result->error = fromUtf8(e.what());
            }
            {
                std::lock_guard lock(result->printerMutex);
                DeleteDC(printer);
                result->printer = nullptr;
            }
            result->done.store(true, std::memory_order_release);
        }).detach();
        return;
    }
    wchar_t path[32768]{};
    {
        auto name = filePath.empty() ? L"Untitled.html" : filePath.stem().wstring() + L".html";
        wcsncpy_s(path, name.c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = owner;
        dialog.lpstrFile = path;
        dialog.nMaxFile = 32768;
        dialog.lpstrFilter = L"HTML document\0*.html\0";
        dialog.lpstrDefExt = L"html";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!themedSaveFile(&dialog))
            return;
        if (!filePath.empty()) {
            std::error_code ec;
            if (std::filesystem::equivalent(filePath, std::filesystem::path(path), ec) ||
                CompareStringOrdinal(std::filesystem::absolute(filePath).lexically_normal().c_str(), -1,
                                     std::filesystem::absolute(path).lexically_normal().c_str(), -1,
                                     TRUE) == CSTR_EQUAL) {
                themedMessageBox(owner, L"Choose a separate HTML file to keep the Markdown source intact.",
                                 L"Export HTML", MB_OK | MB_ICONINFORMATION);
                return;
            }
        }
    }
    auto result = std::make_shared<Auxiliary>();
    result->kind = Auxiliary::Export;
    result->path = path;
    auxiliary = result;
    auto text = cachedText;
    auto original = linkBase();
    std::thread([result, text = std::move(text), original] {
        try {
            auto document = parse(text, &result->cancel);
            document.path = original;
            auto bytes = exportHtml(document);
            if (result->cancel.load())
                throw std::runtime_error("Export cancelled.");
            FileBaseline expected = readBaseline(result->path);
            auto saved = saveDraft(result->path, fromUtf8(bytes), expected, {}, true);
            if (saved.status != SaveResult::Saved)
                throw std::runtime_error("Could not export HTML. The destination may have changed.");
            result->changed = true;
        } catch (const std::exception &e) {
            result->error = fromUtf8(e.what());
        }
        result->done.store(true, std::memory_order_release);
    }).detach();
}
bool Editor::addImage(const std::filesystem::path &path) {
    auto ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
    if (ext != L".png" && ext != L".jpg" && ext != L".jpeg" && ext != L".gif" && ext != L".svg" &&
        ext != L".bmp" && ext != L".webp")
        return false;
    if (auxiliary && auxiliary->kind == Auxiliary::Probe) {
        auxiliary->cancel = true;
        auxiliary.reset();
    }
    if (auxiliary || !ready || saving) {
        themedMessageBox(owner, L"Another editor action is still running. Add the image when it finishes.",
                         L"Add image", MB_OK);
        return true;
    }
    if (filePath.empty()) {
        themedMessageBox(owner, L"Save the Markdown file first so its images have a stable assets folder.",
                         L"Add image", MB_OK);
        return true;
    }
    auto result = std::make_shared<Auxiliary>();
    result->kind = Auxiliary::Image;
    auxiliary = result;
    auto document = filePath;
    std::thread([result, path, document] {
        try {
            result->path = copyImageAsset(path, document);
        } catch (const std::exception &e) {
            result->error = fromUtf8(e.what());
        }
        result->done.store(true, std::memory_order_release);
        if (result->cancel.load() && !result->path.empty()) {
            std::error_code ec;
            std::filesystem::remove(result->path, ec);
        }
    }).detach();
    return true;
}
bool Editor::pasteImage() {
    if (auxiliary && auxiliary->kind == Auxiliary::Probe) {
        auxiliary->cancel = true;
        auxiliary.reset();
    }
    if (!ready || saving)
        return false;
    UINT png = RegisterClipboardFormatW(L"PNG");
    if (!IsClipboardFormatAvailable(CF_DIB) && !IsClipboardFormatAvailable(png) &&
        !IsClipboardFormatAvailable(CF_HDROP))
        return false;
    if (auxiliary) {
        themedMessageBox(owner, L"Another editor action is still running. Paste the image when it finishes.",
                         L"Paste image", MB_OK);
        return true;
    }
    if (!OpenClipboard(panel))
        return false;
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        auto drop = (HDROP)GetClipboardData(CF_HDROP);
        wchar_t path[32768]{};
        DragQueryFileW(drop, 0, path, 32768);
        CloseClipboard();
        return addImage(path);
    }
    if (filePath.empty()) {
        CloseClipboard();
        themedMessageBox(owner, L"Save the Markdown file before pasting an image.", L"Paste image", MB_OK);
        return true;
    }
    bool isPng = IsClipboardFormatAvailable(png) != FALSE;
    auto data = GetClipboardData(isPng ? png : CF_DIB);
    size_t size = data ? GlobalSize(data) : 0;
    auto ptr = data ? GlobalLock(data) : nullptr;
    std::vector<BYTE> bytes;
    if (ptr && size && size <= 64 * 1024 * 1024)
        bytes.assign((BYTE *)ptr, (BYTE *)ptr + size);
    if (ptr)
        GlobalUnlock(data);
    CloseClipboard();
    if (bytes.empty())
        throw std::runtime_error("The clipboard image is unavailable or larger than 64 MiB.");
    auto result = std::make_shared<Auxiliary>();
    result->kind = Auxiliary::Image;
    auxiliary = result;
    auto document = filePath;
    std::thread([result, document, bytes = std::move(bytes), isPng]() mutable {
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            using Microsoft::WRL::ComPtr;
            auto require = [](HRESULT status) {
                if (FAILED(status))
                    throw std::runtime_error("Windows could not encode the clipboard image.");
            };
            if (!isPng) {
                if (bytes.size() < sizeof(BITMAPINFOHEADER))
                    throw std::runtime_error("Invalid clipboard bitmap.");
                auto &header = *(BITMAPINFOHEADER *)bytes.data();
                if (header.biSize < sizeof(header) || header.biSize > bytes.size() || header.biWidth <= 0 ||
                    header.biWidth > 16384 || header.biHeight == 0 || header.biHeight < -16384 ||
                    header.biHeight > 16384 ||
                    (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS))
                    throw std::runtime_error("Unsupported clipboard bitmap dimensions or encoding.");
                size_t colors = header.biClrUsed ? header.biClrUsed
                                                 : (header.biBitCount <= 8 ? (1u << header.biBitCount) : 0);
                if (colors > 256)
                    throw std::runtime_error("Invalid clipboard bitmap palette.");
                size_t offset = header.biSize + colors * 4 +
                                ((header.biSize == 40 && header.biCompression == BI_BITFIELDS) ? 12 : 0);
                if (offset > bytes.size())
                    throw std::runtime_error("Incomplete clipboard bitmap.");
                BITMAPFILEHEADER file{0x4d42, (DWORD)(sizeof(BITMAPFILEHEADER) + bytes.size()), 0, 0,
                                      (DWORD)(sizeof(BITMAPFILEHEADER) + offset)};
                bytes.insert(bytes.begin(), (BYTE *)&file, (BYTE *)&file + sizeof(file));
            }
            ComPtr<IWICImagingFactory> factory;
            require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&factory)));
            ComPtr<IStream> inputStream;
            inputStream.Attach(SHCreateMemStream(bytes.data(), (UINT)bytes.size()));
            if (!inputStream)
                throw std::runtime_error("Could not read clipboard image.");
            ComPtr<IWICBitmapDecoder> decoder;
            require(factory->CreateDecoderFromStream(inputStream.Get(), nullptr,
                                                     WICDecodeMetadataCacheOnDemand, &decoder));
            ComPtr<IWICBitmapFrameDecode> frame;
            require(decoder->GetFrame(0, &frame));
            UINT width = 0, height = 0;
            require(frame->GetSize(&width, &height));
            if (!width || !height || width > 16384 || height > 16384 ||
                (uint64_t)width * height > 32 * 1024 * 1024)
                throw std::runtime_error("Clipboard image exceeds the 32 megapixel limit.");
            auto folder =
                std::filesystem::absolute(document).parent_path() / (document.stem().wstring() + L".assets");
            std::filesystem::create_directories(folder);
            static std::atomic_uint counter = 0;
            result->path =
                folder / (L"image-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                          std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++counter) + L".png");
            ComPtr<IWICStream> stream;
            require(factory->CreateStream(&stream));
            require(stream->InitializeFromFilename(result->path.c_str(), GENERIC_WRITE));
            ComPtr<IWICBitmapEncoder> encoder;
            require(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
            require(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
            ComPtr<IWICBitmapFrameEncode> output;
            require(encoder->CreateNewFrame(&output, nullptr));
            require(output->Initialize(nullptr));
            require(output->SetSize(width, height));
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
            require(output->SetPixelFormat(&format));
            require(output->WriteSource(frame.Get(), nullptr));
            require(output->Commit());
            require(encoder->Commit());
        } catch (const std::exception &e) {
            result->error = fromUtf8(e.what());
            if (!result->path.empty()) {
                std::error_code ec;
                std::filesystem::remove(result->path, ec);
            }
        }
        if (SUCCEEDED(com))
            CoUninitialize();
        result->done.store(true, std::memory_order_release);
        if (result->cancel.load() && !result->path.empty()) {
            std::error_code ec;
            std::filesystem::remove(result->path, ec);
        }
    }).detach();
    return true;
}
void Editor::toolsPoll() {
    if (recoveryJob && recoveryJob->done.load(std::memory_order_acquire)) {
        auto result = std::move(recoveryJob);
        if (!result->error.empty() && !recoveryErrorShown) {
            recoveryErrorShown = true;
            themedMessageBox(
                owner,
                (L"Your draft is still open, but crash recovery could not be updated.\n\n" + result->error)
                    .c_str(),
                L"Draft recovery", MB_OK | MB_ICONWARNING);
        } else if (result->error.empty())
            recoveryErrorShown = false;
        if (result->revision != revision) {
            SetTimer(panel, 3, 500, nullptr);
            checkpointScheduled = true;
        }
    }
    if (!auxiliary || !auxiliary->done.load(std::memory_order_acquire))
        return;
    if (saving && (auxiliary->kind == Auxiliary::Image || auxiliary->kind == Auxiliary::ReplaceAll ||
                   auxiliary->kind == Auxiliary::Reload || auxiliary->kind == Auxiliary::Recover ||
                   auxiliary->kind == Auxiliary::Restore))
        return;
    auto result = std::move(auxiliary);
    if (result->cancel.load() && !result->changed) {
        if (result->kind == Auxiliary::Image && !result->path.empty()) {
            std::error_code ec;
            std::filesystem::remove(result->path, ec);
        }
        return;
    }
    if (!result->error.empty()) {
        themedMessageBox(owner, result->error.c_str(), L"Markdown editor", MB_OK | MB_ICONERROR);
        return;
    }
    if (result->kind == Auxiliary::Image) {
        if (preview)
            togglePreview();
        captureText();
        CHARRANGE range;
        SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&range);
        auto relative = result->path.lexically_relative(std::filesystem::absolute(filePath).parent_path());
        replace(insertMarkdown(cachedText, range.cpMin, range.cpMax, Insert::Image, 0, 0,
                               L"Image description",
                               markdownFileDestination(relative.empty() ? result->path : relative)));
    } else if (result->kind == Auxiliary::Export) {
        if (result->print) {
            themedMessageBox(owner, L"The document has been sent to the selected printer.", L"Print / PDF",
                             MB_OK);
        } else
            themedMessageBox(owner, L"HTML document exported.", L"Export HTML", MB_OK);
    } else if (result->kind == Auxiliary::Probe) {
        if (result->changed && !externalChanged) {
            externalChanged = true;
            SetWindowTextW(buttons[8], L"File changed");
            if (GetForegroundWindow() == owner)
                themedMessageBox(
                    owner,
                    L"The file changed outside Voltura Markdown. Use Insert → Reload from disk to "
                    L"reload it. Saving will still check for conflicts.",
                    L"File changed", MB_OK | MB_ICONINFORMATION);
        }
    } else if (result->kind == Auxiliary::ReplaceAll) {
        if (result->revision != revision) {
            themedMessageBox(
                owner,
                L"The source changed while replacements were being prepared. Run Replace All again "
                L"to apply it to the current draft.",
                L"Replace all", MB_OK);
            return;
        }
        captureText();
        if (preview)
            togglePreview();
        replace({0, cachedText.size(), std::move(result->text), 0, 0});
    } else if (result->kind == Auxiliary::Reload) {
        if (result->revision != revision || saving)
            return;
        if (preview)
            togglePreview();
        updating = true;
        auto text = editText(decode(result->baseline.bytes));
        SETTEXTEX set{ST_DEFAULT, 1200};
        SendMessageW(input, EM_SETTEXTEX, (WPARAM)&set, (LPARAM)text.c_str());
        SendMessageW(input, EM_EMPTYUNDOBUFFER, 0, 0);
        updating = false;
        baseline = std::move(result->baseline);
        baselineText = cachedText = std::move(text);
        resetCapture();
        diskTime = result->time;
        diskSize = baseline.bytes.size();
        textCurrent = true;
        externalChanged = false;
        needsFirstSave = false;
        ++revision;
        dirtyFrom = 0;
        rendered = result->document;
        PostMessageW(owner, EditorSaved, 0, 0);
        check();
        title();
        if (recovery) {
            auto state = recovery;
            retiredRecovery.push_back(state);
            recovery = std::make_shared<DraftRecovery>();
            std::thread([state] { state->clear(true); }).detach();
        }
    } else if (result->kind == Auxiliary::Recover) {
        if (result->revision != revision || modified() || saving)
            return;
        if (result->drafts.empty()) {
            themedMessageBox(owner, L"No recoverable drafts were found.", L"Recover draft", MB_OK);
            return;
        }
        HMENU menu = CreatePopupMenu();
        for (size_t i = 0; i < result->drafts.size(); ++i) {
            auto &d = result->drafts[i];
            auto label = d.original.empty() ? L"Untitled" : d.original.wstring();
            AppendMenuW(menu, MF_STRING, i + 1, label.c_str());
        }
        POINT point;
        GetCursorPos(&point);
        auto choice = trackMenu(menu, 0, point.x, point.y);
        DestroyMenu(menu);
        if (!choice)
            return;
        auto selected = result->drafts[choice - 1].snapshot;
        auto restore = std::make_shared<Auxiliary>();
        restore->kind = Auxiliary::Restore;
        restore->revision = revision;
        auxiliary = restore;
        std::thread([restore, selected] {
            try {
                restore->drafts.push_back(DraftRecovery::read(selected));
            } catch (const std::exception &e) {
                restore->error = fromUtf8(e.what());
            }
            restore->done.store(true, std::memory_order_release);
        }).detach();
    } else if (result->kind == Auxiliary::Restore) {
        if (result->revision != revision || modified() || saving || result->drafts.empty())
            return;
        auto &draft = result->drafts.front();
        if (draft.text.find(L'\0') != draft.text.npos)
            throw std::runtime_error(
                "This recovery file contains null characters and cannot be edited safely.");
        if (preview)
            togglePreview();
        updating = true;
        SETTEXTEX set{ST_DEFAULT, 1200};
        SendMessageW(input, EM_SETTEXTEX, (WPARAM)&set, (LPARAM)draft.text.c_str());
        SendMessageW(input, EM_EMPTYUNDOBUFFER, 0, 0);
        updating = false;
        recoveredSnapshot = draft.snapshot;
        recoveredBase = draft.original;
        filePath.clear();
        baseline = {};
        baselineText.clear();
        cachedText = std::move(draft.text);
        resetCapture();
        textCurrent = true;
        needsFirstSave = true;
        ++revision;
        dirtyFrom = 0;
        check();
        title();
        checkpoint();
        themedMessageBox(owner,
                         L"Recovered into an unsaved document. Use Save As to choose its destination; the "
                         L"original file has not been changed.",
                         L"Recovered draft", MB_OK);
    }
}
bool Editor::key(WPARAM key) {
    if ((key == VK_ESCAPE || key == VK_F3) && SendMessageW(owner, EditorFindKey, key, 0)) return true;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (GetKeyState(VK_MENU) < 0)
        return false;
    if (ctrl && shift && key == 'O') {
        outline();
        return true;
    }
    if (ctrl && key == 'H') {
        findReplace(true);
        return true;
    }
    if (ctrl && key == 'V' && !shift && GetFocus() == input) {
        if (pasteImage())
            return true;
        SendMessageW(input, EM_PASTESPECIAL, CF_UNICODETEXT, 0);
        return true;
    }
    if (!ctrl && key == VK_RETURN && GetFocus() == input) {
        if (shift) {
            // Rich Edit's soft break is not a Markdown line boundary. Keep source
            // paragraphs and fence parsing aligned, without smart list continuation.
            if (ready && !saving && !preview)
                SendMessageW(input, EM_REPLACESEL, TRUE, (LPARAM)L"\r");
            return true;
        }
        if (smartEnter())
            return true;
    }
    if (ctrl && shift) {
        if (key >= '0' && key <= '6') {
            insert(Insert::Heading, (unsigned)(key - '0'));
            return true;
        }
        if (key == '7' || key == '8' || key == '9') {
            command(key == '7' ? 22 : key == '8' ? 21 : 23);
            return true;
        }
        if (key == 'C') {
            insert(Insert::Code);
            return true;
        }
        if (key == 'I') {
            insert(Insert::Image);
            return true;
        }
        if (key == 'K') {
            insert(Insert::InlineCode);
            return true;
        }
        if (key == 'Q') {
            insert(Insert::Quote);
            return true;
        }
        if (key == 'X') {
            insert(Insert::Strike);
            return true;
        }
        if (key == 'L') {
            command(Live);
            return true;
        }
    }
    if (ctrl && key == 'S') {
        save(shift);
        return true;
    }
    if (ctrl && key == 'B') {
        insert(Insert::Bold);
        return true;
    }
    if (ctrl && key == 'I') {
        insert(Insert::Italic);
        return true;
    }
    if (ctrl && key == 'U') {
        insert(Insert::Underline);
        return true;
    }
    if (ctrl && key == 'K') {
        insert(Insert::Link);
        return true;
    }
    if (ctrl && shift && key == 'V') {
        togglePreview();
        return true;
    }
    if (ctrl && key == 'F') {
        find();
        return true;
    }
    if (ctrl && key == 'E') {
        focus();
        return true;
    }
    if (key == VK_ESCAPE) {
        if (preview)
            togglePreview();
        else
            PostMessageW(owner, EditorFinish, 0, 0);
        return true;
    }
    return false;
}
RECT Editor::scrollTrack(HWND window, int axis) const {
    RECT client{}, bounds{};
    GetClientRect(window, &client);
    GetWindowRect(window, &bounds);
    POINT origin{};
    ClientToScreen(window, &origin);
    OffsetRect(&client, origin.x - bounds.left, origin.y - bounds.top);
    if (axis == SB_VERT)
        return {client.right, client.top, bounds.right - bounds.left, client.bottom};
    return {client.left, client.bottom, client.right, bounds.bottom - bounds.top};
}
RECT Editor::scrollThumb(HWND window, int axis) const {
    RECT track = scrollTrack(window, axis);
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    GetScrollInfo(window, axis, &info);
    int length = std::max(0L, axis == SB_VERT ? track.bottom - track.top : track.right - track.left);
    double range = std::max(1., (double)info.nMax - info.nMin + 1);
    int size = std::clamp((int)(info.nPage / range * length), std::min(length, MulDiv(28, dpi, 96)), length);
    int travel = length - size;
    int pos = (int)((info.nPos - info.nMin) / std::max(1., range - info.nPage) * travel);
    int width = std::max(1, MulDiv(6, dpi, 96));
    if (axis == SB_VERT) {
        int left = (track.left + track.right - width) / 2;
        return {left, track.top + pos, left + width, track.top + pos + size};
    }
    int top = (track.top + track.bottom - width) / 2;
    return {track.left + pos, top, track.left + pos + size, top + width};
}
void Editor::updateScrollRegion(HWND window) {
    if (!window || settingScrollRegion)
        return;
    RECT client{}, bounds{};
    GetClientRect(window, &client);
    GetWindowRect(window, &bounds);
    POINT origin{};
    ClientToScreen(window, &origin);
    OffsetRect(&client, origin.x - bounds.left, origin.y - bounds.top);
    auto region = CreateRectRgnIndirect(&client), current = CreateRectRgn(0, 0, 0, 0);
    bool same = GetWindowRgn(window, current) != ERROR && EqualRgn(region, current);
    DeleteObject(current);
    if (same) {
        DeleteObject(region);
        return;
    }
    settingScrollRegion = true;
    if (!SetWindowRgn(window, region, TRUE))
        DeleteObject(region);
    settingScrollRegion = false;
    // Repaint the parent-owned tracks exposed by clipping away the native bars.
    MapWindowPoints(nullptr, panel, (POINT *)&bounds, 2);
    InvalidateRect(panel, &bounds, FALSE);
}
void Editor::paintScrollbars(HWND window, HDC target) const {
    if (!window || (!target && !(GetWindowLongW(window, GWL_STYLE) & WS_VISIBLE)))
        return;
    HDC dc = target ? target : GetDC(panel);
    if (!dc)
        return;
    int saved = SaveDC(dc);
    if (!target) {
        RECT bounds{};
        GetWindowRect(window, &bounds);
        MapWindowPoints(nullptr, panel, (POINT *)&bounds, 2);
        OffsetViewportOrgEx(dc, bounds.left, bounds.top, nullptr);
    }
    auto bg = CreateSolidBrush(dark ? RGB(16, 19, 25) : RGB(250, 251, 253));
    LONG style = GetWindowLongW(window, GWL_STYLE);
    for (int axis : {SB_VERT, SB_HORZ}) {
        if (!(style & (axis == SB_VERT ? WS_VSCROLL : WS_HSCROLL)))
            continue;
        auto track = scrollTrack(window, axis);
        SCROLLBARINFO state{sizeof(state)};
        bool unavailable = GetScrollBarInfo(window, axis == SB_VERT ? OBJID_VSCROLL : OBJID_HSCROLL, &state) &&
                           (state.rgstate[0] & STATE_SYSTEM_UNAVAILABLE);
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE};
        bool visible = !unavailable && GetScrollInfo(window, axis, &info) &&
                       info.nMax - info.nMin + 1 > (int)info.nPage;
        auto thumb = scrollThumb(window, axis);
        bool active = scrollWindow == window && scrollAxis == axis;
        RECT bounds{};
        GetWindowRect(window, &bounds);
        MapWindowPoints(nullptr, panel, (POINT *)&bounds, 2);
        std::array<int64_t, 14> key{track.left, track.top, track.right, track.bottom,
                                   thumb.left, thumb.top, thumb.right, thumb.bottom,
                                   bounds.left, bounds.top, visible, active, dark, dpi};
        auto &cached = paintedScrollbars[(window == input ? 0 : 2) + (axis == SB_VERT ? 0 : 1)];
        bool markers = window == input && axis == SB_VERT && searchResults;
        if (!target && !markers && cached == key)
            continue;
        // Each track is a narrow surface: never expose the erased track before
        // the thumb, including on wheel messages at either scroll boundary.
        ChromePaint buffered(dc, track);
        auto trackDC = buffered.dc();
        if (!target) {
            if (markers) cached.reset();
            else cached = key;
        }
#ifdef VMD_TESTING
        ++scrollbarPaints;
#endif
        FillRect(trackDC, &track, bg);
        if (!visible)
            continue;
        if (window == input && axis == SB_VERT && searchResults) {
            auto marker = CreateSolidBrush(RGB(231, 181, 75));
            for (auto y : searchMarkerRows) {
                RECT tick{track.left, track.top + y, track.right, track.top + y + std::max(1, MulDiv(2, dpi, 96))};
                FillRect(trackDC, &tick, marker);
            }
            DeleteObject(marker);
            if (searchActiveRow >= 0) {
                int y = track.top + searchActiveRow;
                RECT tick{track.left, y, track.right, y + std::max(2, MulDiv(3, dpi, 96))};
                marker = CreateSolidBrush(RGB(231, 150, 50)); FillRect(trackDC, &tick, marker); DeleteObject(marker);
            }
        }
        auto brush = CreateSolidBrush(active ? (dark ? RGB(144, 185, 255) : RGB(39, 95, 199))
                                             : (dark ? RGB(146, 158, 175) : RGB(102, 117, 138)));
        auto oldBrush = SelectObject(trackDC, brush);
        auto oldPen = SelectObject(trackDC, GetStockObject(NULL_PEN));
        int diameter = std::max(2, MulDiv(6, dpi, 96));
        RoundRect(trackDC, thumb.left, thumb.top, thumb.right, thumb.bottom, diameter, diameter);
        SelectObject(trackDC, oldPen);
        SelectObject(trackDC, oldBrush);
        DeleteObject(brush);
    }
    if ((style & WS_VSCROLL) && (style & WS_HSCROLL)) {
        auto v = scrollTrack(window, SB_VERT), h = scrollTrack(window, SB_HORZ);
        RECT corner{v.left, h.top, v.right, h.bottom};
        FillRect(dc, &corner, bg);
    }
    DeleteObject(bg);
    RestoreDC(dc, saved);
    if (!target)
        ReleaseDC(panel, dc);
    if (window == input && !target) {
        paintLineNumbers();
        paintSplit();
    }
}
void Editor::paintLineNumbers(HDC target) const {
    if (!input || preview || !(GetWindowLongW(input, GWL_STYLE) & WS_VISIBLE))
        return;
    RECT client{};
    GetClientRect(input, &client);
    MapWindowPoints(input, panel, (POINT *)&client, 2);
    RECT gutter{MulDiv(12, dpi, 96), client.top, client.left, client.bottom};
    POINT scroll{};
    SendMessageW(input, EM_GETSCROLLPOS, 0, (LPARAM)&scroll);
    std::array<uint64_t, 8> state{revision, (uint64_t)scroll.y, (uint64_t)client.left,
                                 (uint64_t)client.top, (uint64_t)client.right,
                                 (uint64_t)client.bottom, dpi, dark};
    // Rich Edit sends paint/timer/scroll messages even at an unchanged boundary.
    // Only a parent paint (exposure) or a changed viewport/text needs presentation.
    if (!target && (paintedLineNumbers == state || !IsWindowVisible(input)))
        return;
    HDC destination = target ? target : GetDC(panel);
    if (!destination)
        return;
    {
        ChromePaint buffered(destination, gutter);
        HDC dc = buffered.dc();
        int saved = SaveDC(dc);
        IntersectClipRect(dc, gutter.left, gutter.top, gutter.right, gutter.bottom);
        FillRect(dc, &gutter, background);
        SelectObject(dc, mono);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, dark ? RGB(146, 158, 175) : RGB(102, 117, 138));
        Microsoft::WRL::ComPtr<IRichEditOle> ole;
        SendMessageW(input, EM_GETOLEINTERFACE, 0, (LPARAM)ole.GetAddressOf());
        Microsoft::WRL::ComPtr<ITextDocument> document;
        Microsoft::WRL::ComPtr<ITextRange> range;
        LONG first = (LONG)SendMessageW(input, EM_GETFIRSTVISIBLELINE, 0, 0);
        LONG at = (LONG)SendMessageW(input, EM_LINEINDEX, first, 0);
        if (at >= 0 && ole && SUCCEEDED(ole.As(&document)) &&
            SUCCEEDED(document->Range(at, at, &range))) {
            LONG number = 0, start = 0, end = 0;
            // TOM paragraph indices count hard line breaks, never display wraps.
            if (SUCCEEDED(range->GetIndex(tomParagraph, &number)) &&
                SUCCEEDED(range->Expand(tomParagraph, nullptr))) {
                range->GetStart(&start);
                range->GetEnd(&end);
                TEXTMETRICW metrics{};
                GetTextMetricsW(dc, &metrics);
                for (LONG line = first; ; ++line) {
                    LONG offset = (LONG)SendMessageW(input, EM_LINEINDEX, line, 0);
                    if (offset < 0)
                        break;
                    POINTL point{};
                    SendMessageW(input, EM_POSFROMCHAR, (WPARAM)&point, offset);
                    LONG y = client.top + point.y;
                    if (y >= client.bottom)
                        break;
                    if (offset >= end) {
                        ++number;
                        range->SetRange(offset, offset);
                        range->Expand(tomParagraph, nullptr);
                        range->GetStart(&start);
                        range->GetEnd(&end);
                    }
                    if (offset != start)
                        continue;
                    auto label = std::to_wstring(number);
                    RECT row{gutter.left, y, gutter.right - MulDiv(10, dpi, 96),
                             y + metrics.tmHeight};
                    DrawTextW(dc, label.c_str(), (int)label.size(), &row,
                              DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
                }
            }
        }
        RestoreDC(dc, saved);
    }
    if (!target)
        paintedLineNumbers = state;
#ifdef VMD_TESTING
    ++lineNumberPaints;
#endif
    if (!target)
        ReleaseDC(panel, destination);
}
void Editor::dragScrollbar(POINT point) {
    if (!scrollWindow)
        return;
    RECT bounds{};
    GetWindowRect(scrollWindow, &bounds);
    auto track = scrollTrack(scrollWindow, scrollAxis), thumb = scrollThumb(scrollWindow, scrollAxis);
    bool vertical = scrollAxis == SB_VERT;
    int travel = vertical ? track.bottom - track.top - (thumb.bottom - thumb.top)
                          : track.right - track.left - (thumb.right - thumb.left);
    int offset = vertical ? point.y - bounds.top - track.top : point.x - bounds.left - track.left;
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE};
    GetScrollInfo(scrollWindow, scrollAxis, &info);
    int maximum = std::max(info.nMin, info.nMax - (int)info.nPage + 1);
    int pos = info.nMin + (int)((double)std::clamp(offset - scrollGrab, 0, std::max(0, travel)) /
                                std::max(1, travel) * (maximum - info.nMin));
    if (scrollWindow == input) {
        POINT source{};
        SendMessageW(input, EM_GETSCROLLPOS, 0, (LPARAM)&source);
        (vertical ? source.y : source.x) = pos;
        SendMessageW(input, EM_SETSCROLLPOS, 0, (LPARAM)&source);
        if (live && synchronized && !syncing)
            PostMessageW(owner, EditorScroll, 0, 0);
    } else
        SendMessageW(scrollWindow, LB_SETTOPINDEX, pos, 0);
    paintScrollbars(scrollWindow);
}
LRESULT CALLBACK Editor::inputProc(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto editor = (Editor *)data;
    try {
        bool scrollable = window == editor->input || window == editor->warningList;
        // Owner-drawn buttons paint every pixel in one buffered operation.
        // The native erase would otherwise expose an empty button first.
        if (!scrollable && msg == WM_ERASEBKGND)
            return 1;
        if (scrollable && msg == WM_NCPAINT)
            return 0; // The native non-client area is outside the window region.
        if (window == editor->input && msg == WM_DROPFILES) {
            auto drop = (HDROP)wp;
            wchar_t path[32768]{};
            DragQueryFileW(drop, 0, path, 32768);
            if (editor->addImage(path)) {
                DragFinish(drop);
                return 0;
            }
            return SendMessageW(editor->owner, WM_DROPFILES, wp, lp);
        }
        if (window != editor->input && msg == WM_MOUSEMOVE) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            InvalidateRect(window, nullptr, FALSE);
        }
        if (window != editor->input && (msg == WM_MOUSELEAVE || msg == WM_SETFOCUS || msg == WM_KILLFOCUS))
            InvalidateRect(window, nullptr, FALSE);
        if (msg == WM_KEYDOWN) {
            editor->suppressCharacter = false;
            if (editor->key(wp)) {
                editor->suppressCharacter = true;
                return 0;
            }
            bool ctrl = GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0;
            if (wp == VK_F1 || wp == VK_F11 || (ctrl && (wp == 'N' || wp == 'O'))) {
                SendMessageW(editor->owner, WM_KEYDOWN, wp, lp);
                return 0;
            }
        }
        if (msg == WM_CHAR && editor->suppressCharacter) {
            editor->suppressCharacter = false;
            return 0;
        }
        if (window == editor->input && msg == WM_PASTE) {
            if (!editor->pasteImage())
                SendMessageW(window, EM_PASTESPECIAL, CF_UNICODETEXT, 0);
            return 0;
        }
        if (window == editor->input && (msg == WM_CHAR || msg == WM_KEYDOWN || msg == WM_CUT ||
                                        msg == WM_CLEAR || msg == EM_REPLACESEL)) {
            CHARRANGE range;
            SendMessageW(window, EM_EXGETSEL, 0, (LPARAM)&range);
            editor->changeFrom = (size_t)std::max(0L, range.cpMin - 1);
            if (msg == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 && (wp == 'Z' || wp == 'Y'))
                editor->changeFrom = 0;
            if (msg == WM_CHAR && (wp == 26 || wp == 25))
                editor->changeFrom = 0;
        }
        bool track = window == editor->input && !editor->updating && !editor->localMutation &&
                     (msg == EM_REPLACESEL || msg == EM_PASTESPECIAL || msg == WM_CUT || msg == WM_CLEAR ||
                      (msg == WM_CHAR && (wp >= 32 || wp == 8 || wp == 9 || wp == 13)));
        if (track && msg == WM_CHAR && wp != 8) {
            Microsoft::WRL::ComPtr<IRichEditOle> ole;
            SendMessageW(window, EM_GETOLEINTERFACE, 0, (LPARAM)ole.GetAddressOf());
            Microsoft::WRL::ComPtr<ITextDocument> tom;
            Microsoft::WRL::ComPtr<ITextSelection> selection;
            LONG flags = 0;
            if (!ole || FAILED(ole.As(&tom)) || FAILED(tom->GetSelection(&selection)) ||
                FAILED(selection->GetFlags(&flags)) || (flags & tomSelOvertype))
                track = false;
        }
        CHARRANGE before{};
        uint64_t oldRevision = editor->revision;
        bool priorMutation = editor->localMutation;
        if (track) {
            SendMessageW(window, EM_EXGETSEL, 0, (LPARAM)&before);
            editor->localMutation = true;
        }
        if (window == editor->input && !editor->updating &&
            (track || msg == WM_KEYDOWN || msg == WM_SETTEXT || msg == EM_SETTEXTEX || msg == EM_UNDO || msg == EM_REDO))
            editor->clearFindHighlights();
        auto result = DefSubclassProc(window, msg, wp, lp);
        if (window == editor->input && msg == WM_SETFOCUS)
            SendMessageW(window, EM_HIDESELECTION, FALSE, 0);
        if (window == editor->input && !editor->updating &&
            (msg == WM_VSCROLL || msg == WM_HSCROLL || msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL ||
             msg == WM_SIZE || msg == EM_SETSCROLLPOS || msg == WM_KEYUP))
            editor->paintFindHighlights();
        if (scrollable && (msg == WM_SIZE || msg == WM_STYLECHANGED || msg == WM_WINDOWPOSCHANGED)) {
            editor->updateScrollRegion(window);
            if (window == editor->input) editor->projectFindMarkers();
        }
        if (scrollable &&
            (msg == WM_PAINT || msg == WM_TIMER || msg == WM_NCACTIVATE || msg == WM_SETREDRAW ||
             msg == EM_SETREADONLY || msg == WM_WINDOWPOSCHANGED || msg == WM_STYLECHANGED ||
             msg == WM_SIZE || msg == WM_VSCROLL || msg == WM_HSCROLL || msg == WM_MOUSEWHEEL ||
             msg == WM_MOUSEHWHEEL || msg == EM_SETSCROLLPOS || msg == WM_KEYDOWN || msg == WM_CHAR ||
             msg == EM_SCROLLCARET || msg == EM_REPLACESEL || msg == EM_SETTEXTEX || msg == WM_SETTEXT ||
             msg == WM_THEMECHANGED))
            editor->paintScrollbars(window);
        if (scrollable && msg == WM_PRINT && (lp & PRF_NONCLIENT))
            editor->paintScrollbars(window, (HDC)wp);
        if (track) {
            editor->localMutation = priorMutation;
            if (editor->revision != oldRevision) {
                CHARRANGE after;
                SendMessageW(window, EM_EXGETSEL, 0, (LPARAM)&after);
                size_t start = (size_t)before.cpMin;
                if (msg == WM_CHAR && wp == 8 && before.cpMin == before.cpMax)
                    start = (size_t)after.cpMin;
                editor->trackCapture(start, (size_t)before.cpMax, (size_t)after.cpMax);
            }
        }
        editor->changeFrom = SIZE_MAX;
        if (window == editor->input && (msg == WM_MOUSEWHEEL || msg == WM_VSCROLL || msg == WM_KEYUP)) {
            SetTimer(editor->panel, 5, 150, nullptr);
            if (editor->live && editor->synchronized && !editor->syncing)
                PostMessageW(editor->owner, EditorScroll, 0, 0);
        }
        return result;
    } catch (const std::exception &e) {
        editor->changeFrom = SIZE_MAX;
        editor->localMutation = false;
        editor->fullCapture = true;
        themedMessageBox(editor->owner, fromUtf8(e.what()).c_str(), L"Markdown editor", MB_OK | MB_ICONERROR);
        return 0;
    }
}
LRESULT CALLBACK Editor::windowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto editor = (Editor *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        editor = (Editor *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)editor);
        editor->panel = window;
    }
    if (!editor)
        return DefWindowProcW(window, msg, wp, lp);
    try {
        return editor->message(msg, wp, lp);
    } catch (const std::exception &e) {
        themedMessageBox(editor->owner, fromUtf8(e.what()).c_str(), L"Edit Markdown", MB_OK | MB_ICONERROR);
        return 0;
    }
}
LRESULT Editor::message(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == findMessage) {
        findAction(((FINDREPLACEW *)lp)->Flags);
        return 0;
    }
    switch (msg) {
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        PAINTSTRUCT paint{};
        auto dc = msg == WM_PAINT ? BeginPaint(panel, &paint) : (HDC)wp;
        for (auto window : {input, warningList}) {
            if (!window || !(GetWindowLongW(window, GWL_STYLE) & WS_VISIBLE))
                continue;
            RECT bounds{};
            GetWindowRect(window, &bounds);
            MapWindowPoints(nullptr, panel, (POINT *)&bounds, 2);
            int saved = SaveDC(dc);
            OffsetViewportOrgEx(dc, bounds.left, bounds.top, nullptr);
            paintScrollbars(window, dc);
            RestoreDC(dc, saved);
        }
        paintLineNumbers(dc);
        paintSplit(dc);
        if (msg == WM_PAINT)
            EndPaint(panel, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        auto split = splitBounds();
        if (PtInRect(&split, point)) {
            draggingSplit = true;
            SetCapture(panel);
            return 0;
        }
        ClientToScreen(panel, &point);
        for (auto window : {input, warningList}) {
            if (!(GetWindowLongW(window, GWL_STYLE) & WS_VISIBLE))
                continue;
            RECT bounds{};
            GetWindowRect(window, &bounds);
            POINT local{point.x - bounds.left, point.y - bounds.top};
            for (int axis : {SB_VERT, SB_HORZ}) {
                if (!(GetWindowLongW(window, GWL_STYLE) & (axis == SB_VERT ? WS_VSCROLL : WS_HSCROLL)))
                    continue;
                auto track = scrollTrack(window, axis);
                SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE};
                GetScrollInfo(window, axis, &info);
                if (!PtInRect(&track, local) || info.nMax - info.nMin + 1 <= (int)info.nPage)
                    continue;
                SCROLLBARINFO state{sizeof(state)};
                if (GetScrollBarInfo(window, axis == SB_VERT ? OBJID_VSCROLL : OBJID_HSCROLL, &state) &&
                    (state.rgstate[0] & STATE_SYSTEM_UNAVAILABLE))
                    continue;
                scrollWindow = window;
                scrollAxis = axis;
                auto thumb = scrollThumb(window, axis);
                bool vertical = axis == SB_VERT;
                scrollGrab = PtInRect(&thumb, local)
                                 ? (vertical ? local.y - thumb.top : local.x - thumb.left)
                                 : (vertical ? thumb.bottom - thumb.top : thumb.right - thumb.left) / 2;
                SetFocus(window);
                SetCapture(panel);
                dragScrollbar(point);
                return 0;
            }
        }
        break;
    }
    case WM_SETCURSOR: {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(panel, &point);
        auto split = splitBounds();
        if (draggingSplit || PtInRect(&split, point)) {
            RECT r{};
            GetClientRect(panel, &r);
            SetCursor(LoadCursorW(nullptr, r.right >= MulDiv(1000, dpi, 96) ? IDC_SIZEWE : IDC_SIZENS));
            return TRUE;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if (draggingSplit) {
            auto previous = splitBounds();
            auto previousMargin = splitMargin();
            RECT r{};
            GetClientRect(panel, &r);
            if (r.right >= MulDiv(1000, dpi, 96))
                splitWidth = std::clamp((double)GET_X_LPARAM(lp) / std::max(1L, r.right), .1, .9);
            else
                splitHeight = std::clamp((double)(GET_Y_LPARAM(lp) - headerHeight()) /
                                         std::max(1L, r.bottom - headerHeight()), .1, .9);
            auto requested = splitBounds();
            if (EqualRect(&previous, &requested)) return 0;
            SendMessageW(owner, EditorResizeSplit, 0, 0);
            auto next = splitMargin();
            RECT dirty{};
            UnionRect(&dirty, &previousMargin, &next);
            // Clear the old grip and newly exposed margin, without invalidating
            // the toolbar or exposing an erased source pane between frames.
            InvalidateRect(panel, &dirty, FALSE);
            RedrawWindow(panel, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
            return 0;
        }
        if (scrollWindow) {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(panel, &point);
            dragScrollbar(point);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        if (draggingSplit) {
            draggingSplit = false;
            if (msg != WM_CAPTURECHANGED && GetCapture() == panel) ReleaseCapture();
            return 0;
        }
        if (scrollWindow) {
            auto window = scrollWindow;
            scrollWindow = nullptr;
            if (msg != WM_CAPTURECHANGED && GetCapture() == panel)
                ReleaseCapture();
            paintScrollbars(window);
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        if (forwardingScrollWheel)
            return 0;
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (auto window : {input, warningList}) {
            RECT bounds{};
            GetWindowRect(window, &bounds);
            if ((GetWindowLongW(window, GWL_STYLE) & WS_VISIBLE) && PtInRect(&bounds, point)) {
                // Controls may bubble an unhandled wheel message back to us.
                forwardingScrollWheel = true;
                auto result = SendMessageW(window, msg, wp, lp);
                forwardingScrollWheel = false;
                return result;
            }
        }
        break;
    }
    case WM_MENUCHAR: {
        // Owner-drawn menus need to supply native first-letter navigation.
        HMENU menu = (HMENU)lp;
        wchar_t character = LOWORD(wp);
        int first = -1, next = -1, highlighted = -1, matches = 0;
        for (int i = 0; i < GetMenuItemCount(menu); ++i) {
            MENUITEMINFOW item{sizeof(item)};
            item.fMask = MIIM_DATA | MIIM_STATE | MIIM_FTYPE;
            GetMenuItemInfoW(menu, i, TRUE, &item);
            if (item.fState & MFS_HILITE)
                highlighted = i;
        }
        for (int i = 0; i < GetMenuItemCount(menu); ++i) {
            MENUITEMINFOW item{sizeof(item)};
            item.fMask = MIIM_DATA | MIIM_STATE | MIIM_FTYPE;
            GetMenuItemInfoW(menu, i, TRUE, &item);
            if (!item.dwItemData || (item.fType & MFT_SEPARATOR) || (item.fState & MFS_DISABLED))
                continue;
            auto &text = *reinterpret_cast<const std::wstring *>(item.dwItemData);
            auto start = text.find_first_not_of(L' ');
            if (start == text.npos ||
                CompareStringOrdinal(text.c_str() + start, 1, &character, 1, TRUE) != CSTR_EQUAL)
                continue;
            if (first < 0)
                first = i;
            if (next < 0 && i > highlighted)
                next = i;
            ++matches;
        }
        if (first >= 0)
            return MAKELRESULT(next >= 0 ? next : first, matches == 1 ? MNC_EXECUTE : MNC_SELECT);
        return MAKELRESULT(0, MNC_IGNORE);
    }
    case WM_MEASUREITEM:
        if (((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_MENU) {
            measureMenu(*(MEASUREITEMSTRUCT *)lp);
            return TRUE;
        }
        break;
    case WM_DRAWITEM: {
        auto &item = *(DRAWITEMSTRUCT *)lp;
        if (item.CtlType == ODT_MENU) {
            drawMenu(item);
            return TRUE;
        }
        RECT r = item.rcItem;
        ChromePaint buffered(item.hDC, r);
        auto dc = buffered.dc();
        POINT point;
        GetCursorPos(&point);
        ScreenToClient(item.hwndItem, &point);
        bool hover = PtInRect(&r, point), pressed = (item.itemState & ODS_SELECTED) != 0;
        auto base = dark ? RGB(16, 19, 25) : RGB(250, 251, 253);
        SetDCBrushColor(dc, base);
        FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
        auto fill = dark ? (pressed ? RGB(48, 65, 90)
                            : hover ? RGB(46, 59, 78)
                                    : RGB(28, 34, 45))
                         : (pressed ? RGB(209, 223, 248)
                            : hover ? RGB(230, 237, 250)
                                    : RGB(255, 255, 255));
        auto oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
        auto oldPen = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, fill);
        SetDCPenColor(dc, item.itemState & ODS_FOCUS ? (dark ? RGB(144, 185, 255) : RGB(39, 95, 199))
                                                     : (dark ? RGB(48, 58, 73) : RGB(220, 226, 235)));
        int radius = MulDiv(8, dpi, 96);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        auto oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, item.itemState & ODS_DISABLED ? (dark ? RGB(146, 158, 175) : RGB(102, 117, 138))
                                                       : (dark ? RGB(229, 233, 240) : RGB(32, 41, 56)));
        auto text = windowText(item.hwndItem);
        DrawTextW(dc, text.c_str(), (int)text.size(), &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 100 && HIWORD(wp) == EN_CHANGE) {
            changed();
            return 0;
        }
        if (LOWORD(wp) == 100 && HIWORD(wp) == EN_VSCROLL && live && synchronized && !syncing && !updating) {
            PostMessageW(owner, EditorScroll, 0, 0);
            SetTimer(panel, 5, 150, nullptr);
            return 0;
        }
        if (LOWORD(wp) == 101 && HIWORD(wp) == LBN_SELCHANGE) {
            auto at = SendMessageW(warningList, LB_GETCURSEL, 0, 0);
            if (at >= 0 && (size_t)at < diagnostics.size() && checkedRevision == revision) {
                preview = false;
                SetWindowTextW(buttons[1], L"Preview");
                CHARRANGE range{(LONG)diagnostics[at].offset,
                                (LONG)(diagnostics[at].offset + diagnostics[at].length)};
                SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&range);
                SendMessageW(input, EM_SCROLLCARET, 0, 0);
                PostMessageW(owner, EditorLayout, 0, 0);
                focus();
            }
            return 0;
        }
        command(LOWORD(wp));
        return 0;
    case WM_TIMER:
        if (wp == 1) {
            poll();
            toolsPoll();
        }
        if (wp == 2) {
            KillTimer(panel, 2);
            check();
        }
        if (wp == 3) {
            KillTimer(panel, 3);
            checkpointScheduled = false;
            checkpoint();
        }
        if (wp == 5) {
            KillTimer(panel, 5);
            highlight();
        }
        if (wp == 4 && ready && !saving && !auxiliary && !filePath.empty() && baseline.exists &&
            !needsFirstSave && !externalChanged) {
            auto result = std::make_shared<Auxiliary>();
            result->kind = Auxiliary::Probe;
            result->path = filePath;
            auxiliary = result;
            auto time = diskTime;
            auto size = diskSize;
            std::thread([result, time, size] {
                std::error_code ec;
                auto stamp = std::filesystem::last_write_time(result->path, ec);
                result->changed = !!ec || stamp != time;
                if (!ec) {
                    auto count = std::filesystem::file_size(result->path, ec);
                    result->changed = result->changed || !!ec || count != size;
                }
                result->done.store(true, std::memory_order_release);
            }).detach();
        }
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(panel, &r);
        int saved = SaveDC((HDC)wp);
        for (auto window : {input, warningList}) {
            if (!window || !IsWindowVisible(window))
                continue;
            RECT bounds{};
            GetWindowRect(window, &bounds);
            MapWindowPoints(nullptr, panel, (POINT *)&bounds, 2);
            for (int axis : {SB_VERT, SB_HORZ}) {
                auto track = scrollTrack(window, axis);
                OffsetRect(&track, bounds.left, bounds.top);
                ExcludeClipRect((HDC)wp, track.left, track.top, track.right, track.bottom);
            }
        }
        if (input && !preview && IsWindowVisible(input)) {
            RECT client{};
            GetClientRect(input, &client);
            MapWindowPoints(input, panel, (POINT *)&client, 2);
            // The gutter supplies its own buffered background. Never erase
            // its numbers between scroll/layout and the next gutter paint.
            ExcludeClipRect((HDC)wp, MulDiv(12, dpi, 96), client.top, client.left, client.bottom);
        }
        if (background)
            FillRect((HDC)wp, &r, background);
        RestoreDC((HDC)wp, saved);
        return 1;
    }
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, dark ? RGB(229, 233, 240) : RGB(32, 41, 56));
        SetBkColor((HDC)wp, dark ? RGB(16, 19, 25) : RGB(250, 251, 253));
        return (LRESULT)background;
    }
    return DefWindowProcW(panel, msg, wp, lp);
}
#ifdef VMD_TESTING
void Editor::verify() {
    auto require = [](bool value, const char *message) {
        if (!value)
            throw std::runtime_error(message);
    };
    position(78);
    RedrawWindow(panel, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
    struct ToolbarActivity {
        unsigned hides = 0, moves = 0, paints = 0;
    } activity;
    auto watchToolbar = [](HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR,
                           DWORD_PTR data) -> LRESULT {
        auto &counts = *(ToolbarActivity *)data;
        if (message == WM_SHOWWINDOW && !wp)
            ++counts.hides;
        if (message == WM_WINDOWPOSCHANGED) {
            auto &position = *(WINDOWPOS *)lp;
            if (!(position.flags & SWP_NOMOVE) || !(position.flags & SWP_NOSIZE))
                ++counts.moves;
        }
        if (message == WM_PAINT)
            ++counts.paints;
        return DefSubclassProc(window, message, wp, lp);
    };
    for (auto button : buttons)
        require(SetWindowSubclass(button, watchToolbar, 42, (DWORD_PTR)&activity) != FALSE,
                "watch toolbar refresh activity");
    for (int refresh = 0; refresh < 3; ++refresh) {
        position(78);
        for (auto button : buttons)
            UpdateWindow(button);
    }
    for (auto button : buttons)
        RemoveWindowSubclass(button, watchToolbar, 42);
    require(activity.hides == 0, "preview layout keeps toolbar buttons visible");
    require(activity.moves == 0, "preview layout keeps toolbar geometry stable");
    require(activity.paints == 0, "preview layout does not repaint unchanged toolbar buttons");
    paintLineNumbers();
    auto gutterPaints = lineNumberPaints;
    for (int refresh = 0; refresh < 20; ++refresh)
        paintScrollbars(input);
    require(lineNumberPaints == gutterPaints, "unchanged scrollbar refreshes do not repaint line numbers");
    // Exercise parent-owned tracks without involving document parsing or undo.
    std::wstring overflow;
    for (int i = 0; i < 150; ++i)
        overflow += std::wstring(200, L'x') + L"\r";
    updating = true;
    SetWindowTextW(input, overflow.c_str());
    struct ScrollPaintProbe {
        Editor *editor;
        unsigned calls = 0;
        bool clipped = true;
    } scrollPaint{this};
    auto watchScroll = [](HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR,
                          DWORD_PTR data) -> LRESULT {
        auto &probe = *(ScrollPaintProbe *)data;
        if (message == EM_SETSCROLLPOS || message == EM_LINESCROLL || message == WM_MOUSEWHEEL) {
            ++probe.calls;
            auto region = CreateRectRgn(0, 0, 0, 0);
            probe.clipped &= GetWindowRgn(window, region) != ERROR;
            for (int axis : {SB_VERT, SB_HORZ}) {
                auto track = probe.editor->scrollTrack(window, axis);
                probe.clipped &=
                    !PtInRegion(region, (track.left + track.right) / 2, (track.top + track.bottom) / 2);
            }
            DeleteObject(region);
        }
        return DefSubclassProc(window, message, wp, lp);
    };
    // Observe clipping at the native scroll handler, before any themed repaint.
    RemoveWindowSubclass(input, inputProc, 1);
    require(SetWindowSubclass(input, watchScroll, 43, (DWORD_PTR)&scrollPaint) != FALSE,
            "watch native scrollbar painting");
    require(SetWindowSubclass(input, inputProc, 1, (DWORD_PTR)this) != FALSE,
            "restore editor above scrollbar clipping probe");
    for (bool night : {true, false}) {
        theme(night, 96);
        for (int axis : {SB_VERT, SB_HORZ}) {
            SCROLLINFO info{sizeof(info), SIF_ALL};
            require(GetScrollInfo(input, axis, &info) && info.nMax > (int)info.nPage,
                    "source has scrollable themed track");
            auto thumb = scrollThumb(input, axis);
            auto sampleThumb = [&](RECT rectangle) {
                HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
                RECT bounds{};
                GetWindowRect(input, &bounds);
                auto bitmap =
                    CreateCompatibleBitmap(screen, bounds.right - bounds.left, bounds.bottom - bounds.top);
                auto old = SelectObject(dc, bitmap);
                paintScrollbars(input, dc);
                auto color = GetPixel(dc, (rectangle.left + rectangle.right) / 2,
                                      (rectangle.top + rectangle.bottom) / 2);
                SelectObject(dc, old);
                DeleteObject(bitmap);
                DeleteDC(dc);
                ReleaseDC(nullptr, screen);
                return color;
            };
            auto color = sampleThumb(thumb);
            require(color == (night ? RGB(146, 158, 175) : RGB(102, 117, 138)),
                    "source scrollbar matches reader muted palette");
            RECT bounds{};
            GetWindowRect(input, &bounds);
            POINT start{bounds.left + (thumb.left + thumb.right) / 2,
                        bounds.top + (thumb.top + thumb.bottom) / 2};
            ScreenToClient(panel, &start);
            SendMessageW(panel, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(start.x, start.y));
            auto track = scrollTrack(input, axis);
            POINT end{bounds.left + track.right, bounds.top + track.bottom};
            ScreenToClient(panel, &end);
            SendMessageW(panel, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(end.x, end.y));
            GetScrollInfo(input, axis, &info);
            require(info.nPos > info.nMin, "themed scrollbar drag scrolls source");
            thumb = scrollThumb(input, axis);
            color = sampleThumb(thumb);
            require(color == (night ? RGB(144, 185, 255) : RGB(39, 95, 199)),
                    "dragged scrollbar matches reader accent palette");
            SendMessageW(panel, WM_LBUTTONUP, 0, MAKELPARAM(end.x, end.y));
            require(!scrollWindow && GetCapture() != panel, "parent-owned scrollbar releases capture");
            if (axis == SB_VERT) {
                for (int boundary : {SB_TOP, SB_BOTTOM}) {
                    SendMessageW(input, WM_VSCROLL, boundary, 0);
                    UpdateWindow(input);
                    paintLineNumbers();
                    auto boundaryPaints = lineNumberPaints;
                    auto boundaryScrollbarPaints = scrollbarPaints;
                    for (int wheel = 0; wheel < 12; ++wheel) {
                        SendMessageW(input, WM_MOUSEWHEEL,
                                     MAKEWPARAM(0, boundary == SB_TOP ? WHEEL_DELTA : -WHEEL_DELTA), 0);
                        UpdateWindow(input);
                    }
                    require(lineNumberPaints == boundaryPaints,
                            "wheel input at source edges does not repaint unchanged line numbers");
                    require(scrollbarPaints == boundaryScrollbarPaints,
                            "wheel input at source edges does not repaint unchanged scrollbars");
                }
            }
            POINT origin{};
            SendMessageW(input, EM_SETSCROLLPOS, 0, (LPARAM)&origin);
            SendMessageW(input, EM_LINESCROLL, 0, 2);
            SendMessageW(input, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
            SendMessageW(input, EM_SETSCROLLPOS, 0, (LPARAM)&origin);
        }
    }
    RemoveWindowSubclass(input, watchScroll, 43);
    require(scrollPaint.calls >= 16 && scrollPaint.clipped,
            "native scrollbar regions remain invisible inside every scroll handler");
    SetWindowTextW(input, L"");
    SendMessageW(input, EM_EMPTYUNDOBUFFER, 0, 0);
    updating = false;
    replace(insertMarkdown(L"", 0, 0, Insert::Bold));
    captureText();
    require(cachedText == L"**bold text**", "native helper inserts plain Markdown");
    SendMessageW(input, EM_UNDO, 0, 0);
    captureText();
    require(cachedText.empty() && !modified(), "helper single undo restores clean draft");
    SendMessageW(input, EM_REDO, 0, 0);
    captureText();
    require(cachedText == L"**bold text**", "helper redo");
    replace(insertMarkdown(cachedText, cachedText.size(), cachedText.size(), Insert::Table, 2, 2));
    captureText();
    require(cachedText.find(L"\n\n| Header 1 | Header 2 |") != cachedText.npos, "native multiline insertion");
    check(true);
    auto deadline = GetTickCount64() + 10000;
    while (!preview && GetTickCount64() < deadline) {
        poll();
        Sleep(1);
    }
    require(preview && rendered && !rendered->blocks.empty(), "async draft preview");
    togglePreview();
    SendMessageW(input, EM_UNDO, 0, 0);
    captureText();
    require(cachedText == L"**bold text**", "preview preserves undo history");
    auto retained = cachedText;
    theme(true, 216);
    captureText();
    require(cachedText == retained, "theme and DPI preserve draft");
    theme(false, 96);
    highlight();
    SendMessageW(input, EM_UNDO, 0, 0);
    captureText();
    require(cachedText.empty(), "theme and highlighting do not add undo entries");
    SendMessageW(input, EM_REDO, 0, 0);
    captureText();
    require(cachedText == retained, "highlight preserves redo history");
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"Title\r\n=====\r\n\r\nAfter");
    SendMessageW(input, EM_EMPTYUNDOBUFFER, 0, 0);
    CHARRANGE headingCaret{2, 2};
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&headingCaret);
    insert(Insert::Heading, 0);
    captureText();
    require(cachedText == L"Title\n\nAfter", "058 native Normal text removes the Setext underline");
    SendMessageW(input, EM_UNDO, 0, 0);
    captureText();
    require(cachedText == L"Title\n=====\n\nAfter", "058 heading conversion is one undo operation");
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"# Title\r\n---");
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&headingCaret);
    insert(Insert::Heading, 0);
    captureText();
    require(cachedText == L"Title\n\n---",
            "061 native Normal text separates a following rule from the converted paragraph");
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"Before\r\n\r\n![](image.png)\r\nTitle\r\n===\r\n\r\n#");
    captureText();
    headings = sourceOutline(cachedText);
    outlineRevision = revision;
    require(headings.size() == 2, "059 native outline includes a text-free heading");
    sourceAtHeading(0);
    CHARRANGE outlineCaret{};
    SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&outlineCaret);
    require(outlineCaret.cpMin == 8 && outlineCaret.cpMax == 8,
            "059 outline navigation targets the first line of an image-led heading");
    sourceAtHeading(1);
    SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&outlineCaret);
    require((size_t)outlineCaret.cpMin == cachedText.find(L'#'),
            "059 outline navigation targets the empty heading marker");
    for (bool night : {false, true}) {
        theme(night, 96);
        auto outlineRows = sourceOutline(L"# Before\tAfter\n");
        std::wstring expectedLabel = L"Before After";
        MEASUREITEMSTRUCT actual{}, expected{};
        actual.itemData = (ULONG_PTR)&outlineRows[0].title;
        expected.itemData = (ULONG_PTR)&expectedLabel;
        measureMenu(actual);
        measureMenu(expected);
        require(actual.itemWidth == expected.itemWidth && actual.itemHeight == expected.itemHeight,
                "060 native outline sizing does not allocate a shortcut badge for heading tabs");
    }
    theme(false, 96);
    {
        struct KeyboardState {
            BYTE saved[256]{};
            KeyboardState() { GetKeyboardState(saved); }
            ~KeyboardState() { SetKeyboardState(saved); }
        } keyboard;
        BYTE keys[256]{};
        keys[VK_SHIFT] = 0x80;
        SetKeyboardState(keys);
        SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"");
        SetFocus(input);
        const std::wstring sample = L"# test\n\n```json\n{\n\t\"sip\": \"sop\",\n\t\"hip: \"hop\"\n}\n```\n\nsdfdf\n";
        for (auto character : sample) {
            if (character == L'\n')
                SendMessageW(input, WM_KEYDOWN, VK_RETURN, 1);
            SendMessageW(input, WM_CHAR, character == L'\n' ? L'\r' : character, 1);
        }
        captureText();
        require(cachedText == sample, "Shift+Enter inserts Markdown source newlines");
        auto typed = parse(cachedText);
        require(typed.blocks.size() == 3 && typed.blocks[1].kind == Kind::Code,
                "Shift+Enter JSON fence parses as a code block rather than inline code");
        auto count = SendMessageW(input, EM_GETLINECOUNT, 0, 0);
        require(count == 11, "Shift+Enter keeps source line numbers aligned");
        SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"- item");
        CHARRANGE selection{6, 6};
        SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&selection);
        SendMessageW(input, WM_KEYDOWN, VK_RETURN, 1);
        SendMessageW(input, WM_CHAR, L'\r', 1);
        captureText();
        require(cachedText == L"- item\n", "Shift+Enter does not continue list markers");
        SendMessageW(input, EM_UNDO, 0, 0);
        captureText();
        require(cachedText == L"- item", "Shift+Enter newline supports undo");
        selection = {2, 6};
        SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&selection);
        SendMessageW(input, WM_KEYDOWN, VK_RETURN, 1);
        SendMessageW(input, WM_CHAR, L'\r', 1);
        captureText();
        require(cachedText == L"- \n", "Shift+Enter replaces the selection with a source newline");
    }
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"    alpha");
    SetFocus(input);
    CHARRANGE outdentCaret{6, 6};
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&outdentCaret);
    require(tab(true), "043 native Shift+Tab handles an unselected line");
    SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&outdentCaret);
    require(outdentCaret.cpMin == 2 && outdentCaret.cpMax == 2,
            "043 Shift+Tab preserves a collapsed caret rather than selecting the line");
    SendMessageW(input, EM_REPLACESEL, TRUE, (LPARAM)L"X");
    captureText();
    require(cachedText == L"alXpha", "043 typing after outdent retains the rest of the line");
    for (auto line : {L"alpha", L"    alpha", L"\talpha"}) {
        SendMessageW(input, WM_SETTEXT, 0, (LPARAM)line);
        outdentCaret = {0, 0};
        SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&outdentCaret);
        require(tab(true), "043 Shift+Tab handles a caret before indentation");
        SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&outdentCaret);
        require(outdentCaret.cpMin == 0 && outdentCaret.cpMax == 0,
                "043 outdent clamps the caret without creating a selection");
    }
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"- item");
    captureText();
    CHARRANGE end{6, 6};
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&end);
    require(smartEnter(), "native smart list Enter");
    captureText();
    require(cachedText == L"- item\n- ", "native list marker continuation");
    SendMessageW(input, EM_UNDO, 0, 0);
    captureText();
    require(cachedText == L"- item", "smart Enter is one undo step");
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"- ");
    end = {2, 2};
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&end);
    require(smartEnter(), "045 empty list item Enter is handled");
    captureText();
    require(cachedText.empty(), "045 Enter on an empty list item exits the list");
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"- \nFollowing");
    end = {2, 2};
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&end);
    require(smartEnter(), "045 empty list item before another paragraph is handled");
    captureText();
    require(cachedText == L"\nFollowing", "045 exiting a list preserves the following paragraph");
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"> - ");
    end = {4, 4};
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&end);
    require(smartEnter(), "045 empty quoted list item is handled");
    captureText();
    require(cachedText == L"> ", "045 exiting a quoted list retains the quote");
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"* * *");
    end = {5, 5};
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&end);
    require(!smartEnter(), "044 thematic break Enter stays native");
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"> * * *");
    end = {7, 7};
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&end);
    require(smartEnter(), "044 quoted thematic break continues its quote");
    captureText();
    require(cachedText == L"> * * *\n> ", "044 thematic break does not create a list marker");
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"```\n- literal");
    captureText();
    end = {13, 13};
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&end);
    require(!smartEnter(), "smart Enter does not change fenced code");
    for (auto source : {L"> ```\n> - literal", L"- ```\n  - literal", L"```text\n- ```\n- literal",
                        L"-\t```\n\t- literal"}) {
        SendMessageW(input, WM_SETTEXT, 0, (LPARAM)source);
        captureText();
        CHARRANGE at{(LONG)cachedText.size(), (LONG)cachedText.size()};
        SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&at);
        require(!smartEnter(), "container-aware code continuation safety");
    }
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"a A aa");
    captureText();
    wcscpy_s(findBuffer.data(), findBuffer.size(), L"a");
    wcscpy_s(replaceBuffer.data(), replaceBuffer.size(), L"aa");
    findAction(FR_REPLACEALL | FR_WHOLEWORD | FR_DOWN);
    deadline = GetTickCount64() + 10000;
    while (auxiliary && GetTickCount64() < deadline) {
        toolsPoll();
        Sleep(1);
    }
    captureText();
    require(cachedText == L"aa aa aa", "native asynchronous Replace All");
    SendMessageW(input, EM_UNDO, 0, 0);
    captureText();
    require(cachedText == L"a A aa", "Replace All is one undo step");
    auto longLine = L"```cpp\n" + std::wstring(40000, L'x') + L" const value = 42;\n```";
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)longLine.c_str());
    captureText();
    check();
    deadline = GetTickCount64() + 10000;
    while (checkedRevision != revision && GetTickCount64() < deadline) {
        poll();
        Sleep(1);
    }
    auto keyword = longLine.find(L"const");
    CHARRANGE keywordRange{(LONG)keyword, (LONG)(keyword + 5)};
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&keywordRange);
    SendMessageW(input, EM_SCROLLCARET, 0, 0);
    POINTL keywordPoint{};
    SendMessageW(input, (WM_USER + 38), (WPARAM)&keywordPoint, (LPARAM)keyword);
    POINT view{};
    SendMessageW(input, EM_GETSCROLLPOS, 0, (LPARAM)&view);
    view.x += std::max(0L, keywordPoint.x - 100);
    SendMessageW(input, EM_SETSCROLLPOS, 0, (LPARAM)&view);

    highlight();
    CHARFORMAT2W highlighted{sizeof(highlighted)};
    SendMessageW(input, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&highlighted);

    require(highlighted.crTextColor == RGB(39, 95, 199),
            "highlighting follows horizontal scrolling beyond 32K columns");
    auto imageJob = std::make_shared<Auxiliary>();
    imageJob->kind = Auxiliary::Image;
    imageJob->done = true;
    auxiliary = imageJob;
    saving = true;
    toolsPoll();
    require(auxiliary == imageJob, "image completion waits for active save");
    saving = false;
    auxiliary.reset();
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"abc");
    captureText();
    CHARRANGE overtypeAt{1, 1};
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&overtypeAt);
    SendMessageW(input, WM_KEYDOWN, VK_INSERT, 0);
    SendMessageW(input, WM_CHAR, L'X', 1);
    captureText();
    require(cachedText == L"aXc" && cachedText == editText(windowText(input)),
            "native overtype cannot corrupt cached source");
    wchar_t tempFolder[32768]{}, tempFile[32768]{};
    GetTempPathW(32768, tempFolder);
    require(GetTempFileNameW(tempFolder, L"vmd", 0, tempFile) != 0, "overtype test fixture");
    auto beforeOverwrite = readBaseline(tempFile);
    auto written = saveDraft(tempFile, cachedText, beforeOverwrite, beforeOverwrite);
    DeleteFileW(tempFile);
    require(written.status == SaveResult::Saved && written.baseline.bytes == "aXc",
            "overtype saves exact native text");
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"abcdef");
    captureText();
    SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&overtypeAt);
    SendMessageW(input, WM_CHAR, 9, 1);
    captureText();
    require(cachedText == L"a\tcdef" && cachedText == editText(windowText(input)),
            "native overtype tab cannot corrupt cached source");
    require(GetTempFileNameW(tempFolder, L"vmd", 0, tempFile) != 0, "overtype tab fixture");
    beforeOverwrite = readBaseline(tempFile);
    written = saveDraft(tempFile, cachedText, beforeOverwrite, beforeOverwrite);
    DeleteFileW(tempFile);
    require(written.status == SaveResult::Saved && written.baseline.bytes == "a\tcdef",
            "overtype tab saves exact native text");
    SendMessageW(input, WM_KEYDOWN, VK_INSERT, 0);
    SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"α😀\nOne\nTwo\n");
    captureText();
    uint32_t random = 17;
    for (unsigned round = 0; round < 20; ++round) {
        for (unsigned step = 0; step < 20; ++step) {
            auto actual = editText(windowText(input));
            random = random * 1664525 + 1013904223;
            size_t start = random % (actual.size() + 1);
            random = random * 1664525 + 1013904223;
            size_t editEnd = std::min(actual.size(), start + random % 4);
            CHARRANGE selection{(LONG)start, (LONG)editEnd};
            SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&selection);
            const wchar_t *parts[] = {L"å😀\n", L"x", L"", L"\r\n"};
            if (step % 4 == 0)
                SendMessageW(input, EM_REPLACESEL, TRUE, (LPARAM)parts[(round + step) % 4]);
            else if (step % 4 == 1)
                SendMessageW(input, WM_CHAR, L'å', 1);
            else if (step % 4 == 2) {
                SendMessageW(input, WM_CHAR, 0xd83d, 1);
                SendMessageW(input, WM_CHAR, 0xde00, 1);
            } else {
                SendMessageW(input, WM_CHAR, 13, 1);
                SendMessageW(input, WM_CHAR, 8, 1);
            }
            if (step == 19 && round % 3 == 0)
                SendMessageW(input, EM_UNDO, 0, 0);
        }
        captureText();
        require(
            cachedText == editText(windowText(input)),
            "incremental captures match native text after mixed Unicode, newline, deletion and undo edits");
    }
    {
        struct KeyboardRestore {
            BYTE state[256]{};
            KeyboardRestore() {
                GetKeyboardState(state);
            }
            ~KeyboardRestore() {
                SetKeyboardState(state);
            }
        } keyboard;
        BYTE keys[256]{};
        keys[VK_CONTROL] = keys[VK_LCONTROL] = keys[VK_SHIFT] = keys[VK_LSHIFT] = 0x80;
        SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"heading");
        SetKeyboardState(keys);
        SendMessageW(input, WM_KEYDOWN, '3', 0);
        SendMessageW(input, WM_CHAR, L'#', 0);
        captureText();
        require(cachedText == L"### heading", "heading shortcut consumes its translated character");
        keys[VK_MENU] = keys[VK_RMENU] = 0x80;
        SetKeyboardState(keys);
        require(!key('E') && !key('2'), "AltGr combinations remain available for typing");
    }
    {
        SendMessageW(input, WM_SETTEXT, 0, (LPARAM)L"Find find\nOther Find");
        captureText();
        auto originalText = cachedText;
        SendMessageW(input, EM_EMPTYUNDOBUFFER, 0, 0);
        CHARRANGE selected{0, 0}; SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&selected);
        SendMessageW(input, WM_CHAR, L'X', 1);
        captureText();
        auto before = cachedText;
        auto oldRevision = revision;
        auto dirty = SendMessageW(input, EM_GETMODIFY, 0, 0);
        auto canUndo = SendMessageW(input, EM_CANUNDO, 0, 0);
        std::atomic_bool cancel = false;
        auto hits = std::make_shared<MatchIndex>(indexMatches(before, L"find", cancel));
        SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&selected);
        POINT oldScroll{}; SendMessageW(input, EM_GETSCROLLPOS, 0, (LPARAM)&oldScroll);
        setFindResults(hits, hits->offsets.front());
        presentFind();
        CHARRANGE after{}; SendMessageW(input, EM_EXGETSEL, 0, (LPARAM)&after);
        POINT newScroll{}; SendMessageW(input, EM_GETSCROLLPOS, 0, (LPARAM)&newScroll);
        require(selected.cpMin == after.cpMin && selected.cpMax == after.cpMax && oldScroll.x == newScroll.x && oldScroll.y == newScroll.y,
                "Find background formatting preserves editor selection and scroll");
        require(revision == oldRevision && SendMessageW(input, EM_GETMODIFY, 0, 0) == dirty &&
                SendMessageW(input, EM_CANUNDO, 0, 0) == canUndo, "Find formatting preserves revision modified flag and undo availability");
        setFindResults(nullptr, UINT32_MAX);
        SendMessageW(input, EM_UNDO, 0, 0); captureText();
        require(cachedText == originalText, "undo after Find removes the typed character rather than formatting");
        SendMessageW(input, EM_REDO, 0, 0); captureText();
        require(cachedText == before, "redo after Find restores the typed character");
        setFindResults(hits, hits->offsets.front());
        SendMessageW(input, WM_CHAR, L'Y', 1);
        require(!searchResults && activeSearch == UINT32_MAX, "editing immediately invalidates Find highlights");
    }
    for (size_t size : {1024u * 1024u, 10u * 1024u * 1024u}) {
        std::wstring large;
        while (large.size() < size)
            large += L"A short Markdown line for native editing.\n";
        updating = true;
        SETTEXTEX set{ST_DEFAULT, 1200};
        SendMessageW(input, EM_SETTEXTEX, (WPARAM)&set, (LPARAM)large.c_str());
        updating = false;
        textCurrent = false;
        fullCapture = true;
        captureText();
        baselineText = cachedText;
        if (size == 1024u * 1024u) {
            ++revision;
            checkedRevision = UINT64_MAX;
            POINT before{0, 300};
            SendMessageW(input, EM_SETSCROLLPOS, 0, (LPARAM)&before);
            SendMessageW(input, EM_GETSCROLLPOS, 0, (LPARAM)&before);
            check(true);
            deadline = GetTickCount64() + 10000;
            while (!preview && GetTickCount64() < deadline) {
                poll();
                Sleep(1);
            }
            require(preview, "large draft preview completes");
            position(78);
            togglePreview();
            position(78);
            POINT after{};
            SendMessageW(input, EM_GETSCROLLPOS, 0, (LPARAM)&after);
            require(after.y == before.y, "source scroll survives preview resizing");
        }
        CHARRANGE at{0, 0};
        SendMessageW(input, EM_EXSETSEL, 0, (LPARAM)&at);
        auto began = std::chrono::steady_clock::now();
        SendMessageW(input, WM_CHAR, L'X', 1);
        double milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        require(!textCurrent, "typing defers full draft capture");
        require(!fullCapture, "ordinary typing keeps a bounded capture range");
        require(milliseconds < 500, "large draft typing responsiveness");
        auto capturedAt = std::chrono::steady_clock::now();
        captureText();
        double captureMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - capturedAt).count();
        require(cachedText.starts_with(L"XA short"), "large draft typed character");
        std::cout << "editor_typing_" << size << "_units_ms=" << milliseconds << '\n';
        std::cout << "editor_capture_" << size << "_units_ms=" << captureMs << '\n';
    }
    // A completion from a previous revision cannot replace current diagnostics or preview.
    if (job)
        job->cancel.store(true);
    auto stale = std::make_shared<Job>();
    stale->revision = revision - 1;
    stale->done = true;
    job = stale;
    checkDue = false;
    previewPending = false;
    auto previous = rendered;
    poll();
    require(rendered == previous, "stale validation rejected");
}
#endif
} // namespace vm
