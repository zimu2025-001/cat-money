#pragma once

#include <string>

#if __has_include("lucky_cat_private_config.h")
#include "lucky_cat_private_config.h"
#endif

namespace lucky_cat_remote_config {

// The optional test configuration takes precedence without rewriting NVS.
// Without it, the caller's existing NVS/menuconfig selection is unchanged.
inline std::string ResolveUrl(const std::string& fallback_url) {
#ifdef ZHIMU_LUCKY_CAT_SERVER_IP
    (void)fallback_url;
    const std::string server_ip = ZHIMU_LUCKY_CAT_SERVER_IP;
    if (server_ip.empty()) {
        return {};
    }
    return "ws://" + server_ip + ":8080/device";
#else
    return fallback_url;
#endif
}

}  // namespace lucky_cat_remote_config
