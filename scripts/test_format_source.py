#!/usr/bin/env python3
"""指定書式のtoken保持と繰返し整形の安定性を検証する。

Copyright (c) 2026 EnterSystem CO.,LTD.
SPDX-License-Identifier: Apache-2.0
"""
import unittest
from unittest.mock import patch

import format_source


class PrescribedSpacesTest(unittest.TestCase):
    """literal・式・preprocessorを保護する後処理の境界を確認する。"""

    def test_calls_controls_and_declarations(self):
        source = "void f(int x); if(x) { f(x); f(); }"
        self.assertEqual(format_source.prescribed_spaces(source),
                         "void f( int x) ; if( x) { f( x) ; f() ; }")

    def test_for_separators_remain_unpadded(self):
        source = "for(int i = 0; i < 2; ++i) { f(i); }"
        result = format_source.prescribed_spaces(source)
        self.assertEqual(result, "for( int i = 0; i < 2; ++i) { f( i) ; }")

    def test_grouping_and_comparison_are_not_calls(self):
        source = "return (x + y); bool b = value > (limit);"
        result = format_source.prescribed_spaces(source)
        self.assertEqual(result, "return (x + y) ; bool b = value > (limit) ;")

    def test_template_call(self):
        source = "auto x = std::min<int>(a, b);"
        self.assertEqual(format_source.prescribed_spaces(source),
                         "auto x = std::min<int>( a, b) ;")

    def test_function_pointer_is_token_preserving(self):
        """function pointerの括弧補正でも宣言tokenを変更しない。"""
        source = "int (*callback)(int) = nullptr;"
        result = format_source.prescribed_spaces(source)
        self.assertEqual(format_source.token_values(source), format_source.token_values(result))

    def test_overloaded_operator_declarations(self):
        source = "Foo &operator=(const Foo &) = delete;"
        self.assertEqual(format_source.prescribed_spaces(source),
                         "Foo &operator=( const Foo &) = delete ;")

    def test_literals_and_comments(self):
        source = 'auto x = R"tag(if(x); \\"value\\"\nfor(;;))tag"; // f(x);\n'
        result = format_source.prescribed_spaces(source)
        self.assertEqual(format_source.token_values(source), format_source.token_values(result))
        self.assertIn('R"tag(if(x); \\"value\\"\nfor(;;))tag"', result)
        self.assertIn("// f(x);", result)

    def test_embedded_null_and_escaped_quote(self):
        source = r'''const char *x = "a\0b\";"; auto y = L'\0';'''
        result = format_source.prescribed_spaces(source)
        self.assertEqual(format_source.token_values(source), format_source.token_values(result))

    def test_preprocessor_and_line_splice(self):
        source = "#define APPLY(x) \\\n  f(x);\nvoid f();\n"
        result = format_source.prescribed_spaces(source)
        self.assertTrue(result.startswith("#define APPLY(x) \\\n  f(x);\n"))
        self.assertEqual(format_source.token_values(source), format_source.token_values(result))

    def test_one_line_postprocessing_is_idempotent(self):
        source = "if(x) { std::min<int>(a, b); }"
        once = format_source.prescribed_spaces(source)
        self.assertEqual(once, format_source.prescribed_spaces(once))

    def test_independent_blocks_are_separated(self):
        source = "if( x) {\n f() ;\n}\n// 次の処理\ng() ;\n"
        result = format_source.prescribed_breaks(source)
        self.assertIn("}\n\n// 次の処理", result)
        self.assertEqual(result, format_source.prescribed_breaks(result))

    def test_continuous_blocks_stay_together(self):
        source = "if( x) {\n}\n// 代替処理\nelse {\n}\n"
        self.assertEqual(source, format_source.prescribed_breaks(source))
        source = "do {\n}\nwhile( x) ;\n"
        self.assertEqual(source, format_source.prescribed_breaks(source))

    def test_switch_cases_are_separated_after_break(self):
        source = "case 1:\n f() ;\n break ;\n// 次のcase\ncase 2:\n g() ;\n"
        result = format_source.prescribed_breaks(source)
        self.assertIn("break ;\n\n// 次のcase", result)

    def test_changed_literal_is_rejected(self):
        path = format_source.ROOT / "src/formatter_fixture.cpp"
        changed = type("Result", (), {"stdout": b'void f() { use("changed"); }'})()
        with patch.object(format_source.subprocess, "run", return_value=changed):
            with self.assertRaises(RuntimeError):
                format_source.format_text('void f() { use("original"); }', path, "mock")

    def test_doxygen_indent_preserves_contract_text(self):
        source = "\t/**\n     * @brief 説明。\n     * @version 01.00.00\n\t*/\n"
        expected = "\t/**\n\t * @brief 説明。\n\t * @version 01.00.00\n\t */\n"
        self.assertEqual(expected, format_source.prescribed_comment_indent(source))

    def test_real_formatter_fixed_point(self):
        formatter = format_source.find_formatter(None)
        source = "namespace fixture { int f(int x) { if(x > 0) { return x; } return 0; } }"
        path = format_source.ROOT / "src/formatter_fixture.cpp"
        once = format_source.format_text(source, path, formatter)
        self.assertEqual(once, format_source.format_text(once, path, formatter))
        self.assertEqual(format_source.token_values(source), format_source.token_values(once))


if __name__ == "__main__":
    unittest.main()
