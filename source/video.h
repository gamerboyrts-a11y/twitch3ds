#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <citro2d.h>

/* Returns false on old 3DS (no MVD hardware). */
bool video_init(void);
void video_exit(void);

/* channel = bare name (no '#').  oauth_pass = full "PASS oauth:xxx" string. */
void video_start(const char *channel, const char *oauth_pass, const char *client_id);
void video_stop(void);

/* Call BEFORE C3D_FrameBegin to upload any pending decoded frame. */
void video_upload_frame(void);

/* Call from draw_top() inside a C3D frame. */
void video_draw_top(float x, float y);

bool video_is_offline(void);
bool video_is_active(void);
