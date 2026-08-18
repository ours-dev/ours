#pragma once

#include <ours/macro_abi.hpp>

#define X86_MSR_IA32_EFER 0xC0000080

#define X86_EFER_SCE BIT(0)
#define X86_EFER_LME BIT(8)
#define X86_EFER_NXE BIT(11)
