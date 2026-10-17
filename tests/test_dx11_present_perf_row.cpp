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
