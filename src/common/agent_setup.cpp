//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 * @file agent_setup.cpp
 * @brief ユーザー専用配布driveとバックアップ付き固定Agent更新を実装する。
 * @version 01.00.06
 * @date 2026/10/09
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/09  readonly公開と初回配置modeを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  起動と設定の分離、tray常駐、backup付き更新を追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  トレイ終了後の同じRDP内Agent再起動を修正。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  協調停止とINI-only原子更新を追加。
 * Ver.01.00.06   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "common/agent_setup.h"
#include "common/diagnostic_log.h"
#include "client/update/update_image_verifier.h"
#include "client/update/update_windows_handles.h"
#include "agent/network/allowlist.h"
#include "agent/app/session_validation.h"
#include <sddl.h>
#include <shlobj.h>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>

namespace zrdp::common::agent_setup {
namespace {
using client::update::detail::KernelHandle ;
// Agent EXEとINIの保持を有限にするbyte上限
constexpr std::size_t kAgentLimit = 16U * 1024U * 1024U ;
// process内で自分のdrive letter割当だけを直列化する
std::recursive_mutex  g_drive_mutex ;

/** @brief 別Client processとの未使用drive letter割当を直列化する。 */
struct MappingLock	  final {
	   /// current Windows session内だけの割当mutex。
	   KernelHandle handle{
		   CreateMutexW( nullptr, FALSE, L"Local\\ZRdpSvnBridge.DeploymentDriveAllocation")} ;
	   /// 有限waitで所有を取得できた場合だけtrue。
	   bool acquired{false} ;
	   /** @brief startup workerで最大2秒だけ割当の競合を待つ。 */
	   MappingLock()
	   {
		   // 同期objectを生成できなければ既存driveを推測して使わない
		   if( handle.valid()) {
			   const DWORD result = WaitForSingleObject( handle.get(), 2000U) ;
			   acquired			  = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED ;
		   }
	   }

	   /** @brief 自分が取得したmutexだけを解放する。 */
	   ~MappingLock()
	   {
		   // 未取得の同期objectへReleaseを呼ばない
		   if( acquired) {
			   static_cast<void>( ReleaseMutex( handle.get())) ;
		   }
	   }
} ;

/** @brief 平文設定の全終了経路でmemoryを消去する。 */
struct BufferWiper final {
	/// 消去対象の非所有buffer。
	std::vector<std::byte> &bytes ;
	/** @brief scope終了時に所有者より先にbufferをゼロ化する。 */
	~BufferWiper()
	{
		// 空bufferには無効data pointerを渡さない
		if( !bytes.empty()) {
			SecureZeroMemory( bytes.data(), bytes.size()) ;
		}
	}
} ;

/** @brief current-userだけを明示許可するDACLを構築する。
 * @param[in] read_only directoryとfileへの書込みを制限する場合はtrue。
 * @return LocalFreeするsecurity descriptor。失敗はnullptr。
 */
[[nodiscard]] PSECURITY_DESCRIPTOR UserSecurity( bool read_only)
{
	HANDLE raw = nullptr ;
	// 別userのACLを推定して設定しない
	if( !OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &raw)) {
		return nullptr ;
	}

	KernelHandle token( raw) ;
	DWORD		 bytes = 0U ;
	static_cast<void>( GetTokenInformation( token.get(), TokenUser, nullptr, 0U, &bytes)) ;
	// TokenUserのサイズを有限にする
	if( bytes == 0U || bytes > 65536U) {
		return nullptr ;
	}

	std::vector<std::byte> data( bytes) ;
	// current-user SID取得失敗ではeveryone ACLへfallbackしない
	if( !GetTokenInformation( token.get(), TokenUser, data.data(), bytes, &bytes)) {
		return nullptr ;
	}

	LPWSTR sid = nullptr ;
	// Win32が返すTOKEN_USERの型付けをこの境界へ限定する
	if( !ConvertSidToStringSidW( reinterpret_cast<TOKEN_USER *>( data.data())->User.Sid, &sid)) {
		return nullptr ;
	}

	const std::wstring sddl =
		L"D:P(A;;FA;;;SY)(A;;" + std::wstring( read_only ? L"GRGX" : L"FA") + L";;;" + sid + L")" ;
	LocalFree( sid) ;
	PSECURITY_DESCRIPTOR security = nullptr ;
	// 保護DACLを作れない状態ではfileを公開しない
	if( !ConvertStringSecurityDescriptorToSecurityDescriptorW( sddl.c_str(), SDDL_REVISION_1,
															 &security, nullptr)) {
		return nullptr ;
	}

