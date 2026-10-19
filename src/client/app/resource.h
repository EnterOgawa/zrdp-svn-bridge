//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file resource.h
 * @brief ZRdp ClientのWin32 resource IDとchild control IDを定義する。
 * @version 01.00.23
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details IDはresource script、接続管理Window、session Windowで共有する。
 *          接続情報や実環境値をresourceへ埋め込まない。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/07  Clientの同意付き更新と設定の分離を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/08  Agent設定画面とINI入出力のresourceを追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/08  確認付きAgent配置の境界と操作を追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/09  session画面のAgent配置・確認付き起動を追加。
 * Ver.01.00.05   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.06   Shunji Ogawa  2026/10/09  初期配置の停止段階とHRESULT診断を追加。
 * Ver.01.00.07   Shunji Ogawa  2026/10/09  自動入力のfocusとWindowsキー転送を確認。
 * Ver.01.00.08   Shunji Ogawa  2026/10/09  配置から独立した開発用Win+R testを追加。
 * Ver.01.00.09   Shunji Ogawa  2026/10/10  三種類の入力test menuと個別確認を追加。
 * Ver.01.00.10   Shunji Ogawa  2026/10/10  RのSendInput比較と誤入力リスクのresourceを追加。
 * Ver.01.00.11   Shunji Ogawa  2026/10/10  固定Win+RのSendInput比較と停止境界を追加。
 * Ver.01.00.12   Shunji Ogawa  2026/10/10  Agent固定入力をSendInputへ切替え停止境界を維持。
 * Ver.01.00.13   Shunji Ogawa  2026/10/10  配置なしの固定Ctrl+V／Enter診断と失効確認を追加。
 * Ver.01.00.14   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.15   Shunji Ogawa  2026/10/10  起動と設定の分離、tray常駐、backup付き更新を追加。
 * Ver.01.00.16   Shunji Ogawa  2026/10/10  トレイ終了後の同じRDP内Agent再起動を修正。
 * Ver.01.00.17   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * Ver.01.00.18   Shunji Ogawa  2026/10/10  接続別設定と自動Agent管理・診断削除を反映。
 * Ver.01.00.19   Shunji Ogawa  2026/10/10  Agent設定の登録・削除と廃止IDを明記。
 * Ver.01.00.20   Shunji Ogawa  2026/10/10  managerメニューを撤廃し自動更新確認へ統一。
 * Ver.01.00.21   Shunji Ogawa  2026/10/10  認証後のAgent要設定表示を追加。
 * Ver.01.00.22   Shunji Ogawa  2026/10/10  接続変更と登録済み宛先だけの表示・起動を統一。
 * Ver.01.00.23   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

/// 接続managerの接続別Agent設定button。
#define IDC_MANAGER_AGENT_SETTINGS 1020
/// 接続managerのAgent設定button文字列。
#define IDS_MANAGER_AGENT_SETTINGS 359

#include "common/resource_ids.h"

// -----------------------------------------------------------------------------
// RDP接続操作と接続状態の文字列
// -----------------------------------------------------------------------------

