#include "startup_control.h"
#include "../common/elevation_protocol.h"
#include "../common/elevation_windows.h"

namespace ce::startup {
DWORD WaitElevationServiceRemoved() {
    using ce::elevation::Handle;
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
        return GetLastError();
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
    if (!manager)
        return GetLastError();
    SERVICE_NOTIFYW notification{};
    DWORD result = ERROR_SUCCESS;
    const uint64_t deadline = GetTickCount64() + 15000;
    while (true) {
        notification = {};
        notification.dwVersion = SERVICE_NOTIFY_STATUS_CHANGE;
        notification.pContext = event.Get();
        notification.pfnNotifyCallback = [](void* context) {
            auto* notice = static_cast<SERVICE_NOTIFYW*>(context);
            SetEvent(static_cast<HANDLE>(notice->pContext));
        };
        ResetEvent(event.Get());
        result = NotifyServiceStatusChangeW(manager, SERVICE_NOTIFY_DELETED, &notification);
        if (result)
            break;
        // Register before observing absence to avoid missing the deletion edge.
        SC_HANDLE service = OpenServiceW(manager, ce::elevation::kServiceName, SERVICE_QUERY_STATUS);
        const DWORD query = service ? ERROR_SUCCESS : GetLastError();
        if (service)
            CloseServiceHandle(service);
        if (query == ERROR_SERVICE_DOES_NOT_EXIST)
            break;
        if (query && query != ERROR_SERVICE_MARKED_FOR_DELETE) {
            result = query;
            break;
        }
        DWORD wait = WAIT_IO_COMPLETION;
        while (wait == WAIT_IO_COMPLETION) {
            const uint64_t now = GetTickCount64();
            wait = WaitForSingleObjectEx(event.Get(), now < deadline ? static_cast<DWORD>(deadline - now) : 0, TRUE);
        }
        if (wait != WAIT_OBJECT_0) {
            result = ERROR_SERVICE_MARKED_FOR_DELETE;
            break;
        }
        result = notification.dwNotificationStatus;
        if (notification.pszServiceNames) {
            LocalFree(notification.pszServiceNames);
            notification.pszServiceNames = nullptr;
        }
        if (result)
            break;
    }
    CloseServiceHandle(manager);
    while (WaitForSingleObjectEx(event.Get(), 0, TRUE) == WAIT_IO_COMPLETION) {}
    if (notification.pszServiceNames)
        LocalFree(notification.pszServiceNames);
    return result;
}
}  // namespace ce::startup
