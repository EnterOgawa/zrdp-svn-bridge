//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 * @file agent_setup_test.cpp
 * @brief 隔離fixtureで専用driveと初期配置の拒否・保全を検証する。
 * @version 01.00.07
 * @date 2026/10/09
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/09  実RDPなしの初期配置試験を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  起動と設定の分離、tray常駐、backup付き更新を追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  トレイ終了後の同じRDP内Agent再起動を修正。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  接続別設定・自動Agent管理の回帰検証を追加。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  隔離EXEのsetup拒否がdialogなしで終了することを検証。
 * Ver.01.00.07   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "common/agent_setup.h"
#include "agent/app/agent_configuration_policy.h"
#include "agent/app/agent_tray_window.h"
#include "client/update/update_image_verifier.h"
#include "client/update/update_windows_handles.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
#include <memory>

namespace {
/** @brief testが新規作成したfileだけを読み取る。
 * @param[in] path 隔離fixture。
 * @return byte列。読み取れなければ空。
 */
std::string Read( const std::filesystem::path &path)
{
	std::ifstream file( path, std::ios::binary) ;
	return {std::istreambuf_iterator<char>( file), std::istreambuf_iterator<char>()} ;
}

/** @brief 隔離EXEのsetup拒否がmodalなしで終了し、固定codeを記録するか検証する。
 * @param[in] base このtestが新規作成したfixture親directory。
 * @param[in] source resource検証済みAgent EXE。製品の配置先ではない。
 * @return sessionまたは固定転送元pathの拒否で終了し、logが一致した場合true。
 * @note 固定--rdp-setupだけを非転送pathで実行し、SVC、実INI、接続先へ進まない。
 */
bool TestSetupWithoutDialog( const std::filesystem::path &base, const std::filesystem::path &source)
{
	const auto directory = base / L"no-dialog" ;
	// 既存directoryをfixtureとして流用せず、所有範囲を固定する
	if( !std::filesystem::create_directory( directory)) {
		return false ;
	}

	const auto image = directory / L"ZRdpSvnAgent.exe" ;
	std::filesystem::copy_file( source, image) ;
	std::wstring command = L"\"" + image.native() + L"\" --rdp-setup" ;
	STARTUPINFOW startup{} ;
	startup.cb = sizeof( startup) ;
	PROCESS_INFORMATION process{} ;
	// 固定fixtureだけをconsoleなしで起動し、shellや任意argumentを使わない
	if( !CreateProcessW( image.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
					   nullptr, directory.c_str(), &startup, &process)) {
		return false ;
	}

	zrdp::client::update::detail::KernelHandle process_owner( process.hProcess) ;
	zrdp::client::update::detail::KernelHandle thread_owner( process.hThread) ;
	// dialogで停滞した場合は自分が生成したtest childだけを終了して失敗にする
	if( WaitForSingleObject( process_owner.get(), 5000U) != WAIT_OBJECT_0) {
		static_cast<void>( TerminateProcess( process_owner.get(), 99U)) ;
		static_cast<void>( WaitForSingleObject( process_owner.get(), 1000U)) ;
		return false ;
	}

	DWORD exit_code = 0U ;
	// console sessionなら20、RDP sessionでも非転送fixtureなので21で拒否する
	if( !GetExitCodeProcess( process_owner.get(), &exit_code) ||
	   (exit_code != 20U && exit_code != 21U)) {
		return false ;
	}

	const auto log = directory / L"logs" / L"ZRdpSvnAgent.log" ;
	const bool logged =
		Read( log).find( "exit_code=" + std::to_string( exit_code)) != std::string::npos ;
	process_owner.reset() ;
	thread_owner.reset() ;
	// 作成した固定EXEとlogだけを回収し、既存fileや任意directoryを削除しない
	std::filesystem::remove( log) ;
	std::filesystem::remove( directory / L"logs") ;
	std::filesystem::remove( image) ;
	std::filesystem::remove( directory) ;
	return logged ;
}

/** @brief 本番Shellへ触れず専用threadのicon lifecycleを確認する。
 * @param[in] instance Agentのresourceだけをロードしたmodule。
 * @return 全生成・更新・復帰・終了・失敗条件を満たす場合true。
 */
[[nodiscard]] bool TrayFixtureTests( HINSTANCE instance)
{
	using zrdp::agent::app::AgentTrayWindow ;
	using zrdp::agent::app::AgentTrayStatus ;
	std::atomic_bool stop{false}, ready_tip{false} ;
	std::atomic_uint added{0U}, deleted{0U}, modified{0U}, version{0U} ;
	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief fake通知領域でiconの追加・版設定・更新・削除を数える。
	 * @param[in] action AgentTrayWindowからの固定notification操作。
	 * @param[in] data 固定tooltipとcallback版の検査対象。読取りだけを行う。
	 * @return 固定true。実Shell通知領域へは公開しない。
	 * @endcond
	 */
	AgentTrayWindow	 tray( instance, stop, [&]( DWORD action, NOTIFYICONDATAW &data) {
		 // fakeだけで追加回数とVERSION_4の契約を記録する
		 if( action == NIM_ADD) {
			 ++added ;
		 }

		 // 表示変更もpayloadなしの固定文字列だけを確認する
		 if( action == NIM_MODIFY) {
			 ++modified ;
			 ready_tip.store( std::wstring_view( data.szTip).find( L"接続済み") !=
							 std::wstring_view::npos) ;
		 }

		 // 登録のたびに新callback版を指定する
		 if( action == NIM_SETVERSION && data.uVersion == NOTIFYICON_VERSION_4) {
			 ++version ;
		 }

		 // 自分の一つのiconだけが終了で削除される
		 if( action == NIM_DELETE) {
			 ++deleted ;
		 }

		 return true ;
	 }) ;
	// fake Shellでも所有HWNDと初期iconの生成が必要
	if( !tray.Start() || tray.window() == nullptr || added != 1U || version != 1U || stop) {
		return false ;
	}

	tray.SetStatus( AgentTrayStatus::Ready) ;
	const auto deadline = GetTickCount64() + 2000U ;
	// 独立UI timerが数値状態を表示へ変換するまで最大二秒だけ待つ
	while( !ready_tip && GetTickCount64() < deadline) {
		Sleep( 10U) ;
	}

	// timer状態反映をAPI受付だけで成功としない
	if( !ready_tip || modified != 1U) {
		return false ;
	}

	PostMessageW( tray.window(), RegisterWindowMessageW( L"TaskbarCreated"), 0U, 0L) ;
	const auto restart_deadline = GetTickCount64() + 2000U ;
	// 自作通知でExplorer再起動時の再登録だけを隔離検証する
	while( added < 2U && GetTickCount64() < restart_deadline) {
		Sleep( 10U) ;
	}

	tray.Stop() ;
	// shutdownはI/O停止flagへ誤伝播せず、iconを一回削除する
	if( added != 2U || version != 2U || deleted != 1U || tray.window() != nullptr || tray.failed() ||
	   stop) {
		return false ;
	}

	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief tray操作の成功をfakeで再現して通常終了を検証する。
	 * @return 固定true。
	 * @note 無名action/dataは固定成功に不要で、dataの出力変更や実Shell操作はしない。
	 * @endcond
	 */
	AgentTrayWindow quit( instance, stop, []( DWORD, NOTIFYICONDATAW &) { return true ; }) ;
	// 二個目は独立fixtureとして生成し、終了commandを試験する
	if( !quit.Start()) {
		return false ;
	}

	PostMessageW( quit.window(), WM_COMMAND, 1U, 0L) ;
	const auto quit_deadline = GetTickCount64() + 2000U ;
	// 終了menuと同じ固定commandで協調停止を確認する
	while( !stop && GetTickCount64() < quit_deadline) {
		Sleep( 10U) ;
	}

	quit.Stop() ;
	// tray終了だけでI/O ownerへ停止要求が届かなければ失敗
	if( !stop || quit.failed()) {
		return false ;
	}

	stop = false ;
	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief tray生成失敗をfakeで注入し、不透明な常駐を拒否させる。
	 * @return 固定false。
	 * @note 無名action/dataは固定失敗に不要で、出力も通知領域も変更しない。
	 * @endcond
	 */
	AgentTrayWindow missing( instance, stop, []( DWORD, NOTIFYICONDATAW &) { return false ; }) ;
	// 初回Shell拒否は不可視Agentの継続を許可しない
	if( missing.Start() || missing.window() != nullptr) {
		return false ;
	}

	AgentTrayWindow failure( instance, stop,
							/**
							 * @cond LOCAL_CALLBACK_CONTRACT
							 * @brief tray更新だけを失敗させ、協調停止の分類を確認する。
							 * @param[in] action 失敗対象を判定する固定Shell操作。
							 * @return NIM_MODIFY以外の場合はtrue。
							 * @note 無名dataは操作種別だけの試験に不要で、内容は変更しない。
							 * @endcond
							 */
							[]( DWORD action, NOTIFYICONDATAW &) { return action != NIM_MODIFY ; }) ;
	// 最初の成功後のicon更新失敗を独立に試験する
	if( !failure.Start()) {
		return false ;
	}

	failure.SetStatus( AgentTrayStatus::Recovering) ;
	const auto failure_deadline = GetTickCount64() + 2000U ;
	// 常駐中のShell異常でも有限時間でI/O停止へ戻る
	while( !stop && GetTickCount64() < failure_deadline) {
		Sleep( 10U) ;
	}

	failure.Stop() ;
	const auto menu		  = AgentTrayWindow::MakeMenu( instance, AgentTrayStatus::Ready) ;
	const bool menu_valid = menu != nullptr && GetMenuItemCount( menu) == 3 &&
							GetMenuItemID( menu, 2) == 1U &&
							(GetMenuState( menu, 0U, MF_BYPOSITION) & MF_GRAYED) != 0U ;
	// testが生成したmenuだけを回収する
	if( menu != nullptr) {
		DestroyMenu( menu) ;
	}

	return stop && failure.failed() && menu_valid ;
}
} // namespace

