#include "pacing_trace.h"
#include "pacing_trace_boundary.h"
#include "pacing_trace_analysis.h"
#include "perf_logger.h"
#include "hook_common.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace ce::pacing_trace {
namespace {
Ring<49152> ring;
Ring<16384> submissions;
std::atomic<bool> enabled{false};
std::atomic<uint64_t> epoch{0};
std::atomic<int64_t> epochTime{0};
std::mutex sessionMutex;
std::filesystem::path directory;
uint64_t sessionStart = 0;
uint64_t submissionStart = 0;
unsigned saves = 0;
int64_t lastWindow = 0, lastManual = 0;
bool keyWasDown = false;
EpisodeDetector detector;

std::vector<Event> History() {
    auto events = ring.Snapshot(sessionStart);
    auto queueEvents = submissions.Snapshot(submissionStart);
    events.insert(events.end(), queueEvents.begin(), queueEvents.end());
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.timeUs < b.timeUs; });
    return events;
}

void Save(const std::vector<Event>& events, const char* reason, int64_t now) {
    // Per-process/session ceiling, automatic and manual combined. No unbounded dump storm.
    if (saves >= 6 || events.empty()) {
        HookLogImportant("[PacingTrace] save skipped reason=%s (%s)", reason,
                         saves >= 6 ? "session limit reached" : "no observations");
        return;
    }
    const auto analysisStarted = PerfLogger::GetQpcUs();
    const auto analysis = Analyze(events);
    const auto analysisUs = PerfLogger::GetQpcUs() - analysisStarted;
    const auto saveStarted = PerfLogger::GetQpcUs();
    const auto path = directory / ("pacing_trace_" + std::to_string(GetCurrentProcessId()) + "_" +
        std::to_string(now) + ".csv");
    FILE* file = _wfopen(path.c_str(), L"w");
    if (!file) { HookLogImportant("[PacingTrace] save failed error=%lu", GetLastError()); return; }
    ++saves;
    setvbuf(file, nullptr, _IOFBF, 256 * 1024);
    const auto metric = [&](const char* name, const TraceMetric& value) {
        fprintf(file, "# summary_%s n=%llu mean_us=%llu p95_us=%llu max_us=%llu\n", name,
            static_cast<unsigned long long>(value.samples), static_cast<unsigned long long>(value.meanUs),
            static_cast<unsigned long long>(value.p95Us), static_cast<unsigned long long>(value.maxUs));
    };
    fprintf(file, "# version=3 reason=%s suspect_only=1 dropped=%llu events=%zu core_capacity=49152 submission_capacity=16384 save=%u/6\n",
            reason, static_cast<unsigned long long>(ring.Dropped() + submissions.Dropped()), events.size(), saves);
    fprintf(file, "# submission history is independently bounded and may start later than core history; marker_observed flags=1 means latest committed slot at callback entry, flags=0 means reuse check\n");
    fprintf(file, "# kinds=0:epoch,1:display_pair,2:callback_begin,3:callback_end,4:work,5:submit,6:fence,7:marker,8:frame,9:marker_observed,10:fence_signal\n");
    fprintf(file, "# kinds=11:present_begin,12:present_forward,13:present_end; flags stage=0:proxy,1:proxy1,2:detour,3:detour1,4:forward,5:forward1; pair by thread+id, nested spans overlap\n");
    fprintf(file, "# present begin/forward:a=sync,b=DXGI_flags; end:a=elapsed_us,b=HRESULT_bits,c=result_known; object=swapchain; forward stage includes CE routing/waits and foreign/driver calls, NOT pure GPU or driver time\n");
    fprintf(file, "# display:a=present_start_us,b=stream_generation,flags=resolved; callback:id=FSR_frame_id,object=list,flags=generated; callback_end:a=total_us,b=wrapped_us; work:a=overlay_bit1_selfcompose_bit2\n");
    fprintf(file, "# submit:object=queue,a=first_list_ptr,b=list_count,flags=CE_bit1_return_bit2; fence:object=fence,a=completed,b=slot,c=pool_size; marker:object=list,a=value,b=slot,c=buffer_ptr; marker_observed:object=buffer,a=observed,b=expected,c=slot; frame:a=total_us,b=overlay_us,c=fence_wait_us; fence_signal:object=fence,a=value,b=queue,c=HRESULT\n");
    fprintf(file, "# IDs are local to their source; display pairs use host PresentStart, not FSR frame IDs. Pointer reuse requires time/epoch matching. Observed fence/marker progress is a bound, not a GPU timestamp.\n");
    fprintf(file, "# summary window_us=%lld epoch=%llu matched_present=%llu unmatched_begin=%llu unmatched_end=%llu invalid_present=%llu latest_complete=%llu latest_pending=%llu unmatched_marker=%llu\n",
        static_cast<long long>(analysis.windowUs), static_cast<unsigned long long>(analysis.epoch),
        static_cast<unsigned long long>(analysis.matchedPresents), static_cast<unsigned long long>(analysis.unmatchedBegins),
        static_cast<unsigned long long>(analysis.unmatchedEnds), static_cast<unsigned long long>(analysis.invalidPresents),
        static_cast<unsigned long long>(analysis.latestComplete), static_cast<unsigned long long>(analysis.latestPending),
        static_cast<unsigned long long>(analysis.unmatchedMarkers));
    fprintf(file, "# summary is the trailing window filtered to its last epoch, not proof of stable gameplay or cause; nested durations overlap; marker ages bound completion, not execution duration; n=0 means unavailable\n");
    metric("proxy_prework", analysis.proxyPrework);
    metric("proxy_runtime", analysis.proxyRuntime);
    metric("detour_inclusive", analysis.detour);
    metric("forward_inclusive", analysis.forwarding);
    metric("callback_total", analysis.callback);
    metric("completed_marker_age_bound", analysis.completedMarkerAge);
    metric("pending_marker_age_bound", analysis.pendingMarkerAge);
    fprintf(file, "qpc_us,epoch,kind,thread,id,object,a,b,c,flags\n");
    for (const auto& e : events)
        fprintf(file, "%lld,%llu,%u,%u,%llu,%llu,%llu,%llu,%llu,%u\n",
            static_cast<long long>(e.timeUs), static_cast<unsigned long long>(e.epoch), static_cast<unsigned>(e.kind),
            e.thread, static_cast<unsigned long long>(e.id), static_cast<unsigned long long>(e.object),
            static_cast<unsigned long long>(e.a), static_cast<unsigned long long>(e.b),
            static_cast<unsigned long long>(e.c), e.flags);
    const bool failed = ferror(file) != 0;
    const int closeResult = fclose(file);
    HookLogImportant("[PacingTrace] %s reason=%s events=%zu spanMs=%lld dropped=%llu save=%u/6 ioMs=%lld file=%s",
        failed || closeResult != 0 ? "WRITE FAILED" : "saved", reason, events.size(),
        static_cast<long long>((events.back().timeUs - events.front().timeUs) / 1000),
        static_cast<unsigned long long>(ring.Dropped() + submissions.Dropped()), saves,
        static_cast<long long>((PerfLogger::GetQpcUs() - saveStarted) / 1000), path.filename().string().c_str());
    HookLogImportant("[PacingTraceSummary] epoch=%llu windowMs=%lld analysisUs=%lld preworkP95=%lluus runtimeP95=%lluus "
                     "detourInclusiveP95=%lluus forwardInclusiveP95=%lluus latestMarkerComplete=%llu pending=%llu "
                     "(n=0/unmatched details in CSV; CPU spans and completion bounds, not GPU durations or cause)",
        static_cast<unsigned long long>(analysis.epoch), static_cast<long long>(analysis.windowUs / 1000),
        static_cast<long long>(analysisUs),
        static_cast<unsigned long long>(analysis.proxyPrework.p95Us), static_cast<unsigned long long>(analysis.proxyRuntime.p95Us),
        static_cast<unsigned long long>(analysis.detour.p95Us), static_cast<unsigned long long>(analysis.forwarding.p95Us),
        static_cast<unsigned long long>(analysis.latestComplete), static_cast<unsigned long long>(analysis.latestPending));
}
}  // namespace

