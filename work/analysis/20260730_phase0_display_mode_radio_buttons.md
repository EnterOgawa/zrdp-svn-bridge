# Phase 0 RDP表示modeラジオボタン

記録日: 2026-07-30（Asia/Tokyo）

## 実装結果

表示方法を次の3つの排他的なラジオボタンへ変更した。

1. `等倍表示（スクロール）`
2. `Smart Sizing（縮小表示）`
3. `動的解像度（Window追従）`

既定は等倍表示である。等倍表示とSmart Sizingは接続開始時のmonitor寸法を
remote desktop baselineとして維持する。動的解像度だけが、現在のRDP表示領域を
初期寸法に使用し、接続後に`IMsRdpClient9::UpdateSessionDisplaySettings`で
remote desktop寸法を変更する。

window resizeではActiveX hostを即時変更し、動的更新を最後の`WM_SIZE`から
250 ms後へdebounceする。全画面、切断、自動再接続中、終了処理ではtimerを
取消し、window mode復帰と自動再接続完了後に必要な場合だけ再予約する。

## 失敗時動作

- 利用者によるmode切替直後の適用失敗は、直前のmodeへrollbackする。
- resize後の動的更新失敗は、RDP接続を維持して等倍表示へ縮退する。
- Smart Sizingを暗黙fallbackとして有効化しない。
- `Reconnect`を使用しない。
- 診断logには表示mode番号とHRESULTだけを記録し、画面寸法を記録しない。

## ローカル検証

- Visual Studio 2022 x64 Debug build: 成功、warning 0。
- Visual Studio 2022 x64 Release build: 成功、warning 0。
- Debug CTest: 5/5成功。
- Release CTest: 5/5成功。
- ActiveX smoke test:
  - 既定`RdpDisplayMode::NativeScroll`。
  - Smart Sizing接続前設定。
  - 動的解像度接続前設定。
  - 未知mode拒否。
  - 未接続時のSmart Sizing／動的解像度API拒否。
- Release UI probe:
  - 3 controlとも`BS_AUTORADIOBUTTON`。
  - 起動時は等倍表示だけが選択済み。
  - Smart Sizing選択後はSmart Sizingだけが選択済み。
  - 動的解像度選択後は動的解像度だけが選択済み。
- Release EXE:
  - path: `out/ZRdpSvnBridge.exe`
  - version: `0.1.8.0`
  - size: `438784` bytes
  - SHA-256: `9CC465A79A0E8C2C267EFC6874571423EE7C71774570D99E9439667CB40D4B33`
  - x64、High Entropy VA、ASLR、DEP、Control Flow Guard: 有効。
  - runtime dependency: Windows標準DLLのみ。
  - Authenticode: 未署名。署名はユーザーがrelease前に実施する。

## 実機確認結果

ユーザーが`0.1.8.0`を実RDP接続で確認し、次が期待どおり動作した。

- Smart Sizingでremote desktop寸法を維持したまま縮小表示できること。
- 動的解像度でremote desktop寸法が最後のwindow表示領域へ追従すること。
- 3 modeを接続中に切り替えられること。
- 動的解像度で全画面から復帰した後に追従が再開すること。

全画面中は通常windowの接続先欄が見えず、connection barも固定製品名だけで
接続先を識別しにくいという追加課題が確認された。この課題は`0.1.9.0`で、
全画面connection barとwindow titleへ入力済み接続先を併記して対応する。

この確認はRDP表示のPhase 0 Gateであり、SVN Bridge完成またはSVN利用可能を
意味しない。
