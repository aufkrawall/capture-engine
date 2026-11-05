#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "hook/d3d11/dx11_frame_phases.h"
#include "hook/metrics/perf_logger.h"
#include "hook/present/dx11_present_perf_row.h"
#include "hook/present/present_row_owner_log.h"

// The perf CSV row of one DX11/DX10 present (hook/present/dx11_present_perf_row.h), the row CE's DX11
// present hooks share, written to a real CSV. The hooks' use of it is pinned in
// test_dx11_perf_breakdown_source.cpp.
extern DX11FramePhases g_TestDX11FramePhases;  // tests/test_stubs.cpp

namespace {

using DXGIShared::DX11PresentPerfRow;
namespace fs = std::filesystem;

using CsvRow = std::map<std::string, std::string>;

std::vector<std::string> SplitCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }
    return fields;
}

// Opens a fresh perf CSV for one test and reads its rows back, keyed by the header's column names.
class PerfCsv {
public:
    explicit PerfCsv(const char* name) : dir_(fs::temp_directory_path() / name) {
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        path_ = dir_ / "perf_metrics_row.csv";
        PerfLogger::Get().Shutdown();  // Finalize any state a previous test may have left open.
        PerfLogger::Get().Init(path_.string().c_str());
    }
    ~PerfCsv() {
        PerfLogger::Get().Shutdown();
        g_TestDX11FramePhases = {};
        std::error_code ignored;
        fs::remove_all(dir_, ignored);
    }
    PerfCsv(const PerfCsv&) = delete;
    PerfCsv& operator=(const PerfCsv&) = delete;

    std::vector<CsvRow> Rows() {
        PerfLogger::Get().Shutdown();
        std::ifstream file(path_);
        std::string line;
        std::vector<std::string> header;
        std::vector<CsvRow> rows;
        while (std::getline(file, line)) {
            const std::vector<std::string> fields = SplitCsvLine(line);
            if (header.empty()) {
                header = fields;
                continue;
            }
            CsvRow row;
            for (size_t i = 0; i < header.size() && i < fields.size(); ++i) {
                row[header[i]] = fields[i];
            }
            rows.push_back(row);
        }
        return rows;
    }

private:
    fs::path dir_;
    fs::path path_;
};

int Column(const CsvRow& row, const char* name) {
    const auto it = row.find(name);
    EXPECT_NE(it, row.end()) << name;
    return it == row.end() ? -1 : std::stoi(it->second);
}

}  // namespace

// A session through DLSS FG, FSR FG, XeFG and DLSS FG again runs five routes in one process (20261105_011907), and each of
// the 15 that exist may log only its first row and its changes. The fifth used to log every row.
TEST(PresentRowOwnerLogTest, EveryRouteLogsItsFirstRowOnlyHoweverManyRan) {
    using DXGIShared::PresentRowOwner;
    using DXGIShared::PresentRowOwnerLog;
    const char* const routes[] = {"DXGIShared::DetourPresent",
                                  "DXGIShared::DetourPresent1",
                                  "CWrapDXGISwapChain::Present",
                                  "CWrapDXGISwapChain::Present1",
                                  "DXGIShared::RuntimeOwnedOutputPresent",
                                  "Streamline startup-handoff Present",
                                  "Streamline startup-handoff Present1",
                                  "Streamline startup normal-route Present",
                                  "Streamline startup normal-route Present1",
                                  "FFX startup-handoff Present",
                                  "FFX startup-handoff Present1",
                                  "app-thread post-FSR Streamline startup handoff",
                                  "app-thread post-FSR Streamline startup handoff Present1",
                                  "post-FSR confirmed standalone Present",
                                  "post-FSR confirmed standalone Present1"};
    FrameMetrics row{};
    std::strcpy(row.api, "DX12");
    PresentRowOwnerLog log;
    for (const char* route : routes) {
        EXPECT_TRUE(log.Observe(route, row, PresentRowOwner::kLogged).log) << route;
        EXPECT_FALSE(log.Observe(route, row, PresentRowOwner::kLogged).log) << route << " logged a second row";
    }
    for (int present = 0; present < 3; ++present) {
        for (const char* route : routes) {
            EXPECT_FALSE(log.Observe(route, row, PresentRowOwner::kLogged).log) << route;
        }
    }
}

