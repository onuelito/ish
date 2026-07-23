#ifndef CONSUMMER_H
#define CONSUMMER_H

#include "ishio.h"

/*
 * This is the consumer header file. Which is implemented in
 * ffmpeg.c. However, it could be changed to use something
 * like GStreamer instead if necessary.
 */

struct cons {
#define CONSUMMER_OUTPUT_FILE		0
#define CONSUMMER_OUTPUT_LINK		1
	uint8_t type;
};

struct cons *cons_new(uint16_t, uint16_t, uint8_t type, const char *);
void cons_free(struct cons *);

int cons_write_video(struct cons *, struct ishio_video_buf *);
int cons_write_audio(struct cons *, struct ishio_audio_buf *);

#endif
