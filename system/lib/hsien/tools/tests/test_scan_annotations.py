import json
import pathlib
import unittest

from system.lib.hsien.tools import scan_annotations


def function_node(
    name="test_one",
    qualified_name="test_one",
    type_name="void (hsien::Context &)",
    annotation_type="hsien::test_t",
    parameter_values=None,
    line=7,
):
    annotations = [
        {
            "kind": "CXX26AnnotationAttr",
            "inner": [
                {
                    "kind": "CXXConstructExpr",
                    "type": {"desugaredQualType": annotation_type},
                    "inner": [
                        {
                            "kind": "DeclRefExpr",
                            "referencedDecl": {
                                "name": "test",
                                "type": {
                                    "desugaredQualType": "const "
                                    + annotation_type
                                },
                            },
                        }
                    ],
                }
            ],
        }
    ]
    if parameter_values is not None:
        annotations.append(
            {
                "kind": "CXX26AnnotationAttr",
                "inner": [
                    {
                        "kind": "CXXConstructExpr",
                        "type": {
                            "desugaredQualType": "hsien::values_t<"
                            + ", ".join(str(value) for value in parameter_values)
                            + ">"
                        },
                        "inner": [
                            {
                                "kind": "TemplateArgument",
                                "value": value,
                            }
                            for value in parameter_values
                        ],
                    }
                ],
            }
        )
    return {
        "kind": "FunctionDecl",
        "name": name,
        "qualifiedName": qualified_name,
        "loc": {"file": "tests/sample.cpp", "line": line, "col": 1},
        "type": {"qualType": type_name},
        "inner": annotations,
    }


class ScanAnnotationsTest(unittest.TestCase):
    def test_scans_hsien_test_annotation(self):
        records = scan_annotations.scan_ast(
            {"kind": "TranslationUnitDecl", "inner": [function_node()]},
            pathlib.PurePosixPath("tests/sample.cpp"),
        )

        self.assertEqual([record.name for record in records], ["test_one"])
        self.assertEqual(records[0].qualified_name, "test_one")

    def test_rejects_other_annotation_type(self):
        with self.assertRaises(scan_annotations.AnnotationError):
            scan_annotations.scan_ast(
                {
                    "kind": "TranslationUnitDecl",
                    "inner": [function_node(annotation_type="other::tag")],
                },
                pathlib.PurePosixPath("tests/sample.cpp"),
            )

    def test_rejects_wrong_test_signature(self):
        with self.assertRaises(scan_annotations.AnnotationError):
            scan_annotations.scan_ast(
                {
                    "kind": "TranslationUnitDecl",
                    "inner": [function_node(type_name="void ()")],
                },
                pathlib.PurePosixPath("tests/sample.cpp"),
            )

    def test_sorts_records_deterministically(self):
        records = scan_annotations.scan_ast(
            {
                "kind": "TranslationUnitDecl",
                "inner": [
                    function_node(name="z_test", line=20),
                    function_node(name="a_test", line=10),
                ],
            },
            pathlib.PurePosixPath("tests/sample.cpp"),
        )

        ordered = scan_annotations.sort_records(records)
        self.assertEqual([record.name for record in ordered], ["a_test", "z_test"])

    def test_renders_escaped_wrapper_and_section_record(self):
        records = scan_annotations.scan_ast(
            {"kind": "TranslationUnitDecl", "inner": [function_node()]},
            pathlib.PurePosixPath('tests/quote\\name".cpp'),
        )

        wrapper = scan_annotations.render_wrapper(
            pathlib.PurePosixPath('tests/quote\\name".cpp'), records
        )

        self.assertIn('#include "tests/quote\\\\name\\\".cpp"', wrapper)
        self.assertIn(".hsien.tests", wrapper)
        self.assertIn("&test_one", wrapper)

    def test_expands_parameter_values_into_test_instances(self):
        records = scan_annotations.scan_ast(
            {
                "kind": "TranslationUnitDecl",
                "inner": [
                    function_node(
                        type_name="void (hsien::Context &, int)",
                        parameter_values=[1, 2, 3],
                    )
                ],
            },
            pathlib.PurePosixPath("tests/sample.cpp"),
        )

        self.assertEqual(
            [record.name for record in records],
            ["test_one/0", "test_one/1", "test_one/2"],
        )
        self.assertEqual([record.parameter_value for record in records], [1, 2, 3])

    def test_renders_parameter_adapter(self):
        records = scan_annotations.scan_ast(
            {
                "kind": "TranslationUnitDecl",
                "inner": [
                    function_node(
                        type_name="void (hsien::Context &, int)",
                        parameter_values=[42],
                    )
                ],
            },
            pathlib.PurePosixPath("tests/sample.cpp"),
        )

        wrapper = scan_annotations.render_wrapper(
            pathlib.PurePosixPath("tests/sample.cpp"), records
        )

        self.assertIn("test_one(context, 42);", wrapper)
        self.assertIn(".name = \"test_one/0\"", wrapper)

    def test_multi_source_output_uses_unique_record_symbols(self):
        first = scan_annotations.scan_ast(
            {"kind": "TranslationUnitDecl", "inner": [function_node(name="first")]},
            pathlib.PurePosixPath("tests/first.cpp"),
        )
        second = scan_annotations.scan_ast(
            {"kind": "TranslationUnitDecl", "inner": [function_node(name="second")]},
            pathlib.PurePosixPath("tests/second.cpp"),
        )
        wrapper = "\n".join(
            [
                scan_annotations.render_wrapper(first[0].source_path, first),
                scan_annotations.render_wrapper(second[0].source_path, second, record_offset=1),
            ]
        )

        self.assertEqual(wrapper.count("hsien_record_0"), 1)
        self.assertEqual(wrapper.count("hsien_record_1"), 1)


if __name__ == "__main__":
    unittest.main()
