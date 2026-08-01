/**
 * @file rdp_import.h
 * @brief Windows systemのmstscax type library importを一箇所へ隔離する。
 *
 * @details 生成されるCOM declarationとCLSID依存をRDP host境界へ閉じ込め、
 *          UIおよび将来のSVN Bridge層へ拡散させない。
 */
#pragma once

#include <Windows.h>

#if !defined(ZRDP_MSTSCAX_TYPE_LIBRARY)
#error ZRDP_MSTSCAX_TYPE_LIBRARY must identify the system mstscax.dll type library.
#endif

// 登録済みtype libraryが重複生成するWin32 RPC型を除外し、Windows SDK側の
// 正本を使用する。importを本headerへ限定してCLSID／version依存を隔離する。
/// @cond MSTSCAX_IMPORT_OPTIONS
#import ZRDP_MSTSCAX_TYPE_LIBRARY \
    raw_interfaces_only, \
    raw_native_types, \
    no_namespace, \
    named_guids, \
    exclude("wireHWND", "_RemotableHandle", "__MIDL_IWinTypes_0009")
/// @endcond
