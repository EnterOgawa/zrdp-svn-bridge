# Smart Sizing設定の明示表示

> Status: Superseded by `20260730_161054_display_mode_radio_buttons.md`
>
> 利用者から、Smart Sizingとremote desktop解像度追従は別機能であり、
> 3つの排他的な表示modeとして選択する仕様が明示されたため、本提案は実装しない。

## 現象

Microsoft ActiveXのSmart Sizing処理と切替checkboxは実装に残っているが、利用者向け表示が`リモート画面をウィンドウに合わせる`だけであり、Smart Sizing設定として識別できない。

## 変更対象

- `src/client/app/resource.h`
  - resource IDとcontrol IDをSmart Sizingの名称へ統一する。
- `src/client/app/ZRdpSvnBridge.rc`
  - checkboxを`Smart Sizingモード（ウィンドウに合わせる）`として明示する。
  - version resourceを`0.1.8.0`へ更新する。
- `src/client/app/main_window.h`
- `src/client/app/main_window.cpp`
  - 内部member、command、状態取得methodをSmart Sizingの名称へ統一する。
  - 長い表示名が欠けないようcheckbox幅を調整する。
- `tests/unit/resource_contract_test.cpp`
- `tests/CMakeLists.txt`
  - 配布EXEにSmart Sizing設定文字列が含まれることを検証する。
- `CMakeLists.txt`およびPhase 0文書
  - 対象versionと利用者向け表示名を更新する。

## 動作

- checkboxは従来どおり既定OFFとする。
- OFFではremote desktop寸法を維持し、ActiveX標準scroll barを使用する。
- ONではMicrosoft ActiveXの`SmartSizing`だけを切り替え、Client側描画をwindowへ合わせる。
- `DesktopWidth`、`DesktopHeight`、`UpdateSessionDisplaySettings`、`Reconnect`はwindow resize経路から呼び出さない。
- 接続中のON／OFF切替を維持する。

## Security boundary

- RDP credential、接続先、ユーザー名、clipboard、Virtual Channel payloadの扱いは変更しない。
- NLA、CredSSP、認証warning、clipboard redirectionの設定は変更しない。
- 新しいnetwork通信、listen port、Proxy、routing処理を追加しない。
- logは固定eventと数値fieldだけを維持し、設定名以外の任意文字列を追加しない。

## 失敗時動作

ActiveXへのSmart Sizing反映に失敗した場合は、checkboxを変更前の状態へ戻してHRESULTを表示する。RDP接続とremote desktop寸法は維持する。

## Protocol compatibility

RDP Virtual Channelおよび将来のApplication Protocolには影響しない。wire formatの変更はない。

## Test

- x64 Debug／Release build。
- Debug／Release CTest。
- 配布EXEのstring resourceに`Smart Sizing`が明示されていること。
- checkboxの既定OFFと、未接続時の接続前設定適用。
- product sourceに`UpdateSessionDisplaySettings`呼出しがないこと。
- 実RDP sessionでON／OFFと接続先application layout維持をユーザー確認する。

## Rollback

resource文字列、内部identifier、表示幅、version、追加testを変更前へ戻す。保存format、履歴、RDP設定modelには変更がないためdata migrationは不要である。
