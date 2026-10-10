//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_settings_windows_test.cpp
 * @brief 隔離fileでDPAPI、Agent INI互換と安全な出力失敗を検証する。
 * @version 01.00.01
 * @date 2026/10/08
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  自作fixture限定のWindows保存testを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  登録済み接続別復元と切替時の保存保全を検証。
 * </pre>
 *
 * @note current-user KnownFolder、実設定、RDP、取引先networkを使用しない。
 */
//------------------------------------------------------------------------------
#include "client/agent_settings/agent_settings_service.h"
#include "client/update/update_windows_handles.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
// Windows設定境界を短く参照するtest用alias
namespace settings = zrdp::client::agent_settings ;

/**
 * @brief 自作fixtureだけへ短いbyte列を書込む。
 * @param[in] path fixture内の明示file。
 * @param[in] text 架空INIまたは破損保存data。
 * @return 全体を書込めた場合はtrue。
 */
[[nodiscard]] bool WriteFixture( const std::filesystem::path &path, std::string_view text)
{
	std::ofstream file( path, std::ios::binary | std::ios::trunc) ;
	file.write( text.data(), static_cast<std::streamsize>( text.size())) ;
	file.close() ;
	return file.good() ;
}

/**
 * @brief 有限の自作fixtureを読み、保存前後を比較する。
 * @param[in] path fixture内file。
 * @return 読んだbyte列。不在なら空。
 */
[[nodiscard]] std::string ReadFixture( const std::filesystem::path &path)
{
	std::ifstream file( path, std::ios::binary) ;
	return {std::istreambuf_iterator<char>( file), std::istreambuf_iterator<char>()} ;
}

/** @brief build以下に作った固定fixtureだけを所有する。 */
class FileFixture final {
public:
	/** @brief 既存directoryを使わず自作fixtureを一つ生成する。 */
	FileFixture()
	{
		directory = std::filesystem::current_path() /
					(L"agent-settings-fixture-" + std::to_wstring( GetCurrentProcessId())) ;
		// 既存file群をcleanupへ巻き込まない
		if( !std::filesystem::create_directory( directory)) {
			throw std::runtime_error( "Fixture exists") ;
		}
	}

	/** @brief 自作固定fileだけを削除し、recursive cleanupを行わない。 */
	~FileFixture()
	{
		std::error_code ignored ;
		std::filesystem::remove( directory / "hardlink.ini", ignored) ;
		std::filesystem::remove( directory / "agent.ini", ignored) ;
		std::filesystem::remove( directory / "input.Ini", ignored) ;
		std::filesystem::remove( directory / "agent-settings.bin", ignored) ;
		std::filesystem::remove( directory, ignored) ;
	}

	/// testが新規所有したdirectory。
	std::filesystem::path directory ;
} ;
} // namespace

// -----------------------------------------------------------------------------
// 実Windows保存境界の契約
// -----------------------------------------------------------------------------

/**
 * @brief Windows DPAPIとlocal fileだけを使い、実ユーザー設定を変更せず確認する。
 * @return 全契約が成立する場合は0。失敗段階は固定code。
 */
