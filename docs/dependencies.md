# ライセンスと依存物inventory

自作コード・文書・画像はApache-2.0で公開しています。

## repository内の自作物

| 範囲 | 出典・扱い |
|---|---|
| `src/`、`tests/`、build／監査script | 本projectの自作実装。Copyright (c) 2026 EnterSystem CO.,LTD.／Apache-2.0 |
| 自作文書とREADME | 同じ権利者の許諾に基づくApache-2.0 |
| 製品icon | AI生成・加工した独自資産。外部logo・stock assetは使用しない |
| READMEのUI画像 | 説明用の設定で実UIを撮影した自作画像 |

第三者のsourceをvendor取り込みするdependency managerや、submoduleは使用していない。
将来取り込む場合は、このinventory・LICENSE／NOTICEを更新し、元の権利表示を保持する。
外部APIの利用だけを、その実装sourceの転載と混同しない。

## runtime／リンクされるplatform component

| Component | 利用・配布境界 |
|---|---|
| Microsoft Windows x64 | Win32、COM、DPAPI、Winsock、WTS、WinHTTP、CNG、Authenticode等。利用者のWindowsにあるsystem componentを使用 |
| Remote Desktop ActiveX Control | インストール済みcontrolとtype libraryを使用。`mstsc.exe`／`mstscax.dll`／生成type library headerをrepository・packageへ同梱しない |
| MSVC標準C++／CRT・ATL | Releaseはstatic CRT、ATLのActiveX host。追加ATL／Debug runtimeを同梱しない。binaryへの静的組込みの利用・配布条件はVisual Studioの契約が適用される |

Microsoft componentをApache-2.0として再許諾しない。所有するtoolのedition・契約で
再配布条件を確認する。公式の参照先は [Visual Studio 2022再配布文書](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution) である。
本表はinventoryであり、第三者softwareの権利処理を法的に認定するものではない。

## 開発・CI tool（製品に同梱しない）

| Tool | 用途 |
|---|---|
| Visual Studio 2022 C++／ATL、Windows SDK | C++20／Windows x64 build、resource、OS type library import |
| CMake 3.31以上、CTest | configure／build／Mock回帰 |
| Python 3 | 自作の整形・コメント・repository検査script |
| clang-format 19 | 開発時整形。製品binaryへ組み込まない |
| Doxygen | 独立した追加コメント監査。製品のruntime要件ではない |
| Git、GitHub CLI | source管理と承認されたremote操作 |
| GitHub Actions／公式checkout action | 承認されたWindows build・Mock検証。外部Actionはcommit SHAへ固定 |
| Inno Setup 6.3以上 | `.iss` からSetupを生成。compiler本体は同梱しない。下記の標準installer engineは生成物に含む |

各toolのlicenseはtool提供元のものを使う。tool自体を製品ZIPへ同梱しない。
source repositoryの `LICENSE`／`NOTICE` と、本inventoryをpackageへ含める。

## Setup／アンインストーラーに含むcomponent

Inno Setupの標準engineを、生成されるSetupとアンインストーラーに使用する。
アプリと自作 `.iss` はApache-2.0だが、Inno Setup engineは提供元の
[Inno Setup License](https://jrsoftware.org/files/is/license.txt) に従う。
engineを改変せず、About表示等の元の著作権表示とwebsiteを保持する。
`NOTICE` と同梱READMEにも出典を記載する。compiler、SDK、署名toolは同梱しない。
ビルドに使うInno Setupの版を変更した場合は、その版のlicenseを再確認する。

## 設定とデータ

SVN、Mylyn、Trac、Zscalerは外部環境・利用対象であり同梱物ではない。
設定templateは文書用アドレスだけを使う。実INI・履歴・資格情報・署名鍵はGit／packageに含めない。
