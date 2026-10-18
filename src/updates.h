#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <windows.h>

namespace vm {
// Owns no reader objects or HWNDs. Destruction cancels without joining network work.
class Updates {
    struct State;
    std::shared_ptr<State> state;
#ifdef VMD_TESTING
    friend void testUpdates();
#endif
  public:
    void check();
    void cancel();
    void sessionEnd();
    void close(bool install);
    ~Updates();
#ifdef VMD_TESTING
    void fixture(const std::filesystem::path &directory);
#endif
};
// Invoked before graphics/preferences initialization. Returns -1 for a normal reader launch.
int updateHelper(int argc, wchar_t **argv);
// Cooperates with an active installation only; ordinary launches do no disk/network work.
bool updateReaderMayStart();
#ifdef VMD_TESTING
void testUpdates();
#endif
} // namespace vm
