//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file update_manifest.h
 * @brief Client更新の有限manifest、version、同意状態の共通境界を定義する。
 * @version 01.00.01
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  GitHub公開Release用の更新検証モデルを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  同一版Client／Agentのformat 2セットmanifestを定義。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace zrdp::client::update {

// manifestとして保持する最大byte数
inline constexpr std::size_t	  kMaximumUpdateManifestBytes		 = 16U * 1024U ;
// local設定fileとして保持する最大byte数
inline constexpr std::size_t	  kMaximumUpdateConfigurationBytes	 = 64U * 1024U ;
// 一Client EXEのdownload上限。全量をmemoryへ保持しない
inline constexpr std::uint64_t	  kMaximumUpdateExecutableBytes		 = 64ULL * 1024ULL * 1024ULL ;
// Agent配布・setupと一致する一Agent EXEの上限
inline constexpr std::uint64_t	  kMaximumUpdateAgentExecutableBytes = 16ULL * 1024ULL * 1024ULL ;
// GitHub最新Releaseへ添付する固定asset名
inline constexpr std::string_view kUpdateManifestAssetName			 = "zrdp-client-update.ini" ;
// セット更新するClientの固定配布EXE名
inline constexpr std::string_view kUpdateExecutableAssetName		 = "ZRdpSvnBridge.exe" ;
// セット更新するAgentの固定配布EXE名
inline constexpr std::string_view kUpdateAgentAssetName				 = "ZRdpSvnAgent.exe" ;
// ユーザーが指定した公開Release取得先。repositoryの公開操作は行わない
inline constexpr std::string_view kDefaultUpdateRepository			 = "EnterOgawa/zrdp-svn-bridge" ;

/** @brief SHA-256 digestの固定長binary表現。 */
using Sha256 = std::array<std::byte, 32U> ;

/** @brief Windows version resourceと一致する四要素version。 */
struct UpdateVersion {
	/// major、minor、patch、buildの順。各値はWindows WORDの範囲。
	std::array<std::uint16_t, 4U> components{} ;
	/**
	 * @brief 四要素を数値順で比較する。
	 * @param[in] other 比較対象version。
	 * @return 数値比較による順序。
	 */
	[[nodiscard]] auto			  operator<=>( const UpdateVersion &other) const = default ;
} ;

/** @brief URL、payload、certificate識別情報を含まない固定更新error。 */
enum class UpdateError : std::uint32_t {
	/// 検証または操作が成功した。
	None = 0U,
	/// 更新設定が不正であり通信しない。
	InvalidConfiguration,
	/// repository名が許可構文を満たさない。
	InvalidRepository,
	/// manifestの構文、key、必須値が不正である。
	InvalidManifest,
	/// 四要素versionの形式または範囲が不正である。
	InvalidVersion,
	/// manifestのformatが互換ではない。
	UnsupportedFormat,
	/// Bridge protocolが現Clientと互換ではない。
	IncompatibleProtocol,
	/// sizeまたはmemory保持の有限上限を超える。
	DataTooLarge,
	/// HTTPS取得に失敗し、別経路へfallbackしない。
	NetworkFailure,
	/// redirectが許可されたGitHub配布先ではない。
	InvalidRedirect,
	/// updaterの終了または取消が要求された。
	Cancelled,
	/// local fileの検証または操作に失敗した。
	FileFailure,
	/// Windows trustまたは署名条件を満たさない。
	InvalidSignature,
	/// 実行中Clientと署名者の公開鍵が一致しない。
	SignerMismatch,
	/// downloadまたは旧版のSHA-256が一致しない。
	HashMismatch,
	/// 製品名、元file名、version、bootstrap形式が一致しない。
	MetadataMismatch,
	/// x64のWindows PE実行fileではない。
	UnsupportedImage,
	/// EXE配置への書込みまたは差替えが許可されていない。
	PermissionDenied,
	/// 更新検証後に旧版EXEが変化した。
	InstalledImageChanged,
	/// 同じClient配置で別の更新が進行中である。
	Busy,
	/// maintenance helperを安全に開始できなかった。
	HelperFailure,
	/// 検証済み候補への差替えに失敗した。
	ReplacementFailure,
	/// 旧版backupを復元できなかった。
	RollbackFailure,
	/// 新版のprocessを起動できなかった。
	RestartFailure,
	/// memoryまたはthread等を確保できなかった。
	ResourceExhausted,
} ;

/** @brief 公開Releaseに添付するmanifestの検証済み値。 */
struct ReleaseManifest {
	/// 差替え後のFileVersionとProductVersion。
	UpdateVersion	   version ;
	/// URI path componentとして検証済みのRelease tag。
	std::string		   tag ;
	/// 完成した署名済みClient EXEのSHA-256。
	Sha256			   sha256{} ;
	/// downloadするClient EXEの正確なbyte数。
	std::uint64_t	   size_bytes{0U} ;
	/// 同じ版の署名済みAgent EXEの完全digest。
	Sha256			   agent_sha256{} ;
	/// downloadするAgent EXEの正確なbyte数。
	std::uint64_t	   agent_size_bytes{0U} ;
	/// 二EXEで一致するBridge wire version。
	std::uint16_t	   bridge_protocol{0U} ;
	/**
	 * @brief 全manifest値が同じReleaseを表すか比較する。
	 * @param[in] other 比較対象manifest。
	 * @return 全値一致の場合はtrue。
	 */
	[[nodiscard]] bool operator==( const ReleaseManifest &other) const = default ;
} ;

/** @brief Clientの更新取得先。認証tokenや任意URLは受け付けない。 */
struct UpdateConfiguration {
	/// repositoryが有効で明示的に無効化されていない場合だけtrue。
	bool		enabled{false} ;
	/// HTTPSのGitHub固定host配下で使用するowner/repository。
	std::string repository ;
} ;

/**
 * @brief 四要素decimal versionをoverflowなしで解析する。
 * @param[in] text major.minor.patch.build。
 * @param[out] version 成功時の値。失敗時は0へ初期化する。
 * @return 全文字と四要素を正常に解析した場合はtrue。
 */
[[nodiscard]] bool ParseUpdateVersion( std::string_view text, UpdateVersion &version) noexcept ;

/**
 * @brief 数値versionをUIとmanifest用のASCIIへ変換する。
 * @param[in] version 変換元。
 * @return 四要素decimal表記。
 */
[[nodiscard]] std::string FormatUpdateVersion( const UpdateVersion &version) ;

/**
 * @brief 64文字hexをSHA-256へ変換する。
 * @param[in] text 大小文字を許容するhex digest。
 * @param[out] hash 成功時のdigest。失敗時は0へ初期化する。
 * @return 正確に32 byteを解析できた場合はtrue。
 */
[[nodiscard]] bool		  ParseSha256( std::string_view text, Sha256 &hash) noexcept ;

/**
 * @brief SHA-256を小文字hexへ変換する。
 * @param[in] hash binary digest。
 * @return 64文字hex。
 */
[[nodiscard]] std::string FormatSha256( const Sha256 &hash) ;

/**
 * @brief GitHub owner/repositoryを固定hostの二path要素として検証する。
 * @param[in] repository URLではなくowner/repository。
 * @return 許可されたASCII構文と上限を満たす場合はtrue。
 */
[[nodiscard]] bool		  IsValidUpdateRepository( std::string_view repository) noexcept ;

/**
 * @brief 有限ASCII INIから更新sourceだけを解析する。
 * @param[in] text client.ini全体。[proxy]内容はProxy parserに委譲する。
 * @param[out] configuration 検証済み設定。不在時は指定済みの既定source、失敗時は無効。
 * @return 成功はNone、不正値は固定error。
 */
[[nodiscard]] UpdateError ParseUpdateConfiguration( std::string_view		text,
												   UpdateConfiguration &configuration) noexcept ;

/**
 * @brief 取得したmanifestの必須key、型、size、assetを検証する。
 * @param[in] text 有限のASCII manifest。
 * @param[out] manifest 完全検証後の値。失敗時は空。
 * @return 全値が有効な場合はNone。
 */
[[nodiscard]] UpdateError ParseReleaseManifest( std::string_view text,
											   ReleaseManifest &manifest) noexcept ;

/**
 * @brief URLを固定の最新Release asset pathとして構築する。
 * @param[in] repository 検証済みowner/repository。
 * @return 完全HTTPS URL。不正repositoryでは空。
 */
[[nodiscard]] std::string BuildManifestUrl( std::string_view repository) ;

/**
 * @brief 承認したRelease tagの固定Client asset URLを構築する。
 * @param[in] repository 検証済みowner/repository。
 * @param[in] manifest 検証済みRelease。
 * @return 完全HTTPS URL。不正値では空。
 */
[[nodiscard]] std::string BuildExecutableUrl( std::string_view		repository,
											 const ReleaseManifest &manifest) ;

/**
 * @brief HTTPS redirect先をGitHub配布hostと443だけへ制限する。
 * @param[in] url absolute HTTPS URL。署名付きqueryは解析・記録しない。
 * @return userinfo、fragment、非許可host／portがない場合はtrue。
 */
[[nodiscard]] bool		  IsAllowedUpdateUrl( std::string_view url) noexcept ;

/**
 * @brief 同意したRelease tagの固定Agent asset URLを構築する。
 * @param[in] repository 検証済みowner/repository。
 * @param[in] manifest 検証済みセットRelease。
 * @return 完全HTTPS URL。不正値では空。
 */
[[nodiscard]] std::string BuildAgentExecutableUrl( std::string_view		 repository,
												  const ReleaseManifest &manifest) ;

} // namespace zrdp::client::update
