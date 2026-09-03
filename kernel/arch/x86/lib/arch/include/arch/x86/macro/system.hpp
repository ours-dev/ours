#pragma once

#include <ours/macro_abi.hpp>

#define X86_CR0_PE BIT(0)
#define X86_CR0_WP BIT(16)
#define X86_CR0_NW BIT(29)
#define X86_CR0_CD BIT(30)
#define X86_CR0_PG BIT(31)

#define X86_CR4_PAE BIT(5)
#define X86_CR4_PGE BIT(7)
