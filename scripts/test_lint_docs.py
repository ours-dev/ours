import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))

import lint_docs
from lint_docs import Standard, check_text

STD = Standard.load()

CHANGE_LABELS = ["Files", "Symptom", "Cause", "Change", "Reference", "Verification",
                 "Deliberately not done", "How it was found", "The user's prediction",
                 "Three questions you should be able to answer"]
REPAIR_LABELS = ["Severity", "Symptom", "Cause", "Change", "Reference", "Acceptance"]


def entry(title, labels, severity=None):
    out = [f"## {title}", ""]
    for label in labels:
        out += [f"**{label}**", severity if (label == "Severity" and severity) else "Text.", ""]
    return "\n".join(out)


def rules(violations):
    return sorted(v.rule for v in violations)


def lint(path, text, added=None):
    return check_text(path, text, added, STD)


class GenericRules(unittest.TestCase):
    def test_clean_document(self):
        self.assertEqual(lint("docs/notes.md", "# Title\n\nText.\n\n- one\n- two\n"), [])

    def test_trailing_whitespace_tab_and_bullet(self):
        found = lint("docs/notes.md", "# Title\n\ntext \n\n\tindented\n\n* star\n+ plus\n")
        self.assertEqual(rules(found), ["bullet-marker", "bullet-marker", "tab", "trailing-whitespace"])

    def test_fenced_code_is_exempt_from_line_rules(self):
        text = "# Title\n\n```text\ntrailing \n\ttab\n* star\n```\n"
        self.assertEqual(lint("docs/notes.md", text), [])

    def test_fence_needs_a_language(self):
        found = lint("docs/notes.md", "# Title\n\n```\nplain\n```\n")
        self.assertEqual([(v.rule, v.line) for v in found], [("fence-language", 3)])

    def test_nested_fence_is_one_block(self):
        nested = "# Title\n\n````markdown\n```\ninner\n```\n````\n"
        self.assertEqual(lint("docs/notes.md", nested), [])  # the inner ``` lines are content, not openers
        after = nested + "\n```\nplain\n```\n"
        self.assertEqual([(v.rule, v.line) for v in lint("docs/notes.md", after)], [("fence-language", 9)])

    def test_dates_are_iso(self):
        found = lint("docs/notes.md", "# Title\n\nOn 2026/09/28 and 2026.09.28 but `2026/09/28` is code.\n")
        self.assertEqual(rules(found), ["date-format"])

    def test_heading_levels_do_not_skip(self):
        found = lint("docs/notes.md", "# Title\n\n### Deep\n")
        self.assertEqual(rules(found), ["heading-level"])

    def test_new_file_needs_one_h1_first_and_a_final_newline(self):
        self.assertEqual(rules(lint("docs/notes.md", "Text.\n\n# Title\n")), ["h1"])
        self.assertEqual(rules(lint("docs/notes.md", "# A\n\n# B\n")), ["h1"])
        self.assertEqual(rules(lint("docs/notes.md", "# A\n\ntext")), ["final-newline"])
        self.assertEqual(rules(lint("docs/notes.md", "# A\n\ntext\n\n")), ["final-newline"])

    def test_front_matter_is_skipped_for_the_h1(self):
        text = "---\nname: x\ndescription: " + " ".join(["word"] * 30) + "\n---\n\n# Title\n"
        self.assertEqual(lint(".agents/skills/x/SKILL.md", text), [])

    def test_only_added_lines_are_held_to_the_standard(self):
        text = "# Title\n\nold line \n\nnew line \n"  # lines 3 and 5 have trailing whitespace
        self.assertEqual([v.line for v in lint("docs/notes.md", text, added={5})], [5])
        self.assertEqual([v.line for v in lint("docs/notes.md", text, added={3})], [3])
        self.assertEqual(lint("docs/notes.md", text, added={2}), [])

    def test_modified_file_gets_no_new_file_rules(self):
        self.assertEqual(lint("docs/notes.md", "no heading here\n", added={1}), [])


class LineLength(unittest.TestCase):
    LONG = " ".join(["word"] * 20)  # 99 characters, can be wrapped

    def test_instruction_files_wrap_at_80(self):
        for path in ("CLAUDE.md", "AGENTS.md", ".agents/skills/x/SKILL.md", ".agents/skills/x/references/y.md"):
            with self.subTest(path=path):
                self.assertEqual(rules(lint(path, f"# T\n\n{self.LONG}\n")), ["line-length"])

    def test_documents_are_not_wrapped(self):
        self.assertEqual(lint("docs/notes.md", f"# T\n\n{self.LONG}\n"), [])

    def test_exemptions(self):
        exempt = ["| a | b |" + " x" * 50, "See https://example.com/" + "a" * 70 + " for more",
                  "## " + " ".join(["word"] * 20), "x" * 100]
        for line in exempt:
            with self.subTest(line=line[:30]):
                self.assertEqual(lint("CLAUDE.md", f"# T\n\n{line}\n"), [])
        self.assertEqual(lint("CLAUDE.md", f"# T\n\n```text\n{self.LONG}\n```\n"), [])