/// 通常Windowの製品title。
#define IDS_APP_TITLE 100
/// RDP接続先label。
#define IDS_SERVER_LABEL 101
/// RDP user name label。
#define IDS_USER_NAME_LABEL 102
/// 任意domain label。
#define IDS_DOMAIN_LABEL 103
/// 接続button。
#define IDS_CONNECT 104
/// 切断button。
#define IDS_DISCONNECT 105
/// セッション欄の全画面切替button。
#define IDS_FULL_SCREEN 106
/// 同buttonのWindow mode復帰表示。
#define IDS_WINDOW_MODE 107
/// 等倍表示radio。
#define IDS_DISPLAY_MODE_NATIVE 108
/// 未接続状態。
#define IDS_STATUS_DISCONNECTED 109
/// 接続中状態。
#define IDS_STATUS_CONNECTING 110
/// 接続済み状態。
#define IDS_STATUS_CONNECTED 111
/// 切断中状態。
#define IDS_STATUS_DISCONNECTING 112
/// 自動再接続中状態。
#define IDS_STATUS_RECONNECTING 113
/// 認証warning状態。
#define IDS_STATUS_AUTH_WARNING 114
/// ActiveX warning code表示。
#define IDS_STATUS_WARNING_FORMAT 115
/// ActiveX fatal code表示。
#define IDS_STATUS_FATAL_FORMAT 116
/// 切断reason表示。
#define IDS_STATUS_DISCONNECTED_FORMAT 117
/// 接続先必須error。
#define IDS_SERVER_REQUIRED 118
/// ActiveX生成error。
#define IDS_ACTIVE_X_CREATE_FAILED 119
/// 接続開始error。
#define IDS_CONNECT_FAILED 120
/// 切断要求error。
#define IDS_DISCONNECT_FAILED 121
/// 接続中Window終了確認本文。
#define IDS_CLOSE_CONFIRM 122
/// Window終了確認title。
#define IDS_CLOSE_CONFIRM_TITLE 123
/// 未分類例外表示。
#define IDS_UNEXPECTED_ERROR 124
/// SVC受信状態。
#define IDS_STATUS_CHANNEL_DATA 125
/// ActiveX version初期表示。
#define IDS_STATUS_INITIAL_FORMAT 126
/// 接続要求受付状態。
#define IDS_STATUS_CONNECT_REQUESTED 127
/// 診断log生成warning。
#define IDS_LOG_CREATE_FAILED 128
/// Microsoft切断詳細表示。
#define IDS_DISCONNECT_ERROR_FORMAT 129
/// 切断詳細取得不能表示。
#define IDS_DISCONNECT_REASON_UNAVAILABLE 130
/// RDP error dialog title。
#define IDS_RDP_ERROR_TITLE 131
/// 表示mode変更error。
#define IDS_DISPLAY_SETTING_FAILED 132
/// 表示mode群label。
#define IDS_DISPLAY_MODE_LABEL 133
/// Smart Sizing radio。
#define IDS_DISPLAY_MODE_SMART_SIZING 134
/// 動的解像度radio。
#define IDS_DISPLAY_MODE_DYNAMIC_RESOLUTION 135
/// domain／user入力error。
#define IDS_RDP_IDENTITY_INVALID 136

// -----------------------------------------------------------------------------
// Static Virtual Channel／Proxy状態の文字列
// -----------------------------------------------------------------------------

/// Bridge状態群label。
#define IDS_BRIDGE_LABEL 137
/// 手動Echo test button。
#define IDS_BRIDGE_ECHO_TEST 138
/// Bridge停止状態。
#define IDS_BRIDGE_STOPPED 139
/// HELLO_ACK待ち状態。
#define IDS_BRIDGE_WAITING_AGENT 140
/// Agent利用可能状態。
#define IDS_BRIDGE_READY 141
/// Echo進行状況。
#define IDS_BRIDGE_ECHO_RUNNING_FORMAT 142
/// Echo合格結果。
#define IDS_BRIDGE_ECHO_PASSED_FORMAT 143
/// Bridge固定失敗分類。
#define IDS_BRIDGE_FAILED_FORMAT 144
/// protocol非互換状態。
#define IDS_BRIDGE_INCOMPATIBLE 145
/// listener起動中状態。
#define IDS_PROXY_STARTING 146
/// loopback listen状態。
#define IDS_PROXY_LISTENING_FORMAT 147
/// Bridge利用可能Proxy状態。
#define IDS_PROXY_READY_FORMAT 148
/// Proxy固定失敗分類。
#define IDS_PROXY_FAILED_FORMAT 149
/// Proxy停止状態。
#define IDS_PROXY_STOPPED 150

// -----------------------------------------------------------------------------
// 接続管理Windowとsession titleの文字列
// -----------------------------------------------------------------------------

