//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file portable_binary_contract_test.cpp
 * @brief Client／Agent配布EXEのx64種別と共通version resourceを検証する。
 * @version 01.00.00
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details EXEを実行せずfile metadataだけを読み、installer不要の二配布物が
 *          同じProtocol releaseへ揃っていることを確認する。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * </pre>
 */
//------------------------------------------------------------------------------

#include <Windows.h>
#include <WinVer.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

// Microsoft connection barをcontainerより前面に維持するfixed file version
constexpr std::uint16_t kExpectedVersionMajor = 0U ;
// portable EXEで期待する製品versionのminor要素。file独立版とは別管理
constexpr std::uint16_t kExpectedVersionMinor = 7U ;
// portable EXEで期待する製品versionのpatch要素。file独立版とは別管理
constexpr std::uint16_t kExpectedVersionPatch = 20U ;
// portable EXEで期待するWindows version resourceのbuild要素
constexpr std::uint16_t kExpectedVersionBuild = 0U ;

/**
 * @brief 一つのEXEがx64かつ期待version resourceを持つか検証する。
 * @param[in] path 検証対象EXEの絶対またはCTest相対path。
 * @return 全metadata契約を満たす場合はtrue。
 */
[[nodiscard]] bool		ValidateBinary( const wchar_t *path)
{
	// path欠落時にWindows APIへnullを渡さない
	if( path == nullptr || path[0] == L'\0') {
		return false ;
	}

	DWORD binary_type = 0U ;
	// x86／unknown binaryをportable AgentまたはClientとして配布しない
	if( !GetBinaryTypeW( path, &binary_type) || binary_type != SCS_64BIT_BINARY) {
		return false ;
	}

	DWORD		ignored_handle = 0U ;
	const DWORD version_size   = GetFileVersionInfoSizeW( path, &ignored_handle) ;
	// version resource欠落または空dataを受け入れない
	if( version_size == 0U) {
		return false ;
	}

	std::vector<std::byte> version_data( version_size) ;
	// version resourceを取得できなければportable binary契約を検証済みにしない
	if( !GetFileVersionInfoW( path, 0U, version_size, version_data.data())) {
		return false ;
	}

	void *fixed_value = nullptr ;
	UINT  fixed_size  = 0U ;
	// root VS_FIXEDFILEINFOを取得できない文字列だけのresourceを拒否する
	if( !VerQueryValueW( version_data.data(), L"\\", &fixed_value, &fixed_size) ||
	   fixed_value == nullptr || fixed_size < sizeof( VS_FIXEDFILEINFO)) {
		return false ;
	}

	const auto *fixed_info = static_cast<const VS_FIXEDFILEINFO *>( fixed_value) ;
	// signatureを確認してからversion fieldを解釈する
	if( fixed_info->dwSignature != VS_FFI_SIGNATURE) {
		return false ;
	}

	return HIWORD( fixed_info->dwFileVersionMS) == kExpectedVersionMajor &&
		   LOWORD( fixed_info->dwFileVersionMS) == kExpectedVersionMinor &&
		   HIWORD( fixed_info->dwFileVersionLS) == kExpectedVersionPatch &&
		   LOWORD( fixed_info->dwFileVersionLS) == kExpectedVersionBuild &&
		   fixed_info->dwFileVersionMS == fixed_info->dwProductVersionMS &&
		   fixed_info->dwFileVersionLS == fixed_info->dwProductVersionLS ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief CTestが渡すClient／Agentのbinary metadataを検証する。
 * @param[in] argument_count 実行fileを含む引数個数。
 * @param[in] arguments 第1引数Client、第2引数Agent。
 * @return 両方有効なら0、引数または対象別の固定失敗code。
 */
int wmain( const int argument_count, wchar_t *arguments[])
{
	// ClientとAgentの二pathが揃った場合だけ検証を開始する
	if( argument_count != 3 || arguments[1] == nullptr || arguments[2] == nullptr) {
		return 1 ;
	}

	// Client EXEの自己完結性と製品version契約を確認する
	if( !ValidateBinary( arguments[1])) {
		return 2 ;
	}

	// Agent EXEの自己完結性と製品version契約を確認する
	if( !ValidateBinary( arguments[2])) {
		return 3 ;
	}

	return 0 ;
}
