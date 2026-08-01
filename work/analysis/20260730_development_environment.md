# 開発環境確認結果

確認日: 2026-07-30（Asia/Tokyo）

## 判定

- C++20／Windows x64／Visual Studio 2022 toolset／CMake を使用したコマンドラインビルドを開始できる。
- Windows SDK の WTS／Winsock／COM API と、Windows に登録済みの Remote Desktop ActiveX Control を利用できる。
- 本確認はローカル開発環境だけを対象としており、支給 PC、Zscaler、RDP 接続先、実 SVN への接続試験は実施していない。

## 確認した環境

| 項目 | 確認結果 |
|---|---|
| OS | Windows 11 Pro for Workstations x64、10.0.26200 |
| PowerShell | 7.6.4、64 bit process |
| Visual Studio toolset | Visual Studio Build Tools 2022 17.14.31 |
| MSVC | 19.44.35226、Host x64 / Target x64 |
| ATL | `Microsoft.VisualStudio.Component.VC.ATL`、14.44.35207 |
| MSBuild | 17.14.40.60911 |
| CMake | `C:\develop\tools\Microsoft` 配下の 3.31.6-msvc6 |
| Ninja | `C:\develop\tools\Microsoft` 配下の 1.12.1 |
| Windows SDK | 10.0.26100.0 |
| Git | 2.53.0.windows.3 |
| Git repository | 未初期化 |

## Windows／RDP依存関係

- `C:\Windows\System32\mstscax.dll` 10.0.26100.8875 を確認した。
- x64 COM 登録に `Microsoft RDP Client Control (redistributable) - version 13` を確認した。
- `WtsApi32.h`、x64 `Wtsapi32.lib`、`Wtsapi32.dll` を確認した。
- `ws2_32.dll`、`ole32.dll`、`oleaut32.dll` を確認した。
- Windows SDK に `mstscax.h` はないが、MSVC の `#import` でシステムの `mstscax.dll` Type Libraryを取り込めることを確認した。
- `/WX` で Type Library を取り込む場合は、重複型 `wireHWND`、`_RemotableHandle`、`__MIDL_IWinTypes_0009` を `#import` の `exclude` へ明示する必要がある。

## 実行したローカル検証

- `vcvars64.bat` からMSVC、x64 linker、Windows SDKの`rc.exe`および`mt.exe`を解決できた。
- `/std:c++20 /permissive- /Zc:__cplusplus /W4 /WX /MT` で最小プログラムをコンパイル、x64リンク、実行できた。
- WTS APIおよびOLE Automationのimport libraryを使用してリンクできた。
- 生成した試験EXEがx64であることを確認した。
- `mstscax.dll` Type Libraryを`/W4 /WX`で取り込んだ最小プログラムをコンパイル、リンク、実行できた。
- Visual Studio InstallerでATLコンポーネントを既存のBuild Toolsへ追加し、`atlbase.h`、`atlhost.h`、`atlwin.h`を確認した。
- 検証用ソース、object、EXE、生成Type Library headerは確認後に`work/tmp`から削除した。

## 留意事項

- `C:\develop\tools\Microsoft` はVisual Studio IDEではなくBuild Tools構成であり、`devenv.exe`は存在しない。CMake／MSBuildによるビルドには支障がない。
- ActiveX hostにはATLの`CAxWindow`を使用する。ATLはheader-onlyであり、製品EXEへ追加のATL runtime DLLを同梱しない。
- システムPATH上には別のCMake 4.3.1が存在する。初期構成ではVisual Studio 2022に付属するCMake 3.31.6を使用し、ビルド再現性を確保する。
- ActiveXの存在確認は接続成立を保証しない。Zscaler ONでのRDP接続成立はPhase 0のユーザー環境Gateとして別途確認する。
