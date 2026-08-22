// Copyright 2026 The Ours Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Library half of the Rust toolchain smoke test: exercises the rust_rlib
// tool and the --extern wiring between dependent crates.

pub fn answer() -> u32 {
    42
}

#[cfg(test)]
mod tests {
    use super::answer;

    #[test]
    fn answer_is_forty_two() {
        assert_eq!(answer(), 42);
    }
}
