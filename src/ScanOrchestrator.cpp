#include "ScanOrchestrator.h"

#include <utility>

#include "Export/CsvExporter.h"

namespace bv {

ScanOrchestrator::~ScanOrchestrator() {
    shutdown();
}

void ScanOrchestrator::setProgressCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    progressCb_ = std::move(cb);
}

void ScanOrchestrator::setSource(std::wstring s) {
    std::lock_guard<std::mutex> lk(mtx_);
    source_ = std::move(s);
}

void ScanOrchestrator::setDest(std::wstring s) {
    std::lock_guard<std::mutex> lk(mtx_);
    dest_ = std::move(s);
}

void ScanOrchestrator::setSourceFocus(bool f) {
    std::lock_guard<std::mutex> lk(mtx_);
    sourceFocus_ = f;
}

void ScanOrchestrator::setDestFocus(bool f) {
    std::lock_guard<std::mutex> lk(mtx_);
    destFocus_ = f;
}

void ScanOrchestrator::setMode(ScanMode m) {
    std::lock_guard<std::mutex> lk(mtx_);
    mode_ = m;
}

void ScanOrchestrator::setCaseSensitive(bool c) {
    std::lock_guard<std::mutex> lk(mtx_);
    caseSensitive_ = c;
}

void ScanOrchestrator::setBackend(EnumeratorBackend b) {
    std::lock_guard<std::mutex> lk(mtx_);
    backend_ = b;
}

void ScanOrchestrator::setThreadSel(int sel) {
    std::lock_guard<std::mutex> lk(mtx_);
    threadSel_ = sel;
}

void ScanOrchestrator::useLiveSource() {
    std::lock_guard<std::mutex> lk(mtx_);
    useSnapshot_ = false;
    snapshotFile_.clear();
    statusNote_.clear();
}

void ScanOrchestrator::loadSnapshot(std::wstring file) {
    std::lock_guard<std::mutex> lk(mtx_);
    snapshotFile_ = std::move(file);
    useSnapshot_ = true;
    source_.clear(); // the device is no longer needed
    statusNote_ = L"Sorgente da snapshot. Impostare la destinazione e premere AVVIA.";
}

void ScanOrchestrator::clearSnapshot() {
    std::lock_guard<std::mutex> lk(mtx_);
    useSnapshot_ = false;
    snapshotFile_.clear();
    statusNote_ = L"Modalita online: sorgente da enumerare.";
}

bool ScanOrchestrator::startLiveScan() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (running_) return false;
    if (useSnapshot_) {
        if (snapshotFile_.empty() || dest_.empty()) return false;
    } else if (source_.empty() || dest_.empty()) {
        return false;
    }

    // The previous run's worker has finished (running_ == false) but its
    // std::thread object is still joinable. Joining it reaps the OS thread
    // before we reuse worker_; assigning over an un-joined thread would call
    // std::terminate() on the second run.
    if (worker_.joinable()) worker_.join();

    cancel_.store(false);
    resetForRunLocked();

    ScanOptions options;
    options.source = source_;
    options.destination = dest_;
    options.mode = mode_;
    options.caseSensitive = caseSensitive_;
    options.hashThreads = threadToCount();
    options.backend = backend_;
    options.cancel = &cancel_;
    options.onProgress = [this](const ScanProgress& p) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            progress_ = p;
        }
        notify();
    };
    if (useSnapshot_) {
        // Offline comparison: the source index is loaded from the snapshot, the
        // source device is not touched (options.source stays empty).
        options.compareFrom = snapshotFile_;
        progress_.phase = ScanPhase::CompareDestination; // no source pass
    }
    worker_ = std::thread(&ScanOrchestrator::workerThread, this, std::move(options));
    return true;
}

bool ScanOrchestrator::startSnapshotScan(const std::wstring& outFile) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (running_) return false;
    if (source_.empty()) {
        statusNote_ = L"Specificare la sorgente prima di creare uno snapshot.";
        return false;
    }
    if (worker_.joinable()) worker_.join();

    cancel_.store(false);
    resetForRunLocked();
    lastSnapshotPath_ = outFile;

    ScanOptions options;
    options.source = source_;
    options.destination.clear();
    options.mode = mode_;
    options.caseSensitive = caseSensitive_;
    options.hashThreads = threadToCount();
    options.backend = backend_;
    options.snapshotOut = outFile;
    options.cancel = &cancel_;
    options.onProgress = [this](const ScanProgress& p) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            progress_ = p;
        }
        notify();
    };
    worker_ = std::thread(&ScanOrchestrator::workerThread, this, std::move(options));
    return true;
}

