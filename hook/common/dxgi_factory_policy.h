#pragma once

#include <dxgi.h>

namespace ce::dxgi_factory_policy {

inline bool ShouldLogAdapterEnumerationFailure(HRESULT hr) {
    return FAILED(hr) && hr != DXGI_ERROR_NOT_FOUND;
}

}  // namespace ce::dxgi_factory_policy
