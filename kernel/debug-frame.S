// Copyright 2026 The Ours Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Included by every kernel assembly source so offline unwind information is
// emitted in .debug_frame without a runtime .eh_frame dependency.
.cfi_sections .debug_frame
