// Copyright (C) 2021 TiltedPhoques SRL.
// For licensing information see LICENSE at the root of this distribution.
#pragma once

#include <cstdint>
#include <limits>
#include <BuildInfo.h>

#define CLIENT_DLL 0

struct TargetConfig
{
    const wchar_t* dllClientName;
    const wchar_t* fullGameName;
    uint32_t steamAppId;
    uint32_t exeLoadSz;
    // Needs to be kept up to date.
    uint32_t exeDiskSz;
};

// clang-format off

#if TP_SKYRIMVR
// Skyrim VR Configuration
// Steam App ID: 611670
// TODO: Verify exeLoadSz and exeDiskSz against the actual SkyrimVR.exe binary
static constexpr TargetConfig CurrentTarget{
    L"SkyrimVRTogether.dll",
    L"Skyrim VR",
    611670, 0x40000000, 35410264};
#define TARGET_NAME L"SkyrimVR"
#define TARGET_NAME_A "SkyrimVR"
#define PRODUCT_NAME L"Skyrim Together VR"
#define SHORT_NAME L"Skyrim VR"
#else
// Skyrim Special Edition Configuration
// Steam App ID: 489830
static constexpr TargetConfig CurrentTarget{
    L"SkyrimTogether.dll",
    L"Skyrim Special Edition",
    489830, 0x40000000, 35410264};
#define TARGET_NAME L"SkyrimSE"
#define TARGET_NAME_A "SkyrimSE"
#define PRODUCT_NAME L"Skyrim Together"
#define SHORT_NAME L"Skyrim Special Edition"
#endif

// clang-format on
