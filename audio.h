#ifndef AUDIO_H
#define AUDIO_H

struct audio_hdl {
	struct audio_ops *ops;
};

struct audio_ops {
	void (*close) (void);
	struct ishio_dev *(*add) (const char*);
	int (*capture) (struct ishio_audio_buf *);
};

struct audio_hdl *audio_open(void);

#endif