TEST(PresentRowOwnerLogTest, ARouteLogsWhenWhatItWritesChangesAndCountsWhatItSwallowed) {
    using DXGIShared::PresentRowOwner;
    FrameMetrics row{};
    std::strcpy(row.api, "DX12");
    DXGIShared::PresentRowOwnerLog log;
    const char* const route = "DXGIShared::DetourPresent";
    EXPECT_TRUE(log.Observe(route, row, PresentRowOwner::kHeldInnerRow).log);
    EXPECT_FALSE(log.Observe(route, row, PresentRowOwner::kHeldInnerRow).log);
    const auto changed = log.Observe(route, row, PresentRowOwner::kLogged);
    EXPECT_TRUE(changed.log) << "the catch-all replaced the frame's own row";
    EXPECT_EQ(changed.suppressed, 1u);
    std::strcpy(row.api, "DX11");
    EXPECT_TRUE(log.Observe(route, row, PresentRowOwner::kLogged).log) << "another API on the same hook";
}

// DLSS FG after FSR FG with the Steam overlay wrote no row at all (20261104_210842). Its draw runs inside the route's own
// row, whose span already covers it, and the row still takes every column a frame's row has.
TEST(DX11PresentPerfRowTest, ADrawInsideTheRowsSpanCountsAsOverlayOnly) {
    PerfCsv csv("ce_dx12_bypass_perf_row");
    {
        DX11PresentPerfRow row(true, "DX12", "post-FSR confirmed standalone Present");
        ASSERT_TRUE(row.Enabled());
        PerfLogger::NoteDrawOutsideRow(row.NowUs(), 5000);
        row.NoteFpsLimitWaitSince(row.NowUs() - 3000);
        row.BeginPresentCall();
        row.EndPresentCall();
    }
    {
        DX11PresentPerfRow off(false, "DX12", "post-FSR confirmed standalone Present");
        (void)off;
    }
    const std::vector<CsvRow> rows = csv.Rows();
    ASSERT_EQ(rows.size(), 1u) << "one row per Present, none while the CSV is off";
    EXPECT_EQ(rows[0].at("api"), "DX12");
    EXPECT_EQ(Column(rows[0], "overlay_us"), 5000);
    EXPECT_GE(Column(rows[0], "fps_limit_wait_us"), 3000);
    EXPECT_LT(Column(rows[0], "total_us"), 5000) << "this span measured no such draw";
}

// DLSS FG's normal route draws in the routing, before the row opens, and the row is ProcessFrame's: overlay_us read 0 and
// total_us about 8 us while CE spent 116-172 us per Present (20261104_221303).
TEST(DX11PresentPerfRowTest, ADrawBeforeTheRowCountsInAHeldRow) {
    PerfCsv csv("ce_dx12_postsl_held_perf_row");
    PerfLogger::NoteDrawOutsideRow(PerfLogger::GetQpcUs(), 90);
    {
        DX11PresentPerfRow row(true, "DX12", "DXGIShared::DetourPresent", /*holdsInnerRow=*/true);
        FrameMetrics inner{};
        inner.qpcUs = PerfLogger::GetQpcUs();
        inner.totalUs = 8;
        inner.overlayUs = 3;
        strncpy(inner.api, "DX12", sizeof(inner.api) - 1);
        PerfLogger::Get().LogFrame(inner);
    }
    const std::vector<CsvRow> rows = csv.Rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(Column(rows[0], "overlay_us"), 93);
    EXPECT_EQ(Column(rows[0], "total_us"), 98);
}

TEST(DX11PresentPerfRowTest, ADrawBeforeACatchAllRowAddsToItsTotal) {
    PerfCsv csv("ce_dx12_postsl_catchall_perf_row");
    PerfLogger::NoteDrawOutsideRow(PerfLogger::GetQpcUs() - 1, 1000000);
    { DX11PresentPerfRow row(true, "DX12", "test", /*holdsInnerRow=*/true); }
    const std::vector<CsvRow> rows = csv.Rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(Column(rows[0], "overlay_us"), 1000000);
    EXPECT_GE(Column(rows[0], "total_us"), 1000000);
}

// One draw, one row: the next Present on this thread does not report it again.
TEST(DX11PresentPerfRowTest, ADrawGoesToOneRow) {
    PerfCsv csv("ce_dx12_postsl_once_perf_row");
    PerfLogger::NoteDrawOutsideRow(PerfLogger::GetQpcUs(), 70);
    { DX11PresentPerfRow first(true, "DX12", "test"); }
    { DX11PresentPerfRow second(true, "DX12", "test"); }
    const std::vector<CsvRow> rows = csv.Rows();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(Column(rows[0], "overlay_us"), 70);
    EXPECT_EQ(Column(rows[1], "overlay_us"), 0);
}

