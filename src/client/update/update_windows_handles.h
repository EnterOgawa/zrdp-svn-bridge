//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file update_windows_handles.h
 * @brief 更新backendのKernel HANDLEを単独所有するRAIIを定義する。
 * @version 01.00.00
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  更新専用のHANDLE所有権を追加。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include <Windows.h>

#include <utility>

namespace zrdp::client::update::detail {

/** @brief CloseHandle対象を複製せず、移動時だけ所有移転する。 */
class KernelHandle final {
public:
	/**
	 * @brief nullまたは所有HANDLEを保持する。
	 * @param[in] handle CloseHandle責任を移転する値。
	 */
	explicit KernelHandle( HANDLE handle = nullptr) noexcept : handle_( handle)
	{
	}

	/** @brief 有効な所有HANDLEを一度だけ閉じる。 */
	~KernelHandle()
	{
		reset() ;
	}

	/** @brief HANDLEの複製を禁止する。 */
	KernelHandle( const KernelHandle &)			  = delete ;
	/**
	 * @brief HANDLEの複製を禁止する。
	 * @return 呼出し禁止のため値を生成しない。
	 */
	KernelHandle &operator=( const KernelHandle &) = delete ;
	/**
	 * @brief 移動元から単独所有権を引き継ぐ。
	 * @param[in,out] other 所有権を空にする移動元。
	 */
	KernelHandle( KernelHandle &&other) noexcept : handle_( other.release())
	{
	}

	/**
	 * @brief 現在のHANDLEを閉じて移動元を引き継ぐ。
	 * @param[in,out] other 所有権を空にする移動元。
	 * @return 自身への参照。
	 */
	KernelHandle &operator=( KernelHandle &&other) noexcept
	{
		// 自己移動で所有HANDLEを閉じない
		if( this != &other) {
			reset( other.release()) ;
		}

		return *this ;
	}

	/**
	 * @brief API呼出し中だけ使用する非所有値を返す。
	 * @return null／INVALID_HANDLE_VALUEを含む保持値。
	 */
	[[nodiscard]] HANDLE get() const noexcept
	{
		return handle_ ;
	}

	/**
	 * @brief 有効なHANDLEを所有しているか返す。
	 * @return nullとINVALID_HANDLE_VALUE以外ならtrue。
	 */
	[[nodiscard]] bool valid() const noexcept
	{
		return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE ;
	}

	/**
	 * @brief callerへ所有権を移転する。
	 * @return 移転した値。自身はnullとなる。
	 */
	[[nodiscard]] HANDLE release() noexcept
	{
		return std::exchange( handle_, nullptr) ;
	}

	/**
	 * @brief 現在の値を閉じ、新しい所有値を保持する。
	 * @param[in] handle 新たに所有する値。
	 */
	void reset( HANDLE handle = nullptr) noexcept
	{
		// nullやAPI失敗sentinelをCloseHandleへ渡さない
		if( valid()) {
			static_cast<void>( CloseHandle( handle_)) ;
		}

		handle_ = handle ;
	}

private:
	/// このwrapperだけがCloseHandle責任を持つ値。
	HANDLE handle_{nullptr} ;
} ;

} // namespace zrdp::client::update::detail
