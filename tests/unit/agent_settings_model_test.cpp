//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_settings_model_test.cpp
 * @brief Agent設定の入力制限、承認、既存INI互換と有限保存形式を検証する。
 * @version 01.00.02
 * @date 2026/10/08
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  Agent設定modelの正常系と拒否条件を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  接続別設定・自動Agent管理の回帰検証を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  登録済み接続別復元と切替時の保存保全を検証。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/agent_settings/agent_settings_model.h"

#include <array>
#include <iostream>
#include <utility>

namespace {
// Clientの設定境界を短く参照するtest用alias
namespace settings = zrdp::client::agent_settings ;

/**
 * @brief 文書例示用の二宛先を持つ承認済みprofileを生成する。
 * @return 実networkへ接続しない固定fixture。
 */
[[nodiscard]] settings::AgentSettingsProfile Fixture()
{
	return {L"Test Profile",
			{{1001U, {{192U, 0U, 2U, 10U}, 80U}, settings::AgentEndpointPurpose::Svn, true},
			 {1002U, {{192U, 0U, 2U, 10U}, 8080U}, settings::AgentEndpointPurpose::Trac, true}}} ;
}
} // namespace

// -----------------------------------------------------------------------------
// 入力、INI互換、保存形式の契約
// -----------------------------------------------------------------------------

/**
 * @brief 正常な設定とすべての拒否条件をnetworkなしで検証する。
 * @return 全契約が成立する場合は0。失敗段階は固定code。
 */
