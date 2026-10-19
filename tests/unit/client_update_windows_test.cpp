//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file client_update_windows_test.cpp
 * @brief Windowsのfile置換・復元、実metadata、unsigned拒否を隔離fixtureで確認する。
 * @version 01.00.01
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  Windows実file APIとread-only署名testを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  二EXEの実置換・第二file失敗・復元とAgent検証を追加。
 * </pre>
 *
 * @note 実行するEXE、RDP、取引先、GitHub networkは使用しない。署名済み基準は
 *       任意の第三引数でread-only検証し、配布物を書き換えない。
 */
//------------------------------------------------------------------------------
#include "client/update/update_apply.h"
#include "client/update/update_image_verifier.h"
#include "client/update/update_windows.h"
#include "client/update/update_windows_handles.h"

#include <Windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

/** @brief 更新境界を短く参照するtest用namespace。 */
namespace update = zrdp::client::update ;

/**
 * @brief 自作fixtureだけへ短いbyte列を書込む。
 * @param[in] path 新規fixtureまたは同fixture内の固定file。
 * @param[in] text 旧版／新版を区別する架空byte列。
 * @return 完全に保存できた場合はtrue。
 */
[[nodiscard]] bool WriteFixture( const std::filesystem::path &path, const std::string &text)
{
	std::ofstream file( path, std::ios::binary | std::ios::trunc) ;
	file.write( text.data(), static_cast<std::streamsize>( text.size())) ;
	file.close() ;
	return file.good() ;
}

/**
 * @brief 小さな自作fixtureを全byte読取る。
 * @param[in] path 固定fixture内file。
 * @return 読んだbyte列。不在なら空。
 */
[[nodiscard]] std::string ReadFixture( const std::filesystem::path &path)
{
	std::ifstream file( path, std::ios::binary) ;
	return {std::istreambuf_iterator<char>( file), std::istreambuf_iterator<char>()} ;
}

/** @brief build working directoryに新規作成する一件だけのfixture。 */
class FileFixture final {
public:
	/** @brief 既存directoryを再利用せず、固定Clientとstageの関係を作る。 */
	FileFixture()
	{
		directory = std::filesystem::current_path() /
					("client-update-fixture-" + std::to_string( GetCurrentProcessId())) ;
		// 既存directoryの内容をtest cleanupへ巻込まない
		if( !std::filesystem::create_directory( directory)) {
			throw std::runtime_error( "Fixture exists") ;
		}

		stage = directory / ".zrdp-update-00000000000000000000000000000001" ;
		std::filesystem::create_directory( stage) ;
		target			= directory / "ZRdpSvnBridge.exe" ;
		candidate		= stage / "candidate.exe" ;
		backup			= stage / "previous.exe" ;
		agent			= directory / "ZRdpSvnAgent.exe" ;
		agent_candidate = stage / "agent-candidate.exe" ;
		agent_backup	= stage / "agent-previous.exe" ;
		Reset() ;
	}

	/** @brief 自作の固定fileだけを回収し、recursive deleteを行わない。 */
	~FileFixture()
	{
		std::error_code ignored ;
		std::filesystem::remove( stage / "hardlink.bin", ignored) ;
		std::filesystem::remove( agent_backup, ignored) ;
		std::filesystem::remove( agent_candidate, ignored) ;
		std::filesystem::remove( agent, ignored) ;
		std::filesystem::remove( backup, ignored) ;
		std::filesystem::remove( candidate, ignored) ;
		std::filesystem::remove( target, ignored) ;
		std::filesystem::remove( stage, ignored) ;
		std::filesystem::remove( directory, ignored) ;
	}

	/** @brief 自作fixtureを次の失敗注入caseへ初期化する。 */
	void Reset()
	{
		std::filesystem::remove( agent_backup) ;
		std::filesystem::remove( agent_candidate) ;
		std::filesystem::remove( agent) ;
		std::filesystem::remove( backup) ;
		std::filesystem::remove( candidate) ;
		std::filesystem::remove( target) ;
		// fileの完全作成ができない環境では部分fixtureでtestを続けない
		if( !WriteFixture( target, "old") || !WriteFixture( candidate, "new") ||
		   !WriteFixture( agent, "old-agent") || !WriteFixture( agent_candidate, "new-agent")) {
			throw std::runtime_error( "Fixture write failed") ;
		}
	}

