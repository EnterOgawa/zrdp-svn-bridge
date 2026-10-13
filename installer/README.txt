ZRdp SVN Bridge

はじめに
  ZRdpSvnBridge.exe（Client）を起動してください。
  接続先・ドメイン・ユーザー名を指定し、「Agent設定」で利用が許可された
  HTTP方式のSVN／TracサーバーのIP・ポート・用途を登録します。
  RDP認証後、Agentは接続先のユーザーセッションへ自動配置・起動されます。
  Agent設定が空の接続ではAgentを起動しません。

  Agentの配置には、RDPのドライブ転送とクリップボード転送が必要です。
  自動起動中はRDP画面を前面にし、入力や画面の切り替えを控えてください。
  切り替えと重なると手元の別画面へ貼り付け・実行されるリスクがあり、
  クリップボードの内容も置き換わります。Windowsの警告はご自身で確認してください。

SVN／Tracの設定
  既存URLは変更せず、対象のProxyを127.0.0.1と、RDPタイトルに表示された
  ポートへ向けます。既定の最初の接続は18080です。
  登録したIPv4アドレスとポートに一致するHTTP接続だけを扱います。
  Windowsや組織の実行・通信ルールに従って利用してください。

更新・アンインストール
  インストーラーを使用するときは、ClientとすべてのRDP接続を終了してください。
  ClientとAgentは同じバージョンのセットで使用します。
  アンインストール時に「接続履歴・Agent設定も削除する」を選べます。
  既定は保持です。選択すると、このWindowsユーザーの全接続分を削除します。
  ログ・既存バックアップ・手動で置いたINIは、選択にかかわらず保持します。
  接続先PCに配置済みのAgentは、開発PC側のアンインストールでは削除しません。

詳しい手順と不具合報告
  https://github.com/EnterOgawa/zrdp-svn-bridge
  実接続先・認証情報・実環境の画面画像・ダンプを公開Issueへ載せないでください。

ライセンス
  自作コード・文書・画像はApache License 2.0です。LICENSEとNOTICEをご覧ください。
  インストーラーにはInno Setupの標準エンジンを使用しています。
  Copyright (C) 1997-2026 Jordan Russell. All rights reserved.
  Portions Copyright (C) 2000-2026 Martijn Laan. All rights reserved.
  https://jrsoftware.org/
  Windows等の依存物と利用条件はDEPENDENCIES.mdをご覧ください。
