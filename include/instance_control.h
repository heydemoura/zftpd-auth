#ifndef ZFTPD_INSTANCE_CONTROL_H
#define ZFTPD_INSTANCE_CONTROL_H

/* Replace the previous console payload: ask it to shut down cooperatively,
 * verify that it released the console, and only then let this instance start.
 * Returns -1 when a verified previous instance is still alive; the caller must
 * not bind fallback service ports in that case. */
int instance_control_replace_previous(void);

/* Control listener and identity file for this payload. */
int instance_control_start(void);
void instance_control_stop(void);

/* Ask the payload's main thread to run its normal shutdown sequence. */
void zftpd_shutdown_request(void);

#endif
