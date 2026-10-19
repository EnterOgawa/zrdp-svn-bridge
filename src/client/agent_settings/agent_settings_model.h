//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_settings_model.h
 * @brief Clientで編集するAgent接続先と有限profileの検証契約を定義する。
 * @version 01.00.02
 * @date 2026/10/08
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  Agent設定画面の検証・INI・保存形式を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  接続keyと保存形式2の互換読込みを追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  接続変更と登録済み宛先だけの表示・起動を統一。
 * </pre>
 *
 * @note 入出力とnetwork操作を持たず、既存Agent parserで完全一致設定を検証する。
 */
//------------------------------------------------------------------------------
#pragma once

#include "agent/network/allowlist.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zrdp::client::agent_settings {

// 同時接続数と同じ有限数だけ設定profileを保持する
inline constexpr std::size_t kMaximumAgentProfiles			 = 16U ;
// RDP接続名と揃えたUI用設定名のUTF-16 code unit上限
inline constexpr std::size_t kMaximumAgentProfileName		 = 64U ;
// DPAPIへ渡す保存用平文の上限。protocolとは独立する
inline constexpr std::size_t kMaximumAgentSettingsPlaintext	 = 64U * 1024U ;
// 暗号化済み保存fileに許可する有限byte数
inline constexpr std::size_t kMaximumAgentSettingsCiphertext = 128U * 1024U ;

/** @brief HTTPの用途をUIだけで記録する値。Agentの宛先判断はIPv4・portだけで行う。 */
enum class AgentEndpointPurpose : std::uint8_t {
	/// 既存INIから読んだ用途未確認のdraft。承認済みでは使用しない。
	Unspecified = 0U,
	/// HTTP SVNの利用先。
	Svn			= 1U,
	/// 明示承認を必要とするHTTP Tracの利用先。
	Trac		= 2U,
} ;

/** @brief 実endpointやfile pathを持たない設定の固定失敗分類。 */
enum class AgentSettingsError : std::uint32_t {
	/// 設定または操作が成功した。
	None = 0U,
	/// 名前が空、不正UTF-16、制御文字、上限超過である。
	InvalidProfileName,
	/// 同名profileの重複またはprofile件数上限違反である。
	InvalidProfiles,
	/// IPv4、port、匿名ID、重複等がAgentの契約を満たさない。
	InvalidEndpoint,
	/// 保存形式が破損、未知版、過大または末尾に余分なdataがある。
	InvalidData,
	/// 一件以上の接続先が明示承認されていない。
	UnapprovedEndpoint,
	/// INIへ出力できる接続先が一件もない。
	EmptyEndpoints,
	/// Windowsユーザーの暗号化または復号に失敗した。
	ProtectionFailed,
	/// local fileの読取り・書込み・置換に失敗した。
	FileFailure,
	/// 固定名以外やlink、network share等の出力先を拒否した。
	InvalidPath,
	/// workerの終了または未実行要求の取消である。
	Cancelled,
	/// memoryまたはthread等を確保できなかった。
	ResourceExhausted,
} ;

/** @brief 入力内容を含まない検証結果。 */
struct AgentSettingsResult {
	/// 成功時はNone。
	AgentSettingsError			   error{AgentSettingsError::None} ;
	/// 既存Agent parserの固定分類。使用しない場合はNone。
	agent::network::AllowlistError allowlist_error{agent::network::AllowlistError::None} ;
	/// 入力行に起因する失敗の1-origin番号。使用しない場合は0。
	std::uint32_t				   line{0U} ;
	/**
	 * @brief 全体を採用できたか判定する。
	 * @return 失敗分類がない場合だけtrue。
	 */
	[[nodiscard]] bool			   succeeded() const noexcept
	{
		return error == AgentSettingsError::None ;
	}
} ;

