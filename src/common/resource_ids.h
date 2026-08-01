/**
 * @file resource_ids.h
 * @brief ClientとAgentで共有する製品branding resource IDを定義する。
 *
 * @details 同じZRdp製品familyであることをExplorer、taskbar、consoleから
 *          識別できるよう、両EXEへ同一icon resource IDを埋め込む。
 */
#pragma once

/// Client／Agentの大・小window iconとExplorer表示に使用する共通resource。
#define IDI_ZRDP_APP 200
