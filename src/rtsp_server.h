#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rtsp_server_start(void);

// For the /status endpoint: connected control sessions / actively streaming.
int rtsp_server_client_count(void);
int rtsp_server_streaming_count(void);

#ifdef __cplusplus
}
#endif