class ChangeRecords(unittest.TestCase):
    PATH = "docs/changes/260928/kernel-time.md"

    def good(self, title="Fix the tick (2026-09-28)", labels=CHANGE_LABELS):
        return "# kernel-time changes\n\n" + entry(title, labels)

    def test_good_file(self):
        self.assertEqual(lint(self.PATH, self.good()), [])

    def test_missing_field_is_named(self):
        text = self.good(labels=[x for x in CHANGE_LABELS if x != "Verification"])
        found = lint(self.PATH, text)
        self.assertEqual(rules(found), ["entry-fields"])
        self.assertIn("**Verification**", found[0].message)

    def test_fields_out_of_order(self):
        labels = CHANGE_LABELS[:]
        labels[1], labels[2] = labels[2], labels[1]
        self.assertEqual(rules(lint(self.PATH, self.good(labels=labels))), ["entry-fields"])

    def test_title_ends_with_a_real_date(self):
        self.assertEqual(rules(lint(self.PATH, self.good(title="Fix the tick"))), ["entry-title"])
        self.assertEqual(rules(lint(self.PATH, self.good(title="Fix the tick (2026-13-45)"))), ["entry-title"])

    def test_chinese_labels_do_not_satisfy_the_template(self):
        text = self.good(labels=["文件", "症状", "原因", "改法", "参照", "验证", "故意没做"])
        self.assertEqual(rules(lint(self.PATH, text)), ["entry-fields"])

    def test_h1_and_path_for_a_new_file(self):
        self.assertEqual(rules(lint(self.PATH, self.good().replace("kernel-time changes", "kernel-time 改动记录"))), ["h1"])
        self.assertEqual(rules(lint("docs/changes/260928/Kernel_Time.md", self.good())), ["path"])
        self.assertEqual(rules(lint("docs/changes/261345/kernel-time.md", self.good())), ["path"])
        self.assertEqual(rules(lint("docs/changes/kernel-time.md", self.good())), ["path"])

    def test_only_an_added_entry_is_held_to_the_template(self):
        old = "## 旧条目（2026-09-01）\n\n**文件**\n- x\n\n"
        new = entry("New entry (2026-09-28)", CHANGE_LABELS)
        text = "# kernel-time 改动记录\n\n" + old + new
        first_new = text.split("\n").index("## New entry (2026-09-28)") + 1
        n_lines = len(text.split("\n"))
        self.assertEqual(lint(self.PATH, text, added=set(range(first_new, n_lines + 1))), [])
        bad_new = "# kernel-time 改动记录\n\n" + old + entry("New entry (2026-09-28)", CHANGE_LABELS[:3])
        self.assertEqual(rules(lint(self.PATH, bad_new, added=set(range(first_new, n_lines + 1)))), ["entry-fields"])
        self.assertEqual(rules(lint(self.PATH, text, added={3})), ["entry-fields", "entry-title"])  # the old entry, if it were edited

    def test_placeholders_left_in_are_reported(self):
        text = self.good(title="Fix the tick (YYYY-MM-DD)")
        self.assertIn("placeholder", rules(lint(self.PATH, text)))


class Repairs(unittest.TestCase):
    PATH = "docs/repairs/260928/kernel-mm.md"

    def good(self, title="1. Frames leak on unmap", severity="High, wrong on a path that runs today."):
        return "# kernel-mm outstanding repairs\n\n" + entry(title, REPAIR_LABELS, severity)

    def test_good_file(self):
        self.assertEqual(lint(self.PATH, self.good()), [])

    def test_items_are_numbered(self):
        self.assertEqual(rules(lint(self.PATH, self.good(title="Frames leak on unmap"))), ["entry-title"])

    def test_severity_is_high_medium_or_low(self):
        found = lint(self.PATH, self.good(severity="P1, urgent"))
        self.assertEqual(rules(found), ["severity"])
        for word in ("High", "Medium", "Low"):
            self.assertEqual(lint(self.PATH, self.good(severity=f"{word}: reason")), [])

    def test_h1(self):
        self.assertEqual(rules(lint(self.PATH, self.good().replace("outstanding repairs", "待修项"))), ["h1"])