void Initialize(const char* perfPath) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    directory = std::filesystem::path(perfPath).parent_path();
    sessionStart = ring.Total();
    submissionStart = submissions.Total();
    saves = 0; lastWindow = lastManual = 0; keyWasDown = false; detector = {};
    enabled.store(true, std::memory_order_release);
    HookLogImportant("[PacingTrace] armed: 49152 core + 16384 submission events, 6 saves/session, 3 stable 2s suspect windows; manual Ctrl+Shift+F11 (hold briefly)");
}

void Record(Kind kind, uint64_t id, const void* object, uint64_t a, uint64_t b, uint64_t c,
            uint32_t flags, int64_t timeUs) {
    if (!enabled.load(std::memory_order_relaxed)) return;
    const Event event{timeUs ? timeUs : PerfLogger::GetQpcUs(), epoch.load(std::memory_order_relaxed), id,
        reinterpret_cast<uintptr_t>(object), a, b, c, GetCurrentThreadId(), flags, kind};
    if (kind == Kind::Submit) submissions.Push(event);
    else ring.Push(event);
}

bool Enabled() { return enabled.load(std::memory_order_relaxed); }
int64_t TraceBoundaryBackend::Now() { return PerfLogger::GetQpcUs(); }

void Epoch(uint64_t tag, int64_t timeUs) {
    epochTime.store(timeUs, std::memory_order_release);
    epoch.store(tag, std::memory_order_release);
    Record(Kind::Epoch, tag, nullptr, 0, 0, 0, 0, timeUs);
}

