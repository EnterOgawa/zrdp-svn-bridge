# Phase 0 RDP接続成立と動的解像度追従

記録日: 2026-07-30（Asia/Tokyo）

## RDP接続成立

ユーザーからversion `0.1.3.0`でRDP接続が成立したとの確認を得た。通常診断logでも、機密情報を参照せず次を確認した。

- CredSSP有効化readback成功
- authentication warningの表示とdismiss
- `OnConnected`
- `OnLoginComplete`
- 全画面への移行
- ウィンドウモードへの復帰
- ユーザー操作による正常切断
- processの正常終了

ユーザー提供の画面には実環境情報が含まれるため、画像、接続先、ユーザー名、remote application情報をrepositoryの文書やtest dataへ保存しない。

RDP接続成立はPhase 0の重要Gateであるが、clipboard、keyboard、mouse、再接続およびSVN Bridgeは未確認または未実装であり、プロジェクト完了とはしない。

## 観察された表示上の課題

接続時の固定desktop sizeと、変更後のZRdp window内RDP表示領域が一致せず、remote desktop周囲に灰色の余白が発生した。

Smart Sizingは固定remote desktopをclient領域へscaleする機能であり、remote session自体のdesktop size変更とは異なる。ウィンドウ変更へremote desktop解像度も追従させるため、`IMsRdpClient9::UpdateSessionDisplaySettings`を使用する。

## 実装方針

- `WM_SIZE`ではActiveX host windowを即時resizeする。
- 最後の`WM_SIZE`から250 ms後に一度だけsession display updateを実行する。
- 更新値はtoolbarとstatus barを除いた実際のRDP表示領域とする。
- COM呼出しはActiveXを所有するSTA UI threadで行う。
- full-screen中はMicrosoft controlへsizingを委ねる。
- full-screenからwindowへ戻った場合とRDP自動再接続後は、現在のwindow sizeを再適用する。
- resizeのたびにRDPを再接続しない。将来のVirtual Channelおよびstream stateを維持する。
- dynamic update失敗時もRDP接続は維持し、Smart Sizingをfallback表示として残す。
- logには解像度を保存せず、固定event名と`HRESULT`だけを記録する。

Microsoft RDP protocol上、physical monitor sizeは10 mm未満の値なら無視される。この実装ではphysical width／heightを未指定の`0`とし、orientationとscale factorは有効な既定値`0`／`100`を使用する。

## ローカル検証

- x64 Debug build: 成功、warning 0
- x64 Release build: 成功、warning 0
- Debug CTest: 3/3成功
- Release CTest: 3/3成功
- 未接続状態で連続`WM_SIZE` 2回と正常終了: 成功
- dynamic updateの未接続state guard: unit smoke test成功
- dynamic update診断eventの固定名出力: unit test成功
- Release EXE version: `0.1.4.0`
- Release EXE path: `out/ZRdpSvnBridge.exe`
- Release EXE size: `402432` bytes
- Release EXE SHA-256: `F452DC6C474E1DF0AD931BAEDFE17A60E5057D8837E0DA0D59EDBE59D5DA113A`
- x64、High Entropy VA、ASLR、DEP、CFG: 有効
- runtime dependency: Windows標準DLLのみ

接続済みsessionに対するdynamic display updateは、更新版をユーザー実機で確認する。
