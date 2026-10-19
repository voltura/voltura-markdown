#pragma once
// Common-control headers require the Windows declarations first.
#include <windows.h>

#include <algorithm>
#include <commctrl.h>
#include <commdlg.h>
#include <cstring>
#include <dwmapi.h>
#include <string>
#include <string_view>
#include <uxtheme.h>

namespace vm {
// Only dialog creation installs a thread-local hook; the reader's normal message path stays untouched.
inline thread_local bool dialogDark = false;
inline thread_local bool dialogMessageBox = false;
inline thread_local bool savingFileDialog = false;
struct DialogColors {
    bool dark;
    bool messageBox;
    HBRUSH background;
};
inline LRESULT CALLBACK dialogThemeProc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                        DWORD_PTR data) {
    auto self = reinterpret_cast<DialogColors *>(data);
    if (msg == WM_CTLCOLORDLG || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT ||
        msg == WM_CTLCOLORBTN || msg == WM_CTLCOLORLISTBOX) {
        SetTextColor((HDC)wp, self->dark ? RGB(229, 233, 240) : RGB(32, 41, 56));
        SetBkColor((HDC)wp, self->dark ? RGB(16, 19, 25) : RGB(250, 251, 253));
        return (LRESULT)self->background;
    }
    if (msg == WM_DRAWITEM) {
        auto item = (DRAWITEMSTRUCT *)lp;
        if (item->CtlType == ODT_LISTBOX || item->CtlType == ODT_COMBOBOX) {
            bool selected = (item->itemState & ODS_SELECTED) != 0;
            auto dc = item->hDC;
            int saved = SaveDC(dc);
            auto bg = selected ? (self->dark ? RGB(48, 65, 90) : RGB(220, 233, 250))
                               : (self->dark ? RGB(16, 19, 25) : RGB(250, 251, 253));
            SetDCBrushColor(dc, bg);
            FillRect(dc, &item->rcItem, (HBRUSH)GetStockObject(DC_BRUSH));
            SetTextColor(dc, self->dark ? RGB(229, 233, 240) : RGB(32, 41, 56));
            SetBkMode(dc, TRANSPARENT);
            if (item->itemID != (UINT)-1) {
                UINT lengthMessage = item->CtlType == ODT_LISTBOX ? LB_GETTEXTLEN : CB_GETLBTEXTLEN;
                UINT textMessage = item->CtlType == ODT_LISTBOX ? LB_GETTEXT : CB_GETLBTEXT;
                auto length = SendMessageW(item->hwndItem, lengthMessage, item->itemID, 0);
                if (length >= 0 && length < 32768) {
                    std::wstring text((size_t)length + 1, L'\0');
                    SendMessageW(item->hwndItem, textMessage, item->itemID, (LPARAM)text.data());
                    auto rect = item->rcItem;
                    int scale = GetDpiForWindow(dialog);
                    rect.left += MulDiv(3, scale, 96);
                    if (item->CtlID == 0x461) { // Native common-dialog folder list (lst2).
                        wchar_t current[32768]{};
                        DWORD count = GetCurrentDirectoryW(32768, current);
                        size_t depth = 0;
                        if (count && count < 32768) {
                            std::wstring_view path(current, count);
                            while (!path.empty() && path.back() == L'\\')
                                path.remove_suffix(1);
                            depth = std::count(path.begin(), path.end(), L'\\');
                        }
                        rect.left += MulDiv((int)(std::min)((size_t)item->itemID, depth + 1) * 8, scale, 96);
                        int icon = MulDiv(12, scale, 96);
                        int top = rect.top + (rect.bottom - rect.top - icon) / 2;
                        SetDCPenColor(dc, self->dark ? RGB(237, 192, 85) : RGB(139, 99, 23));
                        SelectObject(dc, GetStockObject(DC_PEN));
                        SelectObject(dc, GetStockObject(DC_BRUSH));
                        SetDCBrushColor(dc, self->dark ? RGB(110, 85, 35) : RGB(255, 222, 147));
                        Rectangle(dc, rect.left, top, rect.left + icon / 2, top + icon / 3);
                        Rectangle(dc, rect.left, top + icon / 4, rect.left + icon, top + icon);
                        rect.left += icon + MulDiv(4, scale, 96);
                    }
                    DrawTextW(dc, text.c_str(), (int)length, &rect,
                              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                }
            }
            if ((item->itemState & ODS_FOCUS) && !(item->itemState & ODS_NOFOCUSRECT))
                DrawFocusRect(dc, &item->rcItem);
            RestoreDC(dc, saved);
            return TRUE;
        }
    }
    if (msg == WM_NOTIFY && ((NMHDR *)lp)->code == NM_CUSTOMDRAW) {
        auto draw = (NMCUSTOMDRAW *)lp;
        wchar_t controlClass[32]{};
        GetClassNameW(draw->hdr.hwndFrom, controlClass, 32);
        if (wcscmp(controlClass, L"Button") == 0 && draw->dwDrawStage == CDDS_PREPAINT) {
            HWND button = draw->hdr.hwndFrom;
            auto type = GetWindowLongPtrW(button, GWL_STYLE) & BS_TYPEMASK;
            bool choice = type == BS_AUTOCHECKBOX || type == BS_CHECKBOX || type == BS_AUTORADIOBUTTON ||
                          type == BS_RADIOBUTTON;
            bool radio = type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON;
            bool group = type == BS_GROUPBOX;
            bool enabled = IsWindowEnabled(button) != FALSE;
            auto dc = draw->hdc;
            auto rect = draw->rc;
            FillRect(dc, &rect, self->background);
            COLORREF foreground = !enabled ? (self->dark ? RGB(126, 136, 151) : RGB(125, 132, 144))
                                           : (self->dark ? RGB(229, 233, 240) : RGB(32, 41, 56));
            SetTextColor(dc, foreground);
            SetBkMode(dc, TRANSPARENT);
            auto oldFont = SelectObject(dc, (HFONT)SendMessageW(button, WM_GETFONT, 0, 0));
            auto pen = CreatePen(PS_SOLID, 1, foreground);
            auto oldPen = SelectObject(dc, pen);
            auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            UINT flags = DT_SINGLELINE | DT_VCENTER | DT_CENTER;
            if (SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL)
                flags |= DT_HIDEPREFIX;
            if (choice) {
                int size = MulDiv(13, GetDpiForWindow(dialog), 96);
                RECT mark{rect.left + 1, (rect.bottom - size) / 2, rect.left + 1 + size,
                          (rect.bottom + size) / 2};
                if (radio)
                    Ellipse(dc, mark.left, mark.top, mark.right, mark.bottom);
                else
                    Rectangle(dc, mark.left, mark.top, mark.right, mark.bottom);
                if (SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    if (radio) {
                        SelectObject(dc, GetStockObject(DC_BRUSH));
                        SetDCBrushColor(dc, foreground);
                        InflateRect(&mark, -size / 3, -size / 3);
                        Ellipse(dc, mark.left, mark.top, mark.right, mark.bottom);
                    } else {
                        MoveToEx(dc, mark.left + 2, mark.top + size / 2, nullptr);
                        LineTo(dc, mark.left + size / 2, mark.bottom - 3);
                        LineTo(dc, mark.right - 2, mark.top + 2);
                    }
                }
                rect.left = mark.right + MulDiv(5, GetDpiForWindow(dialog), 96);
                flags = (flags & ~DT_CENTER) | DT_LEFT;
            } else {
                if (group)
                    rect.top += MulDiv(6, GetDpiForWindow(dialog), 96);
                Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
                if (group) {
                    rect.left += MulDiv(8, GetDpiForWindow(dialog), 96);
                    rect.top = draw->rc.top;
                    SetBkMode(dc, OPAQUE);
                    SetBkColor(dc, self->dark ? RGB(16, 19, 25) : RGB(250, 251, 253));
                    flags = (flags & ~(DT_CENTER | DT_VCENTER)) | DT_LEFT;
                } else if (draw->uItemState & CDIS_SELECTED)
                    OffsetRect(&rect, 1, 1);
            }
            int length = GetWindowTextLengthW(button);
            std::wstring text((size_t)length + 1, L'\0');
            GetWindowTextW(button, text.data(), length + 1);
            text.resize(length);
            DrawTextW(dc, text.c_str(), (int)text.size(), &rect, flags);
            if ((draw->uItemState & CDIS_FOCUS) &&
                !(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
                InflateRect(&rect, -3, -3);
                DrawFocusRect(dc, &rect);
            }
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldFont);
            DeleteObject(pen);
            return CDRF_SKIPDEFAULT;
        }
    }
    if (self->messageBox && (msg == WM_PAINT || msg == WM_PRINTCLIENT)) {
        auto result = DefSubclassProc(dialog, msg, wp, lp);
        HDC dc = msg == WM_PRINTCLIENT ? (HDC)wp : GetDC(dialog);
        int saved = SaveDC(dc);
        RECT footer{};
        GetClientRect(dialog, &footer);
        int top = footer.bottom;
        for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsWindowVisible(child))
                continue;
            RECT rect{};
            GetWindowRect(child, &rect);
            MapWindowPoints(nullptr, dialog, (POINT *)&rect, 2);
            wchar_t name[32]{};
            GetClassNameW(child, name, 32);
            if (wcscmp(name, L"Button") == 0)
                top = (std::min)(top, (int)rect.top);
            ExcludeClipRect(dc, rect.left, rect.top, rect.right, rect.bottom);
        }
        footer.top = (std::max)(0, top - MulDiv(10, GetDpiForWindow(dialog), 96));
        FillRect(dc, &footer, self->background);
        RestoreDC(dc, saved);
        if (msg == WM_PAINT)
            ReleaseDC(dialog, dc);
        return result;
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(dialog, dialogThemeProc, id);
        RemovePropW(dialog, L"VolturaDialogColors");
        DeleteObject(self->background);
        delete self;
    }
    return DefSubclassProc(dialog, msg, wp, lp);
}