int main()
{
	FileFixture						fixture ;
	const auto						storage	 = fixture.directory / "agent-settings.bin" ;
	const auto						ini_path = fixture.directory / "agent.ini" ;
	auto							backend	 = settings::MakeFileAgentSettingsBackend( storage) ;
	settings::AgentSettingsProfiles profiles ;
	// 不在の保存fileだけを正常な新規一覧として扱う
	if( !backend->Load( profiles, {}).succeeded() || !profiles.empty()) {
		return 1 ;
	}

	profiles = {
		{L"Windows Fixture",
		 {{1001U, {{192U, 0U, 2U, 10U}, 8080U}, settings::AgentEndpointPurpose::Svn, true}},
		 settings::BuildAgentConnectionKey( L"192.0.2.1", L"", L"fixture-user")},
		{L"Windows Fixture",
		 {{2001U, {{203U, 0U, 113U, 10U}, 80U}, settings::AgentEndpointPurpose::Trac, true}},
		 settings::BuildAgentConnectionKey( L"192.0.2.2", L"", L"fixture-user")}} ;
	settings::AgentSettingsProfiles loaded ;
	// 同名でも異なる接続keyの登録済み二設定をDPAPI保存からそのまま復元する
	if( !backend->Save( profiles, {}).succeeded() || !backend->Load( loaded, {}).succeeded() ||
	   loaded != profiles) {
		return 2 ;
	}

	const auto encrypted = ReadFixture( storage) ;
	// 実endpointを平文でcacheへ保存しない
	if( encrypted.empty() || encrypted.find( "192.0.2.10") != std::string::npos ||
	   encrypted.find( "Windows Fixture") != std::string::npos) {
		return 3 ;
	}

	auto bad = profiles ;
	bad[0].name.clear() ;
	// 不正保存で正常な旧fileを上書きしない
	if( backend->Save( bad, {}).succeeded() || ReadFixture( storage) != encrypted) {
		return 4 ;
	}

	// 承認済み宛先だけを出力し、既存Agent parserの完全一致検索へ渡す
	if( !backend->Export( ini_path, profiles[0], false, {}).succeeded()) {
		return 5 ;
	}

	const auto						original_ini = ReadFixture( ini_path) ;
	zrdp::agent::network::Allowlist allowlist ;
	std::uint32_t					endpoint_id = 0U ;
	// 一致しないIPやportへの接続を増やさない
	if( !zrdp::agent::network::ParseAllowlist( original_ini, allowlist).succeeded() ||
	   !allowlist.Find( profiles[0].endpoints[0].target, endpoint_id) || endpoint_id != 1001U ||
	   allowlist.Find( {{192U, 0U, 2U, 11U}, 8080U}, endpoint_id)) {
		return 6 ;
	}

	profiles[0].endpoints[0].target.port = 80U ;
	// 利用者が上書き未承認の場合は旧INIを保持する
	if( backend->Export( ini_path, profiles[0], false, {}).succeeded() ||
	   ReadFixture( ini_path) != original_ini) {
		return 7 ;
	}

	// 明示承認後にだけ完全な新版へ置換する
	if( !backend->Export( ini_path, profiles[0], true, {}).succeeded() ||
	   ReadFixture( ini_path) == original_ini) {
		return 8 ;
	}

	const auto updated_ini			  = ReadFixture( ini_path) ;
	profiles[0].endpoints[0].approved = false ;
	// 未承認宛先を部分出力せず、旧INIを変更しない
	if( backend->Export( ini_path, profiles[0], true, {}).succeeded() ||
	   ReadFixture( ini_path) != updated_ini) {
		return 9 ;
	}

	profiles[0].endpoints[0].approved = true ;
	// 任意file名へ出力範囲を拡大しない
	if( backend->Export( fixture.directory / "client.ini", profiles[0], true, {}).error !=
		   settings::AgentSettingsError::InvalidPath ||
	   std::filesystem::exists( fixture.directory / "client.ini")) {
		return 10 ;
	}

	settings::AgentSettingsProfile imported ;
	// 既存INIには業務利用の承認情報がないため、すべて未承認で読む
	if( !backend->Import( ini_path, L"Imported", imported, {}).succeeded() ||
	   imported.endpoints.size() != 1U || imported.endpoints[0].approved) {
		return 11 ;
	}

	// 拡張子の大小文字だけが違っても同じINIとして扱う
	if( !WriteFixture( fixture.directory / "input.Ini", updated_ini) ||
	   !backend->Import( fixture.directory / "input.Ini", L"Imported", imported, {}).succeeded()) {
		return 12 ;
	}

	{
		zrdp::client::update::detail::KernelHandle lock(
			CreateFileW( ini_path.c_str(), GENERIC_READ, 0U, nullptr, OPEN_EXISTING,
						FILE_ATTRIBUTE_NORMAL, nullptr)) ;
		// 使用中fileへfallbackや別名保存を行わない
		if( !lock.valid() || backend->Export( ini_path, profiles[0], true, {}).succeeded()) {
			return 13 ;
		}
	}

	// file lockで失敗した場合も元の全byteを保持する
	if( ReadFixture( ini_path) != updated_ini) {
		return 14 ;
	}

	const auto hardlink = fixture.directory / "hardlink.ini" ;
	// 自作fileのhard linkを作り、link入力と出力を両方拒否する
	if( !CreateHardLinkW( hardlink.c_str(), ini_path.c_str(), nullptr) ||
	   backend->Import( ini_path, L"Imported", imported, {}).succeeded() ||
	   backend->Export( ini_path, profiles[0], true, {}).succeeded()) {
		return 15 ;
	}

	std::filesystem::remove( hardlink) ;
	std::stop_source stopped ;
	stopped.request_stop() ;
	// 取消要求後は既存fileを変更せず固定Cancelledを返す
	if( backend->Save( profiles, stopped.get_token()).error !=
		   settings::AgentSettingsError::Cancelled ||
	   ReadFixture( storage) != encrypted) {
		return 16 ;
	}

	// 破損ciphertextを正常な空一覧へ置換しない
	if( !WriteFixture( storage, "broken") || backend->Load( loaded, {}).succeeded() ||
	   !loaded.empty()) {
		return 17 ;
	}

	// 実在する空fileも新規未保存状態とは区別する
	if( !WriteFixture( storage, "") || backend->Load( loaded, {}).succeeded()) {
		return 18 ;
	}

	// 固定cache名以外を保存先にしない
	auto wrong = settings::MakeFileAgentSettingsBackend( fixture.directory / "other.bin") ;
	// 隔離test入口もproductionの固定basename契約を保持する
	if( wrong->Save( profiles, {}).error != settings::AgentSettingsError::InvalidPath) {
		return 19 ;
	}

	// 失敗経路の自作一時fileを残さない
	for( const auto &entry : std::filesystem::directory_iterator( fixture.directory)) {
		// 自作固定fixture以外の残留はcleanup契約違反とする
		if( entry.path().filename().native().starts_with( L".zrdp-agent-settings-")) {
			return 20 ;
		}
	}

	std::cout << "Agent settings Windows backend passed\n" ;
	return 0 ;
}