/// manager Window title。
#define IDS_CONNECTION_MANAGER_TITLE 151
/// 新しい接続button。
#define IDS_CONNECT_NEW_SESSION 152
/// session数と次port表示。
#define IDS_MANAGER_STATUS_FORMAT 153
/// Proxy slot枯渇表示。
#define IDS_MANAGER_NO_PORT_FORMAT 154
/// local設定error表示。
#define IDS_MANAGER_PROXY_CONFIG_ERROR_FORMAT 155
/// 最大session数到達error。
#define IDS_SESSION_LIMIT_REACHED 156
/// port slot利用不能error。
#define IDS_SESSION_PORT_UNAVAILABLE 157
/// session Window生成error。
#define IDS_SESSION_CREATE_FAILED 158
/// session資源不足error。
#define IDS_SESSION_RESOURCE_EXHAUSTED 159
/// 通常session title書式。
#define IDS_SESSION_TITLE_FORMAT 160
/// Proxy異常session title書式。
#define IDS_SESSION_TITLE_CONFIG_ERROR_FORMAT 161
/// title内RDP未接続。
#define IDS_TITLE_RDP_NOT_CONNECTED 162
/// title内RDP接続中。
#define IDS_TITLE_RDP_CONNECTING 163
/// title内RDP接続済み。
#define IDS_TITLE_RDP_CONNECTED 164
/// title内RDP再接続中。
#define IDS_TITLE_RDP_RECONNECTING 165
/// title内RDP切断中。
#define IDS_TITLE_RDP_DISCONNECTING 166
/// title内RDP失敗。
#define IDS_TITLE_RDP_FAILED 167
/// title内Agent停止。
#define IDS_TITLE_AGENT_STOPPED 168
/// title内Agent待機。
#define IDS_TITLE_AGENT_WAITING 169
/// title内Agent接続済み。
#define IDS_TITLE_AGENT_CONNECTED 170
/// title内Echo実行中。
#define IDS_TITLE_AGENT_ECHO_RUNNING 171
/// title内Agent失敗。
#define IDS_TITLE_AGENT_FAILED 172
/// title内Agent非互換。
#define IDS_TITLE_AGENT_INCOMPATIBLE 173
/// 認証済み接続でAgent設定が必要な非modal状態。
#define IDS_TITLE_AGENT_NEEDS_SETTINGS 201
/// title内Proxy停止。
#define IDS_TITLE_PROXY_STOPPED 174
/// title内Proxy起動中。
#define IDS_TITLE_PROXY_STARTING 175
/// title内Proxy listen中。
#define IDS_TITLE_PROXY_LISTENING 176
/// title内Proxy利用可能。
#define IDS_TITLE_PROXY_READY 177
/// title内Proxy失敗。
#define IDS_TITLE_PROXY_FAILED 178

// -----------------------------------------------------------------------------
// 接続profileと音声redirect設定の文字列
// -----------------------------------------------------------------------------

/// 任意接続名label。
#define IDS_CONNECTION_NAME_LABEL 179
/// local resource設定群label。
#define IDS_LOCAL_RESOURCES_LABEL 180
/// microphone redirect check。
#define IDS_REDIRECT_MICROPHONE 181
/// remote audio再生先label。
#define IDS_REMOTE_AUDIO_LABEL 182
/// Client側再生選択。
#define IDS_AUDIO_PLAY_ON_CLIENT 183
/// 接続先側再生選択。
#define IDS_AUDIO_PLAY_ON_REMOTE 184
/// 音声を再生しない選択。
#define IDS_AUDIO_DO_NOT_PLAY 185
/// 音声設定不正error。
#define IDS_AUDIO_SETTING_INVALID 186
/// 一時的なログイン前失敗の再試行待機。
#define IDS_STATUS_PRE_LOGIN_RETRY 187

// -----------------------------------------------------------------------------
// 同意付きClient更新の通知と固定失敗分類
// -----------------------------------------------------------------------------

