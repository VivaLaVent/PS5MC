/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "threads/Thread.h"

#include <array>
#include <chrono>
#include <cstdint>

namespace KODI::PLATFORM::PS5
{

/*!
 * \brief Phase-1 DualSense bridge: polls scePadReadState() on a worker thread
 * and injects XBMC_KEYDOWN/XBMC_KEYUP events for keys Kodi's default
 * keyboard.xml already understands, so the GUI is navigable on day one.
 *
 *   D-pad / left stick  -> arrows        Cross    -> Return   (Select)
 *   Circle              -> Backspace     Triangle -> C        (Context menu)
 *   Square              -> I             Options  -> Play/Pause media key
 *   L1 / R1             -> Page Up/Down  L2 / R2  -> R / F    (rewind / ffwd)
 *   L3                  -> Tab           R3       -> M        (OSD)
 *   Touchpad click      -> Escape
 *
 * The proper replacement is a CPeripheralBus so the DualSense shows up as a
 * real joystick with Kodi's button-mapping dialog and rumble (phase 2).
 */
class CPadInput : public CThread
{
public:
  CPadInput();
  ~CPadInput() override;

  void Start();
  void Stop();

protected:
  void Process() override;

private:
  struct Pad
  {
    int32_t userId{-1};
    int32_t handle{-1};
    uint32_t lastButtons{0};
    uint32_t heldRepeat{0}; // direction bit currently auto-repeating
    bool waitForRelease{false};
    std::chrono::steady_clock::time_point nextRepeat{};
  };

  static constexpr int MAX_USERS = 4;
  static constexpr uint8_t STICK_DEADZONE = 64; // out of 128 either side of centre

  bool InitLibraries();
  void RefreshUsers();
  void OpenPad(Pad& pad);
  void ClosePad(Pad& pad);
  void PollPad(Pad& pad);
  void EmitKey(uint32_t buttonBit, bool down);
  void EmitKeysym(uint16_t sym, bool down, uint16_t mod = 0);

  std::array<Pad, MAX_USERS> m_pads{};
  bool m_librariesReady{false};
};

} // namespace KODI::PLATFORM::PS5
