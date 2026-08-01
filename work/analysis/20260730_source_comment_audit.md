# 全sourceコメント監査

日付: 2026-07-30

## 対象

現時点で存在するC++ source、header、testの全15fileを対象とした。

- `src/common/diagnostic_log.h`
- `src/common/diagnostic_log.cpp`
- `src/client/app/connection_history.h`
- `src/client/app/connection_history.cpp`
- `src/client/app/main_window.h`
- `src/client/app/main_window.cpp`
- `src/client/app/main.cpp`
- `src/client/app/resource.h`
- `src/client/rdp/rdp_control_host.h`
- `src/client/rdp/rdp_control_host.cpp`
- `src/client/rdp/rdp_import.h`
- `tests/unit/build_contract_test.cpp`
- `tests/unit/connection_history_test.cpp`
- `tests/unit/diagnostic_log_test.cpp`
- `tests/unit/rdp_host_smoke_test.cpp`

`src/protocol`および`src/agent`には、現時点でコメント対象となるC++ fileがまだ存在しない。今後追加するfileには`AGENTS.md`の規約を作成時点から適用する。

## 適用内容

- 15/15fileへ、責務と主要制約を示すDoxygen `@file`説明を付与した。
- public type／function、COM callback、thread entry、RAII所有境界、非自明な内部helperへDoxygen説明を付与した。
- parameterの入出力、戻り値、所有権、thread制約、失敗時の扱いを必要な箇所へ明記した。
- security、length、state、COM STA、DPAPI、BSTR binary、credential非保存の判定理由を記載した。
- 25/25loopの直前へ、反復の目的を示すコメントがあることを静的確認した。
- Remote Desktopのwindow resizeはActiveX子windowだけへ適用し、remote desktop寸法を更新しない理由を明記した。
- コメント本文は日本語を基本とし、author、日付入り変更履歴、手動version履歴、コメントアウトcodeを追加していない。
- 実接続先、実ユーザー名、RDP画面内容、credential、HTTP／SVN payloadを記載していない。

今回のsource変更はコメント追加だけであり、実行文、設定値、resource、公開interfaceの動作は変更していない。

## 検証結果

- x64 Debug build: 成功。
- Debug CTest: 4/4成功。
- x64 Release build: 成功。
- Release CTest: 4/4成功。
- `/W4`および`/WX`を維持し、warningは0件。
- Release EXE: `out/ZRdpSvnBridge.exe`
- File version: `0.1.7.0`
- Size: `435200` bytes
- SHA-256: `B7824ABCF4F6B73AF73AA295B5A08619C4EB76942D6EE1C425688D711B075549`

Phase 0の実機表示確認結果は`work/analysis/20260730_phase0_remote_desktop_preservation.md`へ分離して記録した。RDP画面接続の成立だけではSVN Bridgeの完了とは判定しない。