void Invalidate(int64_t timeUs) {
    epochTime.store(timeUs, std::memory_order_release);
    Record(Kind::Epoch, 0, nullptr, 0, 0, 0, 1, timeUs);
}

void Service() {
    if (!enabled.load(std::memory_order_relaxed) || HookIsShuttingDown()) return;
    std::lock_guard<std::mutex> lock(sessionMutex);
    const auto now = PerfLogger::GetQpcUs();
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    const bool down = foregroundPid == GetCurrentProcessId() &&
        (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
        (GetAsyncKeyState(VK_F11) & 0x8000);
    const bool manual = down && !keyWasDown && now - lastManual >= 5'000'000;
    keyWasDown = down;
    if (!manual && now - lastWindow < 2'000'000) return;
    const auto start = lastWindow;
    if (!manual) lastWindow = now;
    auto events = manual ? History() : ring.Snapshot(sessionStart, start);
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.timeUs < b.timeUs; });
    if (manual) { lastManual = now; Save(events, "manual", now); return; }
    const auto tag = epoch.load(std::memory_order_acquire);
    std::vector<uint32_t> displays, presents;
    int64_t firstScreen = 0, previousScreen = 0, previousPresent = 0;
    uint64_t generation = UINT64_MAX, previousId = 0;
    bool continuous = true;
    for (const auto& e : events) {
        if (e.kind != Kind::DisplayPair || e.timeUs <= start || e.timeUs > now) continue;
        if (e.epoch != tag || e.flags == 0 || !e.a || (generation != UINT64_MAX && generation != e.b)) {
            continuous = false; continue;
        }
        if (previousScreen) {
            if (e.id != previousId + 1 || e.timeUs <= previousScreen || e.a <= static_cast<uint64_t>(previousPresent))
                continuous = false;
            else {
                displays.push_back(static_cast<uint32_t>(e.timeUs - previousScreen));
                presents.push_back(static_cast<uint32_t>(e.a - previousPresent));
            }
        }
        if (!firstScreen) firstScreen = e.timeUs;
        previousScreen = e.timeUs; previousPresent = static_cast<int64_t>(e.a); previousId = e.id; generation = e.b;
    }
    const auto display = pacing_health::ComputeChannelStats(displays.data(), static_cast<uint32_t>(displays.size()));
    const auto present = pacing_health::ComputeChannelStats(presents.data(), static_cast<uint32_t>(presents.size()));
    const bool stable = continuous && previousScreen - firstScreen >= (now - start) * 3 / 4 &&
        now - previousScreen <= 250'000 && start > epochTime.load(std::memory_order_acquire) + 8'000'000 &&
        now - start <= 3'000'000 && foregroundPid == GetCurrentProcessId();
    if (detector.Observe(tag, stable, display, present)) {
        auto history = History();
        Save(history, "sustained-downstream-suspect", now);
    }
}
}  // namespace ce::pacing_trace