void ScanOrchestrator::stop() {
    cancel_.store(true);
}

bool ScanOrchestrator::exportCsv(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (running_ || !resultsReady_) {
        statusNote_ = L"Eseguire prima una scansione.";
        return false;
    }
    std::wstring err;
    if (exporting::WriteCsv(path, results_, err)) {
        statusNote_ = L"Esportazione salvata: " + path;
        return true;
    }
    statusNote_ = L"Esportazione fallita: " + err;
    return false;
}

void ScanOrchestrator::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cancel_.store(true);
    }
    // Never join while holding mtx_: the worker's final update takes the lock.
    if (worker_.joinable()) worker_.join();
}

ScanOrchestrator::UiSnapshot ScanOrchestrator::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    UiSnapshot s;
    s.source = source_;
    s.dest = dest_;
    s.sourceFocus = sourceFocus_;
    s.destFocus = destFocus_;
    s.mode = mode_;
    s.caseSensitive = caseSensitive_;
    s.backend = backend_;
    s.threadSel = threadSel_;
    s.useSnapshot = useSnapshot_;
    s.snapshotFile = snapshotFile_;
    s.running = running_;
    s.resultsReady = resultsReady_;
    s.cancelled = cancel_.load();
    s.progress = progress_;
    s.threadCountUsed = threadCountUsed_;
    s.lastSecondsTotal = lastSecondsTotal_;
    s.hashingErrors = hashingErrors_;
    s.lastSnapshotWritten = lastSnapshotWritten_;
    s.lastUsedSnapshot = lastUsedSnapshot_;
    s.lastDegraded = lastDegraded_;
    s.statusNote = statusNote_;
    return s;
}

ResultSet ScanOrchestrator::results() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return results_;
}

unsigned int ScanOrchestrator::threadToCount() const {
    static constexpr unsigned int kChoices[] = {0u, 1u, 2u, 4u, 8u, 16u};
    return (threadSel_ >= 0 && threadSel_ < 6) ? kChoices[threadSel_] : 0u;
}

void ScanOrchestrator::resetForRunLocked() {
    resultsReady_ = false;
    results_ = {};
    progress_ = {};
    running_ = true;
    threadCountUsed_ = 0;
    lastSecondsTotal_ = 0.0;
    lastSnapshotWritten_ = false;
    lastUsedSnapshot_ = false;
    lastDegraded_ = false;
    hashingErrors_ = 0;
    statusNote_.clear();
    lastSnapshotPath_.clear();
}

void ScanOrchestrator::workerThread(ScanOptions options) {
    ScanController controller(options.caseSensitive);
    ScanReport report = controller.run(options);

    {
        std::lock_guard<std::mutex> lk(mtx_);
        results_ = std::move(report.results);
        resultsReady_ = true;
        running_ = false;
        threadCountUsed_ = report.hashThreadsUsed;
        lastSecondsTotal_ = report.secondsTotal;
        hashingErrors_ = report.hashingErrors;
        progress_.phase = ScanPhase::Done;
        progress_.files = results_.stats.sourceFiles;
        progress_.dirs = results_.stats.sourceDirs;
        lastSnapshotWritten_ = report.snapshotWritten;
        lastUsedSnapshot_ = report.usedSnapshot;
        lastDegraded_ = report.contentDegradedToSize;
        if (lastSnapshotWritten_ && !lastSnapshotPath_.empty()) {
            statusNote_ = L"Snapshot salvato: " + lastSnapshotPath_;
        } else if (lastDegraded_) {
            statusNote_ = L"Snapshot senza contenuti: confronto degradato alla dimensione.";
        } else if (lastUsedSnapshot_) {
            statusNote_ = L"Sorgente caricata da snapshot (" +
                          std::to_wstring(results_.stats.sourceFiles) + L" voci).";
        } else {
            statusNote_.clear();
        }
    }
    notify(); // wake the UI outside the lock
}

void ScanOrchestrator::notify() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cb = progressCb_;
    }
    if (cb) cb();
}

} // namespace bv
