#ifndef ISHIO_H
#define ISHIO_H

#include <time.h>

#define ISHIO_AUDIO_SAMPLES		1024

struct ishio_hdl {
	struct ishio_ops *ops;
};

/*
 * Device can be either Video or Audio
 */
struct ishio_dev {
#define ISHIODEV_VIDEO 		0
#define ISHIODEV_AUDIO		1
#define ISHIODEV_CAMERA		2
	unsigned char type;
};

struct ishio_video_buf {
#define ISHIO_FMT_NONE		0
#define ISHIO_FMT_RGB		1
#define ISHIO_FMT_ARGB		2
	uint8_t format;
	uint8_t *data;
	uint16_t width;
	uint16_t height;
	size_t size;
	int depth;
	struct timespec ts;
};

struct ishio_audio_buf {
	uint8_t *data;
	size_t count; /* This the number of samples (already divided by channel count) */
};

struct ishio_ops {
	struct ishio_dev *(*ishio_dev_open) (unsigned char);
};

struct ishio_hdl *ishio_open(void);
void ishio_close(struct ishio_hdl *);

int ishio_ls_video(struct ishio_hdl *);
struct ishio_dev *ishio_add_video(struct ishio_hdl *, int);
struct ishio_dev *ishio_add_audio(struct ishio_hdl *, const char *);

struct ishio_video_buf *ishio_video_buf_new(uint16_t, uint16_t, uint8_t);
int ishio_fill_video_buf(struct ishio_hdl *, struct ishio_dev *, struct ishio_video_buf *);

struct ishio_audio_buf *ishio_audio_buf_new(void);
int ishio_fill_audio_buf(struct ishio_hdl *, struct ishio_dev *, struct ishio_audio_buf *);

#endif
