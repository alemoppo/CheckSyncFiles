#include "Profiling/DirTiming.h"

#include <algorithm>

namespace bv {
namespace profiling {

void DirTopN::add(std::wstring dir, double seconds) {
    if (n_ == 0 || seconds < 0.0) return;
    if (heap_.size() < n_) {
        heap_.push_back({std::move(dir), seconds});
        std::push_heap(heap_.begin(), heap_.end(),
                       [](const DirEntry& a, const DirEntry& b) { return a.seconds > b.seconds; });
        return;
    }
    if (seconds <= heap_.front().seconds) return; // below admission: ignore
    std::pop_heap(heap_.begin(), heap_.end(),
                  [](const DirEntry& a, const DirEntry& b) { return a.seconds > b.seconds; });
    heap_.back() = {std::move(dir), seconds};
    std::push_heap(heap_.begin(), heap_.end(),
                   [](const DirEntry& a, const DirEntry& b) { return a.seconds > b.seconds; });
}

std::vector<DirEntry> DirTopN::sorted() const {
    std::vector<DirEntry> out = heap_;
    std::sort(out.begin(), out.end(),
              [](const DirEntry& a, const DirEntry& b) { return a.seconds > b.seconds; });
    return out;
}

std::wstring DirParent(const std::wstring& absPath) {
    const size_t pos = absPath.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring() : absPath.substr(0, pos);
}

DirHashTop::DirHashTop(size_t displayN, size_t counters)
    : displayN_(displayN), counters_(counters) {}

void DirHashTop::add(Side fileSide, const std::wstring& dir, double seconds) {
    if (counters_ == 0 || displayN_ == 0 || seconds < 0.0) return;
    const int s = (fileSide == Side::Source) ? 0 : 1;
    std::lock_guard<std::mutex> lk(mtx_);
    SideState& st = sides_[s];
    const auto it = st.map.find(dir);
    if (it != st.map.end()) {
        it->second.est += seconds;
        st.total += seconds;
        return;
    }
    if (st.map.size() < counters_) {
        st.map.emplace(dir, Counter{seconds, 0.0});
        st.total += seconds;
        return;
    }
    // Full: evict the minimum estimate and carry its value as the error of
    // the newcomer (classic Space-Saving). This is what keeps small-but-many
    // directories alive: their estimate only grows, never resets.
    auto m = st.map.begin();
    for (auto i = st.map.begin(); i != st.map.end(); ++i) {
        if (i->second.est < m->second.est) m = i;
    }
    const double mine = m->second.est;
    st.map.erase(m);
    st.map.emplace(dir, Counter{mine + seconds, mine});
    st.total += seconds;
}

std::vector<DirEntry> DirHashTop::top(int runSide) const {
    std::vector<DirEntry> out;
    if (runSide < 0 || runSide > 1 || displayN_ == 0) return out;
    std::lock_guard<std::mutex> lk(mtx_);
    const SideState& st = sides_[runSide];
    out.reserve(st.map.size());
    for (const auto& kv : st.map) {
        out.push_back({kv.first, kv.second.est});
    }
    std::sort(out.begin(), out.end(),
              [](const DirEntry& a, const DirEntry& b) { return a.seconds > b.seconds; });
    if (out.size() > displayN_) out.resize(displayN_);
    return out;
}

bool DirHashTop::empty(int runSide) const {
    if (runSide < 0 || runSide > 1) return true;
    std::lock_guard<std::mutex> lk(mtx_);
    return sides_[runSide].map.empty();
}

size_t DirHashTop::tracked(int runSide) const {
    if (runSide < 0 || runSide > 1) return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    return sides_[runSide].map.size();
}

void SnapshotRunTiming(const RunDirTiming& timing, DirTimingReport& out) {
    out.listA = timing.a.list.sorted();
    out.walkA = timing.a.walk.sorted();
    out.listB = timing.b.list.sorted();
    out.walkB = timing.b.walk.sorted();
    out.hashA = timing.hash.top(0);
    out.hashB = timing.hash.top(1);
}

} // namespace profiling
} // namespace bv
