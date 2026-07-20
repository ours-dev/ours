import unittest

from tools.annotation.model import ActionRequest, ActionResult, ActionSpec
from tools.annotation.registry import Registry, RegistryError


def callback(request: ActionRequest) -> ActionResult:
    return ActionResult.empty(request.schema_version)


class RegistryTest(unittest.TestCase):
    def test_registers_action_and_matches_canonical_attribute(self):
        registry = Registry()
        spec = ActionSpec(
            "ours::generate",
            ("record",),
            1,
            callback=callback,
        )

        registry.register(spec)

        self.assertIs(registry.resolve("ours::generate").callback, callback)

    def test_rejects_duplicate_attribute_names(self):
        registry = Registry()
        spec = ActionSpec("ours::generate", ("record",), 1, callback=callback)
        registry.register(spec)

        with self.assertRaises(RegistryError):
            registry.register(spec)

    def test_rejects_callback_and_command_together(self):
        with self.assertRaises(RegistryError):
            ActionSpec(
                "ours::invalid",
                ("record",),
                1,
                callback=callback,
                command=("action",),
            )

    def test_rejects_invalid_external_command_and_schema_version(self):
        with self.assertRaises(RegistryError):
            ActionSpec("ours::invalid", ("record",), 0, command=())

        with self.assertRaises(RegistryError):
            ActionSpec("ours::invalid", ("record",), 1, command=())

    def test_rejects_attribute_without_namespace(self):
        with self.assertRaises(RegistryError):
            ActionSpec("generate", ("record",), 1, callback=callback)

    def test_items_are_sorted_by_canonical_name(self):
        registry = Registry()
        registry.register(ActionSpec("ours::z", ("record",), 1, callback=callback))
        registry.register(ActionSpec("ours::a", ("record",), 1, callback=callback))

        self.assertEqual(
            [spec.canonical_name for spec in registry.items()],
            ["ours::a", "ours::z"],
        )


if __name__ == "__main__":
    unittest.main()
