/**
 * @file resource.h
 * @brief ZRdp ClientのWin32 resource IDとchild control IDを定義する。
 *
 * @details IDはresource script、接続管理Window、session Windowで共有する。
 *          接続情報や実環境値をresourceへ埋め込まない。
 */
#pragma once

#include "common/resource_ids.h"

// -----------------------------------------------------------------------------
// RDP接続操作と接続状態の文字列
// -----------------------------------------------------------------------------

#define IDS_APP_TITLE 100  ///< 通常Windowの製品title。
#define IDS_SERVER_LABEL 101  ///< RDP接続先label。
#define IDS_USER_NAME_LABEL 102  ///< RDP user name label。
#define IDS_DOMAIN_LABEL 103  ///< 任意domain label。
#define IDS_CONNECT 104  ///< 接続button。
#define IDS_DISCONNECT 105  ///< 切断button。
#define IDS_FULL_SCREEN 106  ///< セッション欄の全画面切替button。
#define IDS_WINDOW_MODE 107  ///< 同buttonのWindow mode復帰表示。
#define IDS_DISPLAY_MODE_NATIVE 108  ///< 等倍表示radio。
#define IDS_STATUS_DISCONNECTED 109  ///< 未接続状態。
#define IDS_STATUS_CONNECTING 110  ///< 接続中状態。
#define IDS_STATUS_CONNECTED 111  ///< 接続済み状態。
#define IDS_STATUS_DISCONNECTING 112  ///< 切断中状態。
#define IDS_STATUS_RECONNECTING 113  ///< 自動再接続中状態。
#define IDS_STATUS_AUTH_WARNING 114  ///< 認証warning状態。
#define IDS_STATUS_WARNING_FORMAT 115  ///< ActiveX warning code表示。
#define IDS_STATUS_FATAL_FORMAT 116  ///< ActiveX fatal code表示。
#define IDS_STATUS_DISCONNECTED_FORMAT 117  ///< 切断reason表示。
#define IDS_SERVER_REQUIRED 118  ///< 接続先必須error。
#define IDS_ACTIVE_X_CREATE_FAILED 119  ///< ActiveX生成error。
#define IDS_CONNECT_FAILED 120  ///< 接続開始error。
#define IDS_DISCONNECT_FAILED 121  ///< 切断要求error。
#define IDS_CLOSE_CONFIRM 122  ///< 接続中Window終了確認本文。
#define IDS_CLOSE_CONFIRM_TITLE 123  ///< Window終了確認title。
#define IDS_UNEXPECTED_ERROR 124  ///< 未分類例外表示。
#define IDS_STATUS_CHANNEL_DATA 125  ///< SVC受信状態。
#define IDS_STATUS_INITIAL_FORMAT 126  ///< ActiveX version初期表示。
#define IDS_STATUS_CONNECT_REQUESTED 127  ///< 接続要求受付状態。
#define IDS_LOG_CREATE_FAILED 128  ///< 診断log生成warning。
#define IDS_DISCONNECT_ERROR_FORMAT 129  ///< Microsoft切断詳細表示。
#define IDS_DISCONNECT_REASON_UNAVAILABLE 130  ///< 切断詳細取得不能表示。
#define IDS_RDP_ERROR_TITLE 131  ///< RDP error dialog title。
#define IDS_DISPLAY_SETTING_FAILED 132  ///< 表示mode変更error。
#define IDS_DISPLAY_MODE_LABEL 133  ///< 表示mode群label。
#define IDS_DISPLAY_MODE_SMART_SIZING 134  ///< Smart Sizing radio。
#define IDS_DISPLAY_MODE_DYNAMIC_RESOLUTION 135  ///< 動的解像度radio。
#define IDS_RDP_IDENTITY_INVALID 136  ///< domain／user入力error。

// -----------------------------------------------------------------------------
// Static Virtual Channel／Proxy状態の文字列
// -----------------------------------------------------------------------------

#define IDS_BRIDGE_LABEL 137  ///< Bridge状態群label。
#define IDS_BRIDGE_ECHO_TEST 138  ///< 手動Echo test button。
#define IDS_BRIDGE_STOPPED 139  ///< Bridge停止状態。
#define IDS_BRIDGE_WAITING_AGENT 140  ///< HELLO_ACK待ち状態。
#define IDS_BRIDGE_READY 141  ///< Agent利用可能状態。
#define IDS_BRIDGE_ECHO_RUNNING_FORMAT 142  ///< Echo進行状況。
#define IDS_BRIDGE_ECHO_PASSED_FORMAT 143  ///< Echo合格結果。
#define IDS_BRIDGE_FAILED_FORMAT 144  ///< Bridge固定失敗分類。
#define IDS_BRIDGE_INCOMPATIBLE 145  ///< protocol非互換状態。
#define IDS_PROXY_STARTING 146  ///< listener起動中状態。
#define IDS_PROXY_LISTENING_FORMAT 147  ///< loopback listen状態。
#define IDS_PROXY_READY_FORMAT 148  ///< Bridge利用可能Proxy状態。
#define IDS_PROXY_FAILED_FORMAT 149  ///< Proxy固定失敗分類。
#define IDS_PROXY_STOPPED 150  ///< Proxy停止状態。

// -----------------------------------------------------------------------------
// 接続管理Windowとsession titleの文字列
// -----------------------------------------------------------------------------

