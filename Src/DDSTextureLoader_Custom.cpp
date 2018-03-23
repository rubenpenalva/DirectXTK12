//--------------------------------------------------------------------------------------
// File: DDSTextureLoader.cpp
//
// Functions for loading a DDS texture and creating a Direct3D runtime resource for it
//
// Note these functions are useful as a light-weight runtime loader for DDS files. For
// a full-featured DDS file reader, writer, and texture processing pipeline see
// the 'Texconv' sample and the 'DirectXTex' library.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// http://go.microsoft.com/fwlink/?LinkID=615561
//--------------------------------------------------------------------------------------

#include "pch.h"

#include "DDSTextureLoader_Custom.h"

#include "dds.h"
#include "PlatformHelpers.h"
#include "LoaderHelpers_Custom.h"

namespace DirectX
{
    uint32_t CountMips(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
            return 0;

        uint32_t count = 1;
        while (width > 1 || height > 1)
        {
            width >>= 1;
            height >>= 1;
            count++;
        }
        return count;
    }
}


using namespace DirectX;
using namespace DirectX::LoaderHelpers;

static_assert(static_cast<int>(DDS_DIMENSION_TEXTURE1D) == static_cast<int>(D3D12_RESOURCE_DIMENSION_TEXTURE1D), "dds mismatch");
static_assert(static_cast<int>(DDS_DIMENSION_TEXTURE2D) == static_cast<int>(D3D12_RESOURCE_DIMENSION_TEXTURE2D), "dds mismatch");
static_assert(static_cast<int>(DDS_DIMENSION_TEXTURE3D) == static_cast<int>(D3D12_RESOURCE_DIMENSION_TEXTURE3D), "dds mismatch");

namespace
{
    inline bool IsDepthStencil(DXGI_FORMAT fmt)
    {
        switch (fmt)
        {
            case DXGI_FORMAT_R32G8X24_TYPELESS:
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
            case DXGI_FORMAT_D32_FLOAT:
            case DXGI_FORMAT_R24G8_TYPELESS:
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
            case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
            case DXGI_FORMAT_D16_UNORM:

            #if defined(_XBOX_ONE) && defined(_TITLE)
            case DXGI_FORMAT_D16_UNORM_S8_UINT:
            case DXGI_FORMAT_R16_UNORM_X8_TYPELESS:
            case DXGI_FORMAT_X16_TYPELESS_G8_UINT:
            #endif
                return true;

            default:
                return false;
        }
    }

    //--------------------------------------------------------------------------------------
    inline void AdjustPlaneResource(
        _In_ DXGI_FORMAT fmt,
        _In_ size_t height,
        _In_ size_t slicePlane,
        _Inout_ D3D12_SUBRESOURCE_DATA& res)
    {
        switch (fmt)
        {
            case DXGI_FORMAT_NV12:
            case DXGI_FORMAT_P010:
            case DXGI_FORMAT_P016:

            #if defined(_XBOX_ONE) && defined(_TITLE)
            case DXGI_FORMAT_D16_UNORM_S8_UINT:
            case DXGI_FORMAT_R16_UNORM_X8_TYPELESS:
            case DXGI_FORMAT_X16_TYPELESS_G8_UINT:
            #endif
                if (!slicePlane)
                {
                    // Plane 0
                    res.SlicePitch = res.RowPitch * height;
                }
                else
                {
                    // Plane 1
                    res.pData = reinterpret_cast<const uint8_t*>(res.pData) + res.RowPitch * height;
                    res.SlicePitch = res.RowPitch * ((height + 1) >> 1);
                }
                break;

            case DXGI_FORMAT_NV11:
                if (!slicePlane)
                {
                    // Plane 0
                    res.SlicePitch = res.RowPitch * height;
                }
                else
                {
                    // Plane 1
                    res.pData = reinterpret_cast<const uint8_t*>(res.pData) + res.RowPitch * height;
                    res.RowPitch = (res.RowPitch >> 1);
                    res.SlicePitch = res.RowPitch * height;
                }
                break;

            default:
                break;
        }
    }

