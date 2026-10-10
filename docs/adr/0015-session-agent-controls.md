# セッション画面のAgent配置と確認付き起動

日付: 2026-10-09

固定Enterの入力方式は2026-10-10の[SendInput方式](0017-sendinput-agent-controls.md)で更新する。
Explorer選択確認・認証世代・実Agent応答の境界は維持する。

状態: 実装済み。実RDPでの配置・起動確認は利用者による実機Gate。
配置入口は後続の[自動初期配置](0016-initial-agent-bootstrap.md)により`Agent配置・起動`へ変更する。以下のExplorer選択確認は従来の`Agentを起動`操作に引き続き適用する。

## 決定

利用者が接続中の画面から操作を見つけられるよう、セッションの一行操作欄に
`Agentを配置`と`Agentを起動`を追加する。全画面中は操作欄を隠す。
接続入力と状態欄は重複させない。通常Windowの最小幅は96 DPI基準で1140とする。

配置入口は再利用しないsession IDと認証世代を最大一件queueでmanager STAへ渡す。
既存Agent設定画面を利用し、準備開始時にも同じ対象か検証する。
終了した対象や再接続後の対象を別sessionへ付け替えない。
未保存画面が別対象で開いている場合は、その画面を閉じてから開き直す。
共有clipboardのため、実際の配置時はRDP一件制約を維持する。

起動はsession所有STAのmodeless確認画面で行う。利用者が接続先Explorerで
`%LOCALAPPDATA%\ZRdp\ZRdpSvnAgent.exe`の配置・署名・INI・転送完了・未起動を確認し、
固定EXEだけを選択する。checkboxを選択後の明示button操作で、同じ認証世代の
ActiveXへ固定Enterのdown/upだけを送る。任意command、Win+R自動入力、remote shell、
clipboard書換え、service登録、自動起動、自動再起動は追加しない。

Agentが存在しない段階でもRDPの標準入力を利用できる。一方、Clientからremoteの
選択・フォーカス・ファイル内容は自動確認できないため、利用者による確認を必須とする。
キー要求の成功を起動成功と表示しない。既存HELLO_ACKだけをAgent接続確認に使う。
切断・再認証・二重要求は拒否し、入力失敗後の自動再送は行わない。
確認画面の終了はAgentの停止を意味しない。

## 互換性と確認

Protocol、SVC名、HTTP Proxy、allowlist、Agent Socketは変更しない。
ActiveXの所有STA境界と既存のbounded配置queueを維持する。
[Microsoft SendKeys](https://learn.microsoft.com/en-us/windows/win32/termserv/imsrdpclientnonscriptable-sendkeys)
のscan-code入力を使い、API不在時は別経路へfallbackしない。

Debug／Release各45 CTestを通過した。fake入力で確認なし、二重要求、失効、
応答前の成功表示禁止を検証した。実Windows ActiveXでは未認証・非所有STAの拒否を確認した。
自動接続messageを除去しProxyも停止した実session UIで、最小幅での表示と無効化を確認した。
実履歴・設定・clipboardを使わないfixtureで二画面の96 DPI表示を確認した。
実RDPのExplorer選択、起動警告、GPOによる入力制限、Agent応答は利用者の実機Gateとする。

Rollbackは二つのsession入口と起動確認controller、manager routingを取り除く。
既存配置、Agent設定、更新機能は維持し、remote fileや起動設定は変更しない。