/** @brief 実clipboard・RDP・process起動を行わず、配置境界を確認する。
 * @param[in] argc workspace fixture baseだけの件数。
 * @param[in] argv CTestが指定したworkspace以下のbase。
 * @return 全条件を満たす場合0。
 */
int wmain( int argc, wchar_t **argv)
{
	namespace setup = zrdp::common::agent_setup ;
	// test以外のpathを探索して使用しない
	if( argc != 3 && !(argc == 4 && std::wstring_view( argv[3]) == L"--preview-tray")) {
		return 1 ;
	}

	using ResourceModule = std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype( &FreeLibrary)> ;
	ResourceModule resources(
		LoadLibraryExW( argv[2], nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE),
		&FreeLibrary) ;
	// 不完全resourceで自作tray fixtureを開始しない
	if( !resources) {
		return 30 ;
	}

	// 明示previewだけは自作通知iconを一つ表示し、RDP／設定／Socketへ触れない
	if( argc == 4) {
		std::atomic_bool				  stop{false} ;
		zrdp::agent::app::AgentTrayWindow preview( resources.get(), stop) ;
		// 自分のicon生成成功後にだけ状態menuを表示する
		if( !preview.Start()) {
			return 38 ;
		}

		preview.SetStatus( zrdp::agent::app::AgentTrayStatus::Ready) ;
		PostMessageW( preview.window(), WM_APP + 1U, 0U, WM_CONTEXTMENU) ;
		const auto deadline = GetTickCount64() + 20000U ;
		// 自作menuだけを最大二十秒維持し、通信処理を開始しない
		while( !stop && GetTickCount64() < deadline) {
			Sleep( 50U) ;
		}

		preview.Stop() ;
		return preview.failed() ? 39 : 0 ;
	}

	// tray試験はEXEを実行せずresourceとfake notification操作だけを検証する
	if( !TrayFixtureTests( resources.get())) {
		return 30 ;
	}

	const auto base = std::filesystem::path( argv[1]) /
					  (L"agent-setup-test-" + std::to_wstring( GetCurrentProcessId())) ;
	auto drive = setup::DeploymentDrive::Create( base) ;
	// 空の専用driveが生成され、一般driveを再利用していない
	if( drive == nullptr || drive->letter() < L'E' || drive->sealed() || drive->published()) {
		return 2 ;
	}

	// 起動拒否はdialogなしで返し、実配置や既存設定を変更しない
	if( !TestSetupWithoutDialog( base, argv[2])) {
		return 40 ;
	}

	const auto		   directory = drive->directory() ;
	const wchar_t	   letter	 = drive->letter() ;
	const std::wstring root		 = std::wstring( 1U, letter) + L":\\" ;
	const auto		   expected =
		L"\\\\tsclient\\" + std::wstring( 1U, letter) + L"\\ZRdpSvnAgent.exe --rdp-setup" ;
	// 起動文字列は固定EXEと固定argumentだけにする
	if( drive->launch_command() != expected || !std::filesystem::is_directory( root)) {
		return 3 ;
	}

	const auto						 image = Read( argv[2]) ;
	const std::span<const std::byte> bytes( reinterpret_cast<const std::byte *>( image.data()),
										   image.size()) ;
	const std::string				 ini = "[allowlist]\r\nendpoint.1001=192.0.2.10:80\r\n" ;
	// 不正INIと取消要求は公開前に拒否する
	if( drive->Publish( bytes, "[allowlist]\r\n") || drive->sealed()) {
		return 4 ;
	}

	std::stop_source cancelled ;
	cancelled.request_stop() ;
	// 取消されたpackageを平文fileへ出さない
	if( drive->Publish( bytes, ini, cancelled.get_token())) {
		return 5 ;
	}

	// 完全二fileの公開を確認する
	if( !drive->Publish( bytes, ini) || !drive->sealed() || !drive->published() ||
	   Read( directory / L"agent.ini") != ini) {
		return 6 ;
	}

	// 二重公開は既存packageを変更しない
	if( drive->Publish( bytes, ini)) {
		return 7 ;
	}

	HANDLE write =
		CreateFileW( (directory / L"agent.ini").c_str(), GENERIC_WRITE,
					FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0U, nullptr) ;
	// 公開fileの変更・削除を許可しない
	if( write != INVALID_HANDLE_VALUE) {
		CloseHandle( write) ;
		return 8 ;
	}

	HANDLE extra = CreateFileW( (directory / L"other.dll").c_str(), GENERIC_WRITE, 0U, nullptr,
							   CREATE_NEW, 0U, nullptr) ;
	// 配布directoryへ任意fileを追加させない
	if( extra != INVALID_HANDLE_VALUE) {
		CloseHandle( extra) ;
		return 9 ;
	}

	const auto development = base / L"development" ;
	// 通常経路で未署名の正式Agentを初回配置し、同じ二fileを再利用する
	if( !zrdp::client::update::IsUnsignedDevelopmentImage( argv[2]) ||
	   setup::Install( directory, development) != setup::InstallResult::Installed ||
	   Read( development / L"ZRdpSvnAgent.exe") != image ||
	   Read( development / L"agent.ini") != ini ||
	   setup::Install( directory, development) != setup::InstallResult::AlreadyInstalled) {
		return 17 ;
	}

	// 通常経路の再配置でも同じ未署名Agentを再利用する
	if( setup::Install( directory, development) != setup::InstallResult::AlreadyInstalled) {
		return 18 ;
	}

	{
		std::ofstream changed( development / L"agent.ini", std::ios::binary | std::ios::trunc) ;
		changed << "development-owned" ;
	}

	// 署名なしの通常更新でもbackup後に検証済み設定へ更新する
	if( setup::Install( directory, development) != setup::InstallResult::Updated ||
	   Read( development / L"agent.ini") != ini) {
		return 19 ;
	}

	const auto broken = base / L"broken" ;
	std::filesystem::create_directory( broken) ;
	auto			 malformed = image ;
	IMAGE_DOS_HEADER dos{} ;
	std::memcpy( &dos, malformed.data(), sizeof( dos)) ;
	const auto certificate_offset = static_cast<std::size_t>( dos.e_lfanew) + sizeof( DWORD) +
									sizeof( IMAGE_FILE_HEADER) +
									offsetof( IMAGE_OPTIONAL_HEADER64, DataDirectory) +
									IMAGE_DIRECTORY_ENTRY_SECURITY * sizeof( IMAGE_DATA_DIRECTORY) ;
	// fixtureを編集する前にWindows SDK header範囲を有限EXE内で確認する
	if( certificate_offset + sizeof( IMAGE_DATA_DIRECTORY) > malformed.size()) {
		return 21 ;
	}

	const IMAGE_DATA_DIRECTORY invalid_certificate{static_cast<DWORD>( image.size()), 8U} ;
	std::memcpy( malformed.data() + certificate_offset, &invalid_certificate,
				sizeof( invalid_certificate)) ;
	{
		std::ofstream executable( broken / L"ZRdpSvnAgent.exe", std::ios::binary) ;
		executable.write( malformed.data(), static_cast<std::streamsize>( malformed.size())) ;
		std::ofstream configuration( broken / L"agent.ini", std::ios::binary) ;
		configuration << ini ;
	}

	// 自動更新用の署名不在判定は変更せず、壊れたcertificate tableを未署名と誤認しない
	if( zrdp::client::update::IsUnsignedDevelopmentImage( broken / L"ZRdpSvnAgent.exe")) {
		return 22 ;
	}

	auto test_drive = setup::DeploymentDrive::Create( base / L"test-drive") ;
	// 署名に関係なく固定setup argumentだけを生成する
	if( test_drive == nullptr || !test_drive->Publish( bytes, ini) ||
	   test_drive->launch_command() != L"\\\\tsclient\\" + std::wstring( 1U, test_drive->letter()) +
										   L"\\ZRdpSvnAgent.exe --rdp-setup") {
		return 20 ;
	}

	test_drive.reset() ;
	std::filesystem::remove( base / L"test-drive") ;
	const auto destination = base / L"installed" ;
	// runtimeの通常経路は署名なしの正式Agentも配置する
	if( setup::Install( directory, destination) != setup::InstallResult::Installed ||
	   Read( destination / L"agent.ini") != ini) {
		return 10 ;
	}

	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief fixture EXEの固定Agent/setup metadataだけを確認する。
	 * @param[in] path workspace内のtest EXE。実行や署名判定は行わない。
	 * @return metadata読込み成功とsetup対応が成立すればtrue。
	 * @endcond
	 */
	const setup::PackageVerifier trusted = []( const std::filesystem::path &path) {
		zrdp::client::update::ClientImageMetadata metadata ;
		return zrdp::client::update::ReadAgentImageMetadata( path, metadata) ==
				   zrdp::client::update::UpdateError::None &&
			   metadata.agent_setup ;
	} ;
	// test専用のmetadata境界でも同じ二fileを再利用する
	if( setup::Install( directory, destination, trusted) != setup::InstallResult::AlreadyInstalled ||
	   Read( destination / L"agent.ini") != ini) {
		return 11 ;
	}

	// 再実行で完全に同じ二fileを再利用する
	if( setup::Install( directory, destination, trusted) != setup::InstallResult::AlreadyInstalled) {
		return 12 ;
	}

	{
		std::ofstream changed( destination / L"agent.ini", std::ios::binary | std::ios::trunc) ;
		changed << "user-owned" ;
		std::ofstream old_agent( destination / L"ZRdpSvnAgent.exe",
								std::ios::binary | std::ios::trunc) ;
		old_agent << "old-agent-fixture" ;
	}

	// 明示指示された上書き更新は旧設定をbackupして新設定を反映する
	if( setup::Install( directory, destination, trusted) != setup::InstallResult::Updated ||
	   Read( destination / L"agent.ini") != ini ||
	   Read( destination / L"ZRdpSvnAgent.exe") != image) {
		return 13 ;
	}

	unsigned int backup_count = 0U ;
	// 自分の更新先内の二つの一意backupだけを確認する
	for( const auto &installed_directory : {development, destination}) {
		// backup内旧設定を確認し、incoming stageが回収済みか検証する
		for( const auto &entry : std::filesystem::directory_iterator( installed_directory)) {
			// 固定二fileはbackupとして数えない
			if( entry.is_directory() && entry.path().filename().native().starts_with( L"backup-")) {
				++backup_count ;
				const auto expected_old =
					installed_directory == development ? "development-owned" : "user-owned" ;
				// 新fileの検証前に旧二fileが保全されていたことを確認する
				if( Read( entry.path() / L"agent.ini") != expected_old ||
				   Read( entry.path() / L"ZRdpSvnAgent.exe") !=
					   (installed_directory == development ? image : "old-agent-fixture") ||
				   std::filesystem::exists( entry.path() / L"incoming")) {
					return 31 ;
				}
			}
		}
	}

	// 二つの更新が別のbackupを保持する
	if( backup_count != 2U) {
		return 32 ;
	}

	{
		std::ofstream changed( destination / L"agent.ini", std::ios::binary | std::ios::trunc) ;
		changed << "rollback-owned" ;
	}

	unsigned int update_validations = 0U ;
	// sourceとstage成功後、更新後検証失敗で旧二fileへ復旧する
	if( setup::Install( directory, destination,
					  /**
					   * @cond LOCAL_CALLBACK_CONTRACT
					   * @brief 二file更新後の再検証失敗を注入し旧版復元を確認する。
					   * @return 三回目の検証より前だけtrue。
					   * @note 無名pathは呼出し順だけの失敗注入に不要で、fileを読み書きしない。
					   * @endcond
					   */
					  [&]( const std::filesystem::path &) { return ++update_validations < 3U ; }) !=
		   setup::InstallResult::FileFailure ||
	   Read( destination / L"agent.ini") != "rollback-owned" ||
	   Read( destination / L"ZRdpSvnAgent.exe") != image) {
		return 33 ;
	}

	{
		std::ofstream restored( destination / L"agent.ini", std::ios::binary | std::ios::trunc) ;
		restored << ini ;
	}

	const auto rollback = base / L"rollback" ;
	{
		std::ofstream changed( destination / L"agent.ini", std::ios::binary | std::ios::trunc) ;
		changed << "locked-old" ;
		std::ofstream changed_exe( destination / L"ZRdpSvnAgent.exe",
								  std::ios::binary | std::ios::trunc) ;
		changed_exe << "locked-old-agent" ;
	}

	zrdp::client::update::detail::KernelHandle locked_ini(
		CreateFileW( (destination / L"agent.ini").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
					OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) ;
	// 二番目のrename失敗は先に置換したEXEも旧fileへ戻す
	if( !locked_ini.valid() ||
	   setup::Install( directory, destination, trusted) != setup::InstallResult::FileFailure ||
	   Read( destination / L"agent.ini") != "locked-old" ||
	   Read( destination / L"ZRdpSvnAgent.exe") != "locked-old-agent") {
		return 34 ;
	}

	locked_ini.reset() ;
	unsigned int							   restore_validations = 0U ;
	zrdp::client::update::detail::KernelHandle locked_replacement ;
	// 更新後検証で置換済みEXEをlockし、自動復旧不能の分類を試験する
	if( setup::Install( directory, destination,
					  /**
					   * @cond LOCAL_CALLBACK_CONTRACT
					   * @brief 更新後のtest EXEをlockして復元不能の分類を再現する。
					   * @param[in] path workspace内の自作置換file。三回目だけreadonly lockする。
					   * @return 三回目はlockを残してfalse、それ以前はtrue。
					   * @note 所有lockはfixture cleanupで解放し、実Agentや配布配置へ触れない。
					   * @endcond
					   */
					  [&]( const std::filesystem::path &path) {
						  ++restore_validations ;
						  // 最後の検証時だけ自作fileをlockし、復旧失敗を注入する
						  if( restore_validations == 3U) {
							  locked_replacement.reset(
								  CreateFileW( path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
											  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) ;
							  return false ;
						  }

						  return true ;
					  }) != setup::InstallResult::RollbackFailure ||
	   !locked_replacement.valid() || Read( destination / L"agent.ini") != "locked-old") {
		return 35 ;
	}

	locked_replacement.reset() ;
	{
		std::ofstream empty_ini( destination / L"agent.ini", std::ios::binary | std::ios::trunc) ;
	}

	// 空の通常fileもbackupして、承認済み設定へ修復更新する
	if( setup::Install( directory, destination, trusted) != setup::InstallResult::Updated ||
	   Read( destination / L"agent.ini") != ini) {
		return 36 ;
	}

	const auto alias = base / L"hardlink.ini" ;
	// 既存INIのhard linkは相手側fileを書き換える可能性があるため拒否する
	if( !CreateHardLinkW( alias.c_str(), (destination / L"agent.ini").c_str(), nullptr) ||
	   setup::Install( directory, destination, trusted) != setup::InstallResult::ExistingFiles ||
	   Read( alias) != ini) {
		return 37 ;
	}

	std::filesystem::remove( alias) ;
	unsigned int validations = 0U ;
	// copy後の検証失敗で新規EXEだけを回収する
	if( setup::Install( directory, rollback,
					  /**
					   * @cond LOCAL_CALLBACK_CONTRACT
					   * @brief 新規copy後の検証失敗を再現して新規fileの回収を確認する。
					   * @return 最初の検証だけtrue。
					   * @note 無名pathは検証順の試験に不要で、外部fileへ触れない。
					   * @endcond
					   */
					  [&]( const std::filesystem::path &) { return ++validations == 1U ; }) !=
		   setup::InstallResult::FileFailure ||
	   std::filesystem::exists( rollback / L"ZRdpSvnAgent.exe") ||
	   std::filesystem::exists( rollback / L"agent.ini")) {
		return 14 ;
	}

	const std::string changed_ini = "[allowlist]\nendpoint.1001=192.0.2.12:8080\n" ;
	// diskだけ新しい場合とrelay不能を区別し、旧memoryの設定を利用可にしない
	if( zrdp::agent::app::SelectAgentConfigurationStatus( false, true, true, true) != 1U ||
	   zrdp::agent::app::SelectAgentConfigurationStatus( false, true, false, true) != 2U ||
	   zrdp::agent::app::SelectAgentConfigurationStatus( true, false, false, true) != 2U ||
	   zrdp::agent::app::SelectAgentConfigurationStatus( false, true, true, false) != 3U ||
	   zrdp::agent::app::SelectAgentConfigurationStatus( false, false, false, true) != 3U) {
		return 43 ;
	}

	const auto old_image = Read( destination / L"ZRdpSvnAgent.exe") ;
	const auto old_ini	 = Read( destination / L"agent.ini") ;
	// 稼働EXEには触れず、INIだけをbackup付きで置換する
	if( setup::InstallConfiguration( destination, changed_ini) != setup::InstallResult::Updated ||
	   Read( destination / L"ZRdpSvnAgent.exe") != old_image ||
	   Read( destination / L"agent.ini") != changed_ini ||
	   setup::InstallConfiguration( destination, changed_ini) !=
		   setup::InstallResult::AlreadyInstalled ||
	   setup::InstallConfiguration( destination, "bad") != setup::InstallResult::InvalidPackage ||
	   Read( destination / L"agent.ini") != changed_ini) {
		return 40 ;
	}

	// test後は元の有効INIへ戻し、backupは通常fixture回収で処理する
	if( setup::InstallConfiguration( destination, old_ini) != setup::InstallResult::Updated) {
		return 41 ;
	}

	HANDLE stop_fixture = setup::CreateAgentStopEvent( 0x7fffff00U) ;
	// 実session IDを使わず、重複named objectの拒否を確認する
	if( stop_fixture == nullptr || setup::CreateAgentStopEvent( 0x7fffff00U) != nullptr ||
	   !SetEvent( stop_fixture) || WaitForSingleObject( stop_fixture, 0U) != WAIT_OBJECT_0) {
		return 42 ;
	}

	CloseHandle( stop_fixture) ;

	drive->Cancel() ;
	// 取消は新規入力を失効させるが、別の配置fileを削除しない
	if( drive->sealed() || Read( destination / L"agent.ini") != ini) {
		return 15 ;
	}

	const std::vector<std::byte> changed_agent{std::byte{0x4d}} ;
	// 取消後も物理公開は保持し、差替え・停止要求を再起動として採用しない
	if( !drive->published() || drive->Publish( bytes, ini) ||
	   drive->PrepareLaunch( changed_agent, ini) ||
	   drive->PrepareLaunch( bytes, "[allowlist]\nendpoint.1001=192.0.2.11:80\n") ||
	   drive->PrepareLaunch( bytes, ini, cancelled.get_token()) || drive->sealed() ||
	   Read( directory / L"agent.ini") != ini || Read( directory / L"ZRdpSvnAgent.exe") != image) {
		return 37 ;
	}

	// 完全一致の再検証を繰返してもreadonly二fileを作り直さない
	for( unsigned int attempt = 0U; attempt < 3U; ++attempt) {
		// 各回の入力許可は独立し、取消後も次の明示要求で再検証できる
		if( !drive->PrepareLaunch( bytes, ini) || !drive->sealed() || !drive->published()) {
			return 38 ;
		}

		drive->Cancel() ;
	}

	drive.reset() ;
	// 所有者終了で自分のmappingと公開二fileを回収する
	if( std::filesystem::exists( directory) ||
	   QueryDosDeviceW( std::wstring{letter, L':'}.c_str(), nullptr, 0U) != 0U) {
		return 16 ;
	}

	// このtestで作った固定fixtureだけを回収し、recursive deleteしない
	for( const auto name : {L"ZRdpSvnAgent.exe", L"agent.ini"}) {
		std::filesystem::remove( destination / name) ;
		std::filesystem::remove( development / name) ;
		std::filesystem::remove( broken / name) ;
	}

	// 自分が生成した有限backup内の固定二fileだけを通常削除する
	for( const auto &installed_directory : {destination, development}) {
		// directory名を照合し、fixture外の場所は回収しない
		for( const auto &entry : std::filesystem::directory_iterator( installed_directory)) {
			// testのbackupだけを明示列挙し、recursive deleteしない
			if( entry.is_directory() && entry.path().filename().native().starts_with( L"backup-")) {
				std::filesystem::remove( entry.path() / L"ZRdpSvnAgent.exe") ;
				std::filesystem::remove( entry.path() / L"agent.ini") ;
				std::filesystem::remove( entry.path()) ;
			}
		}
	}

	std::filesystem::remove( destination) ;
	std::filesystem::remove( development) ;
	std::filesystem::remove( broken) ;
	std::filesystem::remove( rollback) ;
	std::filesystem::remove( base) ;
	std::cout << "Agent setup isolation and rollback passed\n" ;
	return 0 ;
}
