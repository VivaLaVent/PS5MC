/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PS5Keyboard.h"

using namespace KODI::PLATFORM::PS5;

CPS5Keyboard::CPS5Keyboard() : CGUIDialog(WINDOW_DIALOG_KEYBOARD_TOUCH, "")
{
  SetSound(false);
}

CPS5Keyboard::~CPS5Keyboard()
{
  stopAutoCloseTimer();
}

bool CPS5Keyboard::ShowAndGetInput(char_callback_t callback,
                                  const std::string& initialString,
                                  std::string& typedString,
                                  const std::string& heading,
                                  bool hiddenInput)
{
  m_initialText = initialString;
  m_heading = heading;
  m_hidden = hiddenInput;
  m_outcome = CPS5ImeDialog::Outcome::CANCELLED;
  Open();
  stopAutoCloseTimer();

  const bool confirmed = !m_cancelled && m_outcome == CPS5ImeDialog::Outcome::CONFIRMED;
  if (m_cancelled)
    m_outcome = CPS5ImeDialog::Outcome::CANCELLED;
  m_cancelled = false;
  if (confirmed)
  {
    typedString = m_result;
    // ImeDialog supplies committed text only, not per-keystroke notifications.
    if (callback)
      callback(this, typedString);
  }
  m_initialText.clear();
  m_heading.clear();
  m_result.clear();
  return confirmed;
}

void CPS5Keyboard::Cancel()
{
  // The idle timer and remote callers may run outside the GUI thread.
  m_cancelled = true;
}

void CPS5Keyboard::Open_Internal(bool processRenderLoop, const std::string& param)
{
  CGUIDialog::Open_Internal(processRenderLoop, param);
  // The modal loop can also exit when Kodi stops rendering or shuts down.
  Close(true);
  m_dialog.Close();
}

void CPS5Keyboard::OnInitWindow()
{
  CGUIDialog::OnInitWindow();
  m_windowLoaded = true;
  if (!m_cancelled)
    m_outcome = m_dialog.Open(m_heading, m_initialText, m_hidden);
  if (m_outcome != CPS5ImeDialog::Outcome::RUNNING)
    Close(true);
}

void CPS5Keyboard::OnDeinitWindow(int nextWindowID)
{
  m_dialog.Close();
  if (m_outcome == CPS5ImeDialog::Outcome::RUNNING)
    m_outcome = CPS5ImeDialog::Outcome::CANCELLED;
  CGUIDialog::OnDeinitWindow(nextWindowID);
}

bool CPS5Keyboard::OnAction(const CAction& action)
{
  // Consume queued Kodi actions too: the system dialog owns controller input.
  return true;
}

void CPS5Keyboard::FrameMove()
{
  CGUIDialog::FrameMove();
  if (!IsDialogRunning())
    return;
  if (CPS5ImeDialog::ConsumeInputActivity())
    resetAutoCloseTimer();
  if (m_cancelled)
    m_outcome = CPS5ImeDialog::Outcome::CANCELLED;
  else
    m_outcome = m_dialog.Poll(m_result);
  if (m_outcome != CPS5ImeDialog::Outcome::RUNNING)
    Close(true);
}
