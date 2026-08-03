#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>

#include <sndio.h>
#include <sys/time.h>
#include <sys/queue.h>

#include "ishio.h"
#include "audio.h"

#define DEFAULT_SAMPLERATE	48000
#define MAX_DEVS			16
#define RING_SIZE			DEFAULT_SAMPLERATE * 5

struct client_ring {
	int16_t data[RING_SIZE * 2]; /* 2 channels */
	uint64_t consumer;
	uint64_t producer;
	size_t count;
};

struct client {
	struct ishio_dev device;
	struct client_ring ring;
	struct sio_hdl *hdl;
	struct sio_par par;
	struct timespec time;
	int64_t diff;
	int started;
	char *name;

	TAILQ_ENTRY(client) entries;
};

struct audio_sndio_hdl {
	struct audio_hdl hdl;
	struct pollfd *pfds;
	size_t npfds;
	int nchildren;

	TAILQ_HEAD(client_queue, client) queue;
};

static struct audio_sndio_hdl *shdl;

static void
ring_produce(struct client_ring *r, int16_t *frames, size_t count)
{
	for (int i = 0; i < count; i++) {
		if (r->count >= RING_SIZE * 2)
			break;

		r->data[r->producer % (RING_SIZE * 2)] = frames[i];
		r->producer++;
		r->count++;
	}
}

static int
ring_consume(struct client_ring *r, int16_t *dst, uint64_t count)
{
	uint64_t read = 0;

	if (r->count == 0)
		return 0;

	for (int i = 0; i < count; i++) {
		if (r->count == 0)
			break;

		uint64_t index = (r->consumer) % (RING_SIZE * 2);
		dst[i] = r->data[index];
		r->consumer++;
		r->count--;
		read++;
	}
	return read;
}

static void
ring_move(struct client_ring *r, int64_t diff)
{
	if (diff <= 0)
		return;

	if (diff > r->count)
		diff = r->count;

	r->consumer += diff;
	r->count -= diff;
}

static void
on_move(void *arg, int delta)
{
	struct client *c = (struct client *)arg;
	struct timespec dt;

	if (c->started == 0) {
		clock_gettime(CLOCK_MONOTONIC, &c->time);
		c->started = 1;
	} else {
		dt.tv_sec = delta / c->par.rate;
		dt.tv_nsec = ((uint64_t)(delta % c->par.rate) * 1000000000ULL) / c->par.rate;
		timespecadd(&c->time, &dt, &c->time);
	}
}

static void audio_sndio_close(void);
static struct ishio_dev *audio_sndio_add(const char *);
static int audio_sndio_capture(struct ishio_audio_buf *);

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
	shdl->pfds = NULL;
	shdl->npfds = 0;
	shdl->nchildren = 0;
	TAILQ_INIT(&shdl->queue);

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
	struct pollfd *pfds;
	struct client *c;
	int npfds;

	TAILQ_FOREACH(c, &shdl->queue, entries) {
		if (strcmp(device, c->name) == 0) {
			fprintf(stderr, "ish: device already in queue\n");
			return NULL;
		}
	}

	c = calloc(1, sizeof(struct client));

	c->hdl = sio_open(device, SIO_REC, 1);
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
	c->started = 0;
	c->name = malloc(strlen(device) +1);
	strlcpy(c->name, device, strlen(device) + 1);
	memset(&c->ring, 0, sizeof(c->ring));
	sio_setpar(c->hdl, &c->par);
	sio_getpar(c->hdl, &c->par); /* What did sndio give? (TODO: actually pass values to device) */

	npfds = sio_nfds(c->hdl);

	if (npfds < 1) {
		sio_close(c->hdl);
		free(c);
		return NULL;
	}

	shdl->npfds += npfds;

	if (shdl->pfds == NULL)
		pfds = malloc(sizeof(*pfds) * shdl->npfds);
	else
		pfds = realloc(shdl->pfds, sizeof(*pfds) * shdl->npfds);

	shdl->pfds = pfds;

	if (TAILQ_EMPTY(&shdl->queue))
		TAILQ_INSERT_HEAD(&shdl->queue, c, entries);
	else
		TAILQ_INSERT_TAIL(&shdl->queue, c, entries);

	shdl->nchildren++;
	sio_onmove(c->hdl, &on_move, c);
	sio_start(c->hdl);

	return &c->device;
}

