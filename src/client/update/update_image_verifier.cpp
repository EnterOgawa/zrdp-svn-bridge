//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file update_image_verifier.cpp
 * @brief 有限file読取り、Windows署名trust、Client metadataの一致を実装する。
 * @version 01.00.05
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  同一publisherの署名済みClientだけを許可。
 * Ver.01.00.01   Shunji Ogawa  2026/10/08  確認付きAgent配置の境界と操作を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  自動更新Agentを署名・hash・同一版・metadataで完全検証。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/update/update_image_verifier.h"
#include "client/update/update_windows_handles.h"
#include "protocol/frame.h"

#include <Windows.h>
#include <bcrypt.h>
#include <Softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <winver.h>

#include <array>
#include <cstring>
#include <cwchar>
#include <memory>
#include <span>
#include <vector>

namespace zrdp::client::update {
namespace {

// file全体を保持せずに読むための固定chunk byte数
constexpr DWORD			  kHashChunkBytes			   = 64U * 1024U ;
// version resourceの解析memory上限
constexpr DWORD			  kMaximumVersionResourceBytes = 64U * 1024U ;

/**
 * @brief non-reparse、単一link、有限sizeのfileだけをreadonlyで所有する。
 * @param[in] path 読取り対象。
 * @param[out] file write／renameを共有しない所有HANDLE。
 * @param[out] size 完全file byte数。
 * @return 完全なregular fileならNone。
 */
[[nodiscard]] UpdateError OpenImage( const std::filesystem::path &path, detail::KernelHandle &file,
									std::uint64_t &size) noexcept
{
	file.reset( CreateFileW( path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
						   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) ;
	// 読取り権限がない配置や消えたfileへ別経路でfallbackしない
	if( !file.valid()) {
		return UpdateError::FileFailure ;
	}

	BY_HANDLE_FILE_INFORMATION information{} ;
	// 別fileへのredirect、directory、hard linkを更新対象として扱わない
	if( !GetFileInformationByHandle( file.get(), &information) ||
	   GetFileType( file.get()) != FILE_TYPE_DISK || information.nNumberOfLinks != 1U ||
	   (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
		   0U) {
		return UpdateError::FileFailure ;
	}

	size =
		(static_cast<std::uint64_t>( information.nFileSizeHigh) << 32U) | information.nFileSizeLow ;
	// 空fileと上限を超えるEXEを署名／resource parserへ渡さない
	if( size == 0U || size > kMaximumUpdateExecutableBytes) {
		return UpdateError::DataTooLarge ;
	}

	return UpdateError::None ;
}

/** @brief CNG algorithm handleの解放責任。 */
struct AlgorithmCloser {
	/**
	 * @brief 所有CNG algorithmを閉じる。
	 * @param[in] value 有効algorithm handle。
	 */
	void operator()( void *value) const noexcept
	{
		static_cast<void>( BCryptCloseAlgorithmProvider( value, 0U)) ;
	}
} ;
/** @brief CNG hash handleの解放責任。 */
struct HashCloser {
	/**
	 * @brief 所有CNG hashを破棄する。
	 * @param[in] value 有効hash handle。
	 */
	void operator()( void *value) const noexcept
	{
		static_cast<void>( BCryptDestroyHash( value)) ;
	}
} ;

/**
 * @brief 開いたfileの不変byte列を固定SHA-256へ変換する。
 * @param[in] file write／rename共有を禁止したreadonly HANDLE。
 * @param[in] size 検証済みfile byte数。
 * @param[out] hash 完全に読み終わったdigest。
 * @return 完全に検証した場合だけNone。
 */
[[nodiscard]] UpdateError HashImage( const HANDLE file, const std::uint64_t size, Sha256 &hash)
{
	LARGE_INTEGER start{} ;
	// 同じHANDLEをPE解析後にも使用できるよう先頭からhashする
	if( !SetFilePointerEx( file, start, nullptr, FILE_BEGIN)) {
		return UpdateError::FileFailure ;
	}

	BCRYPT_ALG_HANDLE raw_algorithm = nullptr ;
	// Windows標準SHA-256だけを使用する
	if( BCryptOpenAlgorithmProvider( &raw_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0U) < 0) {
		return UpdateError::ResourceExhausted ;
	}

	std::unique_ptr<void, AlgorithmCloser> algorithm( raw_algorithm) ;
	BCRYPT_HASH_HANDLE					   raw_hash = nullptr ;
	// CNGに有限hash objectのmemoryを所有させる
	if( BCryptCreateHash( algorithm.get(), &raw_hash, nullptr, 0U, nullptr, 0U, 0U) < 0) {
		return UpdateError::ResourceExhausted ;
	}

	std::unique_ptr<void, HashCloser>  state( raw_hash) ;
	std::array<UCHAR, kHashChunkBytes> buffer{} ;
	std::uint64_t					   total = 0U ;
	// 上限済みfileをchunkごとにhashし、全量memory保持をしない
	while( total < size) {
		DWORD received = 0U ;
		// 読取り失敗、途中EOF、sizeの変化を部分digestで成功扱いにしない
		if( !ReadFile( file, buffer.data(), static_cast<DWORD>( buffer.size()), &received, nullptr) ||
		   received == 0U || received > size - total) {
			return UpdateError::FileFailure ;
		}

		// 読み取ったbyte数だけをCNGへ渡す
		if( BCryptHashData( state.get(), buffer.data(), received, 0U) < 0) {
			return UpdateError::ResourceExhausted ;
		}

		total += received ;
	}

	Sha256 result{} ;
	// CNGのUCHAR境界へ固定byte配列を渡し、文字コード変換を行わない
	if( BCryptFinishHash( state.get(), reinterpret_cast<PUCHAR>( result.data()),
						static_cast<ULONG>( result.size()), 0U) < 0) {
		return UpdateError::ResourceExhausted ;
	}

	hash = result ;
	return UpdateError::None ;
}

/**
 * @brief Windows PEの固定headerを有限offsetから完全に読む。
 * @param[in] file readonly disk HANDLE。
 * @param[in] offset file先頭からのbyte位置。
 * @param[out] destination Windows SDK headerの保存領域。
 * @param[in] bytes 保存領域のbyte数。
 * @return 完全に読めた場合はtrue。
 */
[[nodiscard]] bool ReadAt( const HANDLE file, const std::uint64_t offset, void *destination,
						  const DWORD bytes) noexcept
{
	LARGE_INTEGER position{} ;
	position.QuadPart = static_cast<LONGLONG>( offset) ;
	DWORD received	  = 0U ;
	return SetFilePointerEx( file, position, nullptr, FILE_BEGIN) &&
		   ReadFile( file, destination, bytes, &received, nullptr) && received == bytes ;
}

/**
 * @brief Windows SDKのPE定義で指定subsystemのx64 EXEを識別する。
 * @param[in] file readonly HANDLE。
 * @param[in] size 検証済み全体byte数。
 * @param[in] subsystem ClientはGUI、Agentはconsoleの固定種別。
 * @return 指定subsystemのx64 PEであればNone。
 */
[[nodiscard]] UpdateError VerifyPe( const HANDLE file, const std::uint64_t size,
								   const WORD subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI) noexcept
{
	IMAGE_DOS_HEADER dos{} ;
	// DOS headerと有限の非負PE offsetを持たないfileを実行しない
	if( size < sizeof( dos) || !ReadAt( file, 0U, &dos, sizeof( dos)) ||
	   dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 1024 * 1024) {
		return UpdateError::UnsupportedImage ;
	}

	const auto				offset	  = static_cast<std::uint64_t>( dos.e_lfanew) ;
	DWORD					signature = 0U ;
	IMAGE_FILE_HEADER		header{} ;
	IMAGE_OPTIONAL_HEADER64 optional{} ;
	// fileの範囲と標準PE64 headerを全て検証し、DLLや別architectureを拒否する
	if( offset + sizeof( signature) + sizeof( header) + sizeof( optional) > size ||
	   !ReadAt( file, offset, &signature, sizeof( signature)) || signature != IMAGE_NT_SIGNATURE ||
	   !ReadAt( file, offset + sizeof( signature), &header, sizeof( header)) ||
	   header.Machine != IMAGE_FILE_MACHINE_AMD64 ||
	   (header.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0U ||
	   (header.Characteristics & IMAGE_FILE_DLL) != 0U ||
	   header.SizeOfOptionalHeader != sizeof( optional) ||
	   !ReadAt( file, offset + sizeof( signature) + sizeof( header), &optional, sizeof( optional)) ||
	   optional.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || optional.Subsystem != subsystem) {
		return UpdateError::UnsupportedImage ;
	}

	return UpdateError::None ;
}

/** @brief Windows trust stateとそのscope内だけ有効なpublisher証明書。 */
class TrustedSignature final {
public:
	/**
	 * @brief 証明書UIも外部取得も行わないWindows trust設定を構築する。
	 * @param[in] path 検証対象file。
	 * @param[in] file write／rename共有を禁止したHANDLE。
	 */
	TrustedSignature( const std::filesystem::path &path, const HANDLE file) noexcept
	{
		file_.cbStruct			  = sizeof( file_) ;
		file_.pcwszFilePath		  = path.c_str() ;
		file_.hFile				  = file ;
		data_.cbStruct			  = sizeof( data_) ;
		data_.dwUIChoice		  = WTD_UI_NONE ;
		data_.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN ;
		data_.dwUnionChoice		  = WTD_CHOICE_FILE ;
		data_.pFile				  = &file_ ;
		data_.dwStateAction		  = WTD_STATEACTION_VERIFY ;
		data_.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT | WTD_CACHE_ONLY_URL_RETRIEVAL |
							WTD_DISABLE_MD2_MD4 ;
	}

	/** @brief 署名なしの固定結果だけを確認する。 @return Windowsの署名不在判定だけtrue。 */
	[[nodiscard]] bool IsUnsigned() noexcept
	{
		return WinVerifyTrust( nullptr, &action_, &data_) == TRUST_E_NOSIGNATURE ;
	}

	/** @brief trust判定が失敗した場合もprovider stateを解放する。 */
	~TrustedSignature()
	{
		data_.dwStateAction = WTD_STATEACTION_CLOSE ;
		static_cast<void>( WinVerifyTrust( nullptr, &action_, &data_)) ;
	}

	/** @brief provider stateの複製を禁止する。 */
	TrustedSignature( const TrustedSignature &)			   = delete ;
	/**
	 * @brief provider stateの複製を禁止する。
	 * @return 呼出し禁止のため値を生成しない。
	 */
	TrustedSignature  &operator=( const TrustedSignature &) = delete ;
	/**
	 * @brief SHA-256の署名と検証済みtimestampを確認する。
	 * @return publisher証明書を安全に参照できる場合だけtrue。
	 */
	[[nodiscard]] bool Verify() noexcept
	{
		// WinVerifyTrustの成功はHRESULTのSUCCEEDEDではなく値0で判定する
		if( WinVerifyTrust( nullptr, &action_, &data_) != ERROR_SUCCESS) {
			return false ;
		}

		const auto provider = WTHelperProvDataFromStateData( data_.hWVTStateData) ;
		// 信頼成功でもpublisher chainが欠落した結果を採用しない
		if( provider == nullptr) {
			return false ;
		}

		const auto signer = WTHelperGetProvSignerFromChain( provider, 0U, FALSE, 0U) ;
		// 有効なprimary SHA-256署名とtimestampの両方を必須にする
		if( signer == nullptr || signer->dwError != 0U || signer->csCertChain == 0U ||
		   signer->pasCertChain == nullptr || signer->pasCertChain[0].pCert == nullptr ||
		   signer->psSigner == nullptr || signer->psSigner->HashAlgorithm.pszObjId == nullptr ||
		   std::strcmp( signer->psSigner->HashAlgorithm.pszObjId, szOID_NIST_sha256) != 0 ||
		   signer->csCounterSigners == 0U || signer->pasCounterSigners == nullptr) {
			return false ;
		}

		bool timestamp = false ;
		// providerが検証したtimestamp signerだけを認める
		for( DWORD index = 0U; index < signer->csCounterSigners; ++index) {
			const auto &counter = signer->pasCounterSigners[index] ;
			// 失敗した／非timestampのcountersignatureを有効期限の根拠にしない
			if( counter.dwError == 0U && (counter.dwSignerType & SGNR_TYPE_TIMESTAMP) != 0U &&
			   counter.csCertChain != 0U && counter.pasCertChain != nullptr) {
				timestamp = true ;
			}
		}

		// timestampを確認できないEXEの自動適用は拒否する
		if( !timestamp) {
			return false ;
		}

		certificate_ = signer->pasCertChain[0].pCert ;
		return true ;
	}

	/**
	 * @brief scope中だけ有効なpublisher公開鍵を比較する。
	 * @param[in] other 完全検証済みの候補署名。
	 * @return SubjectPublicKeyInfoが完全一致ならtrue。
	 */
	[[nodiscard]] bool SamePublisher( const TrustedSignature &other) const noexcept
	{
		return certificate_ != nullptr && other.certificate_ != nullptr &&
			   CertComparePublicKeyInfo( X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
										&certificate_->pCertInfo->SubjectPublicKeyInfo,
										&other.certificate_->pCertInfo->SubjectPublicKeyInfo) !=
				   FALSE ;
	}

private:
	/// 検証対象fileへの非所有参照。callerのHANDLEより先に解放する。
	WINTRUST_FILE_INFO file_{} ;
	/// VERIFY後はCLOSEを必ず呼ぶ所有provider state。
	WINTRUST_DATA	   data_{} ;
	/// Windows標準Authenticode検証action。
	GUID			   action_ = WINTRUST_ACTION_GENERIC_VERIFY_V2 ;
	/// provider stateの寿命内だけ参照し、外部へ出さないpublisher証明書。
	PCCERT_CONTEXT	   certificate_{nullptr} ;
} ;

/**
 * @brief 一translationのresource文字列をbounded viewとして取得する。
 * @param[in,out] data Windowsが返した所有resource memory。
 * @param[in] language language/codepageの二WORD。
 * @param[in] key 固定metadata key。
 * @return 有限のNUL終端値。欠落時は空。
 */
[[nodiscard]] std::wstring_view ReadStringValue( std::vector<std::byte>	   &data,
												const std::array<WORD, 2U> &language,
												const wchar_t			   *key) noexcept
{
	std::array<wchar_t, 128U> query{} ;
	// 固定keyだけをWindows resource pathへ組み込む
	if( swprintf_s( query.data(), query.size(), L"\\StringFileInfo\\%04x%04x\\%s", language[0],
				  language[1], key) < 0) {
		return {} ;
	}

	void *raw	 = nullptr ;
	UINT  length = 0U ;
	// 無制限文字列や非終端値を製品識別に使わない
	if( !VerQueryValueW( data.data(), query.data(), &raw, &length) || raw == nullptr ||
	   length == 0U || length > 256U) {
		return {} ;
	}

	// VerQueryValueWが返すUTF-16の型付けをこのWindows境界だけで行う
	const auto value = static_cast<const wchar_t *>( raw) ;
	// 指定長の終端NULと途中NULの欠落を両方確認する
	if( value[length - 1U] != L'\0' ||
	   std::wstring_view( value, length - 1U).find( L'\0') != std::wstring_view::npos) {
		return {} ;
	}

	return {value, length - 1U} ;
}

/**
 * @brief 基本製品metadataと署名対象のbootstrap keyを解析する。
 * @param[in] path readonly lock中のfile path。
 * @param[out] metadata 完全な解析値。
 * @param[in] original_name 採用する固定の製品実行file名。
 * @return 必須基本値が揃う場合はNone。
 */
[[nodiscard]] UpdateError
ReadVersionResource( const std::filesystem::path &path, ClientImageMetadata &metadata,
					const std::wstring_view original_name = L"ZRdpSvnBridge.exe")
{
	DWORD		ignored = 0U ;
	const DWORD bytes	= GetFileVersionInfoSizeW( path.c_str(), &ignored) ;
	// resourceの欠落や異常上限を未確認の製品として拒否する
	if( bytes == 0U || bytes > kMaximumVersionResourceBytes) {
		return UpdateError::MetadataMismatch ;
	}

	std::vector<std::byte> data( bytes) ;
	// Windows resourceの部分読取りを採用しない
	if( !GetFileVersionInfoW( path.c_str(), 0U, bytes, data.data())) {
		return UpdateError::MetadataMismatch ;
	}

	void *raw  = nullptr ;
	UINT  size = 0U ;
	// 固定version情報をWindowsのABI定義に従って所有領域へcopyする
	if( !VerQueryValueW( data.data(), L"\\", &raw, &size) || raw == nullptr ||
	   size != sizeof( VS_FIXEDFILEINFO)) {
		return UpdateError::MetadataMismatch ;
	}

	VS_FIXEDFILEINFO fixed{} ;
	std::memcpy( &fixed, raw, sizeof( fixed)) ;
	// FileVersion、ProductVersion、実行file種別の一致を必須にする
	if( fixed.dwSignature != VS_FFI_SIGNATURE || fixed.dwFileType != VFT_APP ||
	   fixed.dwFileVersionMS != fixed.dwProductVersionMS ||
	   fixed.dwFileVersionLS != fixed.dwProductVersionLS) {
		return UpdateError::MetadataMismatch ;
	}

	ClientImageMetadata result ;
	result.version.components = {HIWORD( fixed.dwFileVersionMS), LOWORD( fixed.dwFileVersionMS),
								 HIWORD( fixed.dwFileVersionLS), LOWORD( fixed.dwFileVersionLS)} ;
	// translation数を有限にし、keyの検索を全fileへ広げない
	if( !VerQueryValueW( data.data(), L"\\VarFileInfo\\Translation", &raw, &size) || raw == nullptr ||
	   size == 0U || size % (2U * sizeof( WORD)) != 0U || size > 64U) {
		return UpdateError::MetadataMismatch ;
	}

	const auto translations = std::span( static_cast<const std::byte *>( raw), size) ;
	// 有効なtranslationから正式Client識別とbootstrapを確認する
	for( std::size_t offset = 0U; offset < translations.size(); offset += 2U * sizeof( WORD)) {
		std::array<WORD, 2U> language{} ;
		std::memcpy( language.data(), translations.data() + offset, sizeof( language)) ;
		// 別製品やAgent EXEをClientとして採用しない
		if( ReadStringValue( data, language, L"ProductName") != L"ZRdp SVN Bridge" ||
		   ReadStringValue( data, language, L"OriginalFilename") != original_name) {
			continue ;
		}

		result.update_bootstrap = ReadStringValue( data, language, L"UpdaterProtocol") == L"1" ;
		result.paired_update	= ReadStringValue( data, language, L"PairedUpdateProtocol") == L"1" ;
		result.agent_setup		= ReadStringValue( data, language, L"AgentSetupProtocol") == L"1" ;
		// signed resourceのBridge protocolもmanifestと独立して検証する
		if( ReadStringValue( data, language, L"BridgeProtocol") == L"1") {
			result.bridge_protocol = 1U ;
		}

		metadata = result ;
		return UpdateError::None ;
	}

	return UpdateError::MetadataMismatch ;
}

} // namespace

UpdateError ComputeUpdateFileHash( const std::filesystem::path &path, Sha256 &hash,
								  std::uint64_t &size_bytes) noexcept
{
	hash	   = {} ;
	size_bytes = 0U ;
	try {
		detail::KernelHandle file ;
		std::uint64_t		 size	= 0U ;
		const UpdateError	 opened = OpenImage( path, file, size) ;
		// 未確認のHANDLEでhashしない
		if( opened != UpdateError::None) {
			return opened ;
		}

		const UpdateError hashed = HashImage( file.get(), size, hash) ;
		// 完全digestが完成した場合だけ全体byte数も公開する
		if( hashed == UpdateError::None) {
			size_bytes = size ;
		}

		return hashed ;
	} catch( ...) {
		return UpdateError::ResourceExhausted ;
	}
}

UpdateError ReadClientImageMetadata( const std::filesystem::path &path,
									ClientImageMetadata			&metadata) noexcept
{
	metadata = {} ;
	try {
		detail::KernelHandle file ;
		std::uint64_t		 size	= 0U ;
		const UpdateError	 opened = OpenImage( path, file, size) ;
		// 未確認fileをPE parserへ渡さない
		if( opened != UpdateError::None) {
			return opened ;
		}

		const UpdateError pe = VerifyPe( file.get(), size) ;
		// 別architectureやDLLのresourceを製品versionとして使わない
		if( pe != UpdateError::None) {
			return pe ;
		}

		return ReadVersionResource( path, metadata) ;
	} catch( ...) {
		return UpdateError::ResourceExhausted ;
	}
}

UpdateError ReadAgentImageMetadata( const std::filesystem::path &path,
								   ClientImageMetadata		   &metadata) noexcept
{
	metadata = {} ;
	try {
		detail::KernelHandle file ;
		std::uint64_t		 size	= 0U ;
		const auto			 opened = OpenImage( path, file, size) ;
		// Agentも通常fileを確認してからPE/resource解析へ渡す
		if( opened != UpdateError::None) {
			return opened ;
		}

		const auto pe = VerifyPe( file.get(), size, IMAGE_SUBSYSTEM_WINDOWS_CUI) ;
		// Client GUIや別architectureをAgentとして配置しない
		if( pe != UpdateError::None) {
			return pe ;
		}

		return ReadVersionResource( path, metadata, L"ZRdpSvnAgent.exe") ;
	} catch( ...) {
		return UpdateError::ResourceExhausted ;
	}
}

bool IsUnsignedDevelopmentImage( const std::filesystem::path &path) noexcept
{
	try {
		detail::KernelHandle file ;
		std::uint64_t		 size = 0U ;
		// 有限regular fileとx64 EXE以外は署名なし扱いにしない
		if( OpenImage( path, file, size) != UpdateError::None ||
		   (VerifyPe( file.get(), size) != UpdateError::None &&
			VerifyPe( file.get(), size, IMAGE_SUBSYSTEM_WINDOWS_CUI) != UpdateError::None)) {
			return false ;
		}

		IMAGE_DOS_HEADER		dos{} ;
		IMAGE_OPTIONAL_HEADER64 optional{} ;
		// PE検証後も読取り失敗やdirectory数不足を拒否する
		if( !ReadAt( file.get(), 0U, &dos, sizeof( dos)) ||
		   !ReadAt( file.get(),
				   static_cast<std::uint64_t>( dos.e_lfanew) + sizeof( DWORD) +
					   sizeof( IMAGE_FILE_HEADER),
				   &optional, sizeof( optional)) ||
		   optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_SECURITY) {
			return false ;
		}

		const auto &certificate = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY] ;
		// 不正・壊れた署名tableを開発modeで通さない
		if( certificate.VirtualAddress != 0U || certificate.Size != 0U) {
			return false ;
		}

		TrustedSignature signature( path, file.get()) ;
		return signature.IsUnsigned() ;
	} catch( ...) {
		return false ;
	}
}

UpdateError VerifyClientSignaturePair( const std::filesystem::path &installed,
									  const std::filesystem::path &candidate) noexcept
{
	try {
		detail::KernelHandle current_file ;
		detail::KernelHandle candidate_file ;
		std::uint64_t		 current_size	= 0U ;
		std::uint64_t		 candidate_size = 0U ;
		// 署名検証中に対象fileが別pathへ入れ替わらないようreadonly lockする
		if( OpenImage( installed, current_file, current_size) != UpdateError::None ||
		   OpenImage( candidate, candidate_file, candidate_size) != UpdateError::None) {
			return UpdateError::FileFailure ;
		}

		TrustedSignature current( installed, current_file.get()) ;
		TrustedSignature newer( candidate, candidate_file.get()) ;
		// 未署名buildやWindowsで信頼されないpublisherをanchorとして扱わない
		if( !current.Verify() || !newer.Verify()) {
			return UpdateError::InvalidSignature ;
		}

		return current.SamePublisher( newer) ? UpdateError::None : UpdateError::SignerMismatch ;
	} catch( ...) {
		return UpdateError::ResourceExhausted ;
	}
}

UpdateError ValidateDownloadedClient( const std::filesystem::path &installed,
									 const std::filesystem::path &candidate,
									 const ReleaseManifest		 &release,
									 const Sha256				 &installed_hash) noexcept
{
	Sha256		  original{} ;
	Sha256		  downloaded{} ;
	std::uint64_t original_size	  = 0U ;
	std::uint64_t downloaded_size = 0U ;
	// 配布先と独立した旧版hashを比較し、実行中fileの変更を検知する
	if( ComputeUpdateFileHash( installed, original, original_size) != UpdateError::None ||
	   original != installed_hash) {
		return UpdateError::InstalledImageChanged ;
	}

	// 完成したdownloadのsizeと全byte digestがmanifestへ一致するまで実行しない
	if( release.size_bytes == 0U || release.size_bytes > kMaximumUpdateExecutableBytes ||
	   ComputeUpdateFileHash( candidate, downloaded, downloaded_size) != UpdateError::None ||
	   downloaded_size != release.size_bytes || downloaded != release.sha256) {
		return UpdateError::HashMismatch ;
	}

	const UpdateError signature = VerifyClientSignaturePair( installed, candidate) ;
	// 有効なWindows署名でも別publisherなら拒否する
	if( signature != UpdateError::None) {
		return signature ;
	}

	ClientImageMetadata current ;
	ClientImageMetadata newer ;
	// Windows version、正式製品名、更新bootstrapを署名対象resourceから確認する
	if( ReadClientImageMetadata( installed, current) != UpdateError::None ||
	   ReadClientImageMetadata( candidate, newer) != UpdateError::None ||
	   !current.update_bootstrap || !newer.update_bootstrap || !current.paired_update ||
	   !newer.paired_update || newer.version != release.version ||
	   newer.version <= current.version) {
		return UpdateError::MetadataMismatch ;
	}

	// 二EXEの更新でもwire非互換版を無確認で採用しない
	if( release.bridge_protocol != protocol::kProtocolVersion ||
	   current.bridge_protocol != protocol::kProtocolVersion ||
	   newer.bridge_protocol != protocol::kProtocolVersion) {
		return UpdateError::IncompatibleProtocol ;
	}

	return UpdateError::None ;
}

UpdateError ValidateDownloadedAgent( const std::filesystem::path &installed,
									const std::filesystem::path &candidate,
									const ReleaseManifest		&release) noexcept
{
	Sha256		  downloaded{} ;
	std::uint64_t bytes = 0U ;
	// Client候補と独立したAgentのexact sizeと全byteを署名検証前に照合する
	if( release.agent_size_bytes == 0U ||
	   release.agent_size_bytes > kMaximumUpdateAgentExecutableBytes ||
	   ComputeUpdateFileHash( candidate, downloaded, bytes) != UpdateError::None ||
	   bytes != release.agent_size_bytes || downloaded != release.agent_sha256) {
		return UpdateError::HashMismatch ;
	}

	const auto signature = VerifyClientSignaturePair( installed, candidate) ;
	// 正規署名があっても現行Clientと異なる発行者は採用しない
	if( signature != UpdateError::None) {
		return signature ;
	}

	ClientImageMetadata agent ;
	// Client EXE、混在版、setup非対応、異なるprotocolのAgentを実行させない
	if( ReadAgentImageMetadata( candidate, agent) != UpdateError::None) {
		return UpdateError::MetadataMismatch ;
	}

	return ValidateAgentUpdateMetadata( agent, release) ;
}

UpdateError ValidateAgentUpdateMetadata( const ClientImageMetadata &agent,
										const ReleaseManifest	  &release) noexcept
{
	// 混在版やsetup非対応Agentをセットの一部として採用しない
	if( agent.version != release.version || !agent.agent_setup) {
		return UpdateError::MetadataMismatch ;
	}

	return agent.bridge_protocol == release.bridge_protocol &&
				   release.bridge_protocol == protocol::kProtocolVersion
			   ? UpdateError::None
			   : UpdateError::IncompatibleProtocol ;
}

} // namespace zrdp::client::update
