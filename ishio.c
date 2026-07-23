#include <stdio.h>
#include <stdlib.h>

#include "ishio.h"
#include "audio.h"
#include "video.h"

struct ishio_int_hdl {
	struct ishio_hdl hdl;
	struct video_hdl *vhdl;
	struct audio_hdl *ahdl;
};

/*
 * Initialize video and audio backends
 */
struct ishio_hdl*
ishio_open()
{
	struct ishio_int_hdl *ihdl = calloc(1, sizeof(struct ishio_int_hdl));

	if (ihdl == NULL)
		return NULL;

	ihdl->vhdl = video_open();
	if (ihdl->vhdl == NULL) {
		free(ihdl);
		return NULL;
	}

	ihdl->ahdl = audio_open();
	if (ihdl->ahdl == NULL) {
		ihdl->vhdl->ops->close();
		free(ihdl);
		return NULL;
	}

	return &(ihdl->hdl);
}

void
ishio_close(struct ishio_hdl *hdl)
{
	struct ishio_int_hdl *ihdl = (struct ishio_int_hdl *)hdl;
	ihdl->vhdl->ops->close();
	ihdl->ahdl->ops->close();
	free(ihdl);
}


int
ishio_ls_video(struct ishio_hdl *hdl)
{
	struct ishio_int_hdl *ihdl = (struct ishio_int_hdl *)hdl;
	return ihdl->vhdl->ops->ls();
}

struct ishio_dev*
ishio_add_video(struct ishio_hdl *hdl, int id)
{
	struct ishio_int_hdl *ihdl = (struct ishio_int_hdl *)hdl;
	return ihdl->vhdl->ops->add(id);
}

struct ishio_dev*
ishio_add_audio(struct ishio_hdl *hdl, const char *device)
{
	struct ishio_int_hdl *ihdl = (struct ishio_int_hdl *)hdl;
	return ihdl->ahdl->ops->add(device);
}

int
ishio_fill_video_buf(struct ishio_hdl *hdl, struct ishio_dev *dev, struct ishio_video_buf *buf)
{
	struct ishio_int_hdl *ihdl = (struct ishio_int_hdl *)hdl;
	int ret = ihdl->vhdl->ops->capture(dev, buf);
	return ret;
}

struct ishio_video_buf*
ishio_video_buf_new(uint16_t width, uint16_t height, uint8_t format)
{
	struct ishio_video_buf *buf = calloc(1, sizeof(struct ishio_video_buf));

	if (buf == NULL)
		return NULL;

	buf->height = height;
	buf->width = width;
	buf->format = format;

	switch (format) {
	case ISHIO_FMT_ARGB:
		buf->size = 4 * width * height;
		buf->depth = 4;
		break;
	case ISHIO_FMT_RGB:
		buf->size = 3 * width * height;
		buf->depth = 3;
		break;
	}

	buf->data = malloc(buf->size * sizeof(uint8_t));
	if (buf->data == NULL) {
		free(buf);
		return NULL;
	}

	return buf;
}

struct ishio_audio_buf*
ishio_audio_buf_new()
{
	struct ishio_audio_buf *buf = calloc(1, sizeof(struct ishio_audio_buf));

	if (buf == NULL)
		return NULL;

	buf->data = calloc(ISHIO_AUDIO_SAMPLES * 4, sizeof(*buf->data));
	if (buf->data == NULL) {
		free(buf);
		return NULL;
	}
	return buf;
}

int 
ishio_fill_audio_buf(struct ishio_hdl *hdl, struct ishio_dev *dev, struct ishio_audio_buf *buf)
{
	struct ishio_int_hdl *ihdl = (struct ishio_int_hdl *)hdl;
	int ret = ihdl->ahdl->ops->capture(dev, buf);
	return 0;
}
