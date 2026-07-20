import json
import pathlib
import sys
import tempfile
import unittest

from tools.annotation.actions import ActionError, dispatch
from tools.annotation.model import ActionRequest, ActionResult, ActionSpec, AnnotationInfo
from tools.annotation.protocol import request_to_json, result_from_json


FIXTURE_ACTION = pathlib.Path(__file__).parent / "fixtures" / "external_action.py"


def request() -> ActionRequest:
    return ActionRequest(
        schema_version=1,
        action_name="ours::generate",
        source_root=pathlib.PurePosixPath("/workspace"),
        annotations=(
            AnnotationInfo(
                schema_version=1,
                canonical_name="ours::generate",
                source_file=pathlib.PurePosixPath("src/message.cpp"),
                line=3,
                column=1,
                declaration_kind="record",
                declaration_name="Message",
                qualified_name="sample::Message",
                declaration_usr=None,
                arguments={"format": "text"},
            ),
        ),
    )


class ActionsTest(unittest.TestCase):
    def test_serializes_request_with_stable_json(self):
        encoded = request_to_json(request())
        decoded = json.loads(encoded)

        self.assertEqual(decoded["action_name"], "ours::generate")
        self.assertEqual(decoded["annotations"][0]["source_file"], "src/message.cpp")

    def test_dispatches_in_process_callback(self):
        seen: list[ActionRequest] = []

        def callback(value: ActionRequest) -> ActionResult:
            seen.append(value)
            return ActionResult.empty(1)

        spec = ActionSpec("ours::generate", ("record",), 1, callback=callback)

        with tempfile.TemporaryDirectory() as directory:
            result = dispatch(spec, request(), pathlib.Path(directory))

        self.assertEqual(result.exit_status, 0)
        self.assertEqual(seen, [request()])

    def test_dispatches_external_command_through_json_protocol(self):
        spec = ActionSpec(
            "ours::generate",
            ("record",),
            1,
            command=(sys.executable, str(FIXTURE_ACTION)),
        )

        with tempfile.TemporaryDirectory() as directory:
            result = dispatch(spec, request(), pathlib.Path(directory))

        self.assertEqual(result.generated_files[0].path.as_posix(), "generated/action.txt")
        self.assertEqual(result.dependencies, (pathlib.PurePosixPath("src/message.cpp"),))

    def test_rejects_malformed_result_json(self):
        with self.assertRaises(ActionError):
            result_from_json('{"schema_version": 1}')

    def test_rejects_result_with_path_traversal(self):
        payload = {
            "schema_version": 1,
            "generated_files": [{"path": "../escape", "contents": "bad"}],
            "dependencies": [],
            "diagnostics": [],
            "exit_status": 0,
        }

        with self.assertRaises(ActionError):
            result_from_json(json.dumps(payload))

    def test_rejects_duplicate_generated_paths(self):
        def callback(value: ActionRequest) -> ActionResult:
            from tools.annotation.model import GeneratedFile

            return ActionResult(
                1,
                (
                    GeneratedFile(pathlib.PurePosixPath("same.cpp"), "one"),
                    GeneratedFile(pathlib.PurePosixPath("same.cpp"), "two"),
                ),
                (),
                (),
                0,
            )

        spec = ActionSpec("ours::generate", ("record",), 1, callback=callback)
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ActionError):
                dispatch(spec, request(), pathlib.Path(directory))


if __name__ == "__main__":
    unittest.main()
