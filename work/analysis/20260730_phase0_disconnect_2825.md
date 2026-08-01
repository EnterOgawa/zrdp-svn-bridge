# Phase 0 disconnect reason 2825の調査と修正

記録日: 2026-07-30（Asia/Tokyo）

## 実機ログから判明した事象

ユーザー実機で「接続」を押した処理は無反応ではなく、次の段階まで進んだ後、約30 msで切断していた。

```text
ui.connect_button_clicked
rdp.connection_settings_apply_returned hresult=0x00000000
rdp.connect_call_returned hresult=0x00000000
rdp.event.connecting
rdp.event.disconnected code=2825
```

`Connect()`の呼出し自体は成功しているため、button event、設定適用、ActiveXの非同期接続開始は成立していた。表示が短時間で「接続中」から「切断」へ戻ったため、見た目では変化を認識しにくい状態だった。

## 原因

インストール済みのMicrosoft RDP ActiveX Controlへ`GetErrorDescription(2825, 0)`を問い合わせた結果、接続先はNetwork Level Authenticationを必要としているが、クライアントでサポートされていないという説明が返った。

実装では認証levelと標準credential promptは設定していたが、`IMsRdpClientAdvancedSettings6::EnableCredSspSupport`を明示的に有効化していなかった。

## 修正

- 接続前に`EnableCredSspSupport=VARIANT_TRUE`を設定する。
- 設定後に値を読み戻し、無効のままならfail-closedで接続を開始しない。
- `rdp.credssp_configuration_returned`へ`HRESULT`と`enabled`だけを記録する。
- `OnDisconnected`で`ExtendedDisconnectReason`と`GetErrorDescription`を取得する。
- Windows由来の説明文はlogへ保存せず、予期しない切断時のUIにだけ表示する。
- modal dialogはActiveX callback内で直接表示せず、Window Messageでcallback復帰後へ遅延する。
- file／product versionを`0.1.2.0`へ更新する。

この変更はNLA、CredSSP、server authenticationを弱めるものではない。Windows標準RDP ControlのCredSSP経路を有効にし、標準credential UIを引き続き使用する。

## ローカル検証

- x64 Debug build: 成功、warning 0
- x64 Release build: 成功、warning 0
- Debug CTest: 3/3成功
- Release CTest: 3/3成功
- ActiveX設定適用smoke test: 成功
- CredSSP有効化後のreadback: `VARIANT_TRUE`
- Release EXE起動、main window作成、通常終了: 成功、exit code `0`
- Release EXE version: `0.1.2.0`
- Release EXE path: `out/ZRdpSvnBridge.exe`
- Release EXE size: `400384` bytes
- Release EXE SHA-256: `5D38F748D373FE33F9436D330F5B58D1011A9396E802BB5FD77DB3393CE0A6AA`
- x64、High Entropy VA、ASLR、DEP、CFG: 有効
- runtime dependency: Windows標準DLLのみ

実接続先でNLA credential UIとRDP sessionが成立するかは、更新版をユーザー実機で再確認する。
