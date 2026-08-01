# Phase 0 remote desktop領域の維持

日付: 2026-07-30

## 現象

ZRdp `0.1.6.0`は、画面サイズcomboで選択した値を接続前の`DesktopWidth`／`DesktopHeight`へ設定していた。この値はClientの表示枠ではなく初期remote desktopの要求値であるため、Windows標準RDPの全画面接続と異なる値を選ぶと接続先applicationのlayoutが変化した。

実機画像に含まれる接続先、ユーザー名、画面内容は分析文書、source、test、logへ転記していない。

## 公式仕様の確認

- `DesktopWidth`／`DesktopHeight`は初期remote desktopの要求寸法である。
- 値を省略または0にしても既存session寸法の維持にはならず、ActiveX controlの寸法が使用される。
- 接続後にcontrolだけをresizeした場合、remote desktop寸法は変わらず、scroll barまたは中央配置となる。
- Smart SizingはClient側表示をcontrolへ合わせて拡縮し、有効時はscroll barを表示しない。
- RDP Client Core Dataには、要求する`desktopWidth`／`desktopHeight`が含まれる。

## 修正方針

1. 接続開始時のremote desktop寸法は、ZRdpが存在する現在のmonitor全体を使用する。
2. Full HD／WQHD／Fullのremote desktop寸法選択欄を廃止する。
3. `WM_SIZE`から`UpdateSessionDisplaySettings`へ至る経路を撤去する。
4. checkboxはSmart Sizingだけを切り替え、接続先desktop領域を変更しない。
5. checkboxは既定OFFとし、OFFはscroll、ONはClient側拡縮とする。

設計判断は`docs/adr/0003-fixed-initial-rdp-desktop-and-local-scaling.md`へ記録した。

## 既知の制約

RDP接続前に既存remote sessionの現在寸法をActiveXから取得することはできない。このため、既存sessionが現在のlocal monitor全体と異なる寸法で作成されていた場合の完全な自動維持は保証できない。追加のnetwork経路や探索はプロジェクト要件に反するため使用しない。

## ローカル検証結果

- sourceの製品経路に`UpdateSessionDisplaySettings`呼出しがないことを確認した。
- Debug buildおよびCTest: 4/4成功。
- Release buildおよびCTest: 4/4成功。
- hidden UI probeで、Full HD／WQHD等の解像度選択欄がなく、Smart Sizing checkboxが既定OFFであることを確認した。
- Release version: `0.1.7.0`。
- Release size: `435200` bytes。
- SHA-256: `B7824ABCF4F6B73AF73AA295B5A08619C4EB76942D6EE1C425688D711B075549`。
- x64、High Entropy VA、ASLR、DEP、Control Flow Guardを確認した。
- dependencyはWindows system DLLだけであり、外部runtime DLLを追加していない。

## ユーザー実機確認結果

`0.1.7.0`を使用した実RDP sessionで、次の結果が期待どおりであることをユーザーが確認した。

- remote desktop寸法を変更するFull HD／WQHD等の選択欄がない。
- `リモート画面をウィンドウに合わせる`は既定OFFである。
- ZRdpのwindowを狭くしてもremote desktop寸法と接続先applicationのlayoutが維持される。
- 等倍表示で水平／垂直scroll barを使用してremote desktop全体を操作できる。

実機画像に含まれる接続先、ユーザー名、画面内容、window寸法は記録していない。

Smart SizingをONにした場合の表示倍率、clipboard、再接続等は別のPhase 0 Gate項目として未確認のまま残す。RDP画面接続だけではSVN Bridgeの完了とは判定しない。

## References

- https://learn.microsoft.com/en-us/windows/win32/termserv/imstscax-desktopwidth
- https://learn.microsoft.com/en-us/windows/win32/termserv/imstscax-desktopheight
- https://learn.microsoft.com/en-us/windows/win32/termserv/imsrdpclientadvancedsettings-smartsizing
- https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/00f1da4a-ee9c-421a-852f-c19f92343d73
