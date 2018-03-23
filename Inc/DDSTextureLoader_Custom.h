//--------------------------------------------------------------------------------------
// File: DDSTextureLoader.h
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

#pragma once

#include <d3d12.h>

#include <memory>
#include <vector>
#include <stdint.h>


namespace DirectX
{
    enum DDS_ALPHA_MODE
    {
        DDS_ALPHA_MODE_UNKNOWN       = 0,
        DDS_ALPHA_MODE_STRAIGHT      = 1,
        DDS_ALPHA_MODE_PREMULTIPLIED = 2,
        DDS_ALPHA_MODE_OPAQUE        = 3,
        DDS_ALPHA_MODE_CUSTOM        = 4,
    };

    enum DDS_LOADER_FLAGS
    {
        DDS_LOADER_DEFAULT      = 0,
        DDS_LOADER_FORCE_SRGB   = 0x1,
        DDS_LOADER_MIP_AUTOGEN  = 0x4,
        DDS_LOADER_MIP_RESERVE  = 0x8,
    };

    // Extended version
    HRESULT __cdecl LoadDDSTextureFromFileEx_Custom(
        _In_z_ const wchar_t* szFileName,
        size_t maxsize,
        std::unique_ptr<uint8_t[]>& ddsData,
        std::vector<D3D12_SUBRESOURCE_DATA>& subresources,
        UINT numberOfPlanes,
        _Out_ D3D12_RESOURCE_DESC* textureResourceDesc,
        _Out_opt_ DDS_ALPHA_MODE* alphaMode = nullptr,
        _Out_opt_ bool* isCubeMap = nullptr,
        D3D12_RESOURCE_FLAGS resFlags = D3D12_RESOURCE_FLAG_NONE,
        unsigned int loadFlags = DDS_LOADER_DEFAULT);
}
