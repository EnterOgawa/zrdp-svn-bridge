# Phase 9改善 I-004: RDP音声リダイレクト設定

## 1. 目的

接続管理 Window で、RDP接続開始前に次のローカルリソース設定を選択できるようにする。

- クライアントPCの既定マイクを接続先RDP sessionへ転送する。
- 接続先の音声を「このPCで再生」「接続先PCで再生」「再生しない」から選択する。

これらは接続時に確定する設定とし、確立済みsession Windowへ途中反映しない。

## 2. 変更対象

- `src/client/app/connection_manager_window.{h,cpp}`
  - マイク転送check boxと音声再生先combo boxを追加する。
  - 名前付き接続を選択した際に、対応する音声設定も復元する。
- `src/client/app/session_launch_settings.h`
  - 接続管理 Windowからsession Windowへ音声設定を所有値として渡す。
- `src/client/app/connection_history.{h,cpp}`
  - DPAPI保護履歴をVersion 3へ更新し、音声設定を接続profileへ追加する。
  - Version 1／2は、マイクOFF、音声をこのPCで再生する既定値で読み込む。
- `src/client/app/{client_application,main_window}.{h,cpp}`
  - 認証成功時の履歴通知と接続前設定へ音声設定を渡す。
- `src/client/rdp/rdp_control_host.{h,cpp}`
  - `IMsRdpClientAdvancedSettings5::AudioRedirectionMode`と
    `IMsRdpClientAdvancedSettings7::AudioCaptureRedirectionMode`を接続前に設定し、
    readbackで完全一致を確認する。
- `src/client/app/{resource.h,ZRdpSvnBridge.rc}`
  - 日本語UI文字列とcontrol IDを追加する。
- `tests/unit/*`
  - ActiveX接続前設定、履歴後方互換、resource、portable versionを回帰確認する。
- `AGENTS.md`、既存の関連設計・試験文書
  - I-004の接続時設定と再接続境界を記録する。

## 3. 設計

```diff
 struct RdpConnectionSettings {
+    bool redirect_microphone{false};
+    RdpAudioPlaybackMode audio_playback_mode{
+        RdpAudioPlaybackMode::PlayOnClient};
 };
```

音声再生先はC++のenumで表し、ActiveX固有の数値をUIと履歴へ散在させない。
履歴Version 3では各entryの固定設定値を32-bit整数で保存し、未知値を
fail-closedで拒否する。Version 1／2の意味は変更しない。

## 4. Security boundaryと失敗時動作

- マイク転送はprivacy上の明示設定であるため既定OFFとする。
- password、音声data、device名は履歴またはlogへ保存しない。
- ActiveX interface欠落、setter失敗、readback不一致時は対象sessionの
  `Connect()`を開始しない。
- GPOや接続先側が転送を禁止する場合に、policy変更や別経路へのfallbackを行わない。
- Bridge、Proxy、Agent、wire protocol、allowlistは変更しない。

## 5. Test観点

- マイクON／OFFと3種類の音声再生先を未接続ActiveXへ設定できる。
- 未知の音声再生先enumを接続前に拒否する。
- Version 3履歴で設定を保存・復元し、同名profile選択時に復元できる。
- Version 1／2履歴は安全な既定値で読み込める。
- 壊れたVersion 3設定値を拒否する。
- UI文字列、x64 portable EXE、version resource、既存全CTestを確認する。

## 6. Rollback

本変更のsource、resource、test、文書を一括して戻す。Version 3履歴を旧Clientは
読めないため、rollback時は
`%LOCALAPPDATA%\ZRdpSvnBridge\connection-history.bin`を退避または削除し、
次の認証成功時に旧形式で再作成する。RDP／Bridge／Agentの通信設定変更は不要である。
