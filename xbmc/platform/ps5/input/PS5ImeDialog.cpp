/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PS5ImeDialog.h"

#include "platform/ps5/sce/SceUserService.h"
#include "utils/CharsetConverter.h"
#include "utils/log.h"

#include <algorithm>
#include <atomic>

extern "C" {
int sceSysmoduleLoadModule(uint16_t id);
}

using namespace KODI::PLATFORM::PS5;

namespace
{
    std::atomic<bool> g_imeOwned{false};
    std::atomic<bool> g_inputActivity{false};

    bool LoadImeModule()
    {
        static const bool available = []
        {
            constexpr uint16_t IME_DIALOG_MODULE = 0x0096;

            const int result = sceSysmoduleLoadModule(IME_DIALOG_MODULE);
            if (result != 0)
            {
                CLog::Log(LOGERROR,
                          "PS5 IME: cannot load dialog module: {:#x}",
                          static_cast<uint32_t>(result));
                return false;
            }

            CLog::Log(LOGINFO, "PS5 IME: native dialog module ready");
            return true;
        }();

        return available;
    }

    template <size_t N>
    bool CopyText(const std::string& utf8, std::array<char16_t, N>& buffer)
    {
        std::u32string text;
        if (utf8.find('\0') != std::string::npos || !CCharsetConverter::utf8ToUtf32(utf8, text, true))
        {
            return false;
        }
        buffer.fill(0);
        size_t length = 0;
        for (char32_t codepoint : text)
        {
            const size_t units = codepoint > 0xffff ? 2 : 1;
            if (length + units > N - 1)
                break;
            if (units == 2)
            {
                codepoint -= 0x10000;
                buffer[length++] = static_cast<char16_t>(0xd800 + (codepoint >> 10));
                buffer[length++] = static_cast<char16_t>(0xdc00 + (codepoint & 0x3ff));
            }
            else
            {
                buffer[length++] = static_cast<char16_t>(codepoint);
            }
        }

        return true;
    }

    void LogError(const char* operation, int result)
    {
        CLog::Log(LOGERROR, "PS5 IME: {} failed: {:#x}", operation, static_cast<uint32_t>(result));
    }
} // namespace

CPS5ImeDialog::~CPS5ImeDialog()
{
    Close();
}

CPS5ImeDialog::Outcome CPS5ImeDialog::Open(const std::string& title,
                                           const std::string& initialText,
                                           bool hidden)
{
    if (m_owned || g_imeOwned.exchange(true))
        return Outcome::CANCELLED;

    m_owned = true;
    g_inputActivity = false;

    if (!LoadImeModule())
    {
        Close();
        return Outcome::UNAVAILABLE;
    }

    if (!CopyText(title, m_title) ||
        !CopyText(initialText, m_text))
    {
        CLog::Log(LOGERROR, "PS5 IME: cannot convert input to UTF-16");
        Close();
        return Outcome::UNAVAILABLE;
    }

    int result = sceUserServiceInitialize(nullptr);

    if (result != 0 && result != USER_SERVICE_ERROR_ALREADY_INITIALIZED)
    {
        LogError("sceUserServiceInitialize", result);
        Close();
        return Outcome::UNAVAILABLE;
    }

    m_param = {};

    result = sceUserServiceGetForegroundUser(&m_param.userId);

    if (result != 0)
    {
        LogError("sceUserServiceGetForegroundUser", result);
        Close();
        return Outcome::UNAVAILABLE;
    }

    m_param.title = m_title.data();

    // Password mode requires Basic Latin.
    // Ordinary input uses the system language.
    m_param.type = hidden ? 1 : 0;

    m_param.inputTextBuffer = m_text.data();
    m_param.maxTextLength = IME_MAX_TEXT_LENGTH;

    m_param.halign = 1;
    m_param.valign = 1;

    // Native dialog coordinate space.
    m_param.posx = 960.0f;
    m_param.posy = 540.0f;
    // when hidden then use special password mode with different options disabled like learning. Else it will use normal mode which includes learning mode.
    m_param.option = hidden ? SCE_IME_OPTION_NO_AUTO_CAPITALIZATION | SCE_IME_OPTION_PASSWORD | SCE_IME_OPTION_NO_LEARNING | SCE_IME_OPTION_DISABLE_COPY_PASTE : SCE_IME_OPTION_NONE;

    result = sceImeDialogInit(&m_param, nullptr);

    if (result != 0)
    {
        LogError("sceImeDialogInit", result);
        Close();
        return Outcome::UNAVAILABLE;
    }

    m_initialized = true;

    return Outcome::RUNNING;
}

CPS5ImeDialog::Outcome CPS5ImeDialog::Poll(std::string& text)
{
    if (!m_initialized)
        return Outcome::ERROR;

    const auto status = sceImeDialogGetStatus();

    if (status == ImeStatus::RUNNING)
        return Outcome::RUNNING;

    if (status != ImeStatus::FINISHED)
    {
        CLog::Log(LOGERROR, "PS5 IME: unexpected dialog status {}", static_cast<int>(status));
        return Outcome::ERROR;
    }

    ImeDialogResult result{};

    const int error = sceImeDialogGetResult(&result);

    if (error != 0)
    {
        LogError("sceImeDialogGetResult", error);
        return Outcome::ERROR;
    }

    if (result.endStatus == ImeEndStatus::USER_CANCELED || result.endStatus == ImeEndStatus::ABORTED)
    {
        return Outcome::CANCELLED;
    }

    if (result.endStatus != ImeEndStatus::OK)
        return Outcome::ERROR;

    const auto end = std::find(m_text.begin(), m_text.end(), u'\0');

    if (end == m_text.end())
        return Outcome::ERROR;

    for (auto it = m_text.begin(); it != end; ++it)
    {
        if (*it >= 0xd800 && *it <= 0xdbff)
        {
            if (++it == end || *it < 0xdc00 || *it > 0xdfff)
            {
                return Outcome::ERROR;
            }
        }
        else if (*it >= 0xdc00 && *it <= 0xdfff)
        {
            return Outcome::ERROR;
        }
    }

    if (!CCharsetConverter::utf16LEtoUTF8(std::u16string(m_text.begin(), end), text))
    {
        CLog::Log(LOGERROR, "PS5 IME: invalid result text");
        return Outcome::ERROR;
    }

    return Outcome::CONFIRMED;
}

void CPS5ImeDialog::Close()
{
    if (m_initialized)
    {
        if (sceImeDialogGetStatus() == ImeStatus::RUNNING)
        {
            const int result = sceImeDialogAbort();
            if (result != 0)
                LogError("sceImeDialogAbort", result);
        }

        const int result = sceImeDialogTerm();

        if (result != 0)
            LogError("sceImeDialogTerm", result);

        m_initialized = false;
    }

    m_text.fill(0);
    m_title.fill(0);

    if (m_owned)
    {
        m_owned = false;
        g_imeOwned = false;
    }
}

bool CPS5ImeDialog::Preload()
{
    return LoadImeModule();
}

bool CPS5ImeDialog::IsActive()
{
    return g_imeOwned.load();
}

void CPS5ImeDialog::NotifyInputActivity()
{
    g_inputActivity = true;
}

bool CPS5ImeDialog::ConsumeInputActivity()
{
    return g_inputActivity.exchange(false);
}
