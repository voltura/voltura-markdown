#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>
#ifdef VMD_TESTING
#include <functional>
#endif
namespace vm {
struct Image {
    std::vector<unsigned char> pixels;
    unsigned width = 0, height = 0;
    bool ready = false, failed = false;
};
// Explicit export jobs reuse the reader's bounded decoders, off the UI thread.
Image imageForExport(const std::wstring &url, const std::wstring &base, const std::atomic_bool &cancel);
class Images {
    HWND window;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic_bool stopping = false;
    struct Job {
        std::wstring key;
        std::shared_ptr<Image> pending;
        std::shared_ptr<std::atomic_bool> cancel;
    };
    std::shared_ptr<std::atomic_bool> generationCancel = std::make_shared<std::atomic_bool>(false);
    std::deque<Job> jobs;
    std::unordered_map<std::wstring, std::shared_ptr<Image>> cache;
    std::vector<std::thread> workers;
    void run();
#ifdef VMD_TESTING
    std::function<Image(const std::wstring &, const std::atomic_bool &)> testLoader;
#endif

  public:
    explicit Images(HWND hwnd);
#ifdef VMD_TESTING
    Images(HWND hwnd, std::function<Image(const std::wstring &, const std::atomic_bool &)> loader);
#endif
    ~Images();
    void cancelPending();
    std::shared_ptr<Image> get(const std::wstring &url, const std::wstring &base);
};
} // namespace vm