static void
combine(int16_t *stream1, int count1, int16_t *stream2, int count2)
{
	int tsize = (count1 > count2) ? count1 : count2;
	long long tmp[tsize]; /* No overflow */

	for (int i = 0; i < tsize; i++) {
		tmp[i] = 0;
		tmp[i] = (count1 > i) ? tmp[i] + stream1[i] : tmp[i];
		tmp[i] = (count2 > i) ? tmp[i] + stream2[i] : tmp[i];

		if (count1 > i && count2 > i)
			tmp[i] /= 2;
	}

	for (int i = 0; i < tsize; i++)
		stream1[i] = (int16_t)tmp[i];
}

/*
 * We're using poll to wake up when samples are available but the samples
 * given are according to a device clock. In other words devices can
 * output samples but there can be delay between them.
 *
 * The fix is to use `sio_onmove` to be able to get the delay and then
 * combine according to that. Increasing the buffer size helps too.
 *
 * The other thing to point out is that we want each device to have
 * a certain amount of frames before being mixed and filling buf.
 */
static int
audio_sndio_capture(struct ishio_audio_buf *buf)
{
	uint8_t tmp[ISHIO_AUDIO_SAMPLES * 4] = { 0 };
	struct timespec snaps[MAX_DEVS] = { 0 };
	struct timespec master;
	struct client *c;
	int offset = 0;
	int revents;
	size_t count = 0;
	int indx = 0;
	int raised[MAX_DEVS] = { 0 };
	int nraised = 0;

	TAILQ_FOREACH(c, &shdl->queue, entries)
		offset += sio_pollfd(c->hdl, &shdl->pfds[offset], POLLIN);

	while (nraised < shdl->nchildren) {
		offset = 0;
		poll(shdl->pfds, shdl->npfds, -1);

		TAILQ_FOREACH(c, &shdl->queue, entries)
			snaps[indx++] = c->time;

		indx = 0;
		master = snaps[0];

		TAILQ_FOREACH(c, &shdl->queue, entries) {
			struct timespec dt;

			timespecsub(&snaps[indx], &master, &dt);
			c->diff = (int64_t)dt.tv_sec * c->par.rate;
			c->diff += ((int64_t)dt.tv_nsec * c->par.rate) / 1000000000LL;
			revents = sio_revents(c->hdl, &shdl->pfds[offset]);

			if (revents & POLLIN) {
				count = sio_read(c->hdl, tmp, ISHIO_AUDIO_SAMPLES * 4) / 4;
				ring_produce(&c->ring, (int16_t *)tmp, count * 2);

				if (raised[indx] == 0 && c->ring.count >= 480 * 2) {
					raised[indx] = 1;
					nraised++;
				}
			} else if (revents & POLLHUP) {
				fprintf(stderr, "ish: device died\n");
				exit(1);
			}

			offset += sio_nfds(c->hdl);
			indx++;
		}
	}

	indx = 0;
	TAILQ_FOREACH(c, &shdl->queue, entries) {
		count = 480;
		int16_t frames[ISHIO_AUDIO_SAMPLES * 4];
		uint64_t to_read = count;
		memset(frames, 0, sizeof(frames));

		ring_move(&c->ring, c->diff * 2);
		uint64_t actual_read = ring_consume(&c->ring, frames, to_read * 2) / 2;

		combine((int16_t *)buf->data, buf->count * 2, frames, actual_read * 2);
		buf->count = (buf->count > actual_read) ? buf->count : actual_read;
	}

	return 0;
}
