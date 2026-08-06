#include "injection_internal.h"

// SHA256 using Windows CNG (bcrypt.dll)
[[maybe_unused]] static std::string ComputeFileHash(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return "";

    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
        return "";

    // Calculate buffer size
    DWORD cbHashObject = 0, cbData = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PBYTE)&cbHashObject, sizeof(DWORD), &cbData, 0);

    std::vector<BYTE> pbHashObject(cbHashObject);
    if (BCryptCreateHash(hAlg, &hHash, pbHashObject.data(), cbHashObject, NULL, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return "";
    }

    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        BCryptHashData(hHash, (PBYTE)buffer, (ULONG)file.gcount(), 0);
        if (file.eof())
            break;
    }

    DWORD cbHash = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PBYTE)&cbHash, sizeof(DWORD), &cbData, 0);
    std::vector<BYTE> pbHash(cbHash);
    BCryptFinishHash(hHash, pbHash.data(), cbHash, 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    std::stringstream ss;
    for (BYTE b : pbHash)
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    return ss.str();
}

bool InjectionManager::ValidateDllSecurity(const std::string& dllPath) {
    char exePathBuf[MAX_PATH];
    GetModuleFileNameA(NULL, exePathBuf, MAX_PATH);
    fs::path exePath = fs::path(exePathBuf).parent_path();
    fs::path checkPath = fs::absolute(dllPath);

    // 1. Path Validation
    std::error_code ec;
    auto canonicalCheck = fs::weakly_canonical(checkPath, ec);
    auto canonicalExe = fs::weakly_canonical(exePath, ec);
    if (ec || canonicalCheck.string().find(canonicalExe.string()) != 0 ||
        (canonicalCheck.string().size() > canonicalExe.string().size() &&
         canonicalCheck.string()[canonicalExe.string().size()] != '\\')) {
        LogError("[Security] DLL path is outside application directory: %s", checkPath.string().c_str());
        return false;
    }

    // 2. ACL Check (Check that no broad identity has Write Access). The hook
    // DLL is injected into third-party processes, so a writable install is a
    // code-execution vector for anyone who can drop a file next to it. World,
    // Authenticated Users, and BUILTIN\Users are all checked; checking only
    // "Everyone" missed default-inherited ACEs that grant write to the
    // authenticated-user group (e.g. user-writable install directories).
    PACL pDacl = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    if (GetNamedSecurityInfoA(dllPath.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &pDacl, NULL,
                              &pSD) == ERROR_SUCCESS) {
        struct SidDefinition {
            SID_IDENTIFIER_AUTHORITY authority;
            BYTE subAuthorityCount;
            DWORD subAuthorities[2];
            const char* displayName;
        };
        static const SidDefinition kWriteProtectionSids[] = {
            {SECURITY_WORLD_SID_AUTHORITY, 1, {SECURITY_WORLD_RID, 0}, "Everyone"},
            {SECURITY_NT_AUTHORITY, 1, {SECURITY_AUTHENTICATED_USER_RID, 0}, "Authenticated Users"},
            {SECURITY_NT_AUTHORITY, 2, {SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS}, "BUILTIN\\Users"},
        };
        bool writableByBroadIdentity = false;
        for (const SidDefinition& sidDefinition : kWriteProtectionSids) {
            TRUSTEE_A trustee = {};
            trustee.TrusteeForm = TRUSTEE_IS_SID;
            trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;

            SID_IDENTIFIER_AUTHORITY authority = sidDefinition.authority;
            PSID pSid = NULL;
            if (!AllocateAndInitializeSid(&authority, sidDefinition.subAuthorityCount,
                                          sidDefinition.subAuthorities[0], sidDefinition.subAuthorities[1], 0, 0, 0,
                                          0, 0, 0, &pSid)) {
                continue;
            }
            trustee.ptstrName = (LPSTR)pSid;

            ACCESS_MASK access = 0;
            GetEffectiveRightsFromAclA(pDacl, &trustee, &access);
            FreeSid(pSid);

            if (access & (FILE_WRITE_DATA | FILE_APPEND_DATA | WRITE_DAC | WRITE_OWNER)) {
                LogError("[Security] DLL is writable by %s! Access Mask: 0x%lX", sidDefinition.displayName, access);
                writableByBroadIdentity = true;
            }
        }
        LocalFree(pSD);  // also frees pDacl if it points into pSD

        if (writableByBroadIdentity) {
#ifdef CE_PRODUCTION_BUILD
            return false;  // Strict mode in production
#else
            LogError("[Security] WARNING: Proceeding despite broadly writable DLL (dev build)");
#endif
        }
    }

    LogInfo("[Security] DLL security validation passed for %s", dllPath.c_str());
    return true;
}