inline LRESULT CALLBACK dialogGroupProc(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT) {
        auto colors = (DialogColors *)GetPropW(GetParent(window), L"VolturaDialogColors");
        if (!colors)
            return DefSubclassProc(window, msg, wp, lp);
        PAINTSTRUCT paint{};
        HDC dc = msg == WM_PAINT ? BeginPaint(window, &paint) : (HDC)wp;
        int saved = SaveDC(dc);
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(dc, &rect, colors->background);
        auto foreground = colors->dark ? RGB(229, 233, 240) : RGB(32, 41, 56);
        SetTextColor(dc, foreground);
        SetBkColor(dc, colors->dark ? RGB(16, 19, 25) : RGB(250, 251, 253));
        SelectObject(dc, (HFONT)SendMessageW(window, WM_GETFONT, 0, 0));
        auto pen = CreatePen(PS_SOLID, 1, foreground);
        SelectObject(dc, pen);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        int gap = MulDiv(8, GetDpiForWindow(window), 96);
        Rectangle(dc, rect.left, rect.top + gap, rect.right, rect.bottom);
        int length = GetWindowTextLengthW(window);
        std::wstring text((size_t)length + 1, L'\0');
        GetWindowTextW(window, text.data(), length + 1);
        rect.left += gap;
        DrawTextW(dc, text.c_str(), length, &rect, DT_SINGLELINE | DT_LEFT);
        RestoreDC(dc, saved);
        DeleteObject(pen);
        if (msg == WM_PAINT)
            EndPaint(window, &paint);
        return 0;
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(window, dialogGroupProc, id);
    return DefSubclassProc(window, msg, wp, lp);
}
inline LRESULT CALLBACK dialogSpinProc(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT) {
        HWND parent = GetParent(window);
        DialogColors *colors = nullptr;
        while (parent && !(colors = (DialogColors *)GetPropW(parent, L"VolturaDialogColors")))
            parent = GetParent(parent);
        if (!colors)
            return DefSubclassProc(window, msg, wp, lp);
        PAINTSTRUCT paint{};
        HDC dc = msg == WM_PAINT ? BeginPaint(window, &paint) : (HDC)wp;
        int saved = SaveDC(dc);
        RECT rect{};
        GetClientRect(window, &rect);
        auto foreground = colors->dark ? RGB(229, 233, 240) : RGB(32, 41, 56);
        FillRect(dc, &rect, colors->background);
        SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, foreground);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
        int middle = (rect.top + rect.bottom) / 2;
        MoveToEx(dc, rect.left, middle, nullptr);
        LineTo(dc, rect.right, middle);
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SetDCBrushColor(dc, foreground);
        int x = (rect.left + rect.right) / 2, size = MulDiv(3, GetDpiForWindow(window), 96);
        int y = middle / 2;
        POINT up[] = {{x, y - size}, {x - size, y + size}, {x + size, y + size}};
        Polygon(dc, up, 3);
        y = (middle + rect.bottom) / 2;
        POINT down[] = {{x, y + size}, {x - size, y - size}, {x + size, y - size}};
        Polygon(dc, down, 3);
        RestoreDC(dc, saved);
        if (msg == WM_PAINT)
            EndPaint(window, &paint);
        return 0;
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(window, dialogSpinProc, id);
    return DefSubclassProc(window, msg, wp, lp);
}
inline void themeDialog(HWND dialog, bool dark, bool messageBox = false) {
    auto colors = (DialogColors *)GetPropW(dialog, L"VolturaDialogColors");
    if (!colors) {
        colors = new DialogColors{};
        if (!SetWindowSubclass(dialog, dialogThemeProc, 1, (DWORD_PTR)colors)) {
            delete colors;
            return;
        }
        SetPropW(dialog, L"VolturaDialogColors", colors);
    }
    colors->dark = dark;
    bool onlyMessageControls = true, hasStatic = false;
    for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t name[32]{};
        GetClassNameW(child, name, 32);
        hasStatic = hasStatic || wcscmp(name, L"Static") == 0;
        if (wcscmp(name, L"Static") != 0 && wcscmp(name, L"Button") != 0)
            onlyMessageControls = false;
    }
    // Include validation and overwrite message boxes created internally by Windows.
    colors->messageBox = messageBox || (onlyMessageControls && hasStatic);
    if (colors->background)
        DeleteObject(colors->background);
    colors->background = CreateSolidBrush(dark ? RGB(16, 19, 25) : RGB(250, 251, 253));
    BOOL night = dark;
    DwmSetWindowAttribute(dialog, 20, &night, sizeof(night));
    COLORREF caption = dark ? RGB(16, 19, 25) : RGB(250, 251, 253);
    DwmSetWindowAttribute(dialog, 35, &caption, sizeof(caption));
    EnumChildWindows(
        dialog,
        [](HWND child, LPARAM night) -> BOOL {
            wchar_t name[64]{};
            GetClassNameW(child, name, 64);
            if (wcscmp(name, L"#32770") == 0)
                themeDialog(child, night != 0);
            if (wcscmp(name, L"Button") == 0 &&
                (GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX)
                SetWindowSubclass(child, dialogGroupProc, 1, 0);
            if (wcscmp(name, UPDOWN_CLASSW) == 0)
                SetWindowSubclass(child, dialogSpinProc, 1, 0);
            // Keep visual styles on buttons so they continue to send NM_CUSTOMDRAW.
            if (wcscmp(name, L"Button") == 0 || wcscmp(name, L"Edit") == 0 ||
                wcscmp(name, L"ComboBox") == 0 || wcscmp(name, L"ListBox") == 0)
                SetWindowTheme(
                    child,
                    wcscmp(name, L"Edit") == 0
                        ? L""
                        : (night ? (wcscmp(name, L"ComboBox") == 0 ? L"DarkMode_CFD" : L"DarkMode_Explorer")
                                 : L"Explorer"),
                    nullptr);
            if (wcscmp(name, WC_LISTVIEWW) == 0) {
                ListView_SetBkColor(child, night ? RGB(16, 19, 25) : RGB(250, 251, 253));
                ListView_SetTextBkColor(child, night ? RGB(16, 19, 25) : RGB(250, 251, 253));
                ListView_SetTextColor(child, night ? RGB(229, 233, 240) : RGB(32, 41, 56));
            }
            return TRUE;
        },
        dark);
    RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}
