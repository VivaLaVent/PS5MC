/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*
 * Clean-room declarations for the PS5 pad library (libScePad.sprx).
 * The ps5-payload-sdk exports these symbols through its stub libraries but
 * ships no headers. Layout and constants follow the public PS5 SDL backend
 * (github.com/ps5-payload-dev/SDL, src/joystick/ps5) and the PS4 ScePadData
 * layout it inherits (0x78 bytes).
 */

#include <cstddef>
#include <cstdint>

extern "C"
{
  int scePadInit(void);
  int scePadOpen(int userId, int type, int index, const void* param);
  int scePadGetHandle(int userId, int type, int index);
  int scePadClose(int handle);
  int scePadReadState(int handle, void* data);
  int scePadSetLightBar(int handle, const void* color);
  int scePadSetVibrationMode(int handle, int mode);
  int scePadSetVibration(int handle, const void* vibration);
}

namespace KODI::PLATFORM::PS5
{

constexpr int PAD_PORT_TYPE_STANDARD = 0;
constexpr int PAD_PORT_TYPE_REMOTE_CONTROL = 16;
constexpr int USER_ID_SYSTEM = 0xFF;
constexpr uint32_t PAD_ERROR_ALREADY_OPENED = 0x80920004u;

// PadData::buttons bits. PS / Create / Mute are reserved by the system.
constexpr uint32_t PAD_BUTTON_L3 = 0x0002;
constexpr uint32_t PAD_BUTTON_R3 = 0x0004;
constexpr uint32_t PAD_BUTTON_OPTIONS = 0x0008;
constexpr uint32_t PAD_BUTTON_UP = 0x0010;
constexpr uint32_t PAD_BUTTON_RIGHT = 0x0020;
constexpr uint32_t PAD_BUTTON_DOWN = 0x0040;
constexpr uint32_t PAD_BUTTON_LEFT = 0x0080;
constexpr uint32_t PAD_BUTTON_L2 = 0x0100;
constexpr uint32_t PAD_BUTTON_R2 = 0x0200;
constexpr uint32_t PAD_BUTTON_L1 = 0x0400;
constexpr uint32_t PAD_BUTTON_R1 = 0x0800;
constexpr uint32_t PAD_BUTTON_TRIANGLE = 0x1000;
constexpr uint32_t PAD_BUTTON_CIRCLE = 0x2000;
constexpr uint32_t PAD_BUTTON_CROSS = 0x4000;
constexpr uint32_t PAD_BUTTON_SQUARE = 0x8000;
constexpr uint32_t PAD_BUTTON_TOUCH_PAD = 0x100000;

struct PadTouch
{
  uint16_t x;
  uint16_t y;
  uint8_t finger;
  uint8_t pad[3];
};

struct PadTouchData
{
  uint8_t fingers;
  uint8_t pad1[3];
  uint32_t pad2;
  PadTouch touch[2];
};

struct PadColor
{
  uint8_t r, g, b, a;
};

struct PadVibration
{
  uint8_t largeMotor;
  uint8_t smallMotor;
};

struct PadData
{
  uint32_t buttons;
  struct
  {
    uint8_t x, y;
  } leftStick;
  struct
  {
    uint8_t x, y;
  } rightStick;
  struct
  {
    uint8_t l2, r2;
  } analogButtons;
  uint16_t padding;
  struct
  {
    float x, y, z, w;
  } orientation;
  struct
  {
    float x, y, z;
  } angularVelocity;
  struct
  {
    float x, y, z;
  } acceleration;
  PadTouchData touch;
  uint8_t connected;
  uint64_t timestamp;
  uint8_t ext[16];
  uint8_t count;
  uint8_t unknown[15];
};

static_assert(sizeof(PadData) == 0x78, "PadData must match the 120-byte ScePadData layout");

} // namespace KODI::PLATFORM::PS5
