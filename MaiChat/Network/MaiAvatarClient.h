#pragma once

#include "MaiCurlDownload.h"
#include "MaiFirstPartyRoute.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiAvatarCancel MaiAvatarCancel;

typedef enum MaiAvatarStatus {
    MAI_AVATAR_OK = 0,
    MAI_AVATAR_INVALID_ARGUMENT = 1,
    MAI_AVATAR_NETWORK_ERROR = 2,
    MAI_AVATAR_HTTP_ERROR = 3,
    MAI_AVATAR_CANCELED = 4,
    MAI_AVATAR_INVALID_RESPONSE = 5
} MaiAvatarStatus;

// Borrowed until the synchronous worker-thread call returns. The host captures
// these credentials and account ID from one login generation, cancels on account
// changes, and rechecks that generation before committing the IM profile update.
typedef struct MaiAvatarOptions {
    const char* media_base_url_utf8;
    const char* account_token_utf8;
    const char* account_id_utf8;
    const char* ca_pem;
    size_t ca_pem_length;
    const char* ca_path_utf8;
    const char* proxy_url_utf8;
    const char* proxy_username_utf8;
    const char* proxy_password_utf8;
    MaiDownloadProxyResolver proxy_resolver;
    void* proxy_context;
    int connect_timeout_ms;
    int transfer_timeout_ms;
    // Defaults to aliyuncs.com; only local TLS fixtures override this suffix.
    const char* signed_upload_host_suffix_utf8;
    MaiFirstPartyRouteSelector first_party_route_selector;
    MaiFirstPartyRouteInvalidator first_party_route_invalidator;
    void* first_party_route_context;
} MaiAvatarOptions;

typedef struct MaiAvatarResult {
    MaiAvatarStatus status;
    long http_status;
    int curl_code;
    // Owned, stable public avatar URL only on success. No signed URLs, account
    // credentials or raw control responses are returned; free with result_free.
    char* avatar_url_utf8;
} MaiAvatarResult;

MaiAvatarCancel* mai_avatar_cancel_create(void);
void mai_avatar_cancel_request(MaiAvatarCancel* cancel);
// Destroy only after the worker call using this handle has returned.
void mai_avatar_cancel_destroy(MaiAvatarCancel* cancel);

// Upload prepared JPEG/PNG bytes (1..2,000,000) via the existing im_avatar signer
// and a direct signed OSS PUT. The input is borrowed and never modified.
// Signer and stable avatar URL must belong to the configured media origin;
// signed PUT must match the returned object path and sends no account Bearer.
// Redirects are refused. This call does not modify the IM profile itself.
void mai_avatar_upload(const MaiAvatarOptions* options, const unsigned char* data, size_t size,
                       const char* content_type_utf8, const MaiAvatarCancel* cancel, MaiAvatarResult* result);
void mai_avatar_result_free(MaiAvatarResult* result);

#ifdef __cplusplus
}
#endif
