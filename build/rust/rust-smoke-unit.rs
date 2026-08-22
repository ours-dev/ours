// Copyright 2026 The Ours Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Unit-test half of the Rust toolchain smoke test: exercises the --test
// harness wiring of rustc_test().

#[test]
fn linked_library_answers() {
    assert_eq!(rust_smoke_lib::answer(), 42);
}
