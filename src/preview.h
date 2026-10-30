#ifndef ARAVIS_STREAM_CLI_PREVIEW_H
#define ARAVIS_STREAM_CLI_PREVIEW_H

#include <arv.h>

#ifdef __cplusplus
extern "C" {
#endif

int preview_init(const char *window_name);
int preview_frame(ArvBuffer *buffer);
void preview_close(void);

#ifdef __cplusplus
}
#endif

#endif
