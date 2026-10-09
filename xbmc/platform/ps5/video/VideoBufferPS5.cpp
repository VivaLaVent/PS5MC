/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoBufferPS5.h"

extern "C"
{
// patches/ps5-opengl/kodi-additions.py (zero-copy part). Weak: a GL driver
// built without them leaves these null, and Kodi keeps copying frames.
void* ps5_opengl_memory_image_create(void* data, unsigned width, unsigned height,
                                     unsigned stride, unsigned components,
                                     unsigned bytes_per_component) __attribute__((weak));
void ps5_opengl_memory_image_destroy(void* image) __attribute__((weak));
}

namespace KODI::PLATFORM::PS5
{

bool IsZeroCopyAvailable()
{
  return ps5_opengl_memory_image_create != nullptr && ps5_opengl_memory_image_destroy != nullptr;
}

void* CreateMemoryImage(const void* data, unsigned width, unsigned height, unsigned stride,
                        unsigned components, unsigned bytesPerComponent)
{
  if (!IsZeroCopyAvailable())
    return nullptr;
  return ps5_opengl_memory_image_create(const_cast<void*>(data), width, height, stride,
                                        components, bytesPerComponent);
}

void DestroyMemoryImage(void* image)
{
  if (image && IsZeroCopyAvailable())
    ps5_opengl_memory_image_destroy(image);
}

void CVideoBufferPS5::Set(const std::shared_ptr<CVideoDec2>& decoder,
                          const VideoDec2Picture& picture, AVPixelFormat format)
{
  m_decoder = decoder;
  m_frameIndex = picture.frameIndex;
  m_data = picture.data;
  m_pitch = picture.pitch;
  m_codedHeight = picture.height;
  m_bitDepth = picture.bitDepth;
  m_pixFormat = format;
}

void CVideoBufferPS5::ReturnFrame()
{
  if (m_decoder && m_frameIndex >= 0)
    m_decoder->ReleaseFrame(m_frameIndex);
  m_decoder.reset(); // the last buffer out keeps the decoder's memory alive until here
  m_frameIndex = -1;
  m_data = nullptr;
}

uint64_t CVideoBufferPS5::DecoderInstance() const
{
  return m_decoder ? m_decoder->Instance() : 0;
}

bool CVideoBufferPS5::HoldsLastDecoderReference() const
{
  return m_decoder && m_decoder.use_count() == 1;
}

void CVideoBufferPS5::GetPlanes(uint8_t* (&planes)[YuvImage::MAX_PLANES])
{
  planes[0] = const_cast<uint8_t*>(m_data);
  planes[1] = m_data ? const_cast<uint8_t*>(Chroma()) : nullptr;
  planes[2] = nullptr;
}

void CVideoBufferPS5::GetStrides(int (&strides)[YuvImage::MAX_PLANES])
{
  strides[0] = static_cast<int>(m_pitch);
  strides[1] = static_cast<int>(m_pitch);
  strides[2] = 0;
}

CVideoBufferPoolPS5::~CVideoBufferPoolPS5()
{
  for (CVideoBufferPS5* buffer : m_all)
    delete buffer;
}

CVideoBuffer* CVideoBufferPoolPS5::Get()
{
  std::unique_lock<std::mutex> lock(m_mutex);
  CVideoBufferPS5* buffer;
  if (!m_free.empty())
  {
    buffer = m_all[m_free.back()];
    m_free.pop_back();
  }
  else
  {
    buffer = new CVideoBufferPS5(static_cast<int>(m_all.size()));
    m_all.push_back(buffer);
  }
  lock.unlock();
  buffer->Acquire(GetPtr());
  return buffer;
}

void CVideoBufferPoolPS5::Return(int id)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (id < 0 || id >= static_cast<int>(m_all.size()))
    return;
  m_all[id]->ReturnFrame();
  m_free.push_back(id);
}

} // namespace KODI::PLATFORM::PS5
