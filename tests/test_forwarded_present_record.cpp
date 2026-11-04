#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "hook/present/forwarded_present_record.h"

// What a route that forwards a Present itself records (hook/present/forwarded_present_record.h), read back from a real
// CSV: XeFG's rows had no present columns (20261104_221303), and the presents before the post-FSR bypass took over had
// no row at all (20261104_222523).
namespace {

using DXGIShared::DX11PresentPerfRow;
using DXGIShared::ForwardedPresentRecord;
namespace fs = std::filesystem;
using CsvRow = std::map<std::string, std::string>;

class PerfCsv {
public:
    explicit PerfCsv(const char* name) : dir_(fs::temp_directory_path() / name) {
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        path_ = dir_ / "perf_metrics_record.csv";
        PerfLogger::Get().Shutdown();
        PerfLogger::Get().Init(path_.string().c_str());
    }
    ~PerfCsv() {
        PerfLogger::Get().Shutdown();
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
            std::vector<std::string> fields;
            std::stringstream stream(line);
            std::string field;
            while (std::getline(stream, field, ',')) {
                fields.push_back(field);
            }
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

void LogProcessFrameRow(int totalUs, int overlayUs) {
    FrameMetrics inner{};
    inner.qpcUs = PerfLogger::GetQpcUs();
    inner.totalUs = totalUs;
    inner.overlayUs = overlayUs;
    strncpy(inner.api, "DX12", sizeof(inner.api) - 1);
    PerfLogger::Get().LogFrame(inner);
}

// Work that takes at least this long on the row's clock: only lower bounds are asserted, so nothing depends on timing.
void SpendUs(int64_t us) {
    const int64_t startUs = PerfLogger::GetQpcUs();
    while (PerfLogger::GetQpcUs() - startUs < us) {
    }
}

}  // namespace

// XeFG's outputs: the frame work logs its ProcessFrame row, and the record completes it after the down-call.
TEST(ForwardedPresentRecordTest, AHoldingRecordCompletesTheFramesOwnRow) {
    PerfCsv csv("ce_forwarded_record_columns");
    {
        ForwardedPresentRecord record(true, "DXGIShared::RuntimeOwnedOutputPresent", /*holdsInnerRow=*/true);
        LogProcessFrameRow(42, 17);
        record.TimeLimiter([] { SpendUs(300); });
        const HRESULT hr = record.Forward([] {
            SpendUs(500);
            return S_OK;
        });
        EXPECT_EQ(hr, S_OK);
    }
    const std::vector<CsvRow> rows = csv.Rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(Column(rows[0], "total_us"), 42) << "the frame's own row";
    EXPECT_EQ(Column(rows[0], "overlay_us"), 17);
    EXPECT_GE(Column(rows[0], "present_call_us"), 500);
    EXPECT_GE(Column(rows[0], "fps_limit_wait_us"), 300);
    EXPECT_GE(Column(rows[0], "pre_present_us"), 300);
    EXPECT_GE(Column(rows[0], "post_present_us"), 0);
}

// The startup routes log no frame row of their own, so the record's row is the Present's.
TEST(ForwardedPresentRecordTest, ARouteWithoutAFrameRowWritesItsOwn) {
    PerfCsv csv("ce_forwarded_record_own");
    {
        ForwardedPresentRecord record(true, "Streamline startup normal-route Present");
        record.BeginDownCall();
        SpendUs(800);
        record.BeginDownCall();  // a guarded overlay invoke declined: what came before it is not the down-call
        const HRESULT hr = record.Forward([] { return S_OK; });
        EXPECT_EQ(hr, S_OK);
    }
    const std::vector<CsvRow> rows = csv.Rows();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].at("api"), "DX12");
    EXPECT_GE(Column(rows[0], "pre_present_us"), 800) << "the restart moved the declined call out of the down-call";
    EXPECT_GE(Column(rows[0], "total_us"), 800);
}

TEST(ForwardedPresentRecordTest, AnInertRecordWritesNothing) {
    PerfCsv csv("ce_forwarded_record_inert");
    {
        ForwardedPresentRecord record(false, "Streamline startup normal-route Present");
        record.TimeLimiter([] {});
        EXPECT_EQ(record.Forward([] { return E_FAIL; }), E_FAIL) << "the route's call still runs";
        EXPECT_EQ(record.NowUs(), 0);
    }
    EXPECT_TRUE(csv.Rows().empty());
}

// A route nested inside a hook that already holds its Present's row (Streamline's synchronous re-entry) must not write a
// second row: the outer hook's row already took the frame's, and LogFrame would pass this one straight to the file.
TEST(ForwardedPresentRecordTest, ANestedRouteLeavesTheRowToTheHookThatHoldsIt) {
    PerfCsv csv("ce_forwarded_record_nested");
    {
        DX11PresentPerfRow outer(true, "DX12", "DXGIShared::DetourPresent", /*holdsInnerRow=*/true);
        LogProcessFrameRow(42, 17);
        outer.BeginPresentCall();
        {
            ForwardedPresentRecord nested(true, "Streamline startup normal-route Present");
            EXPECT_EQ(nested.Forward([] { return S_OK; }), S_OK);
        }
        outer.EndPresentCall();
    }
    const std::vector<CsvRow> rows = csv.Rows();
    ASSERT_EQ(rows.size(), 1u) << "one Present, one row";
    EXPECT_EQ(Column(rows[0], "total_us"), 42);
}
