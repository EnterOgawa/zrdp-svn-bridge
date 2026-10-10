//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 * @file agent_tray_resources.h
 * @brief Agent通知領域の非機密日本語resource IDを定義する。
 * @version 01.00.00
 * @date 2026/10/10
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/10  Agentの通知領域UIを追加。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once
/// Client HELLO待機の固定表示。
#define IDS_AGENT_TRAY_WAITING 501
/// SVN Bridge接続済みの固定表示。
#define IDS_AGENT_TRAY_READY 502
/// 一時切断からの復帰待機。
#define IDS_AGENT_TRAY_RECOVERING 503
/// 設定またはBridge利用不能の固定表示。
#define IDS_AGENT_TRAY_UNAVAILABLE 504
/// 協調停止menu。
#define IDS_AGENT_TRAY_EXIT 505
/// icon生成・維持失敗の固定説明。
#define IDS_AGENT_TRAY_FAILED 506
/// 稼働Agentまたは競合setupの明示終了案内。
#define IDS_AGENT_SETUP_RUNNING 507
/// 対象RDP session不成立の起動失敗。
#define IDS_AGENT_SESSION_REQUIRED 508
/// 同session二重Agentの拒否。
#define IDS_AGENT_ALREADY_RUNNING 509
/// ESVNBRGを開けない起動失敗。
#define IDS_AGENT_CHANNEL_UNAVAILABLE 510
/// protocolまたはstream停止の固定案内。
#define IDS_AGENT_BRIDGE_STOPPED 511
/// 更新途中の復旧不能時は保存backupを手動確認する。
#define IDS_AGENT_SETUP_ROLLBACK_FAILED 512
