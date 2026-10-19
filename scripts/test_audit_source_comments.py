#!/usr/bin/env python3
"""局所callback監査の境界を、実ソースや設定を変更せず検証する。

Copyright (c) 2026 EnterSystem CO.,LTD.
SPDX-License-Identifier: Apache-2.0
"""
import unittest

from audit_source_comments import lambda_contracts


class CallbackContractTest(unittest.TestCase):
    """未対応候補を隠さず、literal/母関数/無名引数を誤認しない。"""

    def test_named_lambda_missing_contract(self):
        result = lambda_contracts("const auto f = [](int value){ return value; };")
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["missing_parameters"], ["value"])
        self.assertIn("missing_return", result[0]["issues"])

    def test_documented_local_contract(self):
        source = "/** @brief 値を返す。 @param[in] value 入力。 @return 入力値。 */\n"
        source += "const auto f = [](int value){ return value; };"
        self.assertEqual(lambda_contracts(source)[0]["issues"], [])

    def test_cond_does_not_hide_source_contract(self):
        source = "/** @cond LOCAL_CALLBACK_CONTRACT\n * @brief 値を返す。\n"
        source += " * @param[in] value 入力。\n * @return 入力値。\n * @endcond\n */\n"
        source += "const auto f = [](int value){ return value; };"
        self.assertEqual(lambda_contracts(source)[0]["issues"], [])

    def test_parent_contract_is_not_local_contract(self):
        source = "/** @brief 親関数。 @return 結果。 */\nvoid Parent(){auto f=[](){return 1;};}"
        self.assertIn("missing_brief", lambda_contracts(source)[0]["issues"])

    def test_no_argument_parentheses(self):
        result = lambda_contracts("const auto predicate = [&] { return ready; };")
        self.assertEqual(len(result), 1)
        self.assertIn("missing_return", result[0]["issues"])

    def test_void_worker(self):
        source = "/** @brief worker。 @param[in] stop 停止。 */\n"
        source += "worker=std::jthread([this](std::stop_token stop) noexcept {Run(stop);});"
        result = lambda_contracts(source)[0]
        self.assertFalse(result["returns_value"])
        self.assertEqual(result["issues"], [])

    def test_unnamed_parameter_note(self):
        source = "/** @brief fake。 @return true。 @note 無名pathは順序のみの試験なので無視する。 */\n"
        source += "Install([](const std::filesystem::path &){return true;});"
        result = lambda_contracts(source)[0]
        self.assertEqual(result["parameters"][0]["name"], None)
        self.assertEqual(result["issues"], [])

    def test_unnamed_without_note_is_rejected(self):
        source = "/** @brief fake。 @return true。 */\nCall([](DWORD){return true;});"
        self.assertIn("missing_unnamed_parameter_note", lambda_contracts(source)[0]["issues"])

    def test_two_lambdas_in_one_line(self):
        self.assertEqual(len(lambda_contracts("Call([]{return true;},[&]{return now;});")), 2)

    def test_nested_body_delimiters(self):
        result = lambda_contracts("auto f=[](int n){if(n){return Result{1,2};}return Result{};};")
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["parameters"][0]["name"], "n")

    def test_template_parameter_comma(self):
        result = lambda_contracts("auto f=[](const std::array<INPUT,4> &inputs){return inputs.size();};")
        self.assertEqual(result[0]["parameters"], [{"name": "inputs", "declaration": "const std :: array < INPUT , 4 > & inputs"}])

    def test_literals_comments_and_array_are_not_lambdas(self):
        source = 'auto x=values[index](); const char *s="[](int n){return n;}"; /* []{} */'
        self.assertEqual(lambda_contracts(source), [])


if __name__ == "__main__":
    unittest.main()
