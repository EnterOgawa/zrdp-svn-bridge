//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_deployment_test.cpp
 * @brief 実clipboardを変更せず、二ファイルとCOM streamの制約を検証する。
 * @version 01.00.02
 * @date 2026/10/08
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  descriptor、内容、seek、上限のtestを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/agent_deployment/agent_deployment.h"
#include "client/update/update_image_verifier.h"
#include <ShlObj.h>
#include <atlbase.h>
#include <array>
#include <cstring>
#include <iostream>

/** @brief 注入byteだけでOLE提供とAgent metadataを確認する。
 * @param[in] argc CMakeの固定二EXE引数を含む件数。
 * @param[in] argv 隔離buildのClientとAgent。実行・clipboard公開はしない。
 * @return 全制約を満たす場合は0。
 */
int wmain( int argc, wchar_t **argv)
{
	namespace deployment = zrdp::client::agent_deployment ;
	namespace settings	 = zrdp::client::agent_settings ;
	namespace update	 = zrdp::client::update ;
	// 複数session、未認証、切断、再接続、別clipboardでは一切貼付けない
	if( deployment::CanPrepareDeployment( 0U, 1U) || deployment::CanPrepareDeployment( 2U, 1U) ||
	   deployment::CanPrepareDeployment( 1U, 0U) || !deployment::CanPrepareDeployment( 1U, 2U) ||
	   deployment::CanPasteDeployment( 1U, 0U, 10U, 10U) ||
	   deployment::CanPasteDeployment( 1U, 2U, 10U, 10U) ||
	   deployment::CanPasteDeployment( 1U, 1U, 10U, 11U) ||
	   deployment::CanPasteDeployment( 1U, 1U, 0U, 0U) ||
	   !deployment::CanPasteDeployment( 1U, 1U, 10U, 10U)) {
		return 18 ;
	}

	// networkやDPAPIなしの有限fixtureを準備する
	auto package   = std::make_shared<deployment::Package>() ;
	package->agent = {std::byte{0x4d}, std::byte{0}, std::byte{0xff}, std::byte{0x5a}} ;
	package->ini   = "[allowlist]\r\nendpoint.1001=192.0.2.10:80\r\n" ;
	ATL::CComPtr<IDataObject> object ;
	// clipboard公開なしでfixed virtual file objectを作れる
	if( FAILED( deployment::MakeDataObject( package, &object))) {
		return 1 ;
	}

	FORMATETC format{static_cast<CLIPFORMAT>( RegisterClipboardFormatW( CFSTR_FILEDESCRIPTORW)),
					 nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL} ;
	STGMEDIUM medium{} ;
	// descriptorが二つの固定basenameと正確なbyte長だけを持つ
	if( object->GetData( &format, &medium) != S_OK) {
		return 2 ;
	}

	const auto *group	   = static_cast<const FILEGROUPDESCRIPTORW *>( GlobalLock( medium.hGlobal)) ;
	const bool	descriptor = group != nullptr && group->cItems == 2U &&
							std::wcscmp( group->fgd[0].cFileName, L"ZRdpSvnAgent.exe") == 0 &&
							std::wcscmp( group->fgd[1].cFileName, L"agent.ini") == 0 &&
							group->fgd[0].nFileSizeHigh == 0U &&
							group->fgd[0].nFileSizeLow == package->agent.size() &&
							group->fgd[1].nFileSizeLow == package->ini.size() ;
	GlobalUnlock( medium.hGlobal) ;
	ReleaseStgMedium( &medium) ;
	// 誤った名前やfile数をリモートへ公開しない
	if( !descriptor) {
		return 3 ;
	}

	format = {static_cast<CLIPFORMAT>( RegisterClipboardFormatW( CFSTR_FILECONTENTS)), nullptr,
			  DVASPECT_CONTENT, 0, TYMED_ISTREAM} ;
	// バイナリのNULを文字列扱いせず保持する
	if( object->GetData( &format, &medium) != S_OK) {
		return 4 ;
	}

	std::array<std::byte, 8U> bytes{} ;
	ULONG					  read = 0U ;
	// EOFが短い読みをS_FALSEとして正確に伝える
	if( medium.pstm->Read( bytes.data(), static_cast<ULONG>( bytes.size()), &read) != S_FALSE ||
	   read != package->agent.size() ||
	   std::memcmp( bytes.data(), package->agent.data(), read) != 0) {
		return 5 ;
	}

	LARGE_INTEGER position{} ;
	position.QuadPart = -1 ;
	// SETの負値をunderflowへ変換しない
	if( medium.pstm->Seek( position, STREAM_SEEK_SET, nullptr) != STG_E_INVALIDFUNCTION) {
		return 6 ;
	}

	position.QuadPart = 0 ;
	ATL::CComPtr<IStream> clone ;
	// cloneは独立位置を持ち、大容量copyをしない
	if( medium.pstm->Seek( position, STREAM_SEEK_SET, nullptr) != S_OK ||
	   medium.pstm->Clone( &clone) != S_OK || clone->Read( bytes.data(), 1U, &read) != S_OK ||
	   medium.pstm->Read( bytes.data(), 1U, &read) != S_OK || bytes[0] != package->agent[0]) {
		return 7 ;
	}

	ULONG written = 9U ;
	// 読取り専用内容へ変更・truncateを許可しない
	if( clone->Write( bytes.data(), 1U, &written) != STG_E_ACCESSDENIED || written != 0U) {
		return 8 ;
	}

	ReleaseStgMedium( &medium) ;
	format.lindex = 1 ;
	// INIもstreamとして正確に提供する
	if( object->GetData( &format, &medium) != S_OK) {
		return 9 ;
	}

	std::array<char, 128U> ini{} ;
	const bool			   ini_matches =
		medium.pstm->Read( ini.data(), static_cast<ULONG>( ini.size()), &read) == S_FALSE &&
		std::string_view( ini.data(), read) == package->ini ;
	ReleaseStgMedium( &medium) ;
	// INI全体を別fileと混同しない
	if( !ini_matches) {
		return 10 ;
	}

	format.lindex = 2 ;
	// index上限、負index、別aspect、text形式を拒否する
	if( object->QueryGetData( &format) == S_OK) {
		return 11 ;
	}

	format.lindex = -1 ;
	// contentには有効なfile indexを必須にする
	if( object->QueryGetData( &format) == S_OK) {
		return 12 ;
	}

	format.cfFormat = CF_UNICODETEXT ;
	// clipboard経由の任意文字列・commandを提供しない
	if( object->QueryGetData( &format) == S_OK) {
		return 13 ;
	}

	ATL::CComPtr<IDataObject> invalid ;
	// null／空／上限外packageを公開しない
	if( deployment::MakeDataObject( {}, &invalid) != E_INVALIDARG) {
		return 14 ;
	}

	auto huge = std::make_shared<deployment::Package>() ;
	huge->agent.resize( deployment::kMaximumAgentBytes + 1U) ;
	huge->ini = "x" ;
	// 最大保持byte数を超えるfake結果をOLEへ渡さない
	if( deployment::MakeDataObject( huge, &invalid) != E_INVALIDARG) {
		return 15 ;
	}

	settings::AgentSettingsProfile			   unapproved{L"test", {}} ;
	std::shared_ptr<const deployment::Package> rejected ;
	// 空設定では実行中testのmoduleや実filesystemへ進む前に拒否する
	if( deployment::PreparePackage( unapproved, rejected, {}).succeeded() || rejected != nullptr) {
		return 16 ;
	}

	// CMakeから渡した隔離Agent metadataだけを読み、Clientとの混同を拒否する
	if( argc == 3) {
		update::ClientImageMetadata agent{}, client{} ;
		// 通常の配置条件は署名なしのClient/Agentでも満たせる
		if( update::ReadAgentImageMetadata( argv[2], agent) != update::UpdateError::None ||
		   update::ReadClientImageMetadata( argv[1], client) != update::UpdateError::None ||
		   agent.version != client.version || !agent.agent_setup ||
		   !deployment::ValidateAgentImagePair( argv[1], argv[2]) ||
		   deployment::ValidateAgentImagePair( argv[2], argv[1]) ||
		   deployment::ValidateAgentImagePair( argv[1], L"nonexistent-agent-test.exe") ||
		   !update::IsUnsignedDevelopmentImage( argv[1]) ||
		   !update::IsUnsignedDevelopmentImage( argv[2]) ||
		   update::VerifyClientSignaturePair( argv[1], argv[2]) == update::UpdateError::None ||
		   update::ReadAgentImageMetadata( argv[1], agent) == update::UpdateError::None) {
			return 17 ;
		}
	}

	// 署名を検証しなくても空／未承認設定を配置packageにしない
	if( deployment::PreparePackage( unapproved, rejected, {}).succeeded() || rejected != nullptr ||
	   update::IsUnsignedDevelopmentImage( L"nonexistent-agent-test.exe")) {
		return 19 ;
	}

	std::cout << "Agent deployment virtual files passed\n" ;
	return 0 ;
}
