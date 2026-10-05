/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "platform/ps5/sce/SceImeDialog.h"

#include <array>
#include <string>

namespace KODI::PLATFORM::PS5
{
// All native calls run on Kodi's GUI thread, including abort and termination.
class CPS5ImeDialog
{
public:
  enum class Outcome
  {
    RUNNING,
    CONFIRMED,
    CANCELLED,
    ERROR,
    UNAVAILABLE, // Only opening failures permit the generic keyboard fallback.
  };

  ~CPS5ImeDialog();
  CPS5ImeDialog() = default;
  CPS5ImeDialog(const CPS5ImeDialog&) = delete;
  CPS5ImeDialog& operator=(const CPS5ImeDialog&) = delete;

  Outcome Open(const std::string& title, const std::string& initialText, bool hidden);
  Outcome Poll(std::string& text);
  void Close();

  // Load the IME dialog system module now: once the sandbox has been opened
  // by the jailbreak daemon (after the first frame), module loads fail.
  static bool Preload();
  static bool IsActive();
  static void NotifyInputActivity();
  static bool ConsumeInputActivity();

private:
  ImeDialogParam m_param{};
  std::array<char16_t, IME_MAX_TITLE_LENGTH + 1> m_title{};
  std::array<char16_t, IME_MAX_TEXT_LENGTH + 1> m_text{};
  bool m_owned{false};
  bool m_initialized{false};
};
} // namespace KODI::PLATFORM::PS5
