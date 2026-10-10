# README画面画像

このフォルダのPNGは、2026-10-11に現行の自作Win32 UIを隔離したfixtureで表示し、
fixtureプロセス自身のウィンドウだけを撮影したものです。実際のRDP接続やHTTP通信は行っていません。
既存の利用者履歴・Agent設定・資格情報・接続先画面を読み込んでいません。

| 画像 | 内容・作成条件 |
|---|---|
| `connection-manager.png` | production接続マネージャーの一時copyに、隔離した空の履歴pathを渡して表示。接続処理はfake observer。接続先は `192.0.2.1`、ドメインは `EXAMPLE`、ユーザーは `demo-user` |
| `agent-settings.png` | `zrdp_agent_settings_window_test --preview` のfake backend。設定名は `UI Fixture`、宛先は文書用アドレス |
| `agent-tray.png` | `zrdp_agent_setup_test --preview-tray` の自作tray controller。表示状態はfixtureから与えたもので、通信成立の証拠ではない |

画像のレイアウトと文字列はアプリのUIを使用しています。Windowsテーマ、DPI、
production／test実行ファイルのmanifestで外観は変わります。接続成功の画面として扱わないでください。
本リポジトリの画像は、実環境のスクリーンショットに置き換えないでください。

自作UI・製品アイコンは本リポジトリ由来です。OSが描画する標準ウィンドウ枠・コントロールも含みます。
アイコンの出典は [アイコン資産のREADME](../../assets/icons/README.md) を参照してください。
