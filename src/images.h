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
namespace vm {
struct Image {
    std::vector<unsigned char> pixels;
    unsigned width = 0, height = 0;
    bool ready = false, failed = false;
};
class Images {
    HWND window;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic_bool stopping = false;
    std::deque<std::pair<std::wstring, std::shared_ptr<Image>>> jobs;
    std::unordered_map<std::wstring, std::shared_ptr<Image>> cache;
    std::vector<std::thread> workers;
    void run();

  public:
    explicit Images(HWND hwnd);
    ~Images();
    std::shared_ptr<Image> get(const std::wstring &url, const std::wstring &base);
};
} // namespace vm