	return security ;
}

/** @brief directoryと親のreparse・renameを拒否してlockする。
 * @param[in] path local directory。
 * @param[out] locks 親から対象までのrename防止HANDLE。
 * @return 完全なlocal directory chainならtrue。
 */
[[nodiscard]] bool LockDirectories( const std::filesystem::path &path,
								   std::vector<KernelHandle>   &locks)
{
	// UNCまたは相対配置先をallowしない
	if( !path.is_absolute() || path.root_name().native().size() != 2U ||
	   GetDriveTypeW( path.root_path().c_str()) != DRIVE_FIXED) {
		return false ;
	}

	std::vector<std::filesystem::path> parents ;
	auto							   item = path ;
	// rootまで有限に親を列挙する
	while( !item.empty()) {
		parents.push_back( item) ;
		const auto next = item.parent_path() ;
		// volume rootで列挙を止める
		if( next == item) {
			break ;
		}

		item = next ;
	}

	// rootからlockし、下位を確認する間の親差替えを防ぐ
	for( auto it = parents.rbegin(); it != parents.rend(); ++it) {
		KernelHandle			   handle( CreateFileW(
			  it->c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
			  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) ;
		BY_HANDLE_FILE_INFORMATION info{} ;
		// junction、symlink、fileをdirectoryとして辿らない
		if( !handle.valid() || !GetFileInformationByHandle( handle.get(), &info) ||
		   (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
		   (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
			return false ;
		}

		locks.push_back( std::move( handle)) ;
	}

	return true ;
}

/** @brief 固定fileを単一linkとして有限読込みする。
 * @param[in] path source file。
 * @param[in] limit 上限byte数。
 * @param[out] data 完全byte列。
 * @param[in] allow_empty 更新前backupだけで空の通常fileを保全する。
 * @return 全体を読めた場合だけtrue。
 */
[[nodiscard]] bool ReadFileBytes( const std::filesystem::path &path, std::size_t limit,
								 std::vector<std::byte> &data, bool allow_empty = false)
{
	KernelHandle file( CreateFileW( path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
								  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) ;
	BY_HANDLE_FILE_INFORMATION info{} ;
	LARGE_INTEGER			   size{} ;
	// sourceのlink、directory、過大なfileを拒否する
	if( !file.valid() || !GetFileInformationByHandle( file.get(), &info) ||
	   !GetFileSizeEx( file.get(), &size) ||
	   (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
	   info.nNumberOfLinks != 1U || size.QuadPart < 0 || (!allow_empty && size.QuadPart == 0) ||
	   static_cast<std::uint64_t>( size.QuadPart) > limit) {
		return false ;
	}

	data.resize( static_cast<std::size_t>( size.QuadPart)) ;
	DWORD read = 0U ;
	return ReadFile( file.get(), data.data(), static_cast<DWORD>( data.size()), &read, nullptr) &&
		   read == data.size() ;
}

/** @brief 固定fileをcreate-newし、成功時だけ書込みを完了する。
 * @param[in] path 新規file。
 * @param[in] data 完全byte列。
 * @return Flushまで成功ならtrue。既存fileは変更しない。
 */
[[nodiscard]] bool WriteNewFile( const std::filesystem::path &path, std::span<const std::byte> data)
{
	KernelHandle file( CreateFileW( path.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW,
								  FILE_ATTRIBUTE_NORMAL, nullptr)) ;
	// 既存fileやlinkを開いて上書きしない
	if( !file.valid()) {
		return false ;
	}

	DWORD	   written = 0U ;
	const bool success =
		WriteFile( file.get(), data.data(), static_cast<DWORD>( data.size()), &written, nullptr) &&
		written == data.size() && FlushFileBuffers( file.get()) ;
	file.reset() ;
	// この呼出しで作った不完全fileだけを回収する
	if( !success) {
		static_cast<void>( DeleteFileW( path.c_str())) ;
	}

	return success ;
}

/** @brief 一fileのDACLをcurrent-user readonlyへ変更する。
 * @param[in] path 専用directoryまたは新規file。
 * @param[in] security readonly DACL。
 * @return 設定成功時だけtrue。
 */
[[nodiscard]] bool Protect( const std::filesystem::path &path, PSECURITY_DESCRIPTOR security)
{
	return SetFileSecurityW( path.c_str(),
							DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
							security) != FALSE ;
}

/** @brief stageした二fileだけを全終了経路で回収する。 */
struct StagedPair final {
	/// 固定二fileしか作成しない新規directory。
	std::filesystem::path directory ;
	/** @brief 既存backupには触れず、自分のstageだけを通常削除する。 */
	~StagedPair()
	{
		try {
			static_cast<void>( DeleteFileW( (directory / L"ZRdpSvnAgent.exe").c_str())) ;
			static_cast<void>( DeleteFileW( (directory / L"agent.ini").c_str())) ;
			static_cast<void>( RemoveDirectoryW( directory.c_str())) ;
		} catch( ...) {
			common::WriteDiagnosticLog( common::DiagnosticLevel::Error,
									   common::DiagnosticEvent::UnexpectedException) ;
		}
	}
} ;

/** @brief 既存二fileを保全して検証済みstageへ置換する。
 * @param[in] destination 固定配置先。親と自身のdirectory lockはcallerが保持する。
 * @param[in] agent_bytes 検証済み新EXE。
 * @param[in] ini_bytes 検証済み新INI。
 * @param[in] verifier 固定Agentの製品metadata検証。
 * @return 更新成功、既存不安全、I/O失敗または復旧失敗。
 */
[[nodiscard]] InstallResult UpdateExistingPair( const std::filesystem::path &destination,
											   std::span<const std::byte>	agent_bytes,
											   std::span<const std::byte>	ini_bytes,
											   const PackageVerifier	   &verifier)
{
	const std::array<std::filesystem::path, 2U> paths{destination / L"ZRdpSvnAgent.exe",
													  destination / L"agent.ini"} ;
	std::array<std::vector<std::byte>, 2U>		previous ;
	BufferWiper									wipe_previous_ini{previous[1]} ;
	std::array<bool, 2U>						existed{}, replaced{} ;
	const std::array<std::size_t, 2U>			limits{kAgentLimit,
											   agent::network::kMaximumAllowlistFileBytes} ;
	// 固定二fileの単一link・有限byte列だけをbackupへ採用する
	for( std::size_t i = 0U; i < paths.size(); ++i) {
		const auto attributes = GetFileAttributesW( paths[i].c_str()) ;
		const auto error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS ;
		existed[i]		 = attributes != INVALID_FILE_ATTRIBUTES ;
		// 権限異常をfileなしと扱わず、link／directory／読込不能を上書きしない
		if( (!existed[i] && error != ERROR_FILE_NOT_FOUND) ||
		   (existed[i] && !ReadFileBytes( paths[i], limits[i], previous[i], true))) {
			return InstallResult::ExistingFiles ;
		}
	}

	// byte完全一致ではbackupを増やさず、同じ二fileを再利用する
	if( existed[0] && existed[1] && std::ranges::equal( previous[0], agent_bytes) &&
	   std::ranges::equal( previous[1], ini_bytes)) {
		return InstallResult::AlreadyInstalled ;
	}

	GUID					 identity{} ;
	std::array<wchar_t, 40U> identifier{} ;
	// 他の更新backupを上書きしない一意の新規名を作る
	if( FAILED( CoCreateGuid( &identity)) || StringFromGUID2( identity, identifier.data(), 40) <= 0) {
		return InstallResult::FileFailure ;
	}

	const auto backup	= destination / (L"backup-" + std::wstring( identifier.data())) ;
	using OwnedSecurity = std::unique_ptr<void, decltype( &LocalFree)> ;
	OwnedSecurity		security( UserSecurity( false), &LocalFree) ;
	SECURITY_ATTRIBUTES attributes{sizeof( SECURITY_ATTRIBUTES), security.get(), FALSE} ;
	const bool created = security != nullptr && CreateDirectoryW( backup.c_str(), &attributes) ;
	// 利用者専用backupを確保する前に既存fileへ触れない
	if( !created) {
		return InstallResult::FileFailure ;
	}

	const auto incoming = backup / L"incoming" ;
	// stage directoryも既存の名前やlinkを再利用しない
	if( !CreateDirectoryW( incoming.c_str(), &attributes)) {
		return InstallResult::FileFailure ;
	}

	StagedPair									stage{incoming} ;
	const std::array<std::filesystem::path, 2U> staged{incoming / L"ZRdpSvnAgent.exe",
													   incoming / L"agent.ini"} ;
	// 旧二fileを保全できない更新は開始しない
	for( std::size_t i = 0U; i < paths.size(); ++i) {
		// 存在したfileだけをcreate-new backupへ複写する
		if( existed[i] && (!WriteNewFile( backup / paths[i].filename(), previous[i]) ||
						  !Protect( backup / paths[i].filename(), security.get()))) {
			return InstallResult::FileFailure ;
		}
	}

	// 完全stageとEXE再検証を確認するまで現行二fileを維持する
	if( !WriteNewFile( staged[0], agent_bytes) || !WriteNewFile( staged[1], ini_bytes) ||
	   !Protect( staged[0], security.get()) || !Protect( staged[1], security.get()) ||
	   !verifier( staged[0])) {
		return InstallResult::FileFailure ;
	}

	bool success = true ;
	try {
		// 二fileを同一volumeのrenameで順番にcommitし、部分成功を追跡する
		for( std::size_t i = 0U; i < paths.size(); ++i) {
			std::vector<std::byte> current ;
			BufferWiper			   wipe_current{current} ;
			const auto			   flags =
				MOVEFILE_WRITE_THROUGH | (existed[i] ? MOVEFILE_REPLACE_EXISTING : 0U) ;
			// backup後に変更されたfileやlinkは置換しない
			if( (existed[i] &&
				(!ReadFileBytes( paths[i], limits[i], current, true) || current != previous[i])) ||
			   !MoveFileExW( staged[i].c_str(), paths[i].c_str(), flags)) {
				success = false ;
				break ;
			}

			replaced[i] = true ;
		}

		// 更新後のEXE検証失敗は成功と扱わず、二fileを復旧する
		if( success && !verifier( paths[0])) {
			success = false ;
		}
	} catch( ...) {
		success = false ;
	}

	// 二fileのcommitと検証が完了した場合だけ起動可能とする
	if( success) {
		return InstallResult::Updated ;
	}

	bool restored = true ;
	try {
		// 置換したfileだけを逆順に復旧し、未変更の既存fileを消さない
		for( std::size_t i = paths.size(); i > 0U; --i) {
			const auto index = i - 1U ;
			// 未commit fileはbackupから戻す必要がない
			if( !replaced[index]) {
				continue ;
			}

			// 既存fileはbackupのcopyから復旧し、backupそのものは保全する
			if( existed[index]) {
				const auto restore = incoming / paths[index].filename() ;
				const bool copied  = CopyFileW( (backup / paths[index].filename()).c_str(),
											   restore.c_str(), TRUE) != FALSE ;
				const bool moved   = copied && MoveFileExW( restore.c_str(), paths[index].c_str(),
														   MOVEFILE_REPLACE_EXISTING |
															   MOVEFILE_WRITE_THROUGH) != FALSE ;
				restored		   = moved && restored ;
			} else {
				restored = DeleteFileW( paths[index].c_str()) != FALSE && restored ;
			}
		}

	} catch( ...) {
		restored = false ;
	}

	return restored ? InstallResult::FileFailure : InstallResult::RollbackFailure ;
}
} // namespace

/**
 * @brief 専用DOS driveと固定配布fileのWindows所有状態を隔離する。
 * @note mutexで公開・再起動準備・終了回収を直列化し、mappingは自身の完全一致だけを回収する。
 */
struct DeploymentDrive::Impl {
	/// 自分の作成した唯一のdirectory。
	std::filesystem::path	  directory ;
	/// DefineDosDeviceへ渡した完全なNT target。
	std::wstring			  target ;
	/// 未使用だったdrive letterだけを所有する。
	wchar_t					  letter{0} ;
	/// 初回公開、再起動検証と終了時回収の並行アクセスを保護する。
	std::mutex				  mutex ;
	/// 検証済みreadonly二fileを今回の起動入力へ許可した状態。
	std::atomic<bool>		  sealed{false} ;
	/// 起動許可の取消とは独立した、固定二fileの不変な公開履歴。
	std::atomic<bool>		  published{false} ;
	/// 公開中のwriteとdeleteを拒否するreadonly file HANDLE。
	std::vector<KernelHandle> files ;
	/// 公開directoryと親のrenameを防ぐHANDLE。
	std::vector<KernelHandle> directories ;
} ;

DeploymentDrive::DeploymentDrive() : impl_( std::make_unique<Impl>())
{
}

DeploymentDrive::~DeploymentDrive()
{
	try {
		std::scoped_lock lock( impl_->mutex, g_drive_mutex) ;
		// 自分の完全一致mappingだけを除去し、他のdriveを削除しない
		if( impl_->letter != 0) {
			const std::wstring drive{impl_->letter, L':'} ;
			static_cast<void>( DefineDosDeviceW( DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE |
												   DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM,
											   drive.c_str(), impl_->target.c_str())) ;
		}

		impl_->files.clear() ;
		impl_->directories.clear() ;
		// 生成に失敗した所有者は既存directoryへ触れない
		if( !impl_->directory.empty()) {
			auto security = UserSecurity( false) ;
			// 書込み権限を戻せない場合は他の経路で強制削除しない
			if( security != nullptr) {
				static_cast<void>( Protect( impl_->directory, security)) ;
				// 専用directoryの固定二fileだけを回収し、recursive deleteしない
				for( const auto name : {L"ZRdpSvnAgent.exe", L"agent.ini"}) {
					const auto file = impl_->directory / name ;
					static_cast<void>( Protect( file, security)) ;
					static_cast<void>( DeleteFileW( file.c_str())) ;
				}

				LocalFree( security) ;
				static_cast<void>( RemoveDirectoryW( impl_->directory.c_str())) ;
			}
		}
	} catch( ...) {
		// 補助配布領域の回収失敗で生存中のRDP processを異常終了させない
		WriteDiagnosticLog( DiagnosticLevel::Warning, DiagnosticEvent::UnexpectedException) ;
	}
}

wchar_t DeploymentDrive::letter() const noexcept
{
	return impl_->letter ;
}

bool DeploymentDrive::sealed() const noexcept
{
	return impl_->sealed.load( std::memory_order_acquire) ;
}

bool DeploymentDrive::published() const noexcept
{
	return impl_->published.load( std::memory_order_acquire) ;
}

void DeploymentDrive::Cancel() noexcept
{
	impl_->sealed.store( false, std::memory_order_release) ;
}

const std::filesystem::path &DeploymentDrive::directory() const noexcept
{
	return impl_->directory ;
}

std::wstring DeploymentDrive::launch_command() const
{
	return L"\\\\tsclient\\" + std::wstring( 1U, impl_->letter) + L"\\ZRdpSvnAgent.exe --rdp-setup" ;
}

std::filesystem::path DeploymentDrive::DefaultBase() noexcept
{
	try {
		PWSTR raw = nullptr ;
		// redirected user profileへ別pathを探索しない
		if( FAILED( SHGetKnownFolderPath( FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw))) {
			return {} ;
		}

		const std::filesystem::path result = std::filesystem::path( raw) / L"ZRdpDeployment" ;
		CoTaskMemFree( raw) ;
		return result ;
	} catch( ...) {
		return {} ;
	}
}

std::shared_ptr<DeploymentDrive> DeploymentDrive::Create( const std::filesystem::path &base) noexcept
{
	try {
		std::scoped_lock drive_lock( g_drive_mutex) ;
		MappingLock		 process_lock ;
		// 他Clientの割当と競合したままDefineDosDeviceを呼ばない
		if( !process_lock.acquired) {
			return {} ;
		}

		auto result = std::make_shared<DeploymentDrive>() ;
		// base生成前にも親chainをlockし、junction先へdirectoryを作らない
		if( !LockDirectories( base.parent_path(), result->impl_->directories)) {
			return {} ;
		}

		auto security = UserSecurity( false) ;
		// current-user ACLが作れなければmappingを生成しない
		if( security == nullptr) {
			return {} ;
		}

		SECURITY_ATTRIBUTES attributes{sizeof( SECURITY_ATTRIBUTES), security, FALSE} ;
		const bool			base_ready =
			CreateDirectoryW( base.c_str(), &attributes) || GetLastError() == ERROR_ALREADY_EXISTS ;
		LocalFree( security) ;
		// 既存baseも親chainをlockしてから採用する
		if( !base_ready || !LockDirectories( base, result->impl_->directories)) {
			return {} ;
		}

		GUID	guid{} ;
		wchar_t name[40]{} ;
		// 推測可能な既存directoryを再利用しない
		if( FAILED( CoCreateGuid( &guid)) || StringFromGUID2( guid, name, 40) == 0) {
			return {} ;
		}

		const auto directory			= base / name ;
		security						= UserSecurity( false) ;
		attributes.lpSecurityDescriptor = security ;
		const bool created =
			security != nullptr && CreateDirectoryW( directory.c_str(), &attributes) ;
		LocalFree( security) ;
		// 既存directoryへfileを配置しない
		if( !created) {
			return {} ;
		}

		result->impl_->directory = directory ;
		security				 = UserSecurity( true) ;
		const bool read_only	 = security != nullptr && Protect( directory, security) ;
		LocalFree( security) ;
		// 接続直後の空driveも書込み可能な領域として公開しない
		if( !read_only) {
			return {} ;
		}

		// 自分のdirectoryを公開中にrenameさせない
		if( !LockDirectories( directory, result->impl_->directories)) {
			return {} ;
		}

		result->impl_->target =
			L"\\??\\" + std::filesystem::path( directory).make_preferred().native() ;
		// 既存driveを転送せず、空いているletterだけを選ぶ
		for( wchar_t letter = L'Z'; letter >= L'E'; --letter) {
			const std::wstring drive{letter, L':'} ;
			wchar_t			   existing[4096]{} ;
			// 他processが割り当てたDOS deviceを上書きしない
			if( QueryDosDeviceW( drive.c_str(), existing, 4096U) != 0U ||
			   GetLastError() != ERROR_FILE_NOT_FOUND) {
				continue ;
			}

			// 新規directoryだけへmappingできた場合に所有を公開する
			if( DefineDosDeviceW( DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM, drive.c_str(),
								result->impl_->target.c_str())) {
				result->impl_->letter = letter ;
				return result ;
			}
		}

		return {} ;
	} catch( ...) {
		return {} ;
	}
}

bool DeploymentDrive::Publish( std::span<const std::byte> agent, std::string_view ini,
							  std::stop_token stop) noexcept
{
	try {
		std::scoped_lock lock( impl_->mutex) ;
		// 一公開を再利用して別設定を上書きしない
		if( published() || impl_->letter == 0 || agent.empty() || agent.size() > kAgentLimit ||
		   ini.empty() || ini.size() > agent::network::kMaximumAllowlistFileBytes ||
		   stop.stop_requested()) {
			return false ;
		}

		agent::network::Allowlist allowlist ;
		// 不正INIを配布directoryへ出力しない
		if( !agent::network::ParseAllowlist( ini, allowlist).succeeded()) {
			return false ;
		}

		const auto executable	 = impl_->directory / L"ZRdpSvnAgent.exe" ;
		const auto configuration = impl_->directory / L"agent.ini" ;
		auto	   writable		 = UserSecurity( false) ;
		const bool can_write	 = writable != nullptr && Protect( impl_->directory, writable) ;
		LocalFree( writable) ;
		// 承認済みのworkerだけがdirectoryを書込み可能へ戻す
		if( !can_write) {
			return false ;
		}

		// create-newだけで二fileを作る
		if( !WriteNewFile( executable, agent)) {
			return false ;
		}

		const std::span<const std::byte> ini_bytes( reinterpret_cast<const std::byte *>( ini.data()),
												   ini.size()) ;
		// 取消またはINI作成失敗では自分のEXEだけを回収する
		if( stop.stop_requested() || !WriteNewFile( configuration, ini_bytes)) {
			static_cast<void>( DeleteFileW( executable.c_str())) ;
			return false ;
		}

		auto	   security		   = UserSecurity( true) ;
		const bool protected_files = security != nullptr && Protect( executable, security) &&
									 Protect( configuration, security) &&
									 Protect( impl_->directory, security) ;
		LocalFree( security) ;
		// ACL制限ができなければsealedと表示しない
		if( !protected_files) {
			return false ;
		}

		// 書込み、削除、差替えを拒否するreadonly HANDLEを保持する
		for( const auto &path : {executable, configuration}) {
			KernelHandle file( CreateFileW( path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
										  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) ;
			// lockできなければ公開成功としない
			if( !file.valid()) {
				return false ;
			}

			impl_->files.push_back( std::move( file)) ;
		}

		impl_->published.store( true, std::memory_order_release) ;
		impl_->sealed.store( true, std::memory_order_release) ;
		return true ;
	} catch( ...) {
		return false ;
	}
}

bool DeploymentDrive::PrepareLaunch( std::span<const std::byte> agent, std::string_view ini,
									std::stop_token stop) noexcept
{
	// 未公開の初回だけcreate-newへ進み、再起動ではfileを変更しない
	if( !published()) {
		return Publish( agent, ini, stop) ;
	}

	try {
		std::scoped_lock lock( impl_->mutex) ;
		// 前回の入力許可を検証中に再利用しない
		Cancel() ;
		// 停止要求・不完全な所有状態では再起動を許可しない
		if( impl_->letter == 0 || impl_->files.size() != 2U || agent.empty() ||
		   agent.size() > kAgentLimit || ini.empty() ||
		   ini.size() > agent::network::kMaximumAllowlistFileBytes || stop.stop_requested()) {
			return false ;
		}

		std::vector<std::byte>			 published_agent, published_ini ;
		BufferWiper						 wipe_ini{published_ini} ;
		const std::span<const std::byte> ini_bytes( reinterpret_cast<const std::byte *>( ini.data()),
												   ini.size()) ;
		// 固定二fileを有限readし、今回再検証したpackageとの完全一致だけを認める
		if( !ReadFileBytes( impl_->directory / L"ZRdpSvnAgent.exe", kAgentLimit, published_agent) ||
		   !ReadFileBytes( impl_->directory / L"agent.ini",
						  agent::network::kMaximumAllowlistFileBytes, published_ini) ||
		   !std::ranges::equal( published_agent, agent) ||
		   !std::ranges::equal( published_ini, ini_bytes) || stop.stop_requested()) {
			return false ;
		}

		impl_->sealed.store( true, std::memory_order_release) ;
		return true ;
	} catch( ...) {
		return false ;
	}
}

InstallResult Install( const std::filesystem::path &source, const std::filesystem::path &destination,
					  PackageVerifier verifier) noexcept
{
	bool	   created_executable	 = false ;
	bool	   created_configuration = false ;
	const auto executable			 = destination / L"ZRdpSvnAgent.exe" ;
	const auto configuration		 = destination / L"agent.ini" ;
	try {
		std::vector<std::byte> agent_bytes, ini_bytes ;
		BufferWiper			   wipe_ini{ini_bytes} ;
		// 固定二file以外をコピーしない
		if( !ReadFileBytes( source / L"ZRdpSvnAgent.exe", kAgentLimit, agent_bytes) ||
		   !ReadFileBytes( source / L"agent.ini", agent::network::kMaximumAllowlistFileBytes,
						  ini_bytes)) {
			return InstallResult::InvalidPackage ;
		}

		const std::string_view	  ini( reinterpret_cast<const char *>( ini_bytes.data()),
									  ini_bytes.size()) ;
		agent::network::Allowlist allowlist ;
		// allowlist不正では配置先へ触れない
		if( !agent::network::ParseAllowlist( ini, allowlist).succeeded()) {
			SecureZeroMemory( ini_bytes.data(), ini_bytes.size()) ;
			return InstallResult::InvalidPackage ;
		}

		// 署名の有無では拒否せず、固定製品とsetup対応metadataを再検証する
		if( !verifier) {
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT
			 * @brief 通常のAgent配置を署名の有無ではなく固定metadataで検証する。
			 * @param[in] path 検証対象のlocal EXE。読込みのみで実行しない。
			 * @return 固定Agent metadataとsetup対応が有効な場合だけtrue。
			 * @endcond
			 */
			verifier = []( const std::filesystem::path &path) {
				client::update::ClientImageMetadata metadata ;
				return client::update::ReadAgentImageMetadata( path, metadata) ==
						   client::update::UpdateError::None &&
					   metadata.agent_setup ;
			} ;
		}

		// sourceはClientで封じたfileでもremote側でも再検証する
		if( !verifier( source / L"ZRdpSvnAgent.exe")) {
			SecureZeroMemory( ini_bytes.data(), ini_bytes.size()) ;
			return InstallResult::InvalidPackage ;
		}

		std::vector<KernelHandle> locks ;
		// 配置先の親を先にlockし、junction経由の書込みを拒否する
		if( !LockDirectories( destination.parent_path(), locks)) {
			return InstallResult::FileFailure ;
		}

		auto				security = UserSecurity( false) ;
		SECURITY_ATTRIBUTES attributes{sizeof( SECURITY_ATTRIBUTES), security, FALSE} ;
		const bool			directory_ready =
			security != nullptr && (CreateDirectoryW( destination.c_str(), &attributes) ||
									GetLastError() == ERROR_ALREADY_EXISTS) ;
		LocalFree( security) ;
		// 配置先がreparseなら作成成功と扱わない
		if( !directory_ready || !LockDirectories( destination, locks)) {
			return InstallResult::FileFailure ;
		}

		const auto exe_attributes = GetFileAttributesW( executable.c_str()) ;
		const auto ini_attributes = GetFileAttributesW( configuration.c_str()) ;

		// 既存二fileは完全一致なら再利用、それ以外はbackup付きで更新する
		if( exe_attributes != INVALID_FILE_ATTRIBUTES || ini_attributes != INVALID_FILE_ATTRIBUTES) {
			return UpdateExistingPair( destination, agent_bytes, ini_bytes, verifier) ;
		}

		created_executable = WriteNewFile( executable, agent_bytes) ;
		// 完成EXEを再検証してからINIをcommitする
		if( created_executable && verifier( executable)) {
			created_configuration = WriteNewFile( configuration, ini_bytes) ;
		}

		SecureZeroMemory( ini_bytes.data(), ini_bytes.size()) ;
		// 両fileが完成した場合だけ起動可能な状態を公開する
		if( created_executable && created_configuration) {
			return InstallResult::Installed ;
		}
	} catch( ...) {
	}

	// 自分のcreate-newしたfileだけをrollbackする
	if( created_configuration) {
		static_cast<void>( DeleteFileW( configuration.c_str())) ;
	}

	// 既存fileを削除対象へ混ぜない
	if( created_executable) {
		static_cast<void>( DeleteFileW( executable.c_str())) ;
	}

	return InstallResult::FileFailure ;
}

HANDLE CreateAgentStopEvent( const DWORD session_id) noexcept
{
	try {
		using OwnedSecurity = std::unique_ptr<void, decltype( &LocalFree)> ;
		OwnedSecurity security( UserSecurity( false), &LocalFree) ;
		// user専用ACLを作れない場合は公開しない
		if( !security) {
			return nullptr ;
		}

		SECURITY_ATTRIBUTES attributes{sizeof( SECURITY_ATTRIBUTES), security.get(), FALSE} ;
		const auto			name  = L"Local\\ZRdpSvnAgent.Stop." + std::to_wstring( session_id) ;
		HANDLE				event = CreateEventW( &attributes, TRUE, FALSE, name.c_str()) ;
		const DWORD			error = GetLastError() ;
		// 既存objectは信頼せず、新規eventだけを所有する
		if( event != nullptr && error == ERROR_ALREADY_EXISTS) {
			CloseHandle( event) ;
			return nullptr ;
		}

		return event ;
	} catch( ...) {
		return nullptr ;
	}
}

InstallResult InstallConfiguration( const std::filesystem::path &destination,
								   const std::string_view		ini) noexcept
{
	try {
		agent::network::Allowlist checked ;
		// allocationとfile操作より前に上限と全allowlistを検証する
		if( ini.empty() || ini.size() > agent::network::kMaximumAllowlistFileBytes ||
		   !agent::network::ParseAllowlist( ini, checked).succeeded()) {
			return InstallResult::InvalidPackage ;
		}

		std::vector<KernelHandle> locks ;
		// 既存の固定local directoryだけを更新し、linkを辿らない
		if( !LockDirectories( destination, locks)) {
			return InstallResult::ExistingFiles ;
		}

		const auto			   path = destination / L"agent.ini" ;
		std::vector<std::byte> previous ;
		BufferWiper			   wipe{previous} ;
		// 読込不能、link、過大な旧INIを上書きしない
		if( !ReadFileBytes( path, agent::network::kMaximumAllowlistFileBytes, previous, true)) {
			return InstallResult::ExistingFiles ;
		}

		const auto bytes = std::as_bytes( std::span( ini.data(), ini.size())) ;
		// 同一設定ではbackupや再起動を増やさない
		if( std::ranges::equal( previous, bytes)) {
			return InstallResult::AlreadyInstalled ;
		}

		GUID					 identity{} ;
		std::array<wchar_t, 40U> identifier{} ;
		// backupは既存名を再利用しない
		if( FAILED( CoCreateGuid( &identity)) ||
		   StringFromGUID2( identity, identifier.data(), 40) <= 0) {
			return InstallResult::FileFailure ;
		}

		using OwnedSecurity = std::unique_ptr<void, decltype( &LocalFree)> ;
		OwnedSecurity		security( UserSecurity( false), &LocalFree) ;
		SECURITY_ATTRIBUTES attributes{sizeof( SECURITY_ATTRIBUTES), security.get(), FALSE} ;
		const auto			backup = destination / (L"backup-" + std::wstring( identifier.data())) ;
		// 旧INI保全用directoryをuser専用で新規作成する
		if( !security || !CreateDirectoryW( backup.c_str(), &attributes)) {
			return InstallResult::FileFailure ;
		}

		const auto incoming = backup / L"incoming" ;
		// stageはbackupと別に回収する
		if( !CreateDirectoryW( incoming.c_str(), &attributes)) {
			return InstallResult::FileFailure ;
		}

		StagedPair			   stage{incoming} ;
		const auto			   staged = incoming / L"agent.ini" ;
		std::vector<std::byte> current ;
		BufferWiper			   wipe_current{current} ;
		// backupと完全stageが出来るまで現行INIには触れない
		if( !WriteNewFile( backup / L"agent.ini", previous) ||
		   !Protect( backup / L"agent.ini", security.get()) || !WriteNewFile( staged, bytes) ||
		   !Protect( staged, security.get()) ||
		   !ReadFileBytes( path, agent::network::kMaximumAllowlistFileBytes, current, true) ||
		   current != previous ||
		   !MoveFileExW( staged.c_str(), path.c_str(),
						MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			return InstallResult::FileFailure ;
		}

		current.clear() ;
		// commit後の完全一致を確認する
		if( ReadFileBytes( path, agent::network::kMaximumAllowlistFileBytes, current) &&
		   std::ranges::equal( current, bytes)) {
			return InstallResult::Updated ;
		}

		const bool restored = CopyFileW( (backup / L"agent.ini").c_str(), staged.c_str(), TRUE) &&
							  MoveFileExW( staged.c_str(), path.c_str(),
										  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ;
		return restored ? InstallResult::FileFailure : InstallResult::RollbackFailure ;
	} catch( ...) {
		return InstallResult::FileFailure ;
	}
}

bool RestartInstalledAgent() noexcept
{
	try {
		PWSTR raw = nullptr ;
		// userの固定local配置先以外をcommandにしない
		if( FAILED( SHGetKnownFolderPath( FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw))) {
			return false ;
		}

		const auto destination = std::filesystem::path( raw) / L"ZRdp" ;
		CoTaskMemFree( raw) ;
		const auto					image = destination / L"ZRdpSvnAgent.exe" ;
		std::array<wchar_t, 32768U> module{} ;
		const DWORD					size =
			GetModuleFileNameW( nullptr, module.data(), static_cast<DWORD>( module.size())) ;
		// 開発用や任意位置のEXEを自己再起動経路へ流さない
		if( size == 0U || size >= module.size() ||
		   CompareStringOrdinal( module.data(), static_cast<int>( size), image.c_str(), -1, TRUE) !=
			   CSTR_EQUAL) {
			return false ;
		}

		std::wstring command = L"\"" + image.native() + L"\"" ;
		STARTUPINFOW startup{} ;
		startup.cb = sizeof( startup) ;
		PROCESS_INFORMATION process{} ;
		// shell、任意argument、別userを使用しない
		if( !CreateProcessW( image.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
						   nullptr, destination.c_str(), &startup, &process)) {
			return false ;
		}

		CloseHandle( process.hThread) ;
		CloseHandle( process.hProcess) ;
		return true ;
	} catch( ...) {
		return false ;
	}
}

int RunRdpSetup() noexcept
{
	try {
		// desktop consoleや別sessionでは初期配置を実行しない
		if( agent::app::ValidateCurrentSession().code != agent::app::SessionValidationCode::Valid) {
			return 20 ;
		}

		std::array<wchar_t, 32768U> module{} ;
		const DWORD					size =
			GetModuleFileNameW( nullptr, module.data(), static_cast<DWORD>( module.size())) ;
		// 固定redirected drive上の固定EXE以外からsetup modeを開始しない
		if( size == 0U || size >= module.size()) {
			return 21 ;
		}

		const std::filesystem::path source_image( std::wstring( module.data(), size)) ;
		const auto					directory = source_image.parent_path().native() ;
		// arbitrary share、subdirectory、serverを受け付けない
		if( directory.size() != 12U || _wcsnicmp( directory.c_str(), L"\\\\tsclient\\", 11U) != 0 ||
		   directory[11] < L'E' || directory[11] > L'Z' ||
		   source_image.filename() != L"ZRdpSvnAgent.exe") {
			return 21 ;
		}

		PWSTR raw = nullptr ;
		// 接続先userの固定local配置先だけを導出する
		if( FAILED( SHGetKnownFolderPath( FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw))) {
			return 22 ;
		}

		const auto destination = std::filesystem::path( raw) / L"ZRdp" ;
		CoTaskMemFree( raw) ;
		DWORD session_id = 0U ;
		// 稼働中Agentを強制終了せず、更新前にsession内のsingletonを確認する
		if( !ProcessIdToSessionId( GetCurrentProcessId(), &session_id)) {
			return 34 ;
		}

		const auto	 mutex_name = L"Local\\ZRdpSvnAgent.Session." + std::to_wstring( session_id) ;
		const auto	 setup_name = L"Local\\ZRdpSvnAgent.Setup." + std::to_wstring( session_id) ;
		KernelHandle setup_guard( CreateMutexW( nullptr, FALSE, setup_name.c_str())) ;
		const DWORD	 setup_error = GetLastError() ;
		// 同一sessionのsetupを並行実行しない
		if( !setup_guard.valid() || setup_error == ERROR_ALREADY_EXISTS) {
			return 34 ;
		}

		{
			KernelHandle running( OpenMutexW( SYNCHRONIZE, FALSE, mutex_name.c_str())) ;
			// 同じsessionのAgentだけへ協調停止を要求する
			if( running.valid()) {
				const auto	 stop_name = L"Local\\ZRdpSvnAgent.Stop." + std::to_wstring( session_id) ;
				KernelHandle stop_event( OpenEventW( EVENT_MODIFY_STATE, FALSE, stop_name.c_str())) ;
				// 未対応旧Agentや権限異常は強制終了せず停止する
				if( !stop_event.valid() || !SetEvent( stop_event.get())) {
					return 34 ;
				}

				running.reset() ;
				stop_event.reset() ;
				const auto deadline = GetTickCount64() + 15000U ;
				// setup workerでのみ有限待機し、他sessionへ触れない
				for( ;;) {
					KernelHandle probe( OpenMutexW( SYNCHRONIZE, FALSE, mutex_name.c_str())) ;
					const DWORD	 error = GetLastError() ;
					// objectの消滅を確認してから二file更新へ進む
					if( !probe.valid()) {
						// アクセス拒否を終了済みと近似しない
						if( error != ERROR_FILE_NOT_FOUND) {
							return 34 ;
						}

						break ;
					}

					probe.reset() ;
					// 応答しないAgentをkillせず、RDPも維持する
					if( GetTickCount64() >= deadline ||
					   agent::app::ValidateCurrentSession().code !=
						   agent::app::SessionValidationCode::Valid) {
						return 34 ;
					}

					Sleep( 50U) ;
				}
			} else {
				// 権限異常は未起動と扱わない
				if( GetLastError() != ERROR_FILE_NOT_FOUND) {
					return 34 ;
				}
			}
		}

		KernelHandle guard( CreateMutexW( nullptr, FALSE, mutex_name.c_str())) ;
		const auto	 singleton_error = GetLastError() ;
		// 二重setupと稼働Agentの二fileを同時に更新しない
		if( !guard.valid() || singleton_error == ERROR_ALREADY_EXISTS) {
			return 34 ;
		}

		const auto installed = Install( source_image.parent_path(), destination) ;
		// 自動復旧不能はprocess起動失敗と別分類にしてbackupを案内する
		if( installed == InstallResult::RollbackFailure) {
			return 35 ;
		}

		// 二fileの配置／更新／再利用を確認した場合だけ起動する
		if( installed != InstallResult::Installed && installed != InstallResult::AlreadyInstalled &&
		   installed != InstallResult::Updated) {
			return 23 + static_cast<int>( installed) ;
		}

		const auto	 image	 = destination / L"ZRdpSvnAgent.exe" ;
		std::wstring command = L"\"" + image.native() + L"\"" ;
		STARTUPINFOW startup{} ;
		startup.cb = sizeof( startup) ;
		PROCESS_INFORMATION process{} ;
		guard.reset() ;
		// shellを使わず固定EXEだけを同じuser sessionでconsoleなし起動する
		if( !CreateProcessW( image.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
						   nullptr, destination.c_str(), &startup, &process)) {
			return 29 ;
		}

		CloseHandle( process.hThread) ;
		CloseHandle( process.hProcess) ;
		return 0 ;
	} catch( ...) {
		return 30 ;
	}
}
} // namespace zrdp::common::agent_setup
