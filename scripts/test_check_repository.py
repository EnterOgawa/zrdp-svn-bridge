#!/usr/bin/env python3
"""公開tree検査の境界をmemoryで検証する。

Copyright (c) 2026 EnterSystem CO.,LTD.
SPDX-License-Identifier: Apache-2.0
"""
import unittest

from check_repository import file_issues, workflow_issues

HEADER = "Copyright (c) 2026 EnterSystem CO.,LTD.\nSPDX-License-Identifier: Apache-2.0\n"
WORKFLOW = """permissions:
  contents: read
jobs:
  test:
    runs-on: windows-2022
    timeout-minutes: 25
    steps:
      - name: checkout
        uses: actions/checkout@""" + "a" * 40 + """
        with:
          persist-credentials: false
"""


class RepositoryBoundaryTest(unittest.TestCase):
    """値を出力せず、不適切なtree／CIを拒否する。"""

    def test_valid_own_source(self):
        self.assertEqual(file_issues("src/example.cpp", HEADER.encode()), [])

    def test_missing_own_header(self):
        self.assertIn("missing-own-license-header", file_issues("scripts/test.py", b"pass"))

    def test_binary_not_committed(self):
        self.assertIn("forbidden-file", file_issues("docs/app.exe", b"MZ\0"))

    def test_real_configuration_not_committed(self):
        self.assertIn("forbidden-file", file_issues("config/agent.ini", b"[allowlist]"))

    def test_fixture_not_committed(self):
        self.assertIn("generated-or-private-directory", file_issues("work/test.md", b"fixture"))

    def test_example_configuration_allowed(self):
        self.assertEqual(file_issues("config/examples/agent.ini.example", b"[allowlist]"), [])

    def test_secret_values_not_returned(self):
        value = "ghp_" + "A" * 40
        result = file_issues("docs/fixture.txt", value.encode())
        self.assertEqual(result, ["secret-pattern"])
        self.assertNotIn(value, str(result))

    def test_valid_workflow(self):
        self.assertEqual(workflow_issues(WORKFLOW), [])

    def test_mutable_action_ref(self):
        self.assertIn("action-not-fixed-official-checkout", workflow_issues(WORKFLOW.replace("a" * 40, "v4")))

    def test_write_permission(self):
        self.assertIn("ci-write-permission", workflow_issues(WORKFLOW.replace("contents: read", "contents: write")))

    def test_persistent_credentials(self):
        self.assertIn("persistent-ci-credential", workflow_issues(WORKFLOW.replace("persist-credentials: false", "persist-credentials: true")))

    def test_secrets_and_upload(self):
        self.assertIn("ci-forbidden-operation", workflow_issues(WORKFLOW + "\nvalue: ${{ secrets.TEST }}\n"))

    def test_privileged_event(self):
        self.assertIn("ci-forbidden-operation", workflow_issues(WORKFLOW + "\npull_request_target:\n"))


if __name__ == "__main__":
    unittest.main()