	/// testが新規所有したdirectory。既存directoryは拒否する。
	std::filesystem::path directory ;
	/// 固定Client配置を模したregular file。
	std::filesystem::path target ;
	/// 固定形式tokenを持つ隣接stage。
	std::filesystem::path stage ;
	/// 架空新版のregular file。
	std::filesystem::path candidate ;
	/// Windows ReplaceFileが生成する旧版backup。
	std::filesystem::path backup ;
	/// 固定Agent配置の架空byte列。
	std::filesystem::path agent ;
	/// 同じstageの固定Agent候補。
	std::filesystem::path agent_candidate ;
	/// Agent置換が生成する旧版backup。
	std::filesystem::path agent_backup ;
} ;

/** @brief 実Windows file APIを使い、署名・起動の部分だけをfixtureに置換する。 */
class FixtureOperations final : public update::IClientUpdateApplyOperations {
public:
	/**
	 * @brief 自作fixtureへの非所有参照を保持する。
	 * @param[in,out] fixture transactionより長く所有するtest data。
	 */
	explicit FixtureOperations( FileFixture &fixture) : fixture_( fixture)
	{
	}

	/**
	 * @brief 架空旧版／候補のbyte列が不変か確認する。
	 * @return 不変の場合だけNone。
	 */
	[[nodiscard]] update::UpdateError ValidateBeforeReplacement() noexcept override
	{
		try {
			return ReadFixture( fixture_.target) == "old" &&
						   ReadFixture( fixture_.candidate) == "new" &&
						   ReadFixture( fixture_.agent) == "old-agent" &&
						   ReadFixture( fixture_.agent_candidate) == "new-agent"
					   ? update::UpdateError::None
					   : update::UpdateError::HashMismatch ;
		} catch( ...) {
			return update::UpdateError::FileFailure ;
		}
	}

	/**
	 * @brief productionと同じWindows ReplaceFile境界を使う。
	 * @return 実file置換の結果。
	 */
	[[nodiscard]] update::UpdateError Replace() noexcept override
	{
		return update::ReplaceClientUpdatePair( fixture_.target, fixture_.stage) ;
	}

	/**
	 * @brief 差替え後のbyte列または注入した検証失敗を返す。
	 * @return 架空新版が完全ならNone。
	 */
	[[nodiscard]] update::UpdateError ValidateInstalledCandidate() noexcept override
	{
		try {
			return !fail_verification && ReadFixture( fixture_.target) == "new" &&
						   ReadFixture( fixture_.agent) == "new-agent"
					   ? update::UpdateError::None
					   : update::UpdateError::HashMismatch ;
		} catch( ...) {
			return update::UpdateError::FileFailure ;
		}
	}

	/**
	 * @brief EXEは実行せず、起動可否だけを注入する。
	 * @return fakeのreadyまたは停止済み結果。
	 */
	[[nodiscard]] update::UpdateRestartResult RestartAndConfirm() noexcept override
	{
		return fail_restart ? update::UpdateRestartResult{}

							: update::UpdateRestartResult{update::UpdateRestartState::Ready,
														  update::UpdateError::None} ;
	}

	/**
	 * @brief 旧版の実byte列を確認する。
	 * @return 完全に旧版ならtrue。
	 */
	[[nodiscard]] bool OriginalIsInstalled() noexcept override
	{
		try {
			return ReadFixture( fixture_.target) == "old" &&
				   ReadFixture( fixture_.agent) == "old-agent" ;
		} catch( ...) {
			return false ;
		}
	}

	/**
	 * @brief 同じWindows原子的復元境界を使う。
	 * @return backupが旧版で復元できた場合だけNone。
	 */
	[[nodiscard]] update::UpdateError RestoreOriginal() noexcept override
	{
		try {
			const bool client_original = ReadFixture( fixture_.target) == "old" ;
			const bool agent_original  = ReadFixture( fixture_.agent) == "old-agent" ;
			// 元byteに一致しない二backupは復元せず、部分復元成功と偽らない
			if( (!client_original && ReadFixture( fixture_.backup) != "old") ||
			   (!agent_original && ReadFixture( fixture_.agent_backup) != "old-agent")) {
				return update::UpdateError::RollbackFailure ;
			}

			// 第二file失敗でClientが不変の場合も、変更済みAgentだけを戻す
			if( !agent_original &&
			   update::RestoreClientUpdateFile( fixture_.agent, fixture_.agent_backup) !=
				   update::UpdateError::None) {
				return update::UpdateError::RollbackFailure ;
			}

			// 不変のClient backupは要求せず、変更済みの場合だけ復元する
			if( !client_original &&
			   update::RestoreClientUpdateFile( fixture_.target, fixture_.backup) !=
				   update::UpdateError::None) {
				return update::UpdateError::RollbackFailure ;
			}

			return OriginalIsInstalled() ? update::UpdateError::None
										 : update::UpdateError::RollbackFailure ;
		} catch( ...) {
			return update::UpdateError::RollbackFailure ;
		}
	}

