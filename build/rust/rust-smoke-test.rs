// Copyright 2026 The Ours Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Native host Rust executable built by the default (host) toolchain's
// rust_bin tool.  Links against the rust_rlib-built smoke library to keep
// both tools and the --extern wiring continuously testable.

fn main() {
    println!("rust toolchain smoke ok: {}", rust_smoke_lib::answer());
}