/** @brief Clientが記録する一件のHTTP接続先。credentialやURL pathは持たない。 */
struct AgentEndpointSetting {
	/// Agent INIで使う非0の匿名ID。
	std::uint32_t		 endpoint_id{0U} ;
	/// DNS、CIDR、rangeを表現しない完全一致IPv4とTCP port。
	protocol::OpenTarget target{} ;
	/// UIへ復元するHTTPの用途。
	AgentEndpointPurpose purpose{AgentEndpointPurpose::Svn} ;
	/// この接続先の業務利用を利用者が明示承認した場合だけtrue。
	bool				 approved{false} ;
	/**
	 * @brief 承認と用途を含め、同一の編集状態か比較する。
	 * @param[in] other 比較対象。
	 * @return 全fieldが一致する場合はtrue。
	 */
	[[nodiscard]] bool	 operator==( const AgentEndpointSetting &other) const = default ;
} ;

/** @brief 一つのcustomer／接続profileに対応する有限設定。 */
struct AgentSettingsProfile {
	/// Client UIで使用する設定名。Agentやlogへ渡さない。
	std::wstring					  name ;
	/// 最大128件。未承認draftを暗号化保存してもINI出力は許可しない。
	std::vector<AgentEndpointSetting> endpoints ;
	/// RDP接続先・domain・userの有限key。空は旧版の未関連付け設定。
	std::wstring					  connection_key ;
	/**
	 * @brief 設定名とentry順を含む完全な編集状態を比較する。
	 * @param[in] other 比較対象。
	 * @return 全fieldが一致する場合はtrue。
	 */
	[[nodiscard]] bool				  operator==( const AgentSettingsProfile &other) const = default ;
} ;

/** @brief credentialを含まない有限profile一覧。 */
using AgentSettingsProfiles = std::vector<AgentSettingsProfile> ;

/** @brief 接続先とlogon組を曖昧さのない長さ付きkeyへ変換する。
 * @param[in] server 利用者が指定したRDP接続先。
 * @param[in] domain 正規化済みdomain。
 * @param[in] user 正規化済みuser。
 * @return 有限key。空接続先、不正文字、上限超過では空。
 * @note password、DNS探索、接続名を使用せず、runtime診断へ出力しない。
 */
[[nodiscard]] std::wstring
BuildAgentConnectionKey( std::wstring_view server, std::wstring_view domain, std::wstring_view user) ;
/** @brief 空でない接続keyをWindows ordinalで照合する。
 * @param[in] left 保存key。
 * @param[in] right 現在接続のkey。
 * @return 大小文字差だけならtrue。空keyは他接続へ継承しない。
 */
[[nodiscard]] bool SameAgentConnectionKey( std::wstring_view left, std::wstring_view right) noexcept ;

/**
 * @brief 設定名を大小文字差だけなら同じWindows ordinal名として比較する。
 * @param[in] left 比較元。
 * @param[in] right 比較先。
 * @return 正常な名前でordinal一致する場合はtrue。
 */
[[nodiscard]] bool SameAgentProfileName( std::wstring_view left, std::wstring_view right) noexcept ;
/**
 * @brief entryの構造と共有allowlist契約を検証する。承認済みかは保存と別に扱う。
 * @param[in] profile 編集済みprofile。
 * @return 完全なprofile構造なら成功。
 */
[[nodiscard]] AgentSettingsResult
ValidateAgentSettingsProfile( const AgentSettingsProfile &profile) noexcept ;
/**
 * @brief profile上限、接続key重複、旧形式の名前重複、各entryを完全検証する。
 * @param[in] profiles 一括保存する一覧。
 * @return 全体を採用できる場合だけ成功。
 */
