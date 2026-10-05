/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "PS5ImeDialog.h"
#include "guilib/GUIDialog.h"
#include "guilib/GUIKeyboard.h"

#include <atomic>
#include <string>

namespace KODI::PLATFORM::PS5
{
class CPS5Keyboard : public CGUIDialog, public CGUIKeyboard
{
public:
  CPS5Keyboard();
  ~CPS5Keyboard() override;

  bool ShowAndGetInput(char_callback_t callback,
                       const std::string& initialString,
                       std::string& typedString,
                       const std::string& heading,
                       bool hiddenInput = false) override;
  void Cancel() override;
  int GetWindowId() const override { return GetID(); }
  bool NativeUnavailable() const { return m_outcome == CPS5ImeDialog::Outcome::UNAVAILABLE; }
  bool OnAction(const CAction& action) override;
  void FrameMove() override;

protected:
  void Open_Internal(bool processRenderLoop, const std::string& param = "") override;
  void OnInitWindow() override;
  void OnDeinitWindow(int nextWindowID) override;

private:
  CPS5ImeDialog m_dialog;
  CPS5ImeDialog::Outcome m_outcome{CPS5ImeDialog::Outcome::CANCELLED};
  std::atomic<bool> m_cancelled{false};
  std::string m_initialText;
  std::string m_heading;
  std::string m_result;
  bool m_hidden{false};
};
} // namespace KODI::PLATFORM::PS5
