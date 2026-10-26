#include "sampleable_source_copy.h"

#include "mediaengine/engine/mediaengine.h"
#include "video_format_policy.h"

ID3D11Texture2D* SampleableSourceCopy::Prepare(ID3D11Device* device, ID3D11DeviceContext* context,
                                               ID3D11Texture2D* source) {
    if (!device || !context || !source)
        return nullptr;
    D3D11_TEXTURE2D_DESC sourceDesc = {};
    source->GetDesc(&sourceDesc);
    const bool resolve = sourceDesc.SampleDesc.Count > 1;
    // A resolve needs a typed format; a copy keeps the source's own.
    const DXGI_FORMAT format =
        resolve ? ce::video_format::GetRgbShaderResourceViewFormat(sourceDesc.Format) : sourceDesc.Format;
    if (format == DXGI_FORMAT_UNKNOWN)
        return nullptr;
    if (!copy_ || width_ != sourceDesc.Width || height_ != sourceDesc.Height || format_ != format) {
        copy_.reset();
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = sourceDesc.Width;
        desc.Height = sourceDesc.Height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const HRESULT hr = device->CreateTexture2D(&desc, nullptr, copy_.addressof());
        DLL_Log("[ShaderInput] Sampleable %s of the capture source: %ux%u fmt=%d->%d samples=%u bind=%x HR=%x",
                resolve ? "resolve" : "copy", sourceDesc.Width, sourceDesc.Height, sourceDesc.Format, format,
                sourceDesc.SampleDesc.Count, sourceDesc.BindFlags, static_cast<unsigned>(hr));
        if (FAILED(hr)) {
            copy_.reset();
            width_ = height_ = 0;
            format_ = DXGI_FORMAT_UNKNOWN;
            return nullptr;
        }
        width_ = sourceDesc.Width;
        height_ = sourceDesc.Height;
        format_ = format;
    }
    if (resolve)
        context->ResolveSubresource(copy_.get(), 0, source, 0, format);
    else
        context->CopySubresourceRegion(copy_.get(), 0, 0, 0, 0, source, 0, nullptr);
    return copy_.get();
}

void SampleableSourceCopy::Reset() {
    copy_.reset();
    width_ = height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
}
