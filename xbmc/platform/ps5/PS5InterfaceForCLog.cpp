/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PS5InterfaceForCLog.h"

#include <string>

#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/dist_sink.h>

extern "C" int sceKernelDebugOutText(int channel, const char* text);

namespace
{
// The distribution sink serialises calls, so no mutex of our own.
class CKlogSink : public spdlog::sinks::base_sink<spdlog::details::null_mutex>
{
protected:
  void sink_it_(const spdlog::details::log_msg& msg) override
  {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    std::string line("[kodi] ");
    line.append(formatted.data(), formatted.size());
    // keep each call well below the kernel's debug-print buffer
    for (size_t pos = 0; pos < line.size(); pos += 900)
    {
      std::string part = line.substr(pos, 900);
      if (part.back() != '\n')
        part.push_back('\n');
      sceKernelDebugOutText(0, part.c_str());
    }
  }

  void flush_() override {}
};
} // namespace

std::unique_ptr<IPlatformLog> IPlatformLog::CreatePlatformLog()
{
  return std::make_unique<CPS5InterfaceForCLog>();
}

void CPS5InterfaceForCLog::AddSinks(
    std::shared_ptr<spdlog::sinks::dist_sink<std::mutex>> distributionSink) const
{
  distributionSink->add_sink(std::make_shared<CKlogSink>());
}
