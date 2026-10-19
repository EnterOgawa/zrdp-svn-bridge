//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_deployment.cpp
 * @brief worker上で固定名Agentと承認済みINIを一括検証する。
 * @version 01.00.03
 * @date 2026/10/08
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  署名付き二ファイルの読込みを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/agent_deployment/agent_deployment.h"
#include "client/update/update_image_verifier.h"
#include "client/update/update_windows_handles.h"
#include <array>
#include <algorithm>

namespace zrdp::client::agent_deployment {

Package::~Package()
{
	// 平文設定を最後の所有参照より長くmemoryへ残さない
	if( !ini.empty()) {
		SecureZeroMemory( ini.data(), ini.size()) ;
	}
}

bool ValidateAgentImagePair( const std::filesystem::path &client,
							const std::filesystem::path &agent) noexcept
{
	update::ClientImageMetadata client_metadata{}, agent_metadata{} ;
	return update::ReadClientImageMetadata( client, client_metadata) == update::UpdateError::None &&
		   update::ReadAgentImageMetadata( agent, agent_metadata) == update::UpdateError::None &&
		   client_metadata.version == agent_metadata.version && agent_metadata.agent_setup ;
}

agent_settings::AgentSettingsResult
PreparePackage( const agent_settings::AgentSettingsProfile &profile,
			   std::shared_ptr<const Package> &package, const std::stop_token stop) noexcept
{
	using agent_settings::AgentSettingsError ;
	package.reset() ;
	try {
		auto	   result = std::make_shared<Package>() ;
		const auto valid  = agent_settings::GenerateAgentSettingsIni( profile, result->ini) ;
		// 未承認・空・不正設定をEXE読込みより先に拒否する
		if( !valid.succeeded()) {
			return valid ;
		}

		std::array<wchar_t, 32768U> module{} ;
		const DWORD					length =
			GetModuleFileNameW( nullptr, module.data(), static_cast<DWORD>( module.size())) ;
		// 切詰めpathで別のAgentを参照しない
		if( length == 0U || length >= module.size()) {
			return {AgentSettingsError::InvalidPath} ;
		}

		const std::filesystem::path client( std::wstring( module.data(), length)) ;
		const auto					agent = client.parent_path() / L"ZRdpSvnAgent.exe" ;
		const auto					root  = agent.root_path() ;
		// UNC、network volume、linkを拒否し、既定配布場所だけを読む
		if( root.empty() || GetDriveTypeW( root.c_str()) != DRIVE_FIXED) {
			return {AgentSettingsError::InvalidPath} ;
		}

		auto parent = agent.parent_path() ;
		// 親directoryのjunction経由で別のAgentへ到達しない
		while( !parent.empty()) {
			const auto attributes = GetFileAttributesW( parent.c_str()) ;
			// 存在しない／reparse directoryを安全側に拒否する
			if( attributes == INVALID_FILE_ATTRIBUTES ||
			   (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
				return {AgentSettingsError::InvalidPath} ;
			}

			const auto next = parent.parent_path() ;
			// volume rootで有限探索を終える
			if( next == parent) {
				break ;
			}

			parent = next ;
		}

		update::detail::KernelHandle file(
			CreateFileW( agent.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
						FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) ;
		BY_HANDLE_FILE_INFORMATION info{} ;
		LARGE_INTEGER			   size{} ;
		// 読込みからmetadata検証完了までwrite/delete共有を許可しない
		if( !file.valid() || !GetFileInformationByHandle( file.get(), &info) ||
		   !GetFileSizeEx( file.get(), &size) ||
		   (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) !=
			   0U ||
		   info.nNumberOfLinks != 1U || size.QuadPart <= 0 ||
		   static_cast<std::uint64_t>( size.QuadPart) > kMaximumAgentBytes) {
			return {AgentSettingsError::FileFailure} ;
		}

		// 正式な同一版のClient/Agentだけを配置し、別製品のEXEを拒否する
		if( !ValidateAgentImagePair( client, agent)) {
			return {AgentSettingsError::InvalidData} ;
		}

		// 署名は起動条件にせず、sourceと配布元の信頼は利用者の責任とする
		result->agent.resize( static_cast<std::size_t>( size.QuadPart)) ;
		result->agent_setup = true ;
		std::size_t offset	= 0U ;
		// 一回64 KiB以下、有限全体サイズで読込みし、取消を確認する
		while( offset < result->agent.size()) {
			// 終了要求では部分packageを公開しない
			if( stop.stop_requested()) {
				return {AgentSettingsError::Cancelled} ;
			}

			const auto count =
				static_cast<DWORD>( std::min<std::size_t>( 65536U, result->agent.size() - offset)) ;
			DWORD received = 0U ;
			// 短い読込みを正常EOFとして採用しない
			if( !ReadFile( file.get(), result->agent.data() + offset, count, &received, nullptr) ||
			   received != count) {
				return {AgentSettingsError::FileFailure} ;
			}

			offset += received ;
		}

		package = std::move( result) ;
		return {} ;
	} catch( ...) {
		return {AgentSettingsError::ResourceExhausted} ;
	}
}

} // namespace zrdp::client::agent_deployment