/// 数値versionだけを表示する更新同意dialog。
#define IDS_UPDATE_OFFER_FORMAT 188
/// manifest確認中の短い状態。
#define IDS_UPDATE_CHECKING 189
/// 同意後の取得・検証状態。
#define IDS_UPDATE_DOWNLOADING 190
/// 全RDP session終了待ち。
#define IDS_UPDATE_WAITING 191
/// helper受付待ちの適用状態。
#define IDS_UPDATE_APPLYING 192
/// 手動確認で同版／旧版しかない場合の通知。
#define IDS_UPDATE_LATEST 193
/// 更新設定を無効にした場合の手動確認通知。
#define IDS_UPDATE_DISABLED 194
/// 一request制限へ到達した手動確認通知。
#define IDS_UPDATE_BUSY 195
/// 外部textを含まない更新失敗の数値format。
#define IDS_UPDATE_FAILURE_FORMAT 196
/// Windows署名・同一publisher要件の失敗format。
#define IDS_UPDATE_SIGNATURE_FAILURE_FORMAT 197
/// 適用中の新規session開始拒否。
#define IDS_UPDATE_CONNECT_BLOCKED 198
/// rollback後の旧版再起動通知。
#define IDS_UPDATE_RECOVERED 199
/// 親終了後のhelper失敗通知。
#define IDS_UPDATE_HELPER_FAILED 200

// -----------------------------------------------------------------------------
// Agent設定の固定表示。既存string blockと重複しない256以降を使用する
// -----------------------------------------------------------------------------

/// Agent設定画面の正式title。
#define IDS_AGENT_SETTINGS_TITLE 256
/// 名前未指定で使う設定名。
#define IDS_AGENT_SETTINGS_DEFAULT_NAME 257
/// 接続先一覧のIPv4列。
#define IDS_AGENT_SETTINGS_COL_IP 258
/// 接続先一覧のHTTP port列。
#define IDS_AGENT_SETTINGS_COL_PORT 259
/// 接続先一覧の用途列。
#define IDS_AGENT_SETTINGS_COL_PURPOSE 260
/// 接続先一覧の承認列。
#define IDS_AGENT_SETTINGS_COL_APPROVAL 261
/// HTTP SVN用途表示。
#define IDS_AGENT_SETTINGS_SVN 262
/// HTTP Trac用途表示。
#define IDS_AGENT_SETTINGS_TRAC 263
/// 明示承認済みの表示。
#define IDS_AGENT_SETTINGS_APPROVED 264
/// INIからは推定しない承認状態。
#define IDS_AGENT_SETTINGS_UNAPPROVED 265
/// worker処理中の固定状態。
#define IDS_AGENT_SETTINGS_BUSY 266
/// 暗号化設定を読み込んだ状態。
#define IDS_AGENT_SETTINGS_LOADED 267
/// current-user保存成功。
#define IDS_AGENT_SETTINGS_SAVED 268
/// Agent互換INIの出力成功。
#define IDS_AGENT_SETTINGS_EXPORTED 269
/// 既存INIを未承認draftへ読んだ状態。
#define IDS_AGENT_SETTINGS_IMPORTED 270
/// 入力内容を含まない失敗format。
#define IDS_AGENT_SETTINGS_ERROR_FORMAT 271
/// 設定名ごとの削除確認。
#define IDS_AGENT_SETTINGS_DELETE_CONFIRM 272
/// 未保存編集の破棄確認。
#define IDS_AGENT_SETTINGS_DISCARD_CONFIRM 273
/// 標準file読込みdialogのtitle。
#define IDS_AGENT_SETTINGS_IMPORT_TITLE 274
/// 標準file出力dialogのtitle。
#define IDS_AGENT_SETTINGS_EXPORT_TITLE 275
/// 全entryの承認がない場合の固定説明。
#define IDS_AGENT_SETTINGS_APPROVAL_REQUIRED 276
/// 接続先欄の未反映編集を明示する説明。
#define IDS_AGENT_SETTINGS_APPLY_FIELDS 277
/// 設定名入力を求める説明。
#define IDS_AGENT_SETTINGS_NAME_REQUIRED 278
/// 未選択entryの編集を拒否する説明。
#define IDS_AGENT_SETTINGS_SELECT_ENTRY 279
/// 適用中更新との競合を禁止する説明。
#define IDS_AGENT_SETTINGS_UPDATE_BLOCKED 280
/// 一件queue使用中の操作を禁止する説明。
#define IDS_AGENT_SETTINGS_QUEUE_BUSY 281
/// 設定画面の生成失敗だけを示す固定説明。
#define IDS_AGENT_SETTINGS_CREATE_FAILED 282
/// 補助dialog自体を生成できない場合の状態欄表示。
#define IDS_AGENT_SETTINGS_FAILURE 283
/// 既存INIから用途を推定しない状態の表示。
#define IDS_AGENT_SETTINGS_UNSPECIFIED 284

