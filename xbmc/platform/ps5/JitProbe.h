/*
 *  Executable-memory feasibility probe. Declared at global scope so main.cpp
 *  (whose helpers live in an anonymous namespace) and JitProbe.cpp agree on
 *  the same symbol.  See JitProbe.cpp.
 */
#pragma once

void XBMC_PS5_RunJitProbe();