TEST(DX11PresentPerfRowTest, LogsOneRowWithCEsPhasesAndTheDownCallSplit) {
    PerfCsv csv("ce_dx11_present_perf_row_split");
    ASSERT_TRUE(PerfLogger::Get().IsEnabled());
    {
        DX11PresentPerfRow row(true, "DX11", "test");
        ASSERT_TRUE(row.Enabled());
        g_TestDX11FramePhases = {110, 120, 130};  // this present's frame processing
        row.TakeFramePhases();
        const int64_t limiterStartUs = row.NowUs();
        EXPECT_GT(limiterStartUs, 0);
        row.NoteFpsLimitWaitSince(limiterStartUs);
        row.BeginPresentCall();
        row.EndPresentCall();
    }
    const std::vector<CsvRow> rows = csv.Rows();
    ASSERT_EQ(rows.size(), 1u) << "one present, one row";
    const CsvRow& row = rows[0];
    EXPECT_EQ(row.at("api"), "DX11");
    EXPECT_EQ(Column(row, "overlay_us"), 110);
    EXPECT_EQ(Column(row, "capture_us"), 120);
    EXPECT_EQ(Column(row, "overlay_gpu_us"), 130);
    EXPECT_GE(Column(row, "fps_limit_wait_us"), 0);
    const int pre = Column(row, "pre_present_us");
    const int call = Column(row, "present_call_us");
    const int post = Column(row, "post_present_us");
    EXPECT_GE(pre, 0);
    EXPECT_GE(call, 0);
    EXPECT_GE(post, 0);
    // The three spans tile the hook: CE's work before the down-call, the down-call, CE's work after it.
    EXPECT_EQ(Column(row, "total_us"), pre + call + post);
}

TEST(DX11PresentPerfRowTest, PhasesAnEarlierFrameLeftBehindAreNotThisPresents) {
    PerfCsv csv("ce_dx11_present_perf_row_stale");
    // A DX11 present on this thread that returned between its frame processing and its row's
    // TakeFramePhases (the wrapper's pass-through after DX11_ProcessFrameExternal) left them behind; this
    // present's frame processing then times nothing.
    g_TestDX11FramePhases = {7, 8, 9};
    {
        DX11PresentPerfRow row(true, "DX11", "test");
        row.TakeFramePhases();
    }
    const std::vector<CsvRow> rows = csv.Rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(Column(rows[0], "overlay_us"), 0);
    EXPECT_EQ(Column(rows[0], "capture_us"), 0);
    EXPECT_EQ(Column(rows[0], "overlay_gpu_us"), 0);
}

TEST(DX11PresentPerfRowTest, AnInnerRowReplacesThePresentRow) {
    PerfCsv csv("ce_dx11_present_perf_row_inner");
    {
        DX11PresentPerfRow row(true, "DX11", "test");
        // The forwarded present logged a row of its own (a per-API ProcessFrame row).
        FrameMetrics inner{};
        inner.qpcUs = PerfLogger::GetQpcUs();
        strncpy(inner.api, "DX12", sizeof(inner.api) - 1);
        PerfLogger::Get().LogFrame(inner);
        row.BeginPresentCall();
        row.EndPresentCall();
    }
    const std::vector<CsvRow> rows = csv.Rows();
    ASSERT_EQ(rows.size(), 1u) << "a present never writes two rows";
    EXPECT_EQ(rows[0].at("api"), "DX12");
}

TEST(DX11PresentPerfRowTest, AnEarlyExitStillLogsTheRow) {
    PerfCsv csv("ce_dx11_present_perf_row_early_exit");
    {
        DX11PresentPerfRow row(true, "DX10", "test");
        row.EndPresentCall();  // no down-call was timed: nothing to split
    }
    const std::vector<CsvRow> rows = csv.Rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].at("api"), "DX10");
    EXPECT_GE(Column(rows[0], "total_us"), 0);
    EXPECT_EQ(Column(rows[0], "pre_present_us"), 0);
    EXPECT_EQ(Column(rows[0], "present_call_us"), 0);
    EXPECT_EQ(Column(rows[0], "post_present_us"), 0);
}