class TaskDocuments(unittest.TestCase):
    def design(self, headings=None, header=None, h1="vmalloc: kernel virtual allocator"):
        headings = STD.task_headings["DESIGN"] if headings is None else headings
        header = ["**Date**: 2026-09-28", "**Status**: Proposed"] if header is None else header
        return "\n".join([f"# {h1}", "", *header, "", *[f"## {h}\nText.\n" for h in headings]])

    PATH = ".agents/tasks/vmalloc/DESIGN.md"

    def test_good_design(self):
        self.assertEqual(lint(self.PATH, self.design()), [])

    def test_missing_and_misordered_sections(self):
        found = lint(self.PATH, self.design(headings=STD.task_headings["DESIGN"][:-2]))
        self.assertEqual(rules(found), ["sections"])
        self.assertIn(STD.task_headings["DESIGN"][-1], found[0].message)
        reordered = STD.task_headings["DESIGN"][::-1]
        self.assertEqual(rules(lint(self.PATH, self.design(headings=reordered))), ["sections"])

    def test_extra_sections_are_allowed(self):
        extra = STD.task_headings["DESIGN"] + ["Appendix"]
        self.assertEqual(lint(self.PATH, self.design(headings=extra)), [])

    def test_header_lines(self):
        self.assertEqual(rules(lint(self.PATH, self.design(header=["**Status**: Proposed"]))), ["header"])
        self.assertEqual(rules(lint(self.PATH, self.design(header=["**Date**: 2026-09-28", "**Status**: Maybe"]))), ["header"])

    def test_result_needs_no_status(self):
        headings = STD.task_headings["RESULT"]
        text = "\n".join(["# vmalloc: delivered", "", "**Date**: 2026-09-28", "", *[f"## {h}\nText.\n" for h in headings]])
        self.assertEqual(lint(".agents/tasks/vmalloc/RESULT.md", text), [])

    def test_title_starts_with_the_topic(self):
        self.assertEqual(rules(lint(self.PATH, self.design(h1="Something else"))), ["h1"])

    def test_paths(self):
        self.assertEqual(rules(lint(".agents/tasks/vmalloc/design.md", self.design())), ["path"])
        self.assertEqual(rules(lint(".agents/tasks/VMalloc/DESIGN.md", self.design(h1="VMalloc: x"))), ["path"])
        self.assertEqual(lint(".agents/tasks/vmalloc/STATUS.md", "# vmalloc: status\n\nFree-form.\n"), [])

    def test_accepted_designs_use_the_design_skeleton(self):
        self.assertEqual(lint("docs/design/260928/vmalloc.md", self.design()), [])
        self.assertEqual(rules(lint("docs/design/260928/vmalloc.md", self.design(headings=["Problem"]))), ["sections"])


class TemplatesAreTheSourceOfTruth(unittest.TestCase):
    def test_change_labels(self):
        self.assertEqual(STD.change_labels, CHANGE_LABELS)

    def test_repair_labels(self):
        self.assertEqual(STD.repair_labels, REPAIR_LABELS)

    def test_task_skeletons(self):
        self.assertEqual(sorted(STD.task_headings), ["DESIGN", "PLAN", "RESULT", "REVIEW"])
        self.assertEqual(STD.task_headings["DESIGN"][0], "Problem")
        self.assertIn("Teaching opportunities", STD.task_headings["REVIEW"])

    def test_templates_follow_their_own_rules(self):
        for rel in sorted(lint_docs.TEMPLATES):
            with self.subTest(template=rel):
                text = (lint_docs.ROOT / rel).read_text(encoding="utf-8")
                self.assertEqual(lint(rel, text), [])

    def test_fenced_block_parsing(self):
        text = "intro\n```markdown\n**A**\nx\n**B**\n```\n```markdown\n**C**\n```\n"
        self.assertEqual(lint_docs.entry_labels(text), ["A", "B"])