    //--------------------------------------------------------------------------------------
    HRESULT FillInitData(_In_ size_t width,
        _In_ size_t height,
        _In_ size_t depth,
        _In_ size_t mipCount,
        _In_ size_t arraySize,
        _In_ size_t numberOfPlanes,
        _In_ DXGI_FORMAT format,
        _In_ size_t maxsize,
        _In_ size_t bitSize,
        _In_reads_bytes_(bitSize) const uint8_t* bitData,
        _Out_ size_t& twidth,
        _Out_ size_t& theight,
        _Out_ size_t& tdepth,
        _Out_ size_t& skipMip,
        std::vector<D3D12_SUBRESOURCE_DATA>& initData)
    {
        if (!bitData)
        {
            return E_POINTER;
        }

        skipMip = 0;
        twidth = 0;
        theight = 0;
        tdepth = 0;

        size_t NumBytes = 0;
        size_t RowBytes = 0;
        const uint8_t* pEndBits = bitData + bitSize;

        initData.clear();

        for (size_t p = 0; p < numberOfPlanes; ++p)
        {
            const uint8_t* pSrcBits = bitData;

            for (size_t j = 0; j < arraySize; j++)
            {
                size_t w = width;
                size_t h = height;
                size_t d = depth;
                for (size_t i = 0; i < mipCount; i++)
                {
                    GetSurfaceInfo(w,
                        h,
                        format,
                        &NumBytes,
                        &RowBytes,
                        nullptr
                    );

                    if ((mipCount <= 1) || !maxsize || (w <= maxsize && h <= maxsize && d <= maxsize))
                    {
                        if (!twidth)
                        {
                            twidth = w;
                            theight = h;
                            tdepth = d;
                        }

                        D3D12_SUBRESOURCE_DATA res =
                        {
                            reinterpret_cast<const void*>(pSrcBits),
                            static_cast<LONG_PTR>(RowBytes),
                            static_cast<LONG_PTR>(NumBytes)
                        };

                        AdjustPlaneResource(format, h, p, res);

                        initData.emplace_back(res);
                    }
                    else if (!j)
                    {
                        // Count number of skipped mipmaps (first item only)
                        ++skipMip;
                    }

                    if (pSrcBits + (NumBytes*d) > pEndBits)
                    {
                        return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
                    }

                    pSrcBits += NumBytes * d;

                    w = w >> 1;
                    h = h >> 1;
                    d = d >> 1;
                    if (w == 0)
                    {
                        w = 1;
                    }
                    if (h == 0)
                    {
                        h = 1;
                    }
                    if (d == 0)
                    {
                        d = 1;
                    }
                }
            }
        }

        return initData.empty() ? E_FAIL : S_OK;
    }

