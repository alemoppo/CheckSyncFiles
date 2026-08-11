// BackupVerifier GUI - Phase 2 (SDL3 + SDL3_ttf).
//
// Runs the SDL3 event loop and delegates the actual comparison to a worker
// thread; progress and results are pushed back to the UI thread.

#include "UI/AppUI.h"

int main() {
    bv::ui::AppUI app;
    return app.run();
}