class InARepository(unittest.TestCase):
    """The git side: which lines of which documents count as yours."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.top = Path(self.tmp.name).resolve()
        self.patch = mock.patch.object(lint_docs, "ROOT", self.top)
        self.patch.start()
        self.git("init", "-q")

    def tearDown(self):
        self.patch.stop()
        self.tmp.cleanup()

    def git(self, *args):
        return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args], cwd=self.top,
                              capture_output=True, text=True, check=True).stdout

    def write(self, rel, text):
        p = self.top / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        return p

    def commit(self):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "x")

    def test_only_the_added_lines_of_a_modified_document(self):
        p = self.write("docs/notes.md", "# Notes\n\nold \n")            # a legacy trailing space
        self.commit()
        p.write_text("# Notes\n\nold \nnew \n")
        self.assertEqual(lint_docs.changed_files(), {"docs/notes.md": {4}})
        found = check_text("docs/notes.md", p.read_text(), {4}, STD)
        self.assertEqual([v.line for v in found], [4])

    def test_a_moved_document_is_not_blamed_for_its_body(self):
        self.write("docs/old.md", "# Old\n\nlegacy \n")
        self.commit()
        self.git("mv", "docs/old.md", "docs/new.md")
        self.assertEqual(lint_docs.changed_files(), {"docs/new.md": set()})
        (self.top / "docs/new.md").write_text("# Old\n\nlegacy \nadded \n")
        self.assertEqual(lint_docs.changed_files(), {"docs/new.md": {4}})

    def test_a_new_document_is_checked_whole(self):
        self.write("docs/keep.md", "# Keep\n")
        self.commit()
        self.write("docs/fresh.md", "# Fresh\n")
        self.assertEqual(lint_docs.changed_files(), {"docs/fresh.md": None})
        self.git("add", "docs/fresh.md")
        self.assertEqual(lint_docs.changed_files(), {"docs/fresh.md": None})    # staged-new is new too

    def test_a_repository_without_commits_has_only_new_files(self):
        self.write("docs/a.md", "# A\n")
        self.git("add", "docs/a.md")
        self.write("docs/b.md", "# B\n")
        self.assertEqual(lint_docs.changed_files(), {"docs/a.md": None, "docs/b.md": None})

    def test_only_restricts_to_the_given_paths(self):
        a, b = self.write("docs/a.md", "# A\n"), self.write("docs/b.md", "# B\n")
        self.commit()
        a.write_text("# A\nmore\n")
        b.write_text("# B\nmore\n")
        self.write("docs/c.md", "# C\n")
        self.assertEqual(sorted(lint_docs.changed_files()), ["docs/a.md", "docs/b.md", "docs/c.md"])
        self.assertEqual(sorted(lint_docs.changed_files(only={"docs/a.md"})), ["docs/a.md"])
        self.assertEqual(sorted(lint_docs.changed_files(only={"docs/c.md"})), ["docs/c.md"])

    def test_files_outside_the_documents_are_ignored(self):
        self.write("README.md", "# R\n")
        self.write("kernel/notes.md", "# K\n")
        self.write("docs/ok.md", "# Ok\n")
        self.assertEqual(sorted(lint_docs.changed_files()), ["docs/ok.md"])

    def hook(self, payload):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = lint_docs.claude_hook(json.dumps(payload))
        return code, out.getvalue()

    def test_hook_blocks_with_the_problems_listed(self):
        p = self.write("docs/notes.md", "# Notes\n\ntext \n")
        code, out = self.hook({"tool_input": {"file_path": str(p)}})
        self.assertEqual(code, 0)
        answer = json.loads(out)
        self.assertEqual(answer["decision"], "block")
        self.assertIn("trailing-whitespace", answer["reason"])
        self.assertIn("line 3", answer["reason"])
        self.assertIn("--changed docs/notes.md", answer["reason"])

    def test_hook_is_silent_when_there_is_nothing_of_yours_to_fix(self):
        legacy = self.write("docs/legacy.md", "# Legacy\n\nold \n")
        self.commit()
        clean = self.write("docs/clean.md", "# Clean\n\ntext\n")
        outside = self.write("README.md", "bad \n")
        legacy.write_text("# Legacy\n\nold \nfine\n")     # only the pre-existing bad line is bad
        for payload in [{"tool_input": {"file_path": str(clean)}}, {"tool_input": {"file_path": str(outside)}},
                        {"tool_input": {"file_path": str(legacy)}}, {"tool_input": {}}, {},
                        {"tool_input": {"file_path": str(self.top / "docs/missing.md")}}]:
            with self.subTest(payload=payload):
                self.assertEqual(self.hook(payload), (0, ""))

    def test_hook_ignores_a_file_of_another_repository(self):
        with tempfile.TemporaryDirectory() as elsewhere:
            p = Path(elsewhere) / "docs" / "x.md"
            p.parent.mkdir()
            p.write_text("bad \n")
            self.assertEqual(self.hook({"tool_input": {"file_path": str(p)}}), (0, ""))


if __name__ == "__main__":
    unittest.main()
