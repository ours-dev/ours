import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import code_size

NM = """\
0000000000001000 0000000000000040 t entry(int)
0000000000001040 0000000000000020 t helper()
0000000000001060 0000000000000010 t tail()
0000000000001070 0000000000000008 t pointee()
0000000000001078 0000000000000030 T lock()
0000000000001078 0000000000000030 T lock_alias()
00000000000010a8 0000000000000000 t empty()
0000000000002000 0000000000000100 d data
"""

DISASSEMBLY = {
    "entry(int)": """\
    1000:      	pushq	%rbp
    1004:      	jne	0x1010 <entry(int)+0x10>
    1008:      	callq	0x1040 <helper()>
    100d:      	leaq	0x5c(%rip), %rdx       # 0x1070 <pointee()>
    1014:      	callq	0x1078 <lock()>
    1019:      	movq	0xfe0(%rip), %rax      # 0x2000 <data>
    1020:      	callq	*%rax
    1030:      	jmp	0x1060 <tail()>
""",
    "helper()": """\
    1040:      	callq	0x1040 <helper()>
    1045:      	jmp	0x1060 <tail()>
""",
    "tail()": "    1060:      	retq\n",
    "pointee()": "    1070:      	retq\n",
}


def disassemble(function):
    return DISASSEMBLY[function.name]


class CodeSize(unittest.TestCase):
    def setUp(self):
        self.symbols = code_size.Symbols(code_size.parse_symbols(NM))

    def test_symbols_keep_text_with_a_size_and_one_name_an_address(self):
        names = [f.name for f in self.symbols.functions]
        self.assertEqual(names, ["entry(int)", "helper()", "tail()", "pointee()", "lock()"])

    def test_an_address_belongs_to_the_function_around_it(self):
        self.assertEqual(self.symbols.containing(0x1050).name, "helper()")
        self.assertIsNone(self.symbols.containing(0x2000))

    def test_the_closure_follows_calls_tail_calls_and_taken_addresses_once(self):
        within = [re.compile(r"^(entry|helper|tail|pointee)\(")]
        entry = self.symbols.matching(r"^entry\(")[0]
        found = code_size.closure(entry, self.symbols, within, disassemble)
        self.assertEqual([f.name for f in found.functions],
                         ["entry(int)", "helper()", "pointee()", "tail()"])
        self.assertEqual(found.size, 0x40 + 0x20 + 0x8 + 0x10)
        self.assertEqual(found.indirect, 1)
        self.assertEqual(found.external, {"lock()"})

    def test_a_function_that_calls_nothing_is_its_own_size(self):
        tail = self.symbols.matching(r"^tail\(")[0]
        found = code_size.closure(tail, self.symbols, [re.compile(".")], disassemble)
        self.assertEqual(found.size, 0x10)
        self.assertEqual(len(found.functions), 1)


if __name__ == "__main__":
    unittest.main()
