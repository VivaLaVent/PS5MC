/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PS5PadInput.h"

#include "PS5ImeDialog.h"

#include "ServiceBroker.h"
#include "application/AppInboundProtocol.h"
#include "input/keyboard/XBMC_keyboard.h"
#include "input/keyboard/XBMC_keysym.h"
#include "utils/log.h"
#include "windowing/XBMC_events.h"

#include "platform/ps5/sce/ScePad.h"
#include "platform/ps5/sce/SceUserService.h"

#include <cstring>
#include <thread>

using namespace KODI::PLATFORM::PS5;
using namespace std::chrono_literals;

namespace
{

constexpr uint32_t DIRECTION_BITS =
    PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT;

constexpr auto POLL_INTERVAL = 8ms; // 125 Hz, matching the pad report rate
constexpr auto REPEAT_DELAY = 400ms;
constexpr auto REPEAT_RATE = 80ms;
constexpr int USER_REFRESH_POLLS = 125; // re-read the login list once a second

// Synthesise d-pad bits from the left stick so lists scroll with the stick.
uint32_t StickToDpad(uint8_t x, uint8_t y, uint8_t deadzone)
{
  uint32_t bits = 0;
  if (x < 128 - deadzone)
    bits |= PAD_BUTTON_LEFT;
  else if (x > 128 + deadzone)
    bits |= PAD_BUTTON_RIGHT;
  if (y < 128 - deadzone)
    bits |= PAD_BUTTON_UP;
  else if (y > 128 + deadzone)
    bits |= PAD_BUTTON_DOWN;
  return bits;
}

uint16_t ButtonToKeysym(uint32_t bit)
{
  switch (bit)
  {
    case PAD_BUTTON_UP:
      return XBMCK_UP;
    case PAD_BUTTON_DOWN:
      return XBMCK_DOWN;
    case PAD_BUTTON_LEFT:
      return XBMCK_LEFT;
    case PAD_BUTTON_RIGHT:
      return XBMCK_RIGHT;
    case PAD_BUTTON_CROSS:
      return XBMCK_RETURN;
    case PAD_BUTTON_CIRCLE:
      return XBMCK_BACKSPACE;
    case PAD_BUTTON_TRIANGLE:
      return XBMCK_c;
    case PAD_BUTTON_SQUARE:
      return XBMCK_i;
    case PAD_BUTTON_OPTIONS:
      return XBMCK_MEDIA_PLAY_PAUSE;
    case PAD_BUTTON_L1:
      return XBMCK_PAGEUP;
    case PAD_BUTTON_R1:
      return XBMCK_PAGEDOWN;
    case PAD_BUTTON_L2:
      return XBMCK_r;
    case PAD_BUTTON_R2:
      return XBMCK_f;
    case PAD_BUTTON_L3:
      return XBMCK_o; // sent with Ctrl+Shift: player debug overlay (see EmitKey)
    case PAD_BUTTON_R3:
      return XBMCK_m;
    case PAD_BUTTON_TOUCH_PAD:
      return XBMCK_ESCAPE;
    default:
      return 0;
  }
}

} // namespace

CPadInput::CPadInput() : CThread("PS5PadInput")
{
}

CPadInput::~CPadInput()
{
  Stop();
}

void CPadInput::Start()
{
  if (!IsRunning())
    Create();
}

void CPadInput::Stop()
{
  StopThread(true);
  for (Pad& pad : m_pads)
    ClosePad(pad);
}

bool CPadInput::InitLibraries()
{
  if (m_librariesReady)
    return true;

  int result = sceUserServiceInitialize(nullptr);
  if (result != 0 && result != USER_SERVICE_ERROR_ALREADY_INITIALIZED)
  {
    CLog::Log(LOGERROR, "CPadInput: sceUserServiceInitialize failed: {:#x}",
              static_cast<uint32_t>(result));
    return false;
  }

  result = scePadInit();
  if (result != 0)
  {
    CLog::Log(LOGERROR, "CPadInput: scePadInit failed: {:#x}", static_cast<uint32_t>(result));
    return false;
  }

  m_librariesReady = true;
  return true;
}

void CPadInput::RefreshUsers()
{
  int32_t userIds[USER_SERVICE_MAX_USERS] = {-1, -1, -1, -1};
  if (sceUserServiceGetLoginUserIdList(userIds) != 0)
    return;

  for (int i = 0; i < MAX_USERS; i++)
  {
    Pad& pad = m_pads[i];
    if (userIds[i] == pad.userId)
      continue;

    ClosePad(pad);
    pad.userId = userIds[i];
    if (pad.userId != -1)
      OpenPad(pad);
  }
}

