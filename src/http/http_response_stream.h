#ifndef ZFTPD_HTTP_RESPONSE_STREAM_H
#define ZFTPD_HTTP_RESPONSE_STREAM_H

#include "http_response.h"

int http_response_stream_send(int fd, http_response_t *response, int send_body);

#endif