// -----------------------------------------------------------------------------
// command routingと状態更新に使用するchild control ID
// -----------------------------------------------------------------------------

/// 接続先combo。
#define IDC_SERVER 1001
/// user name combo。
#define IDC_USER_NAME 1002
/// 等倍表示radio。
#define IDC_DISPLAY_MODE_NATIVE 1003
/// 接続command。
#define IDC_CONNECT 1004
/// 切断command。
#define IDC_DISCONNECT 1005
/// 全画面command。
#define IDC_FULL_SCREEN 1006
/// Smart Sizing radio。
#define IDC_DISPLAY_MODE_SMART_SIZING 1007
/// RDP／manager状態欄。
#define IDC_STATUS 1008
/// 動的解像度radio。
#define IDC_DISPLAY_MODE_DYNAMIC_RESOLUTION 1009
/// domain combo。
#define IDC_DOMAIN 1010
/// Echo test command。
#define IDC_BRIDGE_ECHO_TEST 1011
/// Bridge状態欄。
#define IDC_BRIDGE_STATE 1012
/// 任意接続名combo。
#define IDC_CONNECTION_NAME 1013
/// microphone redirect check。
#define IDC_REDIRECT_MICROPHONE 1014
/// audio再生先combo。
#define IDC_AUDIO_PLAYBACK_MODE 1015

// -----------------------------------------------------------------------------
// 実行時manifest
// -----------------------------------------------------------------------------