void CPadInput::OpenPad(Pad& pad)
{
  int handle = scePadOpen(pad.userId, PAD_PORT_TYPE_STANDARD, 0, nullptr);
  if (static_cast<uint32_t>(handle) == PAD_ERROR_ALREADY_OPENED)
    handle = scePadGetHandle(pad.userId, PAD_PORT_TYPE_STANDARD, 0);

  if (handle <= 0)
  {
    CLog::Log(LOGWARNING, "CPadInput: scePadOpen for user {:#x} failed: {:#x}",
              static_cast<uint32_t>(pad.userId), static_cast<uint32_t>(handle));
    pad.handle = -1;
    return;
  }

  pad.handle = handle;
  pad.lastButtons = 0;
  pad.heldRepeat = 0;

  char name[64] = {};
  sceUserServiceGetUserName(pad.userId, name, sizeof(name) - 1);
  CLog::Log(LOGINFO, "CPadInput: opened controller for user '{}'", name);
}

void CPadInput::ClosePad(Pad& pad)
{
  if (pad.handle > 0)
    scePadClose(pad.handle);
  pad.handle = -1;
  pad.userId = -1;
  pad.lastButtons = 0;
  pad.heldRepeat = 0;
}

void CPadInput::PollPad(Pad& pad)
{
  if (pad.handle <= 0)
    return;

  PadData data;
  std::memset(&data, 0, sizeof(data));
  if (scePadReadState(pad.handle, &data) != 0 || !data.connected)
  {
    // Release anything still held so Kodi doesn't see a stuck key.
    if (pad.lastButtons)
    {
      for (uint32_t bit = 1; bit; bit <<= 1)
        if (pad.lastButtons & bit)
          EmitKey(bit, false);
      pad.lastButtons = 0;
      pad.heldRepeat = 0;
    }
    return;
  }

  uint32_t buttons = data.buttons;
  // Only let the stick drive directions when the physical d-pad is idle.
  if (!(buttons & DIRECTION_BITS))
    buttons |= StickToDpad(data.leftStick.x, data.leftStick.y, STICK_DEADZONE);

  const uint32_t changed = buttons ^ pad.lastButtons;
  const auto now = std::chrono::steady_clock::now();

  const bool nativeKeyboard = CPS5ImeDialog::IsActive();
  if (nativeKeyboard || pad.waitForRelease)
  {
    for (uint32_t bit = 1; bit; bit <<= 1)
      if (pad.lastButtons & bit)
        EmitKey(bit, false);
    pad.lastButtons = 0;
    pad.heldRepeat = 0;
    // Wait for neutral input so the IME's closing press cannot activate Kodi.
    pad.waitForRelease = nativeKeyboard || buttons != 0;
    if (nativeKeyboard && buttons && now >= pad.nextRepeat)
    {
      CPS5ImeDialog::NotifyInputActivity();
      pad.nextRepeat = now + REPEAT_RATE;
    }
    return;
  }

  for (uint32_t bit = 1; bit; bit <<= 1)
  {
    if (!(changed & bit))
      continue;

    const bool down = buttons & bit;
    EmitKey(bit, down);

    if (bit & DIRECTION_BITS)
    {
      if (down)
      {
        pad.heldRepeat = bit;
        pad.nextRepeat = now + REPEAT_DELAY;
      }
      else if (pad.heldRepeat == bit)
      {
        pad.heldRepeat = 0;
      }
    }
  }

  // Keyboard-style auto repeat for held directions (Kodi expects the OS to do this).
  if (pad.heldRepeat && (buttons & pad.heldRepeat) && now >= pad.nextRepeat)
  {
    EmitKey(pad.heldRepeat, true);
    pad.nextRepeat = now + REPEAT_RATE;
  }

  pad.lastButtons = buttons;
}

void CPadInput::EmitKey(uint32_t buttonBit, bool down)
{
  const uint16_t sym = ButtonToKeysym(buttonBit);
  if (!sym)
    return;
  // L3 = Ctrl+Shift+O: Kodi's player debug overlay (dropped frames, A/V sync)
  const uint16_t mod =
      buttonBit == PAD_BUTTON_L3 ? static_cast<uint16_t>(XBMCKMOD_LCTRL | XBMCKMOD_LSHIFT) : 0;
  EmitKeysym(sym, down, mod);
}

void CPadInput::EmitKeysym(uint16_t sym, bool down, uint16_t mod)
{
  XBMC_Event event = {};
  event.type = down ? XBMC_KEYDOWN : XBMC_KEYUP;
  event.key.keysym.scancode = sym;
  event.key.keysym.sym = static_cast<XBMCKey>(sym);
  event.key.keysym.mod = static_cast<XBMCMod>(mod);
  event.key.keysym.unicode = 0;

  std::shared_ptr<CAppInboundProtocol> appPort = CServiceBroker::GetAppPort();
  if (appPort)
    appPort->OnEvent(event);
}

void CPadInput::Process()
{
  if (!InitLibraries())
  {
    CLog::Log(LOGERROR, "CPadInput: controller libraries unavailable, input disabled");
    return;
  }

  int pollsUntilRefresh = 0;
  while (!m_bStop)
  {
    if (pollsUntilRefresh-- <= 0)
    {
      RefreshUsers();
      pollsUntilRefresh = USER_REFRESH_POLLS;
    }

    for (Pad& pad : m_pads)
      PollPad(pad);

    std::this_thread::sleep_for(POLL_INTERVAL);
  }
}