// Verify DLL Authenticode signature (production builds only)
// Returns true if DLL is properly signed, false otherwise
bool InjectionManager::VerifyDLLSignature(const std::string& dllPath, bool logFailures) {
    // Convert to wide string for WinVerifyTrust
    std::wstring widePath(dllPath.begin(), dllPath.end());

    // Setup WINTRUST_FILE_INFO structure
    WINTRUST_FILE_INFO fileInfo = {};
    fileInfo.cbStruct = sizeof(WINTRUST_FILE_INFO);
    fileInfo.pcwszFilePath = widePath.c_str();
    fileInfo.hFile = NULL;
    fileInfo.pgKnownSubject = NULL;

    // Setup WINTRUST_DATA structure
    GUID policyGUID = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA trustData = {};
    trustData.cbStruct = sizeof(WINTRUST_DATA);
    trustData.pPolicyCallbackData = NULL;
    trustData.pSIPClientData = NULL;
    trustData.dwUIChoice = WTD_UI_NONE;
    // Primary pass is offline-tolerant (WTD_REVOKE_NONE): revocation must
    // never block injection on an offline/air-gapped host. Production builds
    // additionally run a revocation-confirmation pass below.
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.hWVTStateData = NULL;
    trustData.pwszURLReference = NULL;
    trustData.dwProvFlags = WTD_SAFER_FLAG;
    trustData.dwUIContext = 0;
    trustData.pFile = &fileInfo;

    // Verify signature
    LONG status = WinVerifyTrust(NULL, &policyGUID, &trustData);

    // Cleanup
    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &policyGUID, &trustData);

    if (status == ERROR_SUCCESS) {
#ifdef CE_PRODUCTION_BUILD
        // Revocation-confirmation pass: only a *confirmed* revocation is fatal.
        // Offline/unavailable revocation results (CERT_E_REVOCATION_FAILURE,
        // CRYPT_E_REVOCATION_OFFLINE) keep the offline-tolerant primary result
        // — a documented availability decision, not a silent weakening.
        WINTRUST_DATA revocationTrustData = trustData;
        revocationTrustData.dwStateAction = WTD_STATEACTION_VERIFY;
        revocationTrustData.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
        revocationTrustData.hWVTStateData = NULL;
        const LONG revocationStatus = WinVerifyTrust(NULL, &policyGUID, &revocationTrustData);
        revocationTrustData.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(NULL, &policyGUID, &revocationTrustData);
        if (revocationStatus == TRUST_E_EXPLICIT_DISTRUST || revocationStatus == CRYPT_E_REVOKED ||
            revocationStatus == CERT_E_REVOKED) {
            LogError("[Security] DLL signature is revoked: %s (error 0x%lX)", dllPath.c_str(),
                     (unsigned long)revocationStatus);
            return false;
        }
        if (revocationStatus != ERROR_SUCCESS) {
            LogWarn(
                "[Security] Revocation check unavailable for %s (error 0x%lX); accepting the "
                "offline-tolerant signature result",
                dllPath.c_str(), (unsigned long)revocationStatus);
        }
#endif
        LogInfo("[Security] DLL signature valid: %s", dllPath.c_str());
        return true;
    } else {
        if (logFailures) {
            LogError("[Security] DLL signature verification failed: %s (error 0x%lu)", dllPath.c_str(),
                     (unsigned long)status);
            if (status == TRUST_E_NOSIGNATURE) {
                LogError("[Security] DLL is not signed");
            } else if (status == TRUST_E_EXPLICIT_DISTRUST) {
                LogError("[Security] DLL signature is explicitly distrusted");
            } else if (status == TRUST_E_SUBJECT_NOT_TRUSTED) {
                LogError("[Security] DLL signer is not trusted");
            } else if (status == CRYPT_E_SECURITY_SETTINGS) {
                LogError("[Security] Security settings prevent verification");
            }
        }
        return false;
    }
}