/// Client実行level／互換性manifest。
#define IDR_MAIN_MANIFEST 1
/// 廃止した接続管理menuのID。再利用しない。
#define IDR_CLIENT_MENU 2
/// 廃止した手動更新確認commandのID。再利用しない。
#define IDM_UPDATE_CHECK 40001
/// 廃止したAgent設定menu commandのID。再利用しない。
#define IDM_AGENT_SETTINGS 40002
/// Agent設定用のmodeless dialog resource。
#define IDD_AGENT_SETTINGS 3
/// 接続組を固定表示するlabel。
#define IDC_AGENT_PROFILE 1100
/// 接続先の有限一覧。
#define IDC_AGENT_ENDPOINTS 1101
/// IPv4 literal入力。
#define IDC_AGENT_IPV4 1102
/// HTTP TCP port入力。
#define IDC_AGENT_PORT 1103
/// SVN／Trac用途選択。
#define IDC_AGENT_PURPOSE 1104
/// 廃止した個別承認checkboxのID。再利用しない。
#define IDC_AGENT_APPROVED 1105
/// 接続先の明示登録と自動保存command。
#define IDC_AGENT_ADD 1106
/// 廃止した選択更新commandのID。再利用しない。
#define IDC_AGENT_UPDATE 1107
/// 選択接続先の削除command。
#define IDC_AGENT_REMOVE 1108
/// 廃止した個別保存commandのID。再利用しない。
#define IDC_AGENT_SAVE 1109
/// 廃止したprofile削除commandのID。再利用しない。
#define IDC_AGENT_DELETE_PROFILE 1110
/// 廃止したINI読込みcommandのID。再利用しない。
#define IDC_AGENT_IMPORT 1111
/// 廃止したINI出力commandのID。再利用しない。
#define IDC_AGENT_EXPORT 1112
/// 固定結果だけを表示する状態欄。
#define IDC_AGENT_STATUS 1113
/// 承認済みAgentとINIのRDP配置準備command。
#define IDC_AGENT_DEPLOY 1114
/// 二ファイルの配置を利用者確認で進めるmodeless dialog。
#define IDD_AGENT_DEPLOYMENT 4
/// 実際のRDP接続先を表示するruntime label。
#define IDC_DEPLOY_TARGET 1120
/// remote Explorerの配置先を確認した明示選択。
#define IDC_DEPLOY_FOLDER_READY 1121
/// 所有ActiveXへCtrl+Vだけを要求するbutton。
#define IDC_DEPLOY_PASTE 1122
/// 自動検証でない利用者による配置確認button。
#define IDC_DEPLOY_COMPLETE 1123
/// 転送完了と混同しない配置状態欄。
#define IDC_DEPLOY_STATUS 1124
/// 接続数と認証状態が配置条件を満たさない説明。
#define IDS_DEPLOY_SESSION_REQUIRED 285
/// 共有clipboardへの公開を明示承認する説明。
#define IDS_DEPLOY_CONFIRM 286
/// 配置準備・公開失敗の固定説明。
#define IDS_DEPLOY_FAILED 287
/// Clipboard公開を転送完了と区別する説明。
#define IDS_DEPLOY_READY 288
/// キー要求成功を配置完了と区別する説明。
#define IDS_DEPLOY_PASTE_SENT 289
/// 対象切断・clipboard変更の固定説明。
#define IDS_DEPLOY_INVALIDATED 290
/// remote結果を利用者が確認する最終説明。
#define IDS_DEPLOY_COMPLETE_CONFIRM 291
/// 転送中の中止・終了を明示確認する説明。
#define IDS_DEPLOY_CANCEL_CONFIRM 292
/// OLE公開中の追加session拒否の説明。
#define IDS_DEPLOY_CONNECTION_BLOCKED 293
/// 一件貼付け要求の受付中の表示。
#define IDS_DEPLOY_PASTE_PENDING 294
/// clipboard公開の終了に失敗した場合の利用者再試行案内。
#define IDS_DEPLOY_CLEAR_FAILED 295
/// 廃止したsession Agent設定入口の予約ID。製品UIに置かず再利用しない。
#define IDC_SESSION_AGENT_DEPLOY 1016
/// 廃止したsession Agent起動入口の予約ID。製品UIに置かず再利用しない。
#define IDC_SESSION_AGENT_START 1017
/// 旧内部起動確認器のfixture専用dialog。製品の自動起動では使用しない。
#define IDD_AGENT_START 5
/// 起動対象のruntime表示。
#define IDC_START_TARGET 1130
/// remote Explorerの選択を利用者確認するcheckbox。
#define IDC_START_CONFIRMED 1131
/// 固定Enterを一度だけ送るbutton。
#define IDC_START_SEND 1132
/// 起動要求とAgent応答を区別する状態欄。
#define IDC_START_STATUS 1133
/// 廃止したsession配置入口の内部fixture用文字列。
#define IDS_SESSION_AGENT_DEPLOY 296
/// 廃止したsession起動入口の内部fixture用文字列。
#define IDS_SESSION_AGENT_START 297
/// 実ファイルの利用者確認前の案内。
#define IDS_START_READY 298
/// 入力要求だけが送信できた状態。
#define IDS_START_SENT 299
/// 実Agent応答の確認表示。
#define IDS_START_CONNECTED 300
/// 固定キーの送信失敗表示。
#define IDS_START_FAILED 301
/// 起動対象世代の失効表示。
#define IDS_START_INVALIDATED 302
/// 別sessionへの設定画面付け替え拒否。
#define IDS_SESSION_AGENT_BUSY 303
/// 起動確認後のAgent応答喪失表示。
#define IDS_START_UNAVAILABLE 304
/// 自動配置の固定説明欄。
#define IDC_DEPLOY_INSTRUCTIONS 1125
/// 初回配置と起動の確認。
#define IDS_AUTO_DEPLOY_CONFIRM 305
/// 初回配置と起動の画面title。
#define IDS_AUTO_DEPLOY_TITLE 306
/// 自動方式で利用者に残る警告確認の案内。
#define IDS_AUTO_DEPLOY_INSTRUCTIONS 307
/// 入力受付と応答確認の区別。
#define IDS_AUTO_DEPLOY_WAITING 308
/// 初期配置・起動の停止理由。
#define IDS_AUTO_DEPLOY_FAILED 309
/// 手動と自動の転送案内を区別する固定footer。
#define IDC_DEPLOY_FOOTER 1126
/// 自動配置の停止段階、固定分類、HRESULTの表示書式。
#define IDS_AUTO_DEPLOY_DETAILS 310
/// 段階未確定の固定表示。
#define IDS_AUTO_STAGE_NONE 311
/// 所有STAへの要求受付段階。
#define IDS_AUTO_STAGE_REQUEST 312
/// 認証、drive、Bridgeの条件確認段階。
#define IDS_AUTO_STAGE_VALIDATION 313
/// 固定文字列のclipboard公開段階。
#define IDS_AUTO_STAGE_CLIPBOARD 314
/// Win+Rの送信段階。
#define IDS_AUTO_STAGE_RUN 315
/// 固定入力timerの開始段階。
#define IDS_AUTO_STAGE_TIMER 316
/// Ctrl+V前の条件確認と送信段階。
#define IDS_AUTO_STAGE_PASTE 317
/// Enter前の条件確認と送信段階。
#define IDS_AUTO_STAGE_ENTER 318
/// Agent応答待ち段階。
#define IDS_AUTO_STAGE_RESPONSE 319
/// 入力focusと標準キー転送設定の確認段階。
#define IDS_AUTO_STAGE_INPUT_PREPARATION 320
/// 配置を行わない固定入力とRのSendInput比較menu入口。
#define IDC_SESSION_RUN_INPUT_TEST 1018
/// 開発用固定入力testのbutton表示。
#define IDS_SESSION_RUN_INPUT_TEST 321
/// 開発用固定入力testのtitle。
#define IDS_RUN_INPUT_TEST_TITLE 322
/// 対象sessionと固定Win+Rだけの既定取消確認。
#define IDS_RUN_INPUT_TEST_CONFIRM 323
/// 入力API受付と実画面確認を区別する結果書式。
#define IDS_RUN_INPUT_TEST_ACCEPTED 324
/// 固定入力testの停止段階とHRESULTの書式。
#define IDS_RUN_INPUT_TEST_FAILED 325
/// 利用者確認段階の固定表示。
#define IDS_RUN_INPUT_TEST_STAGE_CONFIRM 326
/// 開発用testの接続・競合条件確認段階。
#define IDS_RUN_INPUT_TEST_STAGE_VALIDATION 327
/// 三種類のSendKeysと四種類の固定SendInput比較だけを選べるmenu。
#define IDR_INPUT_TEST_MENU 6
/// 既存Win+R testのmenu command。
#define IDM_INPUT_TEST_WIN_R 40003
/// 左Windowsキー単独testのmenu command。
#define IDM_INPUT_TEST_WIN_ONLY 40004
/// Rキー単独testのmenu command。
#define IDM_INPUT_TEST_R_ONLY 40005
/// 左Windowsキー単独の送信段階表示。
#define IDS_INPUT_TEST_WIN_STAGE 328
/// Rキー単独の送信段階表示。
#define IDS_INPUT_TEST_R_STAGE 329
/// 左Windowsキーだけを送る既定取消の確認。
#define IDS_INPUT_TEST_WIN_CONFIRM 330
/// メモ帳の空欄へRキーだけを送る既定取消の確認。
#define IDS_INPUT_TEST_R_CONFIRM 331
/// API受付とスタートメニュー等の実表示を区別する結果。
#define IDS_INPUT_TEST_WIN_ACCEPTED 332
/// API受付とメモ帳の実入力を区別する結果。
#define IDS_INPUT_TEST_R_ACCEPTED 333
/// R単独SendInput比較だけのmenu command。
#define IDM_INPUT_TEST_R_EMULATION 40006
/// R単独のWindows入力比較段階。
#define IDS_INPUT_TEST_R_EMULATION_STAGE 334
/// ローカル誤入力リスクとメモ帳の空欄を確認する既定取消の説明。
#define IDS_INPUT_TEST_R_EMULATION_CONFIRM 335
/// 二event受付と実文字入力を区別する数値結果。
#define IDS_INPUT_TEST_R_EMULATION_ACCEPTED 336
/// 部分受付での手動解放を含む比較testの停止説明。
#define IDS_INPUT_TEST_R_EMULATION_FAILED 337
// 固定Win+RのWindows入力比較command。配置commandではない
#define IDM_INPUT_TEST_WIN_R_EMULATION 40007
// Win+R比較の送信段階
#define IDS_INPUT_TEST_WIN_R_EMULATION_STAGE 338
// ローカル誤入力と部分受付のリスクを含む既定取消確認
#define IDS_INPUT_TEST_WIN_R_EMULATION_CONFIRM 339
// 四要素のAPI受付とremote表示を区別する結果
#define IDS_INPUT_TEST_WIN_R_EMULATION_ACCEPTED 340
// 元数値と手動キー解放を案内する停止結果
#define IDS_INPUT_TEST_WIN_R_EMULATION_FAILED 341
// 部分受付で押下が残り得る固定キーの手動解放案内
#define IDS_AUTO_DEPLOY_PARTIAL_DETAILS 342
// 確認付き起動の遅延Enter待機。まだ入力受付ではない
#define IDS_START_PENDING 343
// 長い安全説明をdialog templateの256文字制約外で保持する
#define IDS_START_INSTRUCTIONS 344
// 確認付き起動の説明control
#define IDC_START_INSTRUCTIONS 1019
/// メモ帳で固定Ctrl+Vだけを確認する開発用command。
#define IDM_INPUT_TEST_PASTE_EMULATION 40008
/// メモ帳で固定Enterだけを確認する開発用command。
#define IDM_INPUT_TEST_ENTER_EMULATION 40009
/// メモ帳の貼付け診断の段階表示。
#define IDS_INPUT_TEST_PASTE_STAGE 345
/// コピー内容・入力先・誤貼付けリスクの個別確認。
#define IDS_INPUT_TEST_PASTE_CONFIRM 346
/// 四eventの受付と実貼付けを区別する結果。
#define IDS_INPUT_TEST_PASTE_ACCEPTED 347
/// clipboard失効・部分受付・手動Ctrl/V解放を案内する停止結果。
#define IDS_INPUT_TEST_PASTE_FAILED 348
/// メモ帳の改行診断の段階表示。
#define IDS_INPUT_TEST_ENTER_STAGE 349
/// Enterの誤実行リスクと入力先の個別確認。
#define IDS_INPUT_TEST_ENTER_CONFIRM 350
/// 二eventの受付と実改行を区別する結果。
#define IDS_INPUT_TEST_ENTER_ACCEPTED 351
/// Enterの部分受付と送信済み操作の不可取消を案内する停止結果。
#define IDS_INPUT_TEST_ENTER_FAILED 352
// resource ID 1115および353～356は旧未署名テスト用の廃止番号。再利用しない。
/// 設定編集と独立した今回だけの起動確認。
#define IDD_AGENT_LAUNCH 7
/// 今回の固定session表示。
#define IDC_LAUNCH_TARGET 1130
/// 保存済み全承認設定だけを明示選択する欄。
#define IDC_LAUNCH_PROFILE 1131
/// 対象と固定配置操作を説明する全文のreadonly警告。
#define IDC_LAUNCH_WARNING 1132
/// 起動せず設定編集を開く入口。
#define IDC_LAUNCH_SETTINGS 1133
/// 起動用保存設定なしの案内。
#define IDS_AGENT_LAUNCH_NEEDS_SETTINGS 357
/// 同じRDP内の不変package再利用を検証できない場合の復旧案内。
#define IDS_AGENT_RESTART_PACKAGE_FAILED 358
/// 選択接続に登録済みHTTP接続先がない場合の案内。
#define IDS_AGENT_SETTINGS_EMPTY 360
/// 接続入力が空または不正な場合の編集不可案内。
#define IDS_AGENT_SETTINGS_SELECT_CONNECTION 361
