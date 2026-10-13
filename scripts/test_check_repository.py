#!/usr/bin/env python3
"""公開tree検査の境界をmemoryで検証する。

Copyright (c) 2026 EnterSystem CO.,LTD.
SPDX-License-Identifier: Apache-2.0
"""
from pathlib import Path
import re
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

    def test_installer_own_header(self):
        self.assertEqual(file_issues("installer/app_installer_setup.iss", HEADER.encode()), [])
        self.assertIn("missing-own-license-header", file_issues("installer/app_installer_setup.iss", b"[Setup]"))

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


class InstallerBoundaryTest(unittest.TestCase):
    """compilerを導入しないCIでも、installerの配布・削除・署名境界を維持する。"""

    @classmethod
    def setUpClass(cls):
        cls.text = (Path(__file__).resolve().parents[1] / "installer/app_installer_setup.iss").read_text(encoding="utf-8-sig")

    def test_fixed_payload_only(self):
        sources = re.findall(r'^Source: "([^"]+)"', self.text, re.M)
        self.assertEqual(sources, ["{#ClientImage}", "{#AgentImage}", "{#RepositoryDirectory}\\LICENSE",
            "{#RepositoryDirectory}\\NOTICE", "{#RepositoryDirectory}\\docs\\dependencies.md", "{#SourcePath}\\README.txt"])
        self.assertNotIn("recursesubdirs", self.text.lower())

    def test_user_install_without_automatic_close(self):
        for directive in ("PrivilegesRequired=lowest", "DefaultDirName={localappdata}\\Programs\\{#AppName}",
                "CloseApplications=no", "RestartApplications=no", "SignToolRetryCount=0"):
            self.assertIn(directive, self.text)

    def test_no_recursive_or_unconditional_delete(self):
        self.assertNotRegex(self.text, r"(?im)^\s*\[(?:InstallDelete|UninstallDelete)\]")
        self.assertNotRegex(self.text, r"(?im)^\s*(?:RegDelete|DelTree|DeleteFile|TerminateProcess)\s*\(")

    def test_opt_in_connection_data_only(self):
        self.assertIn("Option.Checked := False;", self.text)
        self.assertIn("DeleteConnectionData := (Choice = mrOk) and Requested;", self.text)
        self.assertIn("if (CurUninstallStep <> usPostUninstall) or not DeleteConnectionData then Exit;", self.text)
        self.assertIn("{param:DELETECONNECTIONDATA|0}') = '1'", self.text)
        self.assertEqual(self.text.count("DeleteConnectionFile(Path)"), 1)
        paths = self.text.split("function ConnectionDataPath", 1)[1].split("end;", 1)[0]
        self.assertEqual(re.findall(r"\+ '([^']+)'", paths), ["connection-history.bin", "agent-settings.bin"])
        self.assertIn("ExpandConstant('{localappdata}\\ZRdpSvnBridge')", self.text)
        self.assertIn("'{#InstallerOutputDirectory}\\connection-data'", self.text)
        self.assertNotIn("{param:CONNECTIONDATADIR", self.text)

    def test_connection_data_safety_and_cancel(self):
        self.assertIn("(Attributes and $400) <> 0", self.text)
        self.assertIn("(Attributes and ($1 or $10 or $400)) <> 0", self.text)
        self.assertIn("RecordUninstallChoice(mrCancel, True)", self.text)
        self.assertIn("Result := RecordUninstallChoice(Choice, Option.Checked);", self.text)
        cleanup = self.text.split("procedure CurUninstallStepChanged", 1)[1]
        self.assertLess(cleanup.index("ConnectionDataSafetyReason()"), cleanup.index("DeleteConnectionFile(Path)"))

    def test_test_mode_and_explicit_signing(self):
        self.assertIn("#ifdef InstallerTestMode", self.text)
        self.assertIn("CreateUninstallRegKey=no", self.text)
        self.assertIn("#ifndef InstallerTestMode\n[Icons]", self.text)
        self.assertIn("#ifdef EnableEvSigning\nSignTool=ZRdpSvnBridgeEV\nSignedUninstaller=yes", self.text)


if __name__ == "__main__":
    unittest.main()
