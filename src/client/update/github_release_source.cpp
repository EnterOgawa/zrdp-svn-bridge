//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file github_release_source.cpp
 * @brief WinHTTPで許可HTTPS hostと有限redirectだけからReleaseを取得する。
 * @version 01.00.00
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  bounded GitHub HTTPS取得を追加。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/update/github_release_source.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace zrdp::client::update {
namespace {

// 個々のnetwork操作を待つ上限millisecond
constexpr int		  kNetworkOperationTimeoutMilliseconds = 5000 ;
// redirectを含む一回の取得全体の上限millisecond
constexpr ULONGLONG	  kDownloadTimeoutMilliseconds		   = 120000U ;
// 中継先変更を無限に繰り返さない上限
constexpr std::size_t kMaximumRedirects					   = 4U ;
// signed URLを含むheaderの有限byte上限
constexpr DWORD		  kMaximumResponseHeaderBytes		   = 16U * 1024U ;
// 一回の取得で保持するbyte上限
constexpr DWORD		  kDownloadChunkBytes				   = 64U * 1024U ;

/** @brief WinHTTP HANDLEを適切なAPIで閉じる。 */
struct InternetCloser {
	/**
	 * @brief 所有WinHTTP HANDLEを閉じる。
	 * @param[in] value session、connectionまたはrequest。
	 */
	void operator()( void *value) const noexcept
	{
		static_cast<void>( WinHttpCloseHandle( value)) ;
	}
} ;

/** @brief WinHTTPのHANDLE単独所有型。 */
using InternetHandle = std::unique_ptr<void, InternetCloser> ;

/**
 * @brief HTTPS redirectのLocationを有限ASCIIで取得する。
 * @param[in] request 現在のrequest HANDLE。
 * @param[out] location 検証後のURL。
 * @return 完全な許可URLならNone。
 */
[[nodiscard]] UpdateError ReadLocation( const HINTERNET request, std::string &location)
{
	DWORD bytes = 0U ;
	// Locationの欠落や巨大値を別経路の取得先として採用しない
	if( WinHttpQueryHeaders( request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
						   &bytes, WINHTTP_NO_HEADER_INDEX) ||
	   GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof( wchar_t) ||
	   bytes > kMaximumResponseHeaderBytes || bytes % sizeof( wchar_t) != 0U) {
		return UpdateError::InvalidRedirect ;
	}

	std::vector<wchar_t> buffer( bytes / sizeof( wchar_t), L'\0') ;
	// 途中までのLocationを使用しない
	if( !WinHttpQueryHeaders( request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
							buffer.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) {
		return UpdateError::InvalidRedirect ;
	}

	const auto end = std::find( buffer.begin(), buffer.end(), L'\0') ;
	// 終端とURL文字上限を確認してからASCIIへ変換する
	if( end == buffer.end() || end - buffer.begin() > 8192) {
		return UpdateError::InvalidRedirect ;
	}

	std::string result ;
	result.reserve( static_cast<std::size_t>( end - buffer.begin())) ;
	// Unicode、control、userinfo等の別解釈をpolicyで拒否する
	for( auto iterator = buffer.begin(); iterator != end; ++iterator) {
		// 可視ASCII以外をURL文字へ切り詰めない
		if( *iterator <= 0x20 || *iterator >= 0x7f) {
			return UpdateError::InvalidRedirect ;
		}

		result.push_back( static_cast<char>( *iterator)) ;
	}

	// relative URL、HTTP、許可外hostを自動補正せず拒否する
	if( !IsAllowedUpdateUrl( result)) {
		return UpdateError::InvalidRedirect ;
	}

	location = std::move( result) ;
	return UpdateError::None ;
}

/**
 * @brief 宣言されたContent-Lengthを有限値として確認する。
 * @param[in] request 200 responseのrequest HANDLE。
 * @param[in] maximum body上限。
 * @param[out] declared Content-Lengthが存在する場合の値。
 * @return 欠落または有効な宣言ならNone。
 */
[[nodiscard]] UpdateError ReadDeclaredLength( const HINTERNET request, const std::uint64_t maximum,
											 std::optional<std::uint64_t> &declared) noexcept
{
	DWORD value = 0U ;
	DWORD bytes = sizeof( value) ;
	// WinHTTPの数値解析失敗を「長さ不明」として通さない
	if( !WinHttpQueryHeaders( request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
							WINHTTP_HEADER_NAME_BY_INDEX, &value, &bytes,
							WINHTTP_NO_HEADER_INDEX)) {
		return GetLastError() == ERROR_WINHTTP_HEADER_NOT_FOUND ? UpdateError::None
																: UpdateError::NetworkFailure ;
	}

	// 宣言だけで上限超過が分かるbodyを読み始めない
	if( value > maximum) {
		return UpdateError::DataTooLarge ;
	}

	declared = value ;
	return UpdateError::None ;
}

/**
 * @brief request側で自動redirect、cookie、資格情報送信を禁止する。
 * @param[in] request 設定対象request HANDLE。
 * @return 全制限を設定できた場合だけtrue。
 */
[[nodiscard]] bool ConfigureRequest( const HINTERNET request) noexcept
{
	DWORD disabled =
		WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION ;
	DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH ;
	DWORD headers	= kMaximumResponseHeaderBytes ;
	return WinHttpSetOption( request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof( disabled)) &&
		   WinHttpSetOption( request, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon,
							sizeof( autologon)) &&
		   WinHttpSetOption( request, WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE, &headers,
							sizeof( headers)) ;
}

/**
 * @brief 許可済みURL一件のHTTPS requestを構築する。
 * @param[in] session OS通信policyを使用するWinHTTP session。
 * @param[in] url policy検証済みASCII HTTPS URL。
 * @param[out] connection requestより長く所有するconnection。
 * @param[out] request 構成済みのrequest。
 * @return 構成できた場合だけNone。
 */
[[nodiscard]] UpdateError OpenRequest( const HINTERNET session, const std::string &url,
									  InternetHandle &connection, InternetHandle &request)
{
	const std::wstring wide( url.begin(), url.end()) ;
	URL_COMPONENTS	   components{} ;
	components.dwStructSize		 = sizeof( components) ;
	components.dwHostNameLength	 = static_cast<DWORD>( -1) ;
	components.dwUrlPathLength	 = static_cast<DWORD>( -1) ;
	components.dwExtraInfoLength = static_cast<DWORD>( -1) ;
	components.dwUserNameLength	 = static_cast<DWORD>( -1) ;
	components.dwPasswordLength	 = static_cast<DWORD>( -1) ;
	// OS parserでもHTTPS、443、資格情報なしの解釈と一致するか確認する
	if( !WinHttpCrackUrl( wide.c_str(), static_cast<DWORD>( wide.size()), 0U, &components) ||
	   components.nScheme != INTERNET_SCHEME_HTTPS ||
	   components.nPort != INTERNET_DEFAULT_HTTPS_PORT || components.dwUserNameLength != 0U ||
	   components.dwPasswordLength != 0U || components.lpszHostName == nullptr ||
	   components.dwHostNameLength == 0U) {
		return UpdateError::InvalidRedirect ;
	}

	const std::wstring host( components.lpszHostName, components.dwHostNameLength) ;
	std::wstring	   path ;
	// URL pathがない場合だけHTTPSのroot pathを使用する
	if( components.dwUrlPathLength == 0U) {
		path = L"/" ;
	} else {
		path.assign( components.lpszUrlPath, components.dwUrlPathLength) ;
	}

	// GitHubのsigned queryを保持するが、解析・保存・log出力はしない
	if( components.dwExtraInfoLength != 0U) {
		path.append( components.lpszExtraInfo, components.dwExtraInfoLength) ;
	}

	connection.reset( WinHttpConnect( session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0U)) ;
	// 許可hostへの接続準備失敗で他hostやportへfallbackしない
	if( connection == nullptr) {
		return UpdateError::NetworkFailure ;
	}

	const wchar_t *types[] = {L"application/octet-stream", nullptr} ;
	request.reset( WinHttpOpenRequest( connection.get(), L"GET", path.c_str(), nullptr,
									 WINHTTP_NO_REFERER, types, WINHTTP_FLAG_SECURE)) ;
	// 証明書検証の無視設定は一切行わず、制限を設定できなければ送信しない
	if( request == nullptr || !ConfigureRequest( request.get())) {
		return UpdateError::NetworkFailure ;
	}

	return UpdateError::None ;
}

} // namespace

UpdateError DownloadPublicRelease( const std::string_view source, const std::uint64_t maximum,
								  IUpdateContentSink &sink, const std::stop_token stop,
								  std::uint64_t &size_bytes) noexcept
{
	size_bytes = 0U ;
	try {
		// 外部URLと無制限downloadを更新用入口で拒否する
		if( !IsAllowedUpdateUrl( source) || maximum == 0U ||
		   maximum > kMaximumUpdateExecutableBytes) {
			return UpdateError::InvalidRedirect ;
		}

		InternetHandle session( WinHttpOpen( L"ZRdp-SVN-Bridge-Updater/1",
										   WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
										   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0U)) ;
		// OS既存のProxy／policyが許さない場合は更新だけを停止する
		if( session == nullptr ||
		   !WinHttpSetTimeouts( session.get(), kNetworkOperationTimeoutMilliseconds,
							   kNetworkOperationTimeoutMilliseconds,
							   kNetworkOperationTimeoutMilliseconds,
							   kNetworkOperationTimeoutMilliseconds)) {
			return UpdateError::NetworkFailure ;
		}

		DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 ;
		// TLS 1.3のoptionを持たないWindowsでも、TLS 1.2より古い形式は有効化しない
		if( !WinHttpSetOption( session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof( tls))) {
			// optionの互換性以外の失敗をTLS再設定で回避しない
			if( GetLastError() != ERROR_INVALID_PARAMETER) {
				return UpdateError::NetworkFailure ;
			}

			tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 ;
			// platform上でTLS 1.2も設定できなければ取得を行わない
			if( !WinHttpSetOption( session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &tls,
								 sizeof( tls))) {
				return UpdateError::NetworkFailure ;
			}
		}

		std::string		url( source) ;
		const ULONGLONG started = GetTickCount64() ;
		// 最大四回のredirectを手動検証し、最終bodyは一度だけ取得する
		for( std::size_t redirects = 0U; redirects <= kMaximumRedirects; ++redirects) {
			// 終了要求または全体deadline到達で新規requestを始めない
			if( stop.stop_requested() ||
			   GetTickCount64() - started >= kDownloadTimeoutMilliseconds) {
				return UpdateError::Cancelled ;
			}

			InternetHandle	  connection ;
			InternetHandle	  request ;
			const UpdateError opened = OpenRequest( session.get(), url, connection, request) ;
			// 許可URLをOS parserで構成できなければfail-closedにする
			if( opened != UpdateError::None) {
				return opened ;
			}

			// token、Cookie、credential、RDP情報を一切送信しない
			if( !WinHttpSendRequest( request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0U,
								   WINHTTP_NO_REQUEST_DATA, 0U, 0U, 0U) ||
			   !WinHttpReceiveResponse( request.get(), nullptr)) {
				return stop.stop_requested() ? UpdateError::Cancelled : UpdateError::NetworkFailure ;
			}

			DWORD status	   = 0U ;
			DWORD status_bytes = sizeof( status) ;
			// 未確認statusを正常assetと扱わない
			if( !WinHttpQueryHeaders(
				   request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				   WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_bytes, WINHTTP_NO_HEADER_INDEX)) {
				return UpdateError::NetworkFailure ;
			}

			// redirectごとにHTTPSとhostを再検証し、認証／404／error responseを保存しない
			if( status == 301U || status == 302U || status == 303U || status == 307U ||
			   status == 308U) {
				// 無限redirectや上限のもう一回を許可しない
				if( redirects == kMaximumRedirects) {
					return UpdateError::InvalidRedirect ;
				}

				const UpdateError redirected = ReadLocation( request.get(), url) ;
				// 許可外redirectへrequestを発行しない
				if( redirected != UpdateError::None) {
					return redirected ;
				}

				continue ;
			}

			// 部分responseや認証要求を完成したReleaseと扱わない
			if( status != 200U) {
				return UpdateError::NetworkFailure ;
			}

			std::optional<std::uint64_t> declared ;
			const UpdateError length = ReadDeclaredLength( request.get(), maximum, declared) ;
			// 宣言されたsizeが不正ならbodyを取得しない
			if( length != UpdateError::None) {
				return length ;
			}

			std::array<std::byte, kDownloadChunkBytes> buffer{} ;
			std::uint64_t							   total = 0U ;
			// EOFまで固定chunkで取得し、累積上限を毎回確認する
			while( true) {
				// 遅いbodyが有限deadlineを越えて保持されないよう毎chunkで確認する
				if( stop.stop_requested() ||
				   GetTickCount64() - started >= kDownloadTimeoutMilliseconds) {
					return UpdateError::Cancelled ;
				}

				DWORD received = 0U ;
				// network失敗時の部分fileを成功扱いしない
				if( !WinHttpReadData( request.get(), buffer.data(), static_cast<DWORD>( buffer.size()),
									&received)) {
					return stop.stop_requested() ? UpdateError::Cancelled
												 : UpdateError::NetworkFailure ;
				}

				// 完全EOFを得たら宣言と全体sizeを照合する
				if( received == 0U) {
					break ;
				}

				// unsigned underflowを避け、上限を超えるchunkをsinkへ渡さない
				if( received > maximum - total) {
					return UpdateError::DataTooLarge ;
				}

				const UpdateError written = sink.Write( std::span( buffer.data(), received)) ;
				// disk／memory上限の失敗後は取得を継続しない
				if( written != UpdateError::None) {
					return written ;
				}

				total += received ;
			}

			// 完成した空bodyやContent-Length不一致をmanifest／EXEとして使用しない
			if( total == 0U || (declared.has_value() && *declared != total)) {
				return UpdateError::NetworkFailure ;
			}

			size_bytes = total ;
			return UpdateError::None ;
		}

		return UpdateError::InvalidRedirect ;
	} catch( ...) {
		return UpdateError::ResourceExhausted ;
	}
}

} // namespace zrdp::client::update