// Verify DLL integrity using SHA-256 hash comparison
// This is a fallback for debug builds when Authenticode signing is not
// available Expected hashes are stored in a .hashes file next to the DLL
bool InjectionManager::VerifyDLLHash(const std::string& dllPath) {
    // Check if hash file exists (debug builds only)
    std::string hashFilePath = dllPath + ".hash";
    if (!fs::exists(hashFilePath)) {
        // No hash file - skip verification in debug builds
        LogDebug("[Security] No hash file found for %s, skipping hash verification", dllPath.c_str());
        return true;
    }

    // Read expected hash from file
    std::ifstream hashFile(hashFilePath);
    if (!hashFile.is_open()) {
        LogWarn("[Security] Failed to open hash file: %s", hashFilePath.c_str());
        return true;  // Allow in debug mode
    }

    std::string expectedHash;
    std::getline(hashFile, expectedHash);
    hashFile.close();

    // Trim whitespace
    expectedHash.erase(0, expectedHash.find_first_not_of(" \t\r\n"));
    expectedHash.erase(expectedHash.find_last_not_of(" \t\r\n") + 1);

    if (expectedHash.empty()) {
        LogWarn("[Security] Empty hash file: %s", hashFilePath.c_str());
        return true;  // Allow in debug mode
    }

    // Compute actual hash of DLL
    std::string actualHash = ComputeFileHash(dllPath);
    if (actualHash.empty()) {
        LogError("[Security] Failed to compute hash for: %s", dllPath.c_str());
        return false;
    }

    // Compare hashes (case-insensitive)
    bool match = (actualHash.length() == expectedHash.length());
    if (match) {
        for (size_t i = 0; i < actualHash.length(); ++i) {
            if (std::tolower(static_cast<unsigned char>(actualHash[i])) !=
                std::tolower(static_cast<unsigned char>(expectedHash[i]))) {
                match = false;
                break;
            }
        }
    }

    if (match) {
        LogInfo("[Security] DLL hash verified: %s", dllPath.c_str());
        return true;
    } else {
        LogError("[SECURITY] DLL hash mismatch for %s", dllPath.c_str());
        LogError("[SECURITY] Expected: %s", expectedHash.c_str());
        LogError("[SECURITY] Actual:   %s", actualHash.c_str());
        return false;
    }
}

// Compute SHA-256 hash of a file
std::string InjectionManager::ComputeFileHash(const std::string& filePath) {
    // Open file
    HANDLE hFile = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        LogError("[Security] Failed to open file for hashing: %s", filePath.c_str());
        return "";
    }

    // Get file size
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        CloseHandle(hFile);
        return "";
    }

    // Map file into memory for efficient hashing
    HANDLE hMapping = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMapping) {
        CloseHandle(hFile);
        return "";
    }

    LPVOID pData = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    if (!pData) {
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return "";
    }

    // Compute SHA-256 hash using BCrypt
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    std::string result;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == STATUS_SUCCESS) {
        DWORD hashLen = 0;
        DWORD dataLen = 0;

        if (BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &dataLen, 0) ==
            STATUS_SUCCESS) {
            std::vector<BYTE> hashBytes(hashLen);

            if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) == STATUS_SUCCESS) {
                if (BCryptHashData(hHash, (PUCHAR)pData, (ULONG)fileSize.QuadPart, 0) == STATUS_SUCCESS) {
                    if (BCryptFinishHash(hHash, hashBytes.data(), hashLen, 0) == STATUS_SUCCESS) {
                        // Convert to hex string
                        std::stringstream ss;
                        for (BYTE b : hashBytes) {
                            ss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
                        }
                        result = ss.str();
                    }
                }
                BCryptDestroyHash(hHash);
            }
        }
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }

    // Cleanup
    UnmapViewOfFile(pData);
    CloseHandle(hMapping);
    CloseHandle(hFile);

    return result;
}

void InjectionManager::ScanExistingProcesses() {
    const int64_t scanStartUs = Log_GetQpcUs();
    const int64_t snapshotStartUs = scanStartUs;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    const int64_t snapshotUs = Log_GetQpcUs() - snapshotStartUs;
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        LogInfo("[StartupPerf] ScanExistingProcesses: CreateToolhelp32Snapshot failed after %.3f ms (error=%lu)",
                QpcDeltaToMs(snapshotUs), GetLastError());
        return;
    }

    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);
    int scannedProcesses = 0;
    int whitelistedProcesses = 0;
    int injectAttempts = 0;
    int injectSuccesses = 0;

    if (Process32First(hSnapshot, &pe32)) {
        do {
            ++scannedProcesses;
            std::string name = pe32.szExeFile;
            std::lock_guard<std::mutex> injectLock(injectMutex);
            if (IsWhitelisted(name)) {
                ++whitelistedProcesses;
                if (!IsAlreadyInjectedLocked(pe32.th32ProcessID) && !IsRecentlyFailedLocked(pe32.th32ProcessID) &&
                    !IsAlreadyPendingLocked(pe32.th32ProcessID)) {
                    ++injectAttempts;
                    LogInfo("[Scan] Found existing whitelisted process: %s (PID: %lu)", name.c_str(),
                            (unsigned long)pe32.th32ProcessID);
                    pendingInjections.push_back(
                        {pe32.th32ProcessID, name, "StartupScan", GetTickCount64() + injection_kPendingInjectionDelayMs});
                    ++injectSuccesses;
                }
            }
        } while (Process32Next(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
    LogInfo(
        "[StartupPerf] ScanExistingProcesses: snapshot=%.3f ms, total=%.3f ms, scanned=%d, whitelisted=%d, "
        "injectAttempts=%d, queuedForDelayedInjection=%d",
        QpcDeltaToMs(snapshotUs), QpcDeltaToMs(Log_GetQpcUs() - scanStartUs), scannedProcesses, whitelistedProcesses,
        injectAttempts, injectSuccesses);
}
