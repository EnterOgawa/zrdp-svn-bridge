# ADR 0003: 初期RDP desktopの固定とClient側拡縮

- Status: Superseded by ADR 0004
- Date: 2026-07-30

## Context

RDP ActiveXの`DesktopWidth`／`DesktopHeight`は、Client側controlの表示寸法ではなく、初期remote desktopとして接続先へ要求する寸法である。ZRdp `0.1.6.0`でFull HDを既定値にした結果、Windows標準RDPの全画面接続と異なるremote desktop寸法となり、接続先applicationのlayoutが変化した。

MicrosoftのRDP protocol仕様でも、Client Core Dataに`desktopWidth`と`desktopHeight`が要求値として含まれる。ActiveXで値を省略または0にしても既存session寸法は維持されず、controlの幅と高さが使用される。接続前にRDP以外の経路を追加せず、既存remote sessionの現在寸法を取得するActiveX APIもない。

## Decision

- 初期remote desktop寸法は、接続開始時にZRdpが存在する現在のmonitor全体の解像度とする。
- Full HD／WQHD／Fullを選択してremote desktop寸法を変更するUIは廃止する。
- ZRdpのwindow変更ではActiveX host windowだけを変更し、`UpdateSessionDisplaySettings`または`Reconnect`を呼ばない。
- `Smart Sizingモード（ウィンドウに合わせる）`は、`IMsRdpClientAdvancedSettings::SmartSizing`だけを切り替える。
- Smart Sizingは既定OFFとし、OFFではActiveX標準のscroll bar、ONではClient側描画の拡縮を使用する。
- Smart SizingはMicrosoft仕様どおり接続中にも切替可能とする。

## Consequences

### Positive

- ZRdpのwindow size変更がremote desktop寸法と接続先application layoutへ波及しない。
- Windows標準RDPの全画面接続と同じlocal monitor baselineを使用できる。
- 表示倍率の変更でRDP再接続やVirtual Channel再確立を必要としない。
- OFFでは等倍scroll、ONでは全体表示を利用者が選択できる。

### Negative

- 接続前の既存remote session寸法がlocal monitor全体と異なる場合、初回接続時の完全な自動維持は保証できない。
- 異なる解像度のmonitorへZRdpを移動してから新規接続すると、新しいmonitor全体が初期baselineになる。
- Full HD／WQHDをremote desktop寸法として明示選択する機能は提供しない。

## Alternatives

### `DesktopWidth`／`DesktopHeight`を未指定にする

Microsoft仕様ではcontrolの幅と高さが使用され、ZRdpの初期window sizeがremote desktopへ反映されるため不採用とした。

### `Default.rdp`の寸法を読み込む

Windows標準RDPの既定値には近づくが、接続先ごとの`.rdp`設定や既存session寸法を保証できず、機密設定を含み得るfileへの依存も増えるため不採用とした。

### 接続後に`UpdateSessionDisplaySettings`で追従する

remote desktop寸法自体を変更し、今回避けるべきapplication layout変更を発生させるため不採用とした。

### 接続先寸法を手動設定する

技術的には可能だが、値がremote desktopを変更することを利用者が理解する必要がある。実機で現在のmonitor baselineが不足した場合に、target別の明示設定として再評価する。

## Validation

- sourceから`UpdateSessionDisplaySettings`呼出しとwindow resize timerがなくなっていることを確認する。
- Debug／ReleaseのbuildとCTestを実行する。
- hidden UI確認でremote desktop解像度選択欄がなく、Smart Sizingが既定OFFであることを確認する。
- 実RDP接続で、OFF時のscroll、ON時のClient側拡縮、両modeでの接続先application layout維持を確認する。

## References

- https://learn.microsoft.com/en-us/windows/win32/termserv/imstscax-desktopwidth
- https://learn.microsoft.com/en-us/windows/win32/termserv/imstscax-desktopheight
- https://learn.microsoft.com/en-us/windows/win32/termserv/imsrdpclientadvancedsettings-smartsizing
- https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/00f1da4a-ee9c-421a-852f-c19f92343d73
