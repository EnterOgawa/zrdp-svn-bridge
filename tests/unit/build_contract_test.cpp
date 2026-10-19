//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file build_contract_test.cpp
 * @brief 配布対象がx64・C++20・Unicode・静的CRTのbuild契約を満たすか検証する。
 * @version 01.00.00
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * </pre>
 */
//------------------------------------------------------------------------------

#include <cstddef>

namespace {

static_assert( sizeof( void *) == 8U, "ZRdp SVN Bridge must be built as x64.") ;
static_assert( __cplusplus >= 202002L, "ZRdp SVN Bridge requires C++20.") ;

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief build時に確定したx64、Unicode、C++20契約をruntimeでも確認する。
 * @return 全契約を満たす場合は0、それ以外は失敗段階を表す非0 code。
 */
int wmain()
{
	// 各戻り値を、欠落したbuild契約を特定できる固定診断codeとして使用する
#if !defined(UNICODE) || !defined(_UNICODE)
	return 1 ;
#elif !defined(_MT)
	return 2 ;
#elif defined(_DLL)
	return 3 ;
#else
	return 0 ;
#endif
}