    //--------------------------------------------------------------------------------------
    D3D12_RESOURCE_DESC CreateTextureResourceDesc(
        D3D12_RESOURCE_DIMENSION resDim,
        size_t width,
        size_t height,
        size_t depth,
        size_t mipCount,
        size_t arraySize,
        DXGI_FORMAT format,
        D3D12_RESOURCE_FLAGS resFlags,
        unsigned int loadFlags)
    {
        if (loadFlags & DDS_LOADER_FORCE_SRGB)
        {
            format = MakeSRGB(format);
        }

        D3D12_RESOURCE_DESC desc = {};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = static_cast<UINT16>(mipCount);
        desc.DepthOrArraySize = (resDim == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? static_cast<UINT16>(depth) : static_cast<UINT16>(arraySize);
        desc.Format = format;
        desc.Flags = resFlags;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Dimension = resDim;

        return desc;
    }

    //--------------------------------------------------------------------------------------
    HRESULT CreateTextureDescFromDDS(
        _In_ const DDS_HEADER* header,
        _In_reads_bytes_(bitSize) const uint8_t* bitData,
        size_t bitSize,
        size_t maxsize,
        D3D12_RESOURCE_FLAGS resFlags,
        unsigned int loadFlags,
        _Out_ D3D12_RESOURCE_DESC* textureResourceDesc,
        std::vector<D3D12_SUBRESOURCE_DATA>& subresources,
        UINT numberOfPlanes,
        _Out_opt_ bool* outIsCubeMap)
    {
        HRESULT hr = S_OK;

        if (!textureResourceDesc)
            return E_INVALIDARG;

        UINT width = header->width;
        UINT height = header->height;
        UINT depth = header->depth;

        D3D12_RESOURCE_DIMENSION resDim = D3D12_RESOURCE_DIMENSION_UNKNOWN;
        UINT arraySize = 1;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool isCubeMap = false;

        size_t mipCount = header->mipMapCount;
        if (0 == mipCount)
        {
            mipCount = 1;
        }

        if ((header->ddspf.flags & DDS_FOURCC) &&
            (MAKEFOURCC('D', 'X', '1', '0') == header->ddspf.fourCC))
        {
            auto d3d10ext = reinterpret_cast<const DDS_HEADER_DXT10*>((const char*)header + sizeof(DDS_HEADER));

            arraySize = d3d10ext->arraySize;
            if (arraySize == 0)
            {
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }

            switch (d3d10ext->dxgiFormat)
            {
                case DXGI_FORMAT_AI44:
                case DXGI_FORMAT_IA44:
                case DXGI_FORMAT_P8:
                case DXGI_FORMAT_A8P8:
                    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

                default:
                    if (BitsPerPixel(d3d10ext->dxgiFormat) == 0)
                    {
                        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                    }
            }

            format = d3d10ext->dxgiFormat;

            switch (d3d10ext->resourceDimension)
            {
                case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
                    // D3DX writes 1D textures with a fixed Height of 1
                    if ((header->flags & DDS_HEIGHT) && height != 1)
                    {
                        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                    }
                    height = depth = 1;
                    break;

                case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
                    if (d3d10ext->miscFlag & 0x4 /* RESOURCE_MISC_TEXTURECUBE */)
                    {
                        arraySize *= 6;
                        isCubeMap = true;
                    }
                    depth = 1;
                    break;

                case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
                    if (!(header->flags & DDS_HEADER_FLAGS_VOLUME))
                    {
                        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                    }

                    if (arraySize > 1)
                    {
                        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                    }
                    break;

                default:
                    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            }

            resDim = static_cast<D3D12_RESOURCE_DIMENSION>(d3d10ext->resourceDimension);
        }
        else
        {
            format = GetDXGIFormat(header->ddspf);

            if (format == DXGI_FORMAT_UNKNOWN)
            {
                return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            }

            if (header->flags & DDS_HEADER_FLAGS_VOLUME)
            {
                resDim = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            }
            else
            {
                if (header->caps2 & DDS_CUBEMAP)
                {
                    // We require all six faces to be defined
                    if ((header->caps2 & DDS_CUBEMAP_ALLFACES) != DDS_CUBEMAP_ALLFACES)
                    {
                        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                    }

                    arraySize = 6;
                    isCubeMap = true;
                }

                depth = 1;
                resDim = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

                // Note there's no way for a legacy Direct3D 9 DDS to express a '1D' texture
            }

            assert(BitsPerPixel(format) != 0);
        }

        // Bound sizes (for security purposes we don't trust DDS file metadata larger than the Direct3D hardware requirements)
        if (mipCount > D3D12_REQ_MIP_LEVELS)
        {
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }

        switch (resDim)
        {
            case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
                if ((arraySize > D3D12_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION) ||
                    (width > D3D12_REQ_TEXTURE1D_U_DIMENSION))
                {
                    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                }
                break;

            case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
                if (isCubeMap)
                {
                    // This is the right bound because we set arraySize to (NumCubes*6) above
                    if ((arraySize > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) ||
                        (width > D3D12_REQ_TEXTURECUBE_DIMENSION) ||
                        (height > D3D12_REQ_TEXTURECUBE_DIMENSION))
                    {
                        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                    }
                }
                else if ((arraySize > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) ||
                    (width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) ||
                    (height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION))
                {
                    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                }
                break;

            case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
                if ((arraySize > 1) ||
                    (width > D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION) ||
                    (height > D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION) ||
                    (depth > D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION))
                {
                    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                }
                break;

            default:
                return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }

        if (!numberOfPlanes)
            return E_INVALIDARG;

        if ((numberOfPlanes > 1) && IsDepthStencil(format))
        {
            // DirectX 12 uses planes for stencil, DirectX 11 does not
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }

        if (outIsCubeMap != nullptr)
        {
            *outIsCubeMap = isCubeMap;
        }

        // Create the texture
        size_t numberOfResources = (resDim == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
            ? 1 : arraySize;
        numberOfResources *= mipCount;
        numberOfResources *= numberOfPlanes;

        if (numberOfResources > D3D12_REQ_SUBRESOURCES)
            return E_INVALIDARG;

        subresources.reserve(numberOfResources);

        size_t skipMip = 0;
        size_t twidth = 0;
        size_t theight = 0;
        size_t tdepth = 0;
        hr = FillInitData(width, height, depth, mipCount, arraySize,
            numberOfPlanes, format,
            maxsize, bitSize, bitData,
            twidth, theight, tdepth, skipMip, subresources);

        if (SUCCEEDED(hr))
        {
            size_t reservedMips = mipCount;
            if (loadFlags & (DDS_LOADER_MIP_AUTOGEN | DDS_LOADER_MIP_RESERVE))
            {
                reservedMips = std::min<size_t>(D3D12_REQ_MIP_LEVELS, CountMips(width, height));
            }

            *textureResourceDesc = CreateTextureResourceDesc(resDim, twidth, theight, tdepth, reservedMips - skipMip, arraySize,
                                                            format, resFlags, loadFlags);

            if (FAILED(hr) && !maxsize && (mipCount > 1))
            {
                subresources.clear();

                maxsize = (resDim == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                    ? D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION
                    : D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;

                hr = FillInitData(width, height, depth, mipCount, arraySize,
                    numberOfPlanes, format,
                    maxsize, bitSize, bitData,
                    twidth, theight, tdepth, skipMip, subresources);
                if (SUCCEEDED(hr))
                {
                    *textureResourceDesc = CreateTextureResourceDesc(resDim, twidth, theight, tdepth, mipCount - skipMip, arraySize,
                                                                    format, resFlags, loadFlags);
                }
            }
        }

        if (FAILED(hr))
        {
            subresources.clear();
        }

        return hr;
    }
} // anonymous namespace


//--------------------------------------------------------------------------------------
_Use_decl_annotations_
HRESULT DirectX::LoadDDSTextureFromFileEx_Custom(
    const wchar_t* fileName,
    size_t maxsize,
    std::unique_ptr<uint8_t[]>& ddsData,
    std::vector<D3D12_SUBRESOURCE_DATA>& subresources,
    UINT numberOfPlanes,
    _Out_ D3D12_RESOURCE_DESC* textureResourceDesc,
    DDS_ALPHA_MODE* alphaMode,
    bool* isCubeMap,
    D3D12_RESOURCE_FLAGS resFlags,
    unsigned int loadFlags)
{
    if (alphaMode)
    {
        *alphaMode = DDS_ALPHA_MODE_UNKNOWN;
    }
    if (isCubeMap)
    {
        *isCubeMap = false;
    }

    if (!fileName)
    {
        return E_INVALIDARG;
    }

    const DDS_HEADER* header = nullptr;
    const uint8_t* bitData = nullptr;
    size_t bitSize = 0;

    HRESULT hr = LoadTextureDataFromFile(fileName,
        ddsData,
        &header,
        &bitData,
        &bitSize
    );
    if (FAILED(hr))
    {
        return hr;
    }

    hr = CreateTextureDescFromDDS(header, bitData, bitSize, maxsize,
                                  resFlags, loadFlags, textureResourceDesc,
                                  subresources, numberOfPlanes, isCubeMap);

    if (SUCCEEDED(hr))
    {
        if (alphaMode)
            *alphaMode = GetAlphaMode(header);
    }

    return hr;
}
