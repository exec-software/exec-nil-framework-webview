// Copyright (C) 2026 Execute Software. SPDX-License-Identifier: MIT
#pragma once
#include "nilwebview/provider_v1.h"
#include <stdexcept>
#include <string>
#include <string_view>

namespace nilwebview::win32 {
// NavigateToString reports its UTF-8 HTML as this data URI in
// NavigationStarting; the resulting document's Source is about:blank.
// Generate the exact owned request, never an allowlist for arbitrary data URIs.
inline std::string html_navigation_uri(std::string_view html) {
    if (html.size() > NILWV_MAX_TEXT_BYTES)
        throw std::length_error("WebView HTML exceeds the text contract");
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string uri = "data:text/html;charset=utf-8;base64,";
    uri.reserve(uri.size() + ((html.size() + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < html.size(); offset += 3) {
        const auto first = static_cast<unsigned char>(html[offset]);
        const auto second = offset + 1 < html.size() ? static_cast<unsigned char>(html[offset + 1]) : 0;
        const auto third = offset + 2 < html.size() ? static_cast<unsigned char>(html[offset + 2]) : 0;
        uri += alphabet[first >> 2];
        uri += alphabet[((first & 3) << 4) | (second >> 4)];
        uri += offset + 1 < html.size() ? alphabet[((second & 15) << 2) | (third >> 6)] : '=';
        uri += offset + 2 < html.size() ? alphabet[third & 63] : '=';
    }
    return uri;
}
} // namespace nilwebview::win32