[[nodiscard]] AgentSettingsResult
ValidateAgentSettingsProfiles( const AgentSettingsProfiles &profiles) noexcept ;
/**
 * @brief 接続に完全一致する登録済み宛先だけを起動用snapshotへ抽出する。
 * @param[in] profiles 検証済み保存一覧。未関連付け旧設定を含んでもよい。
 * @param[in] connection_key 現在接続の空でないkey。
 * @return 一致したprofileの承認済み宛先だけ。空key・不正・不一致・資源失敗はnullopt。
 * @note 元一覧を変更せず、未承認候補や別接続を自動関連付けしない。
 */
[[nodiscard]] std::optional<AgentSettingsProfile>
FindRegisteredAgentSettingsProfile( const AgentSettingsProfiles &profiles,
								   std::wstring_view			connection_key) noexcept ;

/**
 * @brief 接続key一致、または未関連付けの同名profileを置換する。失敗時は元一覧を保持する。
 * @param[in,out] profiles 保存候補の一覧。
 * @param[in] profile 保存する完全な一件。
 * @return 新しい一覧全体を検証できた場合だけ成功。
 */
[[nodiscard]] AgentSettingsResult
								  UpsertAgentSettingsProfile( AgentSettingsProfiles		&profiles,
															 const AgentSettingsProfile &profile) noexcept ;
/**
 * @brief 編集欄のIPv4とportを共有Agent parserで検証する。
 * @param[in] ipv4 strict dotted-decimal文字列。
 * @param[in] port 十進TCP port。
 * @param[out] target 成功時だけ公開する完全一致宛先。
 * @return Agentと同じ判定で正常なら成功。
 */
[[nodiscard]] AgentSettingsResult ParseAgentSettingsTarget( std::wstring_view	 ipv4,
														   std::wstring_view	 port,
														   protocol::OpenTarget &target) noexcept ;
/**
 * @brief 検証済みIPv4をUI用の十進表記へ戻す。
 * @param[in] target 完全一致宛先。
 * @return IPv4 literal。portは含めない。
 */
[[nodiscard]] std::wstring		  FormatAgentSettingsIpv4( const protocol::OpenTarget &target) ;
/**
 * @brief 全entryが明示承認済みの場合だけAgent互換INIを生成する。
 * @param[in] profile 出力する一件。
 * @param[out] text ASCII INI。失敗時は空。
 * @return 全体を採用できる場合だけ成功。
 */
[[nodiscard]] AgentSettingsResult GenerateAgentSettingsIni( const AgentSettingsProfile &profile,
														   std::string &text) noexcept ;
/**
 * @brief 既存INIをAgent parserで読み、用途と承認を推定せずdraftへ変換する。
 * @param[in] text 利用者が選択した有限INI。
 * @param[in] profile_name Client側の有効な設定名。
 * @param[out] profile 成功時の未承認entry一覧。失敗時は空。
 * @return Agent互換INI全体を読めた場合だけ成功。
 */
[[nodiscard]] AgentSettingsResult ParseAgentSettingsIni( std::string_view	  text,
														std::wstring_view	  profile_name,
														AgentSettingsProfile &profile) noexcept ;
/**
 * @brief DPAPIへ渡す接続key付き独立形式2の有限平文を生成する。
 * @param[in] profiles 完全な保存一覧。
 * @param[out] bytes 明示的little-endian形式。失敗時は空。
 * @return 全体をencodeできた場合だけ成功。
 */
[[nodiscard]] AgentSettingsResult
EncodeAgentSettingsProfiles( const AgentSettingsProfiles &profiles,
							std::vector<std::byte>		&bytes) noexcept ;
/**
 * @brief DPAPI復号後の形式1／2をoverflowと全上限を検証して読む。
 * @param[in] bytes 有限平文。余分な末尾を許可しない。
 * @param[out] profiles 完全検証後の一覧。失敗時は空。
 * @return 全体が有効な場合だけ成功。
 */
[[nodiscard]] AgentSettingsResult
DecodeAgentSettingsProfiles( std::span<const std::byte> bytes,
							AgentSettingsProfiles	  &profiles) noexcept ;

} // namespace zrdp::client::agent_settings