class DialogThemeScope {
    HHOOK hook;
    bool previousMessageBox;
    static LRESULT CALLBACK created(int code, WPARAM wp, LPARAM lp) {
        if (code == HCBT_ACTIVATE) {
            wchar_t name[32]{};
            GetClassNameW((HWND)wp, name, 32);
            if (wcscmp(name, L"#32770") == 0)
                themeDialog((HWND)wp, dialogDark, dialogMessageBox);
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

  public:
    explicit DialogThemeScope(bool messageBox = false) : previousMessageBox(dialogMessageBox) {
        dialogMessageBox = messageBox;
        hook = SetWindowsHookExW(WH_CBT, created, nullptr, GetCurrentThreadId());
    }
    ~DialogThemeScope() {
        if (hook)
            UnhookWindowsHookEx(hook);
        dialogMessageBox = previousMessageBox;
    }
    DialogThemeScope(const DialogThemeScope &) = delete;
    DialogThemeScope &operator=(const DialogThemeScope &) = delete;
};
inline int themedMessageBox(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
    DialogThemeScope theme(true);
    return MessageBoxW(owner, text, title, flags);
}
// Supplying a hook without OFN_EXPLORER selects Windows' hookable common-dialog
// layout. Its native folder navigation and validation remain intact, and every
// control can use the app palette rather than the shell's cached system palette.
inline UINT_PTR CALLBACK fileDialogHook(HWND dialog, UINT message, WPARAM, LPARAM) {
    if (message == WM_INITDIALOG)
        SetDlgItemTextW(dialog, IDOK, savingFileDialog ? L"&Save" : L"&Open");
    return 0;
}
inline BOOL themedFileDialog(OPENFILENAMEW *dialog, bool saving) {
    bool previousSaving = savingFileDialog;
    savingFileDialog = saving;
    auto previousHook = dialog->lpfnHook;
    DWORD previousFlags = dialog->Flags;
    auto previousDirectory = dialog->lpstrInitialDir;
    std::wstring initialDirectory;
    dialog->lpfnHook = fileDialogHook;
    dialog->Flags =
        (dialog->Flags & ~(OFN_EXPLORER | OFN_OVERWRITEPROMPT)) | OFN_ENABLEHOOK | OFN_HIDEREADONLY;
    DialogThemeScope theme;
    BOOL result;
    for (;;) {
        if (!previousDirectory) {
            dialog->lpstrInitialDir = nullptr;
            std::wstring filename(dialog->lpstrFile);
            auto separator = filename.find_last_of(L"\\/");
            if (separator != filename.npos) {
                initialDirectory = filename.substr(0, separator + 1);
                DWORD attributes = GetFileAttributesW(initialDirectory.c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    dialog->lpstrInitialDir = initialDirectory.c_str();
                    // Start in the document's folder with its name selected, as the
                    // Explorer-style chooser did, rather than showing a clipped full path.
                    auto name = filename.substr(separator + 1);
                    std::copy(name.begin(), name.end(), dialog->lpstrFile);
                    dialog->lpstrFile[name.size()] = 0;
                }
            }
        }
        result = saving ? GetSaveFileNameW(dialog) : GetOpenFileNameW(dialog);
        if (!result || !saving || !(previousFlags & OFN_OVERWRITEPROMPT) ||
            GetFileAttributesW(dialog->lpstrFile) == INVALID_FILE_ATTRIBUTES)
            break;
        // Windows' replacement prompt is shell-hosted even for a hookable file
        // dialog. Keep its protection, using the same themed confirmation as the editor.
        std::wstring question(dialog->lpstrFile);
        question += L" already exists.\n\nReplace it?";
        if (themedMessageBox(dialog->hwndOwner, question.c_str(), L"Confirm Save As",
                             MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
            break;
    }
    constexpr DWORD changedFlags = OFN_EXPLORER | OFN_ENABLEHOOK | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
    dialog->Flags = (dialog->Flags & ~changedFlags) | (previousFlags & changedFlags);
    dialog->lpfnHook = previousHook;
    dialog->lpstrInitialDir = previousDirectory;
    savingFileDialog = previousSaving;
    return result;
}
inline BOOL themedOpenFile(OPENFILENAMEW *dialog) {
    return themedFileDialog(dialog, false);
}
inline BOOL themedSaveFile(OPENFILENAMEW *dialog) {
    return themedFileDialog(dialog, true);
}
inline UINT_PTR CALLBACK nativeDialogHook(HWND, UINT, WPARAM, LPARAM) {
    return 0;
}
inline UINT_PTR CALLBACK printDialogHook(HWND dialog, UINT message, WPARAM, LPARAM) {
    if (message == WM_INITDIALOG)
        SetDlgItemTextW(dialog, IDOK, L"&Print");
    return 0;
}
#ifdef VMD_TESTING
inline void selectPdfTestPrinter(PRINTDLGW &dialog) {
    wchar_t enabled[2]{};
    if (dialog.hDevNames || GetEnvironmentVariableW(L"VMD_TEST_PRINT_TO_PDF", enabled, 2) != 1 ||
        enabled[0] != L'1')
        return;
    // Select only this test process's printer. Do not change the Windows default
    // or contact an unavailable physical printer during isolated GUI checks.
    constexpr wchar_t names[] = L"WINSPOOL\0Microsoft Print to PDF\0PORTPROMPT:\0";
    dialog.hDevNames = GlobalAlloc(GHND, sizeof(DEVNAMES) + sizeof(names));
    if (!dialog.hDevNames)
        return;
    auto dev = (DEVNAMES *)GlobalLock(dialog.hDevNames);
    dev->wDriverOffset = sizeof(DEVNAMES) / sizeof(wchar_t);
    dev->wDeviceOffset = dev->wDriverOffset + (WORD)(sizeof(L"WINSPOOL") / sizeof(wchar_t));
    dev->wOutputOffset = dev->wDeviceOffset + (WORD)(sizeof(L"Microsoft Print to PDF") / sizeof(wchar_t));
    memcpy((wchar_t *)dev + dev->wDriverOffset, names, sizeof(names));
    GlobalUnlock(dialog.hDevNames);
}
#endif
inline BOOL themedPrint(PRINTDLGW *dialog) {
#ifdef VMD_TESTING
    selectPdfTestPrinter(*dialog);
#endif
    auto printHook = dialog->lpfnPrintHook, setupHook = dialog->lpfnSetupHook;
    DWORD previousFlags = dialog->Flags;
    dialog->Flags |= PD_ENABLEPRINTHOOK | PD_ENABLESETUPHOOK;
    dialog->lpfnPrintHook = printDialogHook;
    dialog->lpfnSetupHook = nativeDialogHook;
    DialogThemeScope theme;
    BOOL result = PrintDlgW(dialog);
    constexpr DWORD changedFlags = PD_ENABLEPRINTHOOK | PD_ENABLESETUPHOOK;
    dialog->Flags = (dialog->Flags & ~changedFlags) | (previousFlags & changedFlags);
    dialog->lpfnPrintHook = printHook;
    dialog->lpfnSetupHook = setupHook;
    if (!result) {
        if (dialog->hDevNames)
            GlobalFree(dialog->hDevNames);
        if (dialog->hDevMode)
            GlobalFree(dialog->hDevMode);
        dialog->hDevNames = dialog->hDevMode = nullptr;
    }
    return result;
}
} // namespace vm