TEST(DX11PresentPerfRowTest, ADisabledRowIsInert) {
    PerfCsv csv("ce_dx11_present_perf_row_disabled");
    // An enclosing present already logged its row: a disabled row (a D3D12 Present1, or the CSV off) must
    // neither reopen the dedup scope nor take the DX11 phases another row owns.
    PerfLogger::BeginPresentRowScope();
    FrameMetrics outer{};
    outer.qpcUs = PerfLogger::GetQpcUs();
    strncpy(outer.api, "DX12", sizeof(outer.api) - 1);
    PerfLogger::Get().LogFrame(outer);
    g_TestDX11FramePhases = {1, 2, 3};
    {
        DX11PresentPerfRow row(false, "DX11", "test");
        EXPECT_FALSE(row.Enabled());
        EXPECT_EQ(row.NowUs(), 0);
        row.TakeFramePhases();
        row.NoteFpsLimitWaitSince(0);
        row.BeginPresentCall();
        row.EndPresentCall();
    }
    EXPECT_TRUE(PerfLogger::InnerRowLoggedInPresentRowScope());
    EXPECT_EQ(g_TestDX11FramePhases.overlayUs, 1);
    EXPECT_EQ(g_TestDX11FramePhases.captureUs, 2);
    EXPECT_EQ(g_TestDX11FramePhases.overlayGpuUs, 3);
    const std::vector<CsvRow> rows = csv.Rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].at("api"), "DX12");
}

// A D3D12 present (2026-10-18): its row is the one its ProcessFrame logs before the Present; the DXGI
// detour's row object holds it and logs it once the down-call returned, with the split and this
// present's limiter wait, keeping ProcessFrame's own total_us and columns.
TEST(DX11PresentPerfRowTest, AHeldInnerRowGetsTheDownCallSplit) {
    PerfCsv csv("ce_dx11_present_perf_row_held");
    {
        DX11PresentPerfRow row(true, "DX12", "test", true);
        FrameMetrics inner{};
        inner.qpcUs = PerfLogger::GetQpcUs();
        inner.totalUs = 42;
        inner.overlayUs = 17;
        inner.prerenderWaitUs = 5;
        inner.fpsLimitWaitUs = 999999;  // the previous present's wait: all ProcessFrame can know
        strncpy(inner.api, "DX12", sizeof(inner.api) - 1);
        PerfLogger::Get().LogFrame(inner);
        EXPECT_TRUE(PerfLogger::InnerRowLoggedInPresentRowScope());
        const int64_t limiterStartUs = row.NowUs();
        row.NoteFpsLimitWaitSince(limiterStartUs);
        row.BeginPresentCall();
        row.EndPresentCall();
    }
    const std::vector<CsvRow> rows = csv.Rows();
    ASSERT_EQ(rows.size(), 1u) << "held, not written twice";
    const CsvRow& row = rows[0];
    EXPECT_EQ(row.at("api"), "DX12");
    EXPECT_EQ(Column(row, "total_us"), 42) << "ProcessFrame's span";
    EXPECT_EQ(Column(row, "overlay_us"), 17);
    EXPECT_EQ(Column(row, "prerender_wait_us"), 5);
    EXPECT_LT(Column(row, "fps_limit_wait_us"), 999999) << "this present's limiter wait";
    EXPECT_GE(Column(row, "pre_present_us"), 0);
    EXPECT_GE(Column(row, "present_call_us"), 0);
    EXPECT_GE(Column(row, "post_present_us"), 0);
}

TEST(DX11PresentPerfRowTest, AHoldingRowWithoutAnInnerRowLogsTheCatchAll) {
    PerfCsv csv("ce_dx11_present_perf_row_held_none");
    {
        DX11PresentPerfRow row(true, "DX12", "test", true);
        row.BeginPresentCall();
        row.EndPresentCall();
    }
    const std::vector<CsvRow> rows = csv.Rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].at("api"), "DX12");
    EXPECT_EQ(Column(rows[0], "total_us"),
              Column(rows[0], "pre_present_us") + Column(rows[0], "present_call_us") +
                  Column(rows[0], "post_present_us"));
}

// The sink belongs to the row that set it: once that row ended, a row is written again at once.
TEST(DX11PresentPerfRowTest, TheHoldEndsWithItsRow) {
    PerfCsv csv("ce_dx11_present_perf_row_hold_scope");
    { DX11PresentPerfRow row(true, "DX12", "test", true); }
    PerfLogger::BeginPresentRowScope();
    FrameMetrics later{};
    later.qpcUs = PerfLogger::GetQpcUs();
    strncpy(later.api, "DXGI", sizeof(later.api) - 1);
    PerfLogger::Get().LogFrame(later);
    const std::vector<CsvRow> rows = csv.Rows();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[1].at("api"), "DXGI");
}