	/// 差替え後の検証失敗を注入する。
	bool fail_verification{false} ;
	/// 新版起動失敗を注入する。実processは使用しない。
	bool fail_restart{false} ;

private:
	/// このtestで新規作成した唯一のfixtureへの非所有参照。
	FileFixture &fixture_ ;
} ;

} // namespace

/**
 * @brief 実file置換、復元、readonly hash、metadata、unsigned拒否を確認する。
 * @param[in] argc 必須build二fileと任意署名済み基準の引数数。
 * @param[in] argv Client build、Agent build、任意のreadonly署名済みClient。
 * @return 成功は0、失敗は検証段階を表す非0 code。
 */
int wmain( const int argc, wchar_t **argv)
{
	// 明示されたbuildだけを読取り、任意fileの探索をしない
	if( argc != 3 && argc != 4) {
		return 1 ;
	}

	try {
		FileFixture		  fixture ;
		FixtureOperations operations( fixture) ;
		// Windowsの正常置換が旧版backupを残し、候補の全byteを原配置へ移すことを確認する
		if( update::ApplyClientUpdate( operations) != update::UpdateError::None ||
		   ReadFixture( fixture.target) != "new" || ReadFixture( fixture.backup) != "old" ||
		   ReadFixture( fixture.agent) != "new-agent" ||
		   ReadFixture( fixture.agent_backup) != "old-agent" ||
		   std::filesystem::exists( fixture.agent_candidate) ||
		   std::filesystem::exists( fixture.candidate)) {
			return 2 ;
		}

		fixture.Reset() ;
		operations.fail_verification = true ;
		// 差替え後の検証失敗では実Windows file APIで旧版へ戻す
		if( update::ApplyClientUpdate( operations) != update::UpdateError::HashMismatch ||
		   ReadFixture( fixture.target) != "old" || ReadFixture( fixture.agent) != "old-agent" ||
		   std::filesystem::exists( fixture.agent_backup) ||
		   std::filesystem::exists( fixture.backup)) {
			return 3 ;
		}

		fixture.Reset() ;
		operations.fail_verification = false ;
		operations.fail_restart		 = true ;
		// 新版起動失敗も同じ実backupから復元し、実EXEの起動はしない
		if( update::ApplyClientUpdate( operations) != update::UpdateError::RestartFailure ||
		   ReadFixture( fixture.target) != "old" || ReadFixture( fixture.agent) != "old-agent") {
			return 4 ;
		}

		fixture.Reset() ;
		operations.fail_restart = false ;
		{
			update::detail::KernelHandle locked( CreateFileW( fixture.target.c_str(), GENERIC_READ,
															FILE_SHARE_READ, nullptr, OPEN_EXISTING,
															FILE_ATTRIBUTE_NORMAL, nullptr)) ;
			// 別process相当のfile lockで置換に失敗しても旧版と候補を保持する
			if( !locked.valid() ||
			   update::ApplyClientUpdate( operations) != update::UpdateError::ReplacementFailure ||
			   ReadFixture( fixture.target) != "old" || ReadFixture( fixture.candidate) != "new" ||
			   ReadFixture( fixture.agent) != "old-agent" ||
			   std::filesystem::exists( fixture.agent_backup)) {
				return 5 ;
			}
		}

		fixture.Reset() ;
		{
			update::detail::KernelHandle locked( CreateFileW( fixture.agent.c_str(), GENERIC_READ,
															FILE_SHARE_READ, nullptr, OPEN_EXISTING,
															FILE_ATTRIBUTE_NORMAL, nullptr)) ;
			// 第一fileのlockではClientもAgentも候補も変更しない
			if( !locked.valid() ||
			   update::ApplyClientUpdate( operations) != update::UpdateError::ReplacementFailure ||
			   ReadFixture( fixture.target) != "old" || ReadFixture( fixture.agent) != "old-agent" ||
			   ReadFixture( fixture.candidate) != "new" ||
			   ReadFixture( fixture.agent_candidate) != "new-agent") {
				return 15 ;
			}
		}

		// Client候補とAgent backupを混ぜた固定名の組合せも拒否する
		if( update::ReplaceClientUpdateFile( fixture.agent, fixture.candidate,
										   fixture.agent_backup) !=
		   update::UpdateError::ReplacementFailure) {
			return 16 ;
		}

		// 固定配置以外のfile名をraw Windows置換境界でも拒否する
		if( update::ReplaceClientUpdateFile( fixture.directory / "other.exe", fixture.candidate,
										   fixture.backup) !=
		   update::UpdateError::ReplacementFailure) {
			return 6 ;
		}

		// digest fixtureを書けない場合は既知値testを継続しない
		if( !WriteFixture( fixture.candidate, "abc")) {
			return 7 ;
		}

		update::Sha256 hash{} ;
		std::uint64_t  bytes = 0U ;
		// Windows CNGのstreaming digestが既知のSHA-256値と一致することを確認する
		if( update::ComputeUpdateFileHash( fixture.candidate, hash, bytes) !=
			   update::UpdateError::None ||
		   bytes != 3U ||
		   update::FormatSha256( hash) !=
			   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
			return 7 ;
		}

		update::ClientImageMetadata metadata ;
		// 非PE byte列とAgentをClient metadataとして使用しない
		if( update::ReadClientImageMetadata( fixture.candidate, metadata) !=
			   update::UpdateError::UnsupportedImage ||
		   update::ReadClientImageMetadata( argv[2], metadata) == update::UpdateError::None) {
			return 8 ;
		}

		// 実Client buildの署名対象bootstrap／protocol keyを読む
		if( update::ReadClientImageMetadata( argv[1], metadata) != update::UpdateError::None ||
		   !metadata.update_bootstrap || !metadata.paired_update ||
		   metadata.bridge_protocol != 1U) {
			return 9 ;
		}

		// unsigned開発buildを自動更新のtrust anchorにしない
		if( update::VerifyClientSignaturePair( argv[1], argv[1]) !=
		   update::UpdateError::InvalidSignature) {
			return 10 ;
		}

		update::ReleaseManifest release ;
		release.version			= metadata.version ;
		release.bridge_protocol = 1U ;
		release.size_bytes		= 3U ;
		update::Sha256 old_hash{} ;
		std::uint64_t  old_bytes = 0U ;
		// 外部size／digest不一致を署名検証より前にも拒否する
		if( update::ComputeUpdateFileHash( argv[1], old_hash, old_bytes) !=
			   update::UpdateError::None ||
		   update::ValidateDownloadedClient( argv[1], fixture.candidate, release, old_hash) !=
			   update::UpdateError::HashMismatch) {
			return 11 ;
		}

		update::ClientImageMetadata agent_metadata ;
		// 実Agent metadataと取得候補の同版・setup・wireを照合する
		if( update::ReadAgentImageMetadata( argv[2], agent_metadata) != update::UpdateError::None ||
		   update::ValidateAgentUpdateMetadata( agent_metadata, release) !=
			   update::UpdateError::None ||
		   update::ComputeUpdateFileHash( argv[2], release.agent_sha256, release.agent_size_bytes) !=
			   update::UpdateError::None ||
		   update::ValidateDownloadedAgent( argv[1], argv[2], release) !=
			   update::UpdateError::InvalidSignature) {
			return 17 ;
		}

		++agent_metadata.version.components[3] ;
		// 正式Agentでも別版を同じセットへ混ぜない
		if( update::ValidateAgentUpdateMetadata( agent_metadata, release) !=
		   update::UpdateError::MetadataMismatch) {
			return 18 ;
		}

		agent_metadata.version	   = release.version ;
		agent_metadata.agent_setup = false ;
		// 固定setup非対応の候補を次回接続用Agentへ採用しない
		if( update::ValidateAgentUpdateMetadata( agent_metadata, release) !=
		   update::UpdateError::MetadataMismatch) {
			return 19 ;
		}

		agent_metadata.agent_setup	   = true ;
		agent_metadata.bridge_protocol = 2U ;
		// 同じ製品版でもwire非互換なら取得セットを拒否する
		if( update::ValidateAgentUpdateMetadata( agent_metadata, release) !=
		   update::UpdateError::IncompatibleProtocol) {
			return 20 ;
		}

		++release.agent_size_bytes ;
		// Agentだけのexact size不一致を署名検証より前に拒否する
		if( update::ValidateDownloadedAgent( argv[1], argv[2], release) !=
		   update::UpdateError::HashMismatch) {
			return 21 ;
		}

		// 明示された署名済み基準だけをread-onlyで追加検証する
		if( argc == 4) {
			const auto trust = update::VerifyClientSignaturePair( argv[3], argv[3]) ;
			// Windows trustとtimestampを満たす同一署名者が正しく受入れられることを確認する
			if( trust != update::UpdateError::None) {
				std::cerr << "Trusted baseline check failed: " << static_cast<unsigned int>( trust)
						  << '\n' ;
				return 12 ;
			}

			// 有効な署名済みanchorからでもunsigned候補は拒否する
			if( update::VerifyClientSignaturePair( argv[3], argv[1]) !=
			   update::UpdateError::InvalidSignature) {
				return 13 ;
			}
		}

		return 0 ;
	} catch( ...) {
		return 14 ;
	}
}
