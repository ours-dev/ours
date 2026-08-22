// Copyright 2026 The Ours Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ELF_INTERP_INC
#error "ELF_INTERP_INC must name a generated assembly include"
#endif

// A physboot-compatible ELF identifies the loader it was built against by
// carrying that loader's GNU build ID as its PT_INTERP string.
.section .interp, "aR", %progbits
.balign 1
#include ELF_INTERP_INC
.byte 0
