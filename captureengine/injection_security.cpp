#include "injection_internal.h"

#include "injection_path_policy.h"

bool InjectionManager::ValidateDllSecurity(const std::string& dllPath) {
    char exePathBuf[MAX_PATH];
    GetModuleFileNameA(NULL, exePathBuf, MAX_PATH);
    fs::path exePath = fs::path(exePathBuf).parent_path();
    fs::path checkPath = fs::absolute(dllPath);

    // 1. Path Validation
    std::error_code ec;
    auto canonicalCheck = fs::weakly_canonical(checkPath, ec);
    auto canonicalExe = fs::weakly_canonical(exePath, ec);
    if (ec || !ce::injection::IsPathInsideDirectory(canonicalCheck.string(), canonicalExe.string())) {
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
    // Convert to wide string for WinVerifyTrust. The ANSI codepage matches what
    // GetModuleFileNameA/LoadLibraryA interpreted, so non-ASCII install paths
    // verify the exact file the loader opens instead of a sign-extension-mangled
    // name. Conversion failure fails closed.
    const std::wstring widePath = ce::injection::AnsiPathToWide(dllPath);
    if (widePath.empty()) {
        if (logFailures) {
            LogError("[Security] Could not convert DLL path for signature verification: %s", dllPath.c_str());
        }
        return false;
    }

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