int main()
{
	// 同一IPv4の別portを許可し、既存Agentの完全一致検索へ渡す
	auto							profile = Fixture() ;
	std::string						ini ;
	zrdp::agent::network::Allowlist parsed ;
	std::uint32_t					endpoint_id = 0U ;
	// 生成した全宛先がAgent側の完全一致検索に到達することを必須にする
	if( !settings::ValidateAgentSettingsProfile( profile).succeeded() ||
	   !settings::GenerateAgentSettingsIni( profile, ini).succeeded() ||
	   !zrdp::agent::network::ParseAllowlist( ini, parsed).succeeded() || parsed.size() != 2U ||
	   !parsed.Find( profile.endpoints[0].target, endpoint_id) ||
	   !parsed.Find( profile.endpoints[1].target, endpoint_id)) {
		return 1 ;
	}

	// INIの用途・承認は復元せず、再承認なしの出力を拒否する
	settings::AgentSettingsProfile imported ;
	// 未承認の復元状態からのINI出力を禁止する
	if( !settings::ParseAgentSettingsIni( ini, L"Imported", imported).succeeded() ||
	   imported.endpoints.size() != 2U || imported.endpoints[1].approved ||
	   imported.endpoints[1].purpose != settings::AgentEndpointPurpose::Unspecified ||
	   settings::GenerateAgentSettingsIni( imported, ini).error !=
		   settings::AgentSettingsError::UnapprovedEndpoint ||
	   !ini.empty()) {
		return 2 ;
	}

	// 空draftの保存は許可するが、接続可能なINIとしては出力しない
	const settings::AgentSettingsProfile empty{L"Empty", {}} ;
	// 空設定を接続可能な配布設定と表示しない
	if( !settings::ValidateAgentSettingsProfile( empty).succeeded() ||
	   settings::GenerateAgentSettingsIni( empty, ini).error !=
		   settings::AgentSettingsError::EmptyEndpoints) {
		return 3 ;
	}

	// IP、port、任意host、CIDR、range等を共有parserと同じ境界で拒否する
	const std::array<std::pair<std::wstring_view, std::wstring_view>, 14U> invalid{
		{{L"", L"80"},
		 {L"192.0.2.256", L"80"},
		 {L"192.0.2", L"80"},
		 {L"example.invalid", L"80"},
		 {L"192.0.2.0/24", L"80"},
		 {L"192.0.2.*", L"80"},
		 {L"http://192.0.2.10", L"80"},
		 {L"::1", L"80"},
		 {L"192.0.2.10", L"0"},
		 {L"192.0.2.10", L"65536"},
		 {L"192.0.2.10", L"80-90"},
		 {L"192.0.2.10", L"*"},
		 {L"192.0.2.10", L"+80"},
		 {L"１９２.0.2.10", L"80"}}} ;
	// 不正入力は宛先を部分公開せず、既定portへfallbackしない
	for( const auto &[address, port] : invalid) {
		zrdp::protocol::OpenTarget target{} ;
		// 一つでも許可された場合は境界違反として停止する
		if( settings::ParseAgentSettingsTarget( address, port, target).succeeded() ||
		   target.port != 0U) {
			return 4 ;
		}
	}

	zrdp::protocol::OpenTarget maximum{} ;
	// 上限portと通常IPv4を正確に復元する
	if( !settings::ParseAgentSettingsTarget( L"203.0.113.20", L"65535", maximum).succeeded() ||
	   maximum.port != 65535U || settings::FormatAgentSettingsIpv4( maximum) != L"203.0.113.20") {
		return 5 ;
	}

	// 同じ宛先と匿名IDの重複、未知用途、上限超過をすべて拒否する
	auto bad				= profile ;
	bad.endpoints[1].target = bad.endpoints[0].target ;
	// 同じIPとportを複数のIDで重複登録しない
	if( settings::ValidateAgentSettingsProfile( bad).succeeded()) {
		return 6 ;
	}

	bad							 = profile ;
	bad.endpoints[1].endpoint_id = bad.endpoints[0].endpoint_id ;
	// 別portでも同じ匿名IDを使わない
	if( settings::ValidateAgentSettingsProfile( bad).succeeded()) {
		return 7 ;
	}

	bad							 = profile ;
	bad.endpoints[0].endpoint_id = 0U ;
	// connection-levelの予約IDをINIへ出さない
	if( settings::ValidateAgentSettingsProfile( bad).succeeded()) {
		return 8 ;
	}

	bad						 = profile ;
	bad.endpoints[0].purpose = static_cast<settings::AgentEndpointPurpose>( 3U) ;
	// 将来用途を現在のSVNへ推定しない
	if( settings::ValidateAgentSettingsProfile( bad).succeeded()) {
		return 9 ;
	}

	settings::AgentSettingsProfiles profiles{profile, imported, empty} ;
	std::vector<std::byte>			encoded ;
	settings::AgentSettingsProfiles decoded ;
	// 承認、用途、entry順、空draftを完全にround-tripする
	if( !settings::EncodeAgentSettingsProfiles( profiles, encoded).succeeded() ||
	   !settings::DecodeAgentSettingsProfiles( encoded, decoded).succeeded() ||
	   decoded != profiles) {
		return 10 ;
	}

	// すべての途中切れを空一覧の失敗として返す
	for( std::size_t size = 0U; size < encoded.size(); ++size) {
		decoded = profiles ;
		// 部分profileの採用と欠落byteの補完を許可しない
		if( settings::DecodeAgentSettingsProfiles( std::span<const std::byte>( encoded).first( size),
												 decoded)
			   .succeeded() ||
		   !decoded.empty()) {
			return 11 ;
		}
	}

	auto corrupted = encoded ;
	corrupted[7]   = std::byte{3} ;
	// 未知の保存版を旧版互換と推測しない
	if( settings::DecodeAgentSettingsProfiles( corrupted, decoded).succeeded()) {
		return 12 ;
	}

	corrupted	 = encoded ;
	corrupted[8] = std::byte{17} ;
	// allocation前にprofile数上限を守る
	if( settings::DecodeAgentSettingsProfiles( corrupted, decoded).succeeded()) {
		return 13 ;
	}

	corrupted = encoded ;
	corrupted.push_back( std::byte{0}) ;
	// 余分な末尾を黙って無視しない
	if( settings::DecodeAgentSettingsProfiles( corrupted, decoded).succeeded()) {
		return 14 ;
	}

	const std::size_t first_entry = 10U + 2U + profile.name.size() * 2U + 4U ;
	corrupted					  = encoded ;
	corrupted[first_entry + 11U]  = std::byte{2} ;
	// 任意flagを承認済みとして読むことを防ぐ
	if( settings::DecodeAgentSettingsProfiles( corrupted, decoded).succeeded()) {
		return 15 ;
	}

	corrupted					 = encoded ;
	corrupted[first_entry + 10U] = std::byte{0} ;
	// 未知用途を復号後にも拒否する
	if( settings::DecodeAgentSettingsProfiles( corrupted, decoded).succeeded()) {
		return 16 ;
	}

	// 大小文字だけが異なる名を保存で置換し、読込みでは重複を拒否する
	auto renamed = profile ;
	renamed.name = L"test profile" ;
	renamed.endpoints.pop_back() ;
	// 同名置換で一覧を増殖させず最新一件に統合する
	if( !settings::UpsertAgentSettingsProfile( profiles, renamed).succeeded() ||
	   profiles.size() != 3U || profiles[0] != renamed) {
		return 17 ;
	}

	profiles.push_back( profile) ;
	// 重複一覧を後勝ちで採用しない
	if( settings::ValidateAgentSettingsProfiles( profiles).succeeded()) {
		return 18 ;
	}

	profiles.clear() ;
	// 最大件数の有効な名前付きdraftを準備する
	for( std::size_t index = 0U; index < settings::kMaximumAgentProfiles; ++index) {
		profiles.push_back( {L"Profile " + std::to_wstring( index), {}}) ;
	}

	const auto preserved = profiles ;
	// 上限超過で古い設定を暗黙に削除しない
	if( settings::UpsertAgentSettingsProfile( profiles, profile).succeeded() ||
	   profiles != preserved) {
		return 19 ;
	}

	bad		 = profile ;
	bad.name = std::wstring( 65U, L'x') ;
	// 上限超過名を64文字へ切って別設定として保存しない
	if( settings::ValidateAgentSettingsProfile( bad).succeeded()) {
		return 20 ;
	}

	bad.name = L"   " ;
	// 空白だけの名前をkeyにしない
	if( settings::ValidateAgentSettingsProfile( bad).succeeded()) {
		return 21 ;
	}

	bad.name = std::wstring( 1U, static_cast<wchar_t>( 0xd800U)) ;
	// 孤立surrogateをDPAPI平文へ渡さない
	if( settings::ValidateAgentSettingsProfile( bad).succeeded()) {
		return 22 ;
	}

	bad = profile ;
	bad.endpoints.resize( 129U) ;
	// 宛先件数もAgentと同じ上限へ固定する
	if( settings::ValidateAgentSettingsProfile( bad).succeeded()) {
		return 23 ;
	}

	const auto key		 = settings::BuildAgentConnectionKey( L"PC-A", L"DOMAIN", L"user") ;
	const auto other_key = settings::BuildAgentConnectionKey( L"PC-B", L"DOMAIN", L"user") ;
	// 接続名に依存せず、別PCと別userは同じ設定へ束ねない
	if( key.empty() ||
	   !settings::SameAgentConnectionKey(
		   key, settings::BuildAgentConnectionKey( L"pc-a", L"domain", L"USER")) ||
	   settings::SameAgentConnectionKey( key, other_key) ||
	   settings::SameAgentConnectionKey(
		   key, settings::BuildAgentConnectionKey( L"PC-A", L"DOMAIN", L"other")) ||
	   settings::SameAgentConnectionKey( L"", L"") ||
	   !settings::BuildAgentConnectionKey( L"", L"", L"").empty() ||
	   !settings::BuildAgentConnectionKey( L"PC\nA", L"", L"").empty()) {
		return 24 ;
	}

	auto bound			  = profile ;
	bound.connection_key  = key ;
	auto second			  = profile ;
	second.connection_key = other_key ;
	settings::AgentSettingsProfiles bound_profiles{bound, second} ;
	// 表示名が同じでも接続組が異なれば独立して保存・復元する
	if( !settings::EncodeAgentSettingsProfiles( bound_profiles, encoded).succeeded() ||
	   encoded[7] != std::byte{2} ||
	   !settings::DecodeAgentSettingsProfiles( encoded, decoded).succeeded() ||
	   decoded != bound_profiles) {
		return 25 ;
	}

	bound.name					   = L"renamed" ;
	bound.endpoints[0].target.port = 8082U ;
	// 表示名変更は同じ接続の一件だけを置換する
	if( !settings::UpsertAgentSettingsProfile( bound_profiles, bound).succeeded() ||
	   bound_profiles.size() != 2U || bound_profiles[1] != second) {
		return 26 ;
	}

	bound_profiles.push_back( bound) ;
	// 同じ接続組の重複は名前を変えても採用しない
	if( settings::ValidateAgentSettingsProfiles( bound_profiles).succeeded()) {
		return 27 ;
	}

	std::vector<std::byte> legacy ;
	// 旧形式の空一覧を読み込んでも未関連付けとして保全する
	if( !settings::EncodeAgentSettingsProfiles( {}, legacy).succeeded()) {
		return 28 ;
	}

	legacy[7] = std::byte{1} ;
	// 旧形式1の読み込み契約は引き続き有効である
	if( !settings::DecodeAgentSettingsProfiles( legacy, decoded).succeeded() || !decoded.empty()) {
		return 29 ;
	}

	// 非空の旧形式1も接続keyなしで完全に読み込む
	if( !settings::EncodeAgentSettingsProfiles( {profile}, legacy).succeeded()) {
		return 30 ;
	}

	const auto legacy_key_offset = 12U + profile.name.size() * 2U ;
	legacy.erase( legacy.begin() + legacy_key_offset, legacy.begin() + legacy_key_offset + 2U) ;
	legacy[7] = std::byte{1} ;
	// 旧profileを別接続へ自動関連付けしない
	if( !settings::DecodeAgentSettingsProfiles( legacy, decoded).succeeded() ||
	   decoded != settings::AgentSettingsProfiles{profile} || !decoded[0].connection_key.empty()) {
		return 31 ;
	}

	auto mixed					= Fixture() ;
	mixed.connection_key		= settings::BuildAgentConnectionKey( L"192.0.2.1", L"", L"user") ;
	mixed.endpoints[1].approved = false ;
	auto independent			= Fixture() ;
	independent.connection_key	= settings::BuildAgentConnectionKey( L"192.0.2.2", L"", L"user") ;
	independent.endpoints[0].target.ipv4 = {203U, 0U, 113U, 20U} ;
	settings::AgentSettingsProfiles stored{Fixture(), mixed, independent} ;
	const auto						before_projection = stored ;
	auto registered = settings::FindRegisteredAgentSettingsProfile( stored, mixed.connection_key) ;
	std::string registered_ini ;
	// 一致接続の登録済み宛先だけを出力し、hidden候補と旧未関連付け設定を変更しない
	if( !registered || registered->endpoints.size() != 1U ||
	   registered->endpoints[0] != mixed.endpoints[0] ||
	   !settings::GenerateAgentSettingsIni( *registered, registered_ini).succeeded() ||
	   registered_ini.find( ":8080") != std::string::npos || stored != before_projection ||
	   settings::FindRegisteredAgentSettingsProfile( stored, L"") ||
	   settings::FindRegisteredAgentSettingsProfile(
		   stored, settings::BuildAgentConnectionKey( L"192.0.2.3", L"", L"user")) ||
	   settings::FindRegisteredAgentSettingsProfile( {Fixture()}, mixed.connection_key)) {
		return 32 ;
	}

	registered = settings::FindRegisteredAgentSettingsProfile( stored, independent.connection_key) ;
	// 同名profileでも別接続の登録済み宛先を返す
	if( !registered || *registered != independent) {
		return 33 ;
	}

	mixed.endpoints[0].approved = false ;
	registered = settings::FindRegisteredAgentSettingsProfile( {mixed}, mixed.connection_key) ;
	// 一致profileがあっても登録済み0件なら起動用INIを生成できない
	if( !registered || !registered->endpoints.empty() ||
	   settings::GenerateAgentSettingsIni( *registered, registered_ini).succeeded()) {
		return 34 ;
	}

	mixed.endpoints[0].target.port = 0U ;
	// 破損した登録情報を空や部分的な成功へ変換しない
	if( settings::FindRegisteredAgentSettingsProfile( {mixed}, mixed.connection_key)) {
		return 35 ;
	}

	std::cout << "Agent settings model passed\n" ;
	return 0 ;
}
