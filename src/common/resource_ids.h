//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file resource_ids.h
 * @brief ClientとAgentで共有する製品branding resource IDを定義する。
 * @version 01.00.00
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details 同じZRdp製品familyであることをExplorer、taskbar、consoleから
 *          識別できるよう、両EXEへ同一icon resource IDを埋め込む。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

/// Client／Agentの大・小window iconとExplorer表示に使用する共通resource。
#define IDI_ZRDP_APP 200
