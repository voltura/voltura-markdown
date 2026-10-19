#pragma once
#include "edit.h"
#include "editor-tools.h"
#include <array>
#include <commctrl.h>
#include <commdlg.h>
#include <memory>

namespace vm {
constexpr UINT EditorLayout = WM_APP + 30, EditorPreview = WM_APP + 31, EditorSaved = WM_APP + 32,
               EditorFinish = WM_APP + 33, EditorChanged = WM_APP + 34, EditorScroll = WM_APP + 35;
class Editor {
    struct Job;
    struct Auxiliary;
    HWND owner = nullptr, panel = nullptr, input = nullptr, warningList = nullptr, tooltip = nullptr;
    std::array<HWND, 15> buttons{};
    HFONT font = nullptr, mono = nullptr;
    HBRUSH background = nullptr;
    HMODULE richEdit = nullptr;
    bool dark = false, ready = false, updating = false, preview = false, expanded = false;
    bool saving = false, previewPending = false, overwrite = false, convertUtf8 = false;
    bool textCurrent = true, checkDue = false;
    bool fullCapture = false, localMutation = false;
    size_t liveLength = 0, keptPrefix = 0, keptSuffix = 0;
    void resetCapture();
    void trackCapture(size_t start, size_t oldEnd, size_t newEnd);
    bool needsFirstSave = false;
    bool live = false;
    bool suppressCharacter = false;
    HWND scrollWindow = nullptr;
    bool settingScrollRegion = false;
    bool forwardingScrollWheel = false;
    int scrollAxis = SB_VERT, scrollGrab = 0;
    RECT scrollTrack(HWND window, int axis) const;
    RECT scrollThumb(HWND window, int axis) const;
    void updateScrollRegion(HWND window);
    void paintScrollbars(HWND window, HDC dc = nullptr) const;
    void dragScrollbar(POINT point);
    POINT sourceScroll{};
    bool restoreScroll = false;
    unsigned dpi = 96;
    uint64_t revision = 0, checkedRevision = UINT64_MAX;
    std::wstring baselineText, cachedText, findQuery;
    std::filesystem::path filePath, savePath;
    std::filesystem::path recoveredSnapshot;
    std::filesystem::path recoveredBase;
    std::filesystem::path linkBase() const {
        return filePath.empty() ? recoveredBase : filePath;
    }
    FileBaseline baseline;
    std::vector<Diagnostic> diagnostics;
    std::shared_ptr<Document> rendered;
    std::shared_ptr<Job> job;
    std::shared_ptr<Auxiliary> auxiliary;
    std::shared_ptr<DraftRecovery> recovery;
    std::shared_ptr<Auxiliary> recoveryJob;
    std::vector<std::shared_ptr<DraftRecovery>> retiredRecovery;
    bool checkpointScheduled = false, recoveryErrorShown = false;
    std::vector<HeadingEntry> headings;
    std::vector<FencePoint> fences;
    std::wstring fenceSource;
    size_t dirtyFrom = 0, changeFrom = SIZE_MAX;
    std::optional<FenceState> fenceAt(LONG lineStart);
    uint64_t outlineRevision = UINT64_MAX;
    bool highlighting = true, synchronized = true, syncing = false, externalChanged = false;
    HWND findDialog = nullptr;
    FINDREPLACEW findState{};
    std::array<wchar_t, 4096> findBuffer{}, replaceBuffer{};
    UINT findMessage = 0;
    std::filesystem::file_time_type diskTime{};
    uintmax_t diskSize = 0;
    void findAction(DWORD flags);
    void outline();
    void highlight();
    void checkpoint();
    void toolsPoll();
    void exportDocument(bool print);
    bool smartEnter();
    void tableAction(TableAction action);
    void recover();
    size_t hiddenFrom = 10;
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK inputProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT message(UINT, WPARAM, LPARAM);
    void layout();
    void changed();
    void captureText();
    void check(bool forPreview = false);
    void poll();
    void updateWarnings();
    void startSave(bool readExpected = false);
    void replace(const TextEdit &edit);
    void insert(Insert kind, unsigned level = 0);
    void popup(bool overflow);
    unsigned trackMenu(HMENU menu, UINT flags, int x, int y);
    void measureMenu(MEASUREITEMSTRUCT &item) const;
    void drawMenu(const DRAWITEMSTRUCT &item) const;
    void title();

  public:
    enum class Leave { Allow, Cancel, Saving };
    Editor(HWND owner, const Document &document, bool dark, unsigned dpi, bool newFile = false);
    ~Editor();
    Editor(const Editor &) = delete;
    Editor &operator=(const Editor &) = delete;
    void position(int top);
    void theme(bool dark, unsigned dpi);
    bool key(WPARAM key);
    bool tab(bool backward);
    void command(unsigned id);
    void togglePreview();
    void focus();
    void find();
    void findReplace(bool replace);
    bool dialogMessage(MSG &message);
    bool addImage(const std::filesystem::path &path);
    bool pasteImage();
    void previewScroll(double fraction);
    double sourcePosition() const;
    bool canSynchronize() const {
        return live && synchronized && checkedRevision == revision;
    }
    void sourceAtHeading(size_t index);
    bool save(bool saveAs = false);
    Leave leave();
    bool modified() const;
    bool busy() const;
    bool isPreview() const {
        return preview;
    }
    bool isLive() const {
        return live;
    }
#ifdef VMD_TESTING
    bool sourceFocused() const {
        return GetFocus() == input;
    }
#endif
    RECT previewBounds(int top) const;
    int headerHeight() const;
    const std::filesystem::path &path() const {
        return filePath;
    }
    std::shared_ptr<Document> previewDocument() const {
        return rendered;
    }
    void setVisible(bool visible);
#ifdef VMD_TESTING
    void verify();
#endif
};
} // namespace vm
