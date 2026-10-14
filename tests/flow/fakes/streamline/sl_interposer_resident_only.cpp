// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

// Linked only into the takeover_resident/ variant of the fake interposer: an
// export the configured copy lacks, as an older build of the same Streamline
// generation can. A game importing it must keep its own interposer.
extern "C" __declspec(dllexport) int flowResidentOnlyExport() {
    return 7;
}
