//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_deployment_clipboard.cpp
 * @brief 二つのvirtual fileだけを標準OLE clipboardへ提供する。
 * @version 01.00.00
 * @date 2026/10/08
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  遅延転送用descriptorとreadonly streamを追加。
 * </pre>
 * @note CLIPRDRはMicrosoft ActiveXに任せる。独自のnetwork通信は行わない。
 */
//------------------------------------------------------------------------------
#include "client/agent_deployment/agent_deployment.h"
#include <ShlObj.h>
#include <Shlwapi.h>
#include <atomic>
#include <array>
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace zrdp::client::agent_deployment {
namespace {

/** @brief packageを保持してreadonly memoryを独立位置で読むCOM stream。 */
class MemoryStream final : public IStream {
public:
	/** @brief immutable snapshotの一fileを保持する。
	 * @param[in] package 二ファイルの所有参照。
	 * @param[in] index Agent=0、INI=1。
	 * @param[in] position clone時の読取り位置。
	 */
	MemoryStream( std::shared_ptr<const Package> package, LONG index, std::uint64_t position = 0U)
		: package_( std::move( package)), index_( index), position_( position)
	{
	}

	/** @brief COM interfaceを一参照で返す。
	 * @param[in] iid 要求interface。
	 * @param[out] value 所有参照。失敗時はnullptr。
	 * @return 対応interfaceだけS_OK。
	 */
	HRESULT STDMETHODCALLTYPE QueryInterface( REFIID iid, void **value) override
	{
		// nullのout参照へ書かない
		if( value == nullptr) {
			return E_POINTER ;
		}

		*value = nullptr ;
		// stream以外の機能を公開しない
		if( iid != IID_IUnknown && iid != IID_ISequentialStream && iid != IID_IStream) {
			return E_NOINTERFACE ;
		}

		*value = static_cast<IStream *>( this) ;
		AddRef() ;
		return S_OK ;
	}

	/** @brief COM参照を増やす。 @return 新しい参照数。 */
	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return ++references_ ;
	}

	/** @brief 最後の参照でsnapshotを解放する。 @return 残り参照数。 */
	ULONG STDMETHODCALLTYPE Release() override
	{
		const auto remaining = --references_ ;
		// 転送中の別stream参照があるpackageは残る
		if( remaining == 0U) {
			delete this ;
		}

		return remaining ;
	}

	/** @brief 有限memoryを読み、EOFを区別する。
	 * @param[out] buffer caller所有の受信領域。
	 * @param[in] count 要求byte数。
	 * @param[out] received 実際のbyte数。省略可能。
	 * @return 全要求ならS_OK、短いEOFならS_FALSE。
	 */
	HRESULT STDMETHODCALLTYPE Read( void *buffer, ULONG count, ULONG *received) override
	{
		// 失敗時も読込み済みbyteを偽装しない
		if( received != nullptr) {
			*received = 0U ;
		}

		// 非空読みでnull bufferを許可しない
		if( buffer == nullptr && count != 0U) {
			return STG_E_INVALIDPOINTER ;
		}

		AcquireSRWLockExclusive( &lock_) ;
		const auto bytes = Bytes() ;
		const auto length =
			static_cast<ULONG>( std::min<std::uint64_t>( count, bytes.size() - position_)) ;
		// 零byteではnull pointerをmemcpyへ渡さない
		if( length != 0U) {
			std::memcpy( buffer, bytes.data() + position_, length) ;
		}

		position_ += length ;
		ReleaseSRWLockExclusive( &lock_) ;
		// 実際に読めた長さだけをcallerへ返す
		if( received != nullptr) {
			*received = length ;
		}

		return length == count ? S_OK : S_FALSE ;
	}

	/** @brief immutable fileへの書込みを拒否する。
	 * @param[out] written 常に0。省略可能。
	 * @return STG_E_ACCESSDENIED。
	 * @note input bufferとcountはreadonlyのため未使用。
	 */
	HRESULT STDMETHODCALLTYPE Write( const void *, ULONG, ULONG *written) override
	{
		// 呼出側へ書込み成功を返さない
		if( written != nullptr) {
			*written = 0U ;
		}

		return STG_E_ACCESSDENIED ;
	}

	/** @brief file内だけへ符号付きoffsetを適用する。
	 * @param[in] offset 基準からのbyte差。
	 * @param[in] origin SET/CUR/ENDのみ。
	 * @param[out] result 移動後のbyte位置。省略可能。
	 * @return 上限・underflow・未知originではSTG_E_INVALIDFUNCTION。
	 */
	HRESULT STDMETHODCALLTYPE Seek( LARGE_INTEGER offset, DWORD origin,
								   ULARGE_INTEGER *result) override
	{
		// 未知の基準で位置を変化させない
		if( origin > STREAM_SEEK_END) {
			return STG_E_INVALIDFUNCTION ;
		}

		AcquireSRWLockExclusive( &lock_) ;
		const auto size	 = static_cast<std::uint64_t>( Bytes().size()) ;
		const auto base	 = origin == STREAM_SEEK_SET   ? 0U
						   : origin == STREAM_SEEK_CUR ? position_
													   : size ;
		const auto delta = offset.QuadPart ;
		// INT64_MINの符号反転を避け、file範囲外を拒否する
		if( delta < -static_cast<std::int64_t>( base) ||
		   delta > static_cast<std::int64_t>( size - base)) {
			ReleaseSRWLockExclusive( &lock_) ;
			return STG_E_INVALIDFUNCTION ;
		}

		position_ = static_cast<std::uint64_t>( static_cast<std::int64_t>( base) + delta) ;
		// callerが位置を要求した場合だけout参照へ書く
		if( result != nullptr) {
			result->QuadPart = position_ ;
		}

		ReleaseSRWLockExclusive( &lock_) ;
		return S_OK ;
	}

	/** @brief readonly streamのresizeを拒否する。 @return STG_E_ACCESSDENIED。
	 * @note 要求sizeは変更禁止のため未使用。
	 */
	HRESULT STDMETHODCALLTYPE SetSize( ULARGE_INTEGER) override
	{
		return STG_E_ACCESSDENIED ;
	}

	/** @brief 一回64 KiB以下でcaller streamへ内容を渡す。
	 * @param[in,out] target caller所有の出力stream。
	 * @param[in] count 要求byte数。
	 * @param[out] read 読んだbyte数。省略可能。
	 * @param[out] written 書けたbyte数。省略可能。
	 * @return EOFはS_FALSE、部分writeはSTG_E_MEDIUMFULL。
	 */
	HRESULT STDMETHODCALLTYPE CopyTo( IStream *target, ULARGE_INTEGER count, ULARGE_INTEGER *read,
									 ULARGE_INTEGER *written) override
	{
		ULARGE_INTEGER input{}, output{} ;
		// null targetではoutを0にして失敗する
		if( read != nullptr) {
			*read = input ;
		}

		// callerが指定したoutだけを初期化する
		if( written != nullptr) {
			*written = output ;
		}

		// 非所有targetを確認してからCOMを呼ぶ
		if( target == nullptr) {
			return E_POINTER ;
		}

		std::array<std::byte, 65536U> buffer{} ;
		HRESULT						  status = S_OK ;
		// 有限のfile EOFまたは要求byte数まで、所有lock外でtargetを呼ぶ
		while( input.QuadPart < count.QuadPart) {
			ULONG	   got = 0U, put = 0U ;
			const auto step = static_cast<ULONG>(
				std::min<std::uint64_t>( buffer.size(), count.QuadPart - input.QuadPart)) ;
			status = Read( buffer.data(), step, &got) ;
			input.QuadPart += got ;
			const auto write_status = target->Write( buffer.data(), got, &put) ;
			output.QuadPart += put ;
			// 出力失敗・短いwriteは未完了として返す
			if( FAILED( write_status) || put != got) {
				status = FAILED( write_status) ? write_status : STG_E_MEDIUMFULL ;
				break ;
			}

			// 入力EOFで無限loopを防ぐ
			if( status != S_OK) {
				break ;
			}
		}

		// 確認済みの実際の入力量を返す
		if( read != nullptr) {
			*read = input ;
		}

		// 確認済みの実際の出力量を返す
		if( written != nullptr) {
			*written = output ;
		}

		return status ;
	}

	/** @brief readonlyなのでflush不要。 @return S_OK。 @note flagsは未使用。 */
	HRESULT STDMETHODCALLTYPE Commit( DWORD) override
	{
		return S_OK ;
	}

	/** @brief transaction機能を提供しない。 @return STG_E_INVALIDFUNCTION。 */
	HRESULT STDMETHODCALLTYPE Revert() override
	{
		return STG_E_INVALIDFUNCTION ;
	}

	/** @brief region lockを提供しない。 @return STG_E_INVALIDFUNCTION。 @note 全引数は未使用。 */
	HRESULT STDMETHODCALLTYPE LockRegion( ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
	{
		return STG_E_INVALIDFUNCTION ;
	}

	/** @brief region unlockを提供しない。 @return STG_E_INVALIDFUNCTION。 @note 全引数は未使用。 */
	HRESULT STDMETHODCALLTYPE UnlockRegion( ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
	{
		return STG_E_INVALIDFUNCTION ;
	}

	/** @brief pathを公開せずfile長だけを返す。
	 * @param[out] stat readonly streamの全byte長。
	 * @return 正常なout参照ならS_OK。
	 * @note flagsは名前を常に省略するため未使用。
	 */
	HRESULT STDMETHODCALLTYPE Stat( STATSTG *stat, DWORD) override
	{
		// null outを拒否する
		if( stat == nullptr) {
			return E_POINTER ;
		}

		*stat				  = {} ;
		stat->type			  = STGTY_STREAM ;
		stat->cbSize.QuadPart = Bytes().size() ;
		stat->grfMode		  = STGM_READ ;
		return S_OK ;
	}

	/** @brief 大容量copyなしで独立位置のstreamを作る。
	 * @param[out] result callerがReleaseするclone。
	 * @return 成功時はS_OK。allocation失敗はE_OUTOFMEMORY。
	 */
	HRESULT STDMETHODCALLTYPE Clone( IStream **result) override
	{
		// null出力先を拒否する
		if( result == nullptr) {
			return E_POINTER ;
		}

		AcquireSRWLockShared( &lock_) ;
		const auto position = position_ ;
		ReleaseSRWLockShared( &lock_) ;
		*result = new( std::nothrow) MemoryStream( package_, index_, position) ;
		return *result == nullptr ? E_OUTOFMEMORY : S_OK ;
	}

private:
	/** @brief 固定indexの不変byte viewを返す。 @return AgentまたはINIの全範囲。 */
	std::span<const std::byte> Bytes() const noexcept
	{
		return index_ == 0 ? std::span<const std::byte>( package_->agent)
						   : std::as_bytes( std::span( package_->ini.data(), package_->ini.size())) ;
	}

	/// COMの所有参照数。
	std::atomic<ULONG>			   references_{1U} ;
	/// 両fileを転送終了まで保持するsnapshot。
	std::shared_ptr<const Package> package_ ;
	/// 一つの固定file index。
	LONG						   index_ ;
	/// positionだけを保護する非blockingな短いlock。
	SRWLOCK						   lock_ = SRWLOCK_INIT ;
	/// file内の読取り位置。
	std::uint64_t				   position_ ;
} ;

/** @brief 名前・長さとreadonly内容だけを提供する固定二ファイルobject。 */
class FileDataObject final : public IDataObject {
public:
	/** @brief clipboardを変更せずsnapshotと登録formatを保持する。
	 * @param[in] package 完全な二ファイル。
	 */
	explicit FileDataObject( std::shared_ptr<const Package> package) : package_( std::move( package))
	{
		formats_ = {static_cast<CLIPFORMAT>( RegisterClipboardFormatW( CFSTR_FILEDESCRIPTORW)),
					static_cast<CLIPFORMAT>( RegisterClipboardFormatW( CFSTR_FILECONTENTS)),
					static_cast<CLIPFORMAT>( RegisterClipboardFormatW( CFSTR_PREFERREDDROPEFFECT))} ;
	}

	/** @brief format登録の成否を確認する。 @return 三形式とも有効ならtrue。 */
	bool valid() const noexcept
	{
		return formats_[0] != 0 && formats_[1] != 0 && formats_[2] != 0 ;
	}

	/** @brief IDataObjectだけを一参照で公開する。
	 * @param[in] iid 要求interface。
	 * @param[out] result caller所有の一参照。
	 * @return 対応する場合だけS_OK。
	 */
	HRESULT STDMETHODCALLTYPE QueryInterface( REFIID iid, void **result) override
	{
		// null outを拒否する
		if( result == nullptr) {
			return E_POINTER ;
		}

		*result = nullptr ;
		// 汎用実行・他interfaceを公開しない
		if( iid != IID_IUnknown && iid != IID_IDataObject) {
			return E_NOINTERFACE ;
		}

		*result = static_cast<IDataObject *>( this) ;
		AddRef() ;
		return S_OK ;
	}

	/** @brief COM参照を増やす。 @return 新参照数。 */
	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return ++references_ ;
	}

	/** @brief 最後の参照でsnapshotを解放する。 @return 残り参照数。 */
	ULONG STDMETHODCALLTYPE Release() override
	{
		const auto remaining = --references_ ;
		// 別streamが保持するsnapshotはこの解放後も生存する
		if( remaining == 0U) {
			delete this ;
		}

		return remaining ;
	}

	/** @brief 未知format、index、aspect、mediumをfail-closedで拒否する。
	 * @param[in] format 要求するfile形式。
	 * @return 採用できる場合だけS_OK。
	 */
	HRESULT STDMETHODCALLTYPE QueryGetData( FORMATETC *format) override
	{
		// null formatではmetadataを参照しない
		if( format == nullptr) {
			return E_POINTER ;
		}

		// 印刷deviceや別aspectはfile転送へ持ち込まない
		if( format->ptd != nullptr || format->dwAspect != DVASPECT_CONTENT) {
			return DV_E_FORMATETC ;
		}

		// 内容は独立streamと二つの有効indexだけに限定する
		if( format->cfFormat == formats_[1]) {
			return format->lindex >= 0 && format->lindex < 2 &&
						   (format->tymed & TYMED_ISTREAM) != 0U
					   ? S_OK
					   : DV_E_FORMATETC ;
		}

		return (format->cfFormat == formats_[0] || format->cfFormat == formats_[2]) &&
					   format->lindex == -1 && (format->tymed & TYMED_HGLOBAL) != 0U
				   ? S_OK
				   : DV_E_FORMATETC ;
	}

	/** @brief 小さなdescriptorまたは大容量copyなしの内容streamを返す。
	 * @param[in] format 検証する要求形式。
	 * @param[out] medium callerがReleaseStgMediumする所有結果。
	 * @return 完全に作れた場合だけS_OK。
	 */
	HRESULT STDMETHODCALLTYPE GetData( FORMATETC *format, STGMEDIUM *medium) override
	{
		// null outを拒否する
		if( medium == nullptr) {
			return E_POINTER ;
		}

		*medium					= {} ;
		const auto valid_format = QueryGetData( format) ;
		// 未知形式に内容を返さない
		if( valid_format != S_OK) {
			return valid_format ;
		}

		// 遅延読込みstreamはshared snapshotを保持するだけでfile I/Oをしない
		if( format->cfFormat == formats_[1]) {
			medium->pstm = new( std::nothrow) MemoryStream( package_, format->lindex) ;
			// allocation失敗でTYMED_ISTREAMを偽装しない
			if( medium->pstm == nullptr) {
				return E_OUTOFMEMORY ;
			}

			medium->tymed = TYMED_ISTREAM ;
			return S_OK ;
		}

		const auto bytes  = format->cfFormat == formats_[0]
								? offsetof( FILEGROUPDESCRIPTORW, fgd) + 2U * sizeof( FILEDESCRIPTORW)
								: sizeof( DWORD) ;
		const auto memory = GlobalAlloc( GMEM_MOVEABLE | GMEM_ZEROINIT, bytes) ;
		// OLEに所有を渡せないmemoryを返さない
		if( memory == nullptr) {
			return E_OUTOFMEMORY ;
		}

		auto raw = GlobalLock( memory) ;
		// lock失敗時は未公開領域を回収する
		if( raw == nullptr) {
			GlobalFree( memory) ;
			return E_OUTOFMEMORY ;
		}

		// 名前を製品固定名だけにし、pathやprofile名はclipboardへ出さない
		if( format->cfFormat == formats_[0]) {
			auto *group	  = static_cast<FILEGROUPDESCRIPTORW *>( raw) ;
			group->cItems = 2U ;
			const std::array<const wchar_t *, 2U> names{L"ZRdpSvnAgent.exe", L"agent.ini"} ;
			// 正式名とbyte長を二fileへ設定し、directoryや実行要求は指定しない
			for( std::size_t i = 0U; i < 2U; ++i) {
				auto &file	 = group->fgd[i] ;
				file.dwFlags = static_cast<DWORD>( FD_ATTRIBUTES | FD_FILESIZE | FD_UNICODE) ;
				file.dwFileAttributes = FILE_ATTRIBUTE_NORMAL ;
				file.nFileSizeLow =
					static_cast<DWORD>( i == 0U ? package_->agent.size() : package_->ini.size()) ;
				wcscpy_s( file.cFileName, names[i]) ;
			}
		} else {
			*static_cast<DWORD *>( raw) = DROPEFFECT_COPY ;
		}

		GlobalUnlock( memory) ;
		medium->tymed	= TYMED_HGLOBAL ;
		medium->hGlobal = memory ;
		return S_OK ;
	}

	/** @brief caller領域への直接書込みを提供しない。 @return E_NOTIMPL。 @note 両引数は未使用。 */
	HRESULT STDMETHODCALLTYPE GetDataHere( FORMATETC *, STGMEDIUM *) override
	{
		return E_NOTIMPL ;
	}

	/** @brief 変換を提供しない。 @param[out] out deviceはnullptrにする。 @return E_NOTIMPL。 @note inputは未使用。 */
	HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc( FORMATETC *, FORMATETC *out) override
	{
		// caller所有のdeviceを生成しない
		if( out != nullptr) {
			out->ptd = nullptr ;
		}

		return E_NOTIMPL ;
	}

	/** @brief clipboardからの書込みやmoveを採用しない。 @return E_NOTIMPL。 @note 全引数は未使用で所有も受け取らない。 */
	HRESULT STDMETHODCALLTYPE SetData( FORMATETC *, STGMEDIUM *, BOOL) override
	{
		return E_NOTIMPL ;
	}

	/** @brief 固定三形式をWindows標準enumeratorへ渡す。
	 * @param[in] direction GETのみ。
	 * @param[out] result caller所有のenumerator。
	 * @return GET列挙を作れた場合はS_OK。
	 */
	HRESULT STDMETHODCALLTYPE EnumFormatEtc( DWORD direction, IEnumFORMATETC **result) override
	{
		// null outを拒否する
		if( result == nullptr) {
			return E_POINTER ;
		}

		*result = nullptr ;
		// callerからの更新形式を公開しない
		if( direction != DATADIR_GET) {
			return E_NOTIMPL ;
		}

		std::array<FORMATETC, 3U> formats{
			{{formats_[0], nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
			 {formats_[1], nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM},
			 {formats_[2], nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}}} ;
		return SHCreateStdEnumFmtEtc( static_cast<UINT>( formats.size()), formats.data(), result) ;
	}

	/** @brief advisoryを提供しない。 @return OLE_E_ADVISENOTSUPPORTED。 @note 全引数は未使用。 */
	HRESULT STDMETHODCALLTYPE DAdvise( FORMATETC *, DWORD, IAdviseSink *, DWORD *) override
	{
		return OLE_E_ADVISENOTSUPPORTED ;
	}

	/** @brief advisory解除を提供しない。 @return OLE_E_ADVISENOTSUPPORTED。 @note cookieは未使用。 */
	HRESULT STDMETHODCALLTYPE DUnadvise( DWORD) override
	{
		return OLE_E_ADVISENOTSUPPORTED ;
	}

	/** @brief advisory一覧を持たない。 @param[out] result 常にnullptr。 @return OLE_E_ADVISENOTSUPPORTED。 */
	HRESULT STDMETHODCALLTYPE EnumDAdvise( IEnumSTATDATA **result) override
	{
		// 所有していないenumeratorを返さない
		if( result != nullptr) {
			*result = nullptr ;
		}

		return OLE_E_ADVISENOTSUPPORTED ;
	}

private:
	/// OLE所有参照数。
	std::atomic<ULONG>			   references_{1U} ;
	/// FileContentsで返すstreamにも共有するsnapshot。
	std::shared_ptr<const Package> package_ ;
	/// Descriptor、Contents、PreferredDropEffectの固定順登録ID。
	std::array<CLIPFORMAT, 3U>	   formats_{} ;
} ;

} // namespace

HRESULT MakeDataObject( std::shared_ptr<const Package> package, IDataObject **object) noexcept
{
	// null outを拒否する
	if( object == nullptr) {
		return E_POINTER ;
	}

	*object = nullptr ;
	// fakeを含むcallerから上限外・空packageを公開しない
	if( package == nullptr || package->agent.empty() || package->agent.size() > kMaximumAgentBytes ||
	   package->ini.empty() ||
	   package->ini.size() > agent_settings::kMaximumAgentSettingsPlaintext) {
		return E_INVALIDARG ;
	}

	auto *result = new( std::nothrow) FileDataObject( std::move( package)) ;
	// 登録失敗やallocation失敗では部分objectを公開しない
	if( result == nullptr) {
		return E_OUTOFMEMORY ;
	}

	// Clipboard format登録が全て成功した場合だけ採用する
	if( !result->valid()) {
		result->Release() ;
		return E_FAIL ;
	}

	*object = result ;
	return S_OK ;
}

} // namespace zrdp::client::agent_deployment
