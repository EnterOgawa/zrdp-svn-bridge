# 接続管理Windowの背景統一と名前付き接続プロファイル

## 1. 目的

- 接続管理Windowの背景をWindows標準ダイアログ色へ統一し、ラベルと
  ラジオボタンの背景矩形が浮いて見える状態を解消する。
- 任意の「接続名」を付け、接続名の選択だけで接続先、ドメイン、
  ユーザー名を復元できるようにする。
- 既存の名前なし接続履歴を保持し、更新後も読み込めるようにする。

## 2. 変更対象

### UI・session設定

- `src/client/app/connection_manager_window.h/.cpp`
  - `ConnectionManagerWindow::RegisterWindowClass`
    - top-level Windowの背景brushを`COLOR_BTNFACE`へ変更する。
  - `ConnectionManagerWindow::CreateControls`／`LayoutControls`
    - 編集可能な接続名コンボボックスを追加する。
    - 接続名、接続先、接続ボタンを一行に収め、現行のcompactな高さを維持する。
  - `ConnectionManagerWindow::PopulateConnectionHistoryCombos`
    - 接続名候補も大小文字を無視して重複排除する。
  - 接続名選択処理
    - 選択した履歴の接続先、ドメイン、ユーザー名を一組として復元する。
  - `ConnectionManagerWindow::OnConnect`
    - 任意の接続名をsession生成要求へ含める。
- `src/client/app/session_launch_settings.h`
  - `SessionConnectionRequest`と成功通知に接続名を追加する。
- `src/client/app/client_application.h/.cpp`
  - 接続名を共通履歴ownerへ中継する。
- `src/client/app/main_window.cpp`
  - 認証完了時に接続名を履歴ownerへ通知する。
  - 接続名がある場合、session titleへ`接続名 (実接続先)`の形で表示する。
    全画面titleは従来どおり実接続先を表示する。
- `src/client/app/resource.h`、`src/client/app/ZRdpSvnBridge.rc`
  - 接続名ラベルとVersion表示を追加・更新する。

### 接続履歴

- `src/client/app/connection_history.h/.cpp`
  - `ConnectionHistoryEntry`へ任意の接続名を追加する。
  - 現行の暗号化平文形式をVersion 2へ更新する。
  - Version 1は名前なし履歴として引き続き読み込む。
  - 同じ「接続先 + ドメイン + ユーザー名」、または同じ非空接続名を
    大小文字を無視して一件へ統合する。
  - 接続名は有限長とし、passwordを含むfieldは追加しない。
- `tests/unit/connection_history_test.cpp`
  - 名前付き保存／復元、名前と接続組の重複排除、Version 1読込互換、
    長さ・NUL拒否、DPAPI暗号化を検証する。
- `tests/unit/resource_contract_test.cpp`
  - 接続名ラベルとVersion resourceの完全一致契約を検証する。

### 文書・Version

- `AGENTS.md`
  - 接続名、重複排除、履歴Version 1読込互換をUI要件へ追記する。
- `docs/configuration.md`、`docs/security.md`、
  `docs/rdp-activex.md`、`docs/test-plan.md`、
  `docs/roadmap.md`、`docs/known-limitations.md`等
  - 名前付きプロファイルの保存内容、security境界、検証項目を更新する。
- `CMakeLists.txt`、Client／Agentのresource・manifest
  - 配布Versionを`0.7.3.0`へそろえる。

## 3. 変更理由

`STATIC`とradio buttonはWindows標準ダイアログ色で描画される一方、
接続管理Window本体は白い`COLOR_WINDOW`で登録されているため、
controlの矩形領域が見えている。親Windowを標準ダイアログ色へ合わせれば、
独自描画なしでWindows標準の一貫した外観になる。

現行履歴は接続先を起点に選択するため、同じ用途の接続を利用者が識別しにくい。
任意の接続名を履歴entryへ加え、選択時に一組のRDP識別情報を復元することで、
複数接続の選択を明確にする。

## 4. security boundaryと失敗時動作

- 接続名は接続先等と同じcurrent-user DPAPI保護file内だけへ保存し、
  診断log、protocol frame、Agent設定へ出力しない。
- password、資格情報、RDP画面、HTTP payloadは保存対象へ追加しない。
- 接続名の長さと埋め込みNULを保存前・復号後に検証する。
- 履歴Version、長さ、UTF-8、重複等が不正なfileはfail-closedで破棄し、
  RDP接続UIは空履歴のまま利用可能にする。
- 履歴保存失敗は成立済みRDP／Bridge sessionを切断しない。
- titleに表示する接続名は利用者が明示入力した識別情報である。
  実接続先も併記し、名前だけで接続先を誤認しないようにする。

## 5. 他境界への影響

- **ActiveX／COM**: 接続前のServer、Domain、UserName設定とCOM STA所有権は
  変更しない。接続名はActiveXへ渡さない。
- **Virtual Channel**: `ESVNBRG`、frame、HELLO、stream stateを変更しない。
- **HTTP Proxy**: loopback bind、port割当、HTTP変換を変更しない。
- **Agent Socket**: allowlist、Socket接続、Agent実装を変更しない。
- **protocol compatibility**: wire protocolのVersionとpayloadは変更しない。
  Client `0.7.3.0`と従来Agentはwire互換だが、配布物は同一Versionへそろえる。

## 6. test

- 名前付き／名前なし履歴の保存と再読込。
- Version 1暗号化履歴をVersion 2実装で読めること。
- 接続組が同じ場合と、非空接続名が同じ場合の大小文字非依存重複排除。
- 別名かつ別接続組は独立して保持されること。
- 接続名の上限、埋め込みNUL、不正長、破損fileの拒否。
- 暗号化fileに接続名、接続先、ユーザー名が平文で現れないこと。
- resource契約、Version整合、Debug／Release全CTest。
- portable Client／AgentのVersion、hash、署名状態、出力先を確認する。
- 実機では接続名の選択復元、背景、RDP接続、履歴の再起動復元を確認する。

## 7. rollback

コード、resource、文書を`0.7.2.0`へ戻す。Version 2履歴は旧版が採用しないため、
rollback時に履歴が必要なら
`%LOCALAPPDATA%\ZRdpSvnBridge\connection-history.bin`だけを退避または削除し、
名前なし履歴を再作成する。履歴は非credential情報であり、削除しても
RDP、Virtual Channel、Proxy、Agent設定には影響しない。
