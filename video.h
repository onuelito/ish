#ifndef SCREEN_H
#define SCREEN_H

struct video_hdl {
	struct video_ops *ops;
};

struct video_ops {
	void (*close) (void);
	int (*ls) (void);
	struct ishio_dev* (*add) (int);
	int (*capture) (struct ishio_dev *, struct ishio_video_buf *);
};

struct video_hdl *video_open(void);

#endif
