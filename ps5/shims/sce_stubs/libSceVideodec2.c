/*
 *  Link stub for the system library libSceVideodec2 (soname
 *  libSceVideodec2.sprx), in the style of the payload SDK's sce_stubs: only
 *  symbol names, so the title's ELF imports them from the real module.
 *  Built by scripts/30-deploy.sh.
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#define STUB(name) asm(".global " #name "\n.type " #name " @function\n" #name ":\n")

STUB(sceVideodec2QueryComputeMemoryInfo);
STUB(sceVideodec2AllocateComputeQueue);
STUB(sceVideodec2ReleaseComputeQueue);
STUB(sceVideodec2QueryDecoderMemoryInfo);
STUB(sceVideodec2CreateDecoder);
STUB(sceVideodec2DeleteDecoder);
STUB(sceVideodec2Decode);
STUB(sceVideodec2Flush);
STUB(sceVideodec2Reset);
