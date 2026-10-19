//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file github_release_source.h
 * @brief 公開GitHub ReleaseのHTTPS取得と有限stream sinkを定義する。
 * @version 01.00.00
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  認証なしHTTPS更新source境界を追加。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "client/update/update_manifest.h"

#include <span>
#include <stop_token>

namespace zrdp::client::update {

/** @brief 一chunkごとに内容を所有先へ渡す更新専用sink。 */
class IUpdateContentSink {
public:
	/** @brief sinkの資源を解放する。 */
	virtual ~IUpdateContentSink()													   = default ;
	/**
	 * @brief callback中だけ有効なbyte列を有限の所有先へ書込む。
	 * @param[in] bytes 一回の取得chunk。保持する場合はcopyする。
	 * @return 書込み成功時だけNone。
	 */
	[[nodiscard]] virtual UpdateError Write( std::span<const std::byte> bytes) noexcept = 0 ;
} ;

/**
 * @brief 公開GitHubから認証なしHTTPSで有限byte列だけを取得する。
 * @param[in] url 固定Release URLまたは検証済みasset URL。
 * @param[in] maximum 全bodyのbyte上限。最大64 MiB。
 * @param[in,out] sink worker上で同期的に呼出す所有先。
 * @param[in] stop worker終了要求。
 * @param[out] size_bytes 完全取得したbyte数。失敗時は0。
 * @return redirect、TLS、size、全bodyの検証が成功した場合だけNone。
 * @note UI threadでは呼出さない。OS policy、証明書、Proxy設定を変更しない。
 */
[[nodiscard]] UpdateError DownloadPublicRelease( std::string_view url, std::uint64_t maximum,
												IUpdateContentSink &sink, std::stop_token stop,
												std::uint64_t &size_bytes) noexcept ;

} // namespace zrdp::client::update
