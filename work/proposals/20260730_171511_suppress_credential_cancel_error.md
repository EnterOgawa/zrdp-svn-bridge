# 資格情報画面キャンセル時の切断error抑止提案

## 1. 変更理由

Microsoft標準credential UIの右上close buttonまたはCancelで認証を取り消すと、
現在のRemote Desktop ActiveXは`OnDisconnected`を通知する。実機では
disconnect reason `7943 (0x1F07)`、extended reason `0`が通知されるが、現状の
`MainWindow::QueueDisconnectError`はZRdp自身が要求した切断以外をすべて異常と
みなすため、利用者が閉じた直後に追加のRDP接続error dialogを表示する。

資格情報を入力せずに閉じる操作は利用者が完了させたキャンセルであり、ZRdpの
追加error dialogを表示せず未接続画面へ戻す。

## 2. 変更対象

- `AGENTS.md` RDP UI要件
- `src/client/rdp/rdp_control_host.h`
  - `RdpEvent`
  - credential promptキャンセル判定の公開契約
- `src/client/rdp/rdp_control_host.cpp`
  - `RdpControlHost::Impl::OnRdpEvent`
  - disconnect reason分類
- `src/client/app/main_window.cpp`
  - `MainWindow::OnRdpEvent`
  - `MainWindow::QueueDisconnectError`
- `tests/unit/rdp_host_smoke_test.cpp`
- `docs/rdp-activex.md`
- `docs/diagnostics.md`
- `docs/test-plan.md`
- `docs/roadmap.md`
- version resource、manifest、CMake project version

## 3. 判定条件

次のすべてを満たす場合だけ、credential UIの利用者キャンセルと分類する。

1. 切断直前のstateが`Connecting`または`Connected`で、`LoginComplete`前である。
2. disconnect reasonが`7943 (0x1F07)`である。
3. extended disconnect reasonが`exDiscReasonNoInfo (0)`である。
4. ZRdpの切断buttonまたは終了処理から要求した切断ではない。

`IMsTscAxEvents::OnDisconnected`のMicrosoft公開文書はreasonが切断理由を表すことと
既知code群を示すが、`7943`のsymbol名は公開一覧にない。このためreason単独で
全lifecycleを正常扱いせず、今回再現した「ログイン完了前＋拡張理由なし」へ
限定する。

## 4. UIと診断log

- credential UIキャンセル時:
  - 追加のRDP接続error dialogを表示しない。
  - 状態欄を固定の未接続表示へ戻す。
  - 接続入力と接続buttonを再び有効化する。
  - 数値reasonは診断logへInformationとして残す。
- それ以外:
  - 現在どおりMicrosoft由来説明、disconnect reason、extended reasonを
    error dialogへ表示する。

接続先、ドメイン、ユーザー名、password、画面内容は新規判定にもlogにも使用しない。

## 5. ActiveX／COMへの影響

`OnDisconnected` callbackから取得済みの所有eventと現在stateだけで分類する。
ActiveX property、credential UI、NLA、CredSSP、証明書処理は変更しない。
callback中にmodal dialogを表示せず、既存のCOM STA／遅延表示方針を維持する。

## 6. SVN Bridge／protocol compatibility

Phase 0のRDP UI表示制御だけの変更である。Virtual Channel、Application Protocol、
localhost HTTP Proxy、Agent、SVN通信には影響しない。

## 7. Security boundary

- 認証失敗、network failure、証明書error、account errorをreasonの近似値で
  正常扱いしない。
- ログイン完了後の同じ数値reasonは通常の異常切断として表示する。
- warningや証明書確認を自動承認しない。
- credentialや実接続情報を新規stateまたはlogへ保持しない。

## 8. 失敗時動作

判定条件が一つでも一致しなければ、既存の切断error表示へfail-safeに戻る。
分類処理はallocation、COM call、file I/Oを行わない。

## 9. Test

- `Connecting + 7943 + extended 0`をキャンセルと判定する。
- `Connected + 7943 + extended 0`をキャンセルと判定する。
- `LoggedIn + 7943 + extended 0`を通常切断と判定する。
- reason違い、extended reason違いを通常切断と判定する。
- Debug／Release buildと全CTestを実行する。
- 実機でcredential UIのclose後に追加dialogが出ず、再接続可能なことを確認する。

## 10. Rollback

`RdpEvent`のキャンセル分類fieldと判定functionを削除し、
`MainWindow::QueueDisconnectError`を`locally_requested_disconnect`だけの判定へ
戻す。protocol、履歴file、設定fileのmigrationは不要である。
