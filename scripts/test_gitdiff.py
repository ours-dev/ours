import json
import os
import sys
import unittest
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import claude_hook
import gitdiff

DIFF = ("diff --git a/a.md b/a.md\n--- a/a.md\n+++ b/a.md\n@@ -1,2 +1,3 @@\n+x\n@@ -9 +10 @@\n-y\n+z\n@@ -20,3 +21,0 @@\n-gone\n"
        "diff --git a/old.md b/new.md\nsimilarity index 100%\nrename from old.md\nrename to new.md\n"
        "diff --git a/c.md b/moved.md\nrename from c.md\nrename to moved.md\n--- a/c.md\n+++ b/moved.md\n@@ -5 +5 @@\n-p\n+q\n")


class GitDiff(unittest.TestCase):
    def test_ranges_skip_pure_deletions(self):
        self.assertEqual(gitdiff.changed_ranges(gitdiff.diff_section(DIFF, "a.md")), [(1, 3), (10, 10)])

    def test_a_pure_rename_has_a_section_and_no_lines(self):
        self.assertNotEqual(gitdiff.diff_section(DIFF, "new.md"), "")
        self.assertEqual(gitdiff.added_lines(DIFF, "new.md"), set())

    def test_a_renamed_and_edited_file_follows_the_new_name(self):
        self.assertEqual(gitdiff.added_lines(DIFF, "moved.md"), {5})
        self.assertEqual(gitdiff.added_lines(DIFF, "c.md"), set())      # the old name no longer exists

    def test_an_untouched_file_has_no_section(self):
        self.assertEqual(gitdiff.diff_section(DIFF, "other.md"), "")
        self.assertEqual(gitdiff.added_lines(DIFF, "other.md"), set())


class GitEnvironment(unittest.TestCase):
    def test_variables_that_change_where_git_looks_are_dropped(self):
        env = {"GIT_DIR": "/x/.git", "GIT_WORK_TREE": "/x", "GIT_PREFIX": "sub/", "GIT_INDEX_FILE": "/x/.git/next-index", "PATH": "/bin"}
        with mock.patch.dict(os.environ, env, clear=True):
            cleaned = gitdiff.git_env()
        self.assertEqual(cleaned, {"GIT_INDEX_FILE": "/x/.git/next-index", "PATH": "/bin"})   # the temp index stays


class ClaudeHook(unittest.TestCase):
    def test_payload_parsing_never_raises(self):
        self.assertEqual(claude_hook.read_payload("not json"), {})
        self.assertEqual(claude_hook.read_payload("[1, 2]"), {})
        self.assertEqual(claude_hook.read_payload(""), {})
        self.assertEqual(claude_hook.read_payload('{"a": 1}'), {"a": 1})

    def test_the_edited_path(self):
        self.assertEqual(claude_hook.edited_path({"tool_input": {"file_path": "/x/y.cpp"}}), Path("/x/y.cpp"))
        self.assertEqual(claude_hook.edited_path({"tool_input": {"file_path": "y.cpp"}, "cwd": "/x"}), Path("/x/y.cpp"))
        self.assertEqual(claude_hook.edited_path({"tool_response": {"filePath": "/z.cpp"}}), Path("/z.cpp"))
        self.assertEqual(claude_hook.edited_path({"tool_input": {"file_path": "/a"}, "tool_response": {"filePath": "/b"}}), Path("/a"))
        self.assertIsNone(claude_hook.edited_path({}))
        self.assertIsNone(claude_hook.edited_path({"tool_input": {}, "tool_response": "done"}))

    def test_the_two_answers_mean_different_things(self):
        note = json.loads(claude_hook.context("fyi"))
        self.assertEqual(note, {"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext": "fyi"}})
        stop = json.loads(claude_hook.block("fix it"))
        self.assertEqual(stop, {"decision": "block", "reason": "fix it"})


if __name__ == "__main__":
    unittest.main()