#define IDS_CONNECTION_MANAGER_TITLE 151  ///< manager Window title。
#define IDS_CONNECT_NEW_SESSION 152  ///< 新しい接続button。
#define IDS_MANAGER_STATUS_FORMAT 153  ///< session数と次port表示。
#define IDS_MANAGER_NO_PORT_FORMAT 154  ///< Proxy slot枯渇表示。
#define IDS_MANAGER_PROXY_CONFIG_ERROR_FORMAT 155  ///< local設定error表示。
#define IDS_SESSION_LIMIT_REACHED 156  ///< 最大session数到達error。
#define IDS_SESSION_PORT_UNAVAILABLE 157  ///< port slot利用不能error。
#define IDS_SESSION_CREATE_FAILED 158  ///< session Window生成error。
#define IDS_SESSION_RESOURCE_EXHAUSTED 159  ///< session資源不足error。
#define IDS_SESSION_TITLE_FORMAT 160  ///< 通常session title書式。
#define IDS_SESSION_TITLE_CONFIG_ERROR_FORMAT 161  ///< Proxy異常session title書式。
#define IDS_TITLE_RDP_NOT_CONNECTED 162  ///< title内RDP未接続。
#define IDS_TITLE_RDP_CONNECTING 163  ///< title内RDP接続中。
#define IDS_TITLE_RDP_CONNECTED 164  ///< title内RDP接続済み。
#define IDS_TITLE_RDP_RECONNECTING 165  ///< title内RDP再接続中。
#define IDS_TITLE_RDP_DISCONNECTING 166  ///< title内RDP切断中。
#define IDS_TITLE_RDP_FAILED 167  ///< title内RDP失敗。
#define IDS_TITLE_AGENT_STOPPED 168  ///< title内Agent停止。
#define IDS_TITLE_AGENT_WAITING 169  ///< title内Agent待機。
#define IDS_TITLE_AGENT_CONNECTED 170  ///< title内Agent接続済み。
#define IDS_TITLE_AGENT_ECHO_RUNNING 171  ///< title内Echo実行中。
#define IDS_TITLE_AGENT_FAILED 172  ///< title内Agent失敗。
#define IDS_TITLE_AGENT_INCOMPATIBLE 173  ///< title内Agent非互換。
#define IDS_TITLE_PROXY_STOPPED 174  ///< title内Proxy停止。
#define IDS_TITLE_PROXY_STARTING 175  ///< title内Proxy起動中。
#define IDS_TITLE_PROXY_LISTENING 176  ///< title内Proxy listen中。
#define IDS_TITLE_PROXY_READY 177  ///< title内Proxy利用可能。
#define IDS_TITLE_PROXY_FAILED 178  ///< title内Proxy失敗。

// -----------------------------------------------------------------------------
// 接続profileと音声redirect設定の文字列
// -----------------------------------------------------------------------------

#define IDS_CONNECTION_NAME_LABEL 179  ///< 任意接続名label。
#define IDS_LOCAL_RESOURCES_LABEL 180  ///< local resource設定群label。
#define IDS_REDIRECT_MICROPHONE 181  ///< microphone redirect check。
#define IDS_REMOTE_AUDIO_LABEL 182  ///< remote audio再生先label。
#define IDS_AUDIO_PLAY_ON_CLIENT 183  ///< Client側再生選択。
#define IDS_AUDIO_PLAY_ON_REMOTE 184  ///< 接続先側再生選択。
#define IDS_AUDIO_DO_NOT_PLAY 185  ///< 音声を再生しない選択。
#define IDS_AUDIO_SETTING_INVALID 186  ///< 音声設定不正error。
#define IDS_STATUS_PRE_LOGIN_RETRY 187  ///< 一時的なログイン前失敗の再試行待機。

// -----------------------------------------------------------------------------
// command routingと状態更新に使用するchild control ID
// -----------------------------------------------------------------------------

#define IDC_SERVER 1001  ///< 接続先combo。
#define IDC_USER_NAME 1002  ///< user name combo。
#define IDC_DISPLAY_MODE_NATIVE 1003  ///< 等倍表示radio。
#define IDC_CONNECT 1004  ///< 接続command。
#define IDC_DISCONNECT 1005  ///< 切断command。
#define IDC_FULL_SCREEN 1006  ///< 全画面command。
#define IDC_DISPLAY_MODE_SMART_SIZING 1007  ///< Smart Sizing radio。
#define IDC_STATUS 1008  ///< RDP／manager状態欄。
#define IDC_DISPLAY_MODE_DYNAMIC_RESOLUTION 1009  ///< 動的解像度radio。
#define IDC_DOMAIN 1010  ///< domain combo。
#define IDC_BRIDGE_ECHO_TEST 1011  ///< Echo test command。
#define IDC_BRIDGE_STATE 1012  ///< Bridge状態欄。
#define IDC_CONNECTION_NAME 1013  ///< 任意接続名combo。
#define IDC_REDIRECT_MICROPHONE 1014  ///< microphone redirect check。
#define IDC_AUDIO_PLAYBACK_MODE 1015  ///< audio再生先combo。

// -----------------------------------------------------------------------------
// 実行時manifest
// -----------------------------------------------------------------------------

#define IDR_MAIN_MANIFEST 1  ///< Client実行level／互換性manifest。
