#pragma once

// Spelling of the session-wide object names: named events, mutexes and file mappings that every process of the
// Windows session shares. Product builds keep the shipped spelling, which is the ABI with games, the Vulkan layer and
// installed peers. Unit-test and FG flow-test builds (CE_UNIT_TESTS, CE_FLOW_TEST) spell them in their own namespace,
// so a CaptureEngine running in the same session can neither be reached by a test nor reach into one. Every
// session-wide name is built from these macros. PID-keyed names are unique per process and need no namespace.

#if defined(CE_UNIT_TESTS)
#define CE_SESSION_NAMESPACE L"Local\\CE_UT_"
#define CE_SESSION_CLAIM_NAME L"Local\\CE_UT_CaptureEngine_Instance_Mutex"
#elif defined(CE_FLOW_TEST)
#define CE_SESSION_NAMESPACE L"Local\\CE_FG_"
#define CE_SESSION_CLAIM_NAME L"Local\\CE_FG_CaptureEngine_Instance_Mutex"
#else
#define CE_SESSION_NAMESPACE L"Local\\CE_"
#define CE_SESSION_CLAIM_NAME L"Local\\CaptureEngine_Instance_Mutex"
#endif

// The shipped spellings, for checks that must hold in every build.
#define CE_PRODUCT_SESSION_NAMESPACE L"Local\\CE_"
#define CE_PRODUCT_SESSION_CLAIM_NAME L"Local\\CaptureEngine_Instance_Mutex"
