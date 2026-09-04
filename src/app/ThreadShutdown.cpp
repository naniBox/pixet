#include "ThreadShutdown.h"

#include <QDebug>
#include <QThread>

#include <cstdlib>

#include "util/Shutdown.h"

#ifdef _WIN32
#include <process.h> // _exit
#else
#include <unistd.h> // _exit
#endif

namespace threadshutdown {

void stopWorker(QThread &thread, const char *name, int graceMs) {
    thread.quit();
    if (thread.wait(graceMs)) return;

    if (!pixet::shutdownRequested()) {
        // A window closing while the rest of the app carries on - see the header. Back to
        // the old unbounded wait, which is correct here even though it is slow: the worker
        // is about to be destroyed and everything it touches goes with it.
        qWarning() << "pixet:" << name << "did not stop within" << graceMs
                    << "ms; waiting for it (the application is not quitting)";
        thread.wait();
        return;
    }

    qWarning() << "pixet:" << name << "did not stop within" << graceMs
                << "ms during shutdown; exiting the process rather than hanging on it";

    // _exit, not exit() or QCoreApplication::quit(): the worker is still running and still
    // touching objects that are mid-destruction on this thread, so running static
    // destructors and atexit handlers now would race it - which is the crash this is
    // supposed to prevent, not cause. _exit skips all of that and hands the process
    // straight back to the OS, which reclaims the memory regardless of what the worker
    // thought it was still doing.
    std::fflush(nullptr);
    _exit(0);
}

} // namespace threadshutdown
