/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstddef>
#include <cstdint>

// Public homebrew ABI: based on ps5-payload-dev/offact IME_dialog.c. The payload SDK
// supplies libSceImeDialog's link stub but does not supply its declarations.
namespace KODI::PLATFORM::PS5
{
    constexpr size_t IME_MAX_TEXT_LENGTH = 2048;
    constexpr size_t IME_MAX_TITLE_LENGTH = 127;

    constexpr uint32_t SCE_IME_OPTION_NONE = 0x00000000;
    constexpr uint32_t SCE_IME_OPTION_MULTILINE = 0x00000001;
    constexpr uint32_t SCE_IME_OPTION_NO_AUTO_CAPITALIZATION = 0x00000002;
    constexpr uint32_t SCE_IME_OPTION_PASSWORD = 0x00000004;
    constexpr uint32_t SCE_IME_OPTION_EXTERNAL_KEYBOARD = 0x00000010;
    constexpr uint32_t SCE_IME_OPTION_NO_LEARNING = 0x00000020;
    constexpr uint32_t SCE_IME_OPTION_FIXED_POSITION = 0x00000040;
    constexpr uint32_t SCE_IME_OPTION_DISABLE_COPY_PASTE = 0x00000080;



    enum class ImeStatus : int32_t
    {
        NONE,
        RUNNING,
        FINISHED,
    };

    enum class ImeEndStatus : int32_t
    {
        OK,
        USER_CANCELED,
        ABORTED,
    };

    struct ImeDialogParam
    {
        int32_t userId;
        int32_t type;
        uint64_t supportedLanguages;
        int32_t enterLabel;
        int32_t inputMethod;
        int (*filter)(char16_t*, uint32_t*, const char16_t*, uint32_t);
        uint32_t option;
        uint32_t maxTextLength;
        char16_t* inputTextBuffer;
        float posx;
        float posy;
        int32_t halign;
        int32_t valign;
        const char16_t* placeholder;
        const char16_t* title;
        int8_t reserved[16];
    };

    struct ImeDialogResult
    {
        ImeEndStatus endStatus;
        int8_t reserved[12];
    };

    static_assert(sizeof(char16_t) == 2);
    static_assert(sizeof(ImeDialogParam) == 96);
    static_assert(offsetof(ImeDialogParam, inputTextBuffer) == 40);
    static_assert(offsetof(ImeDialogParam, title) == 72);
    static_assert(sizeof(ImeDialogResult) == 16);
} // namespace KODI::PLATFORM::PS5

// Used for function-pointer types; entry points are resolved after module loading.
extern "C"
{
int sceImeDialogInit(const KODI::PLATFORM::PS5::ImeDialogParam* param, const void* extended);
KODI::PLATFORM::PS5::ImeStatus sceImeDialogGetStatus();
int sceImeDialogGetResult(KODI::PLATFORM::PS5::ImeDialogResult* result);
int sceImeDialogAbort();
int sceImeDialogTerm();
}
