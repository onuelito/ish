#include <stdio.h>
#include <stdlib.h>

#include <sndio.h>

#include "ishio.h"
#include "audio.h"

#define DEFAULT_SAMPLERATE	48000

struct client {
	struct ishio_dev device;
	struct sio_hdl *hdl;
	struct sio_par par;
};

struct audio_sndio_hdl {
	struct audio_hdl hdl;
};

static struct audio_sndio_hdl *shdl;

static void audio_sndio_close(void);
static struct ishio_dev *audio_sndio_add(const char *);
static int audio_sndio_capture(struct ishio_dev *, struct ishio_audio_buf *);

struct audio_ops audio_sndio_ops = {
	audio_sndio_close,
	audio_sndio_add,
	audio_sndio_capture,
};

struct audio_hdl*
audio_open()
{
	shdl = calloc(1, sizeof(struct audio_sndio_hdl));

	if (shdl == NULL)
		return NULL;

	shdl->hdl.ops = &audio_sndio_ops;
	return &(shdl->hdl);
}

static void
audio_sndio_close()
{
}

/*
 * Right now this function assumes sndio gave exacly what was requested.
 * In the future, values set by sio_getpar will be used to set values of
 * the ishio_device.
 */
static struct ishio_dev*
audio_sndio_add(const char *device)
{
	struct client *c = calloc(1, sizeof(struct client));
	c->hdl = sio_open(device, SIO_REC, 0); /* Blocking is fine since its multithreaded */
	if (c->hdl == NULL) {
		free(c);
		return NULL;
	}

	sio_initpar(&c->par);
	c->par.rchan = 2;
	c->par.rate = DEFAULT_SAMPLERATE;
	c->par.bits = 16;
	c->par.le = 1;
	c->par.sig = 1;
	sio_setpar(c->hdl, &c->par);
	sio_getpar(c->hdl, &c->par); /* What did sndio give? (TODO: actually pass values to device) */
	sio_start(c->hdl);

	return &c->device;
}

static int
audio_sndio_capture(struct ishio_dev *dev, struct ishio_audio_buf *buf)
{
	struct client *c = (struct client *)dev;
	buf->count = sio_read(c->hdl, buf->data, ISHIO_AUDIO_SAMPLES * 4) / 4;
	return 0;
}
