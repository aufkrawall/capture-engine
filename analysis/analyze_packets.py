import csv
import sys
import statistics


def analyze(csv_file):
    pts_list = []
    dur_list = []
    with open(csv_file, "r") as f:
        reader = csv.reader(f)
        for row in reader:
            if not row or len(row) < 2:
                continue
            try:
                pts = float(row[0])
                dur = float(row[1])
                pts_list.append(pts)
                dur_list.append(dur)
            except:
                pass

    if not pts_list:
        print("No packets")
        return

    def analyze_window(name, start_idx, end_idx):
        window_pts = pts_list[start_idx:end_idx]
        window_durs = dur_list[start_idx:end_idx]

        # Calculate actual delta PTS between consecutive frames
        delta_pts = [
            window_pts[i] - window_pts[i - 1] for i in range(1, len(window_pts))
        ]

        print(f"--- {name} ---")
        print(f"Frames: {len(window_pts)}")
        if not delta_pts:
            return
        print(f"Duration: {window_pts[-1] - window_pts[0]:.3f}s")
        print(f"Delta PTS Avg: {statistics.mean(delta_pts):.6f}s")
        print(f"Delta PTS Min: {min(delta_pts):.6f}s")
        print(f"Delta PTS Max: {max(delta_pts):.6f}s")

        # Check standard deviation of frame times
        print(
            f"Delta PTS StdDev: {statistics.stdev(delta_pts) if len(delta_pts) > 1 else 0:.6f}s"
        )

        # Count non-standard frame times (assuming 120fps -> 0.008333)
        expected_dur = 1.0 / 120.0
        abnormal = sum(1 for d in delta_pts if abs(d - expected_dur) > 0.001)
        print(
            f"Frames NOT ~8.33ms apart: {abnormal} ({abnormal / len(delta_pts) * 100:.1f}%)"
        )

        # Let's see some samples
        print(f"First 10 deltas: {[round(d, 5) for d in delta_pts[:10]]}")
        print()

    N = len(pts_list)
    fps120 = 120 * 10  # 10 seconds worth of frames ~ 1200

    analyze_window("First 10s", 0, min(fps120, N))

    mid_start = max(0, N // 2 - fps120 // 2)
    analyze_window("Middle 10s", mid_start, min(mid_start + fps120, N))

    analyze_window("Last 10s", max(0, N - fps120), N)


if __name__ == "__main__":
    analyze(sys.argv[1])
