#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include "cmd.h"
#include "cons.h"
#include "token.h"
#include "ishio.h"

#define ISH_MODE_VIDEO 	0
#define ISH_MODE_AUDIO	1

extern char *__progname;

struct app {
	struct cons *cons;
	struct ishio_hdl *hdl;

	struct ishio_dev *video;
	struct ishio_dev *audio;

	const char *output;

	uint16_t output_width;
	uint16_t output_height;

	int running;
	int capture;
	int mode;

	struct timespec frame_interval;
	struct timespec start_time;
	struct timespec snext_time;

	pthread_mutex_t hdl_lock;
	pthread_mutex_t run_lock;
	pthread_mutex_t mode_lock;
	pthread_mutex_t video_lock;
	pthread_mutex_t audio_lock;
	pthread_mutex_t capture_lock;
	pthread_cond_t capture_cond;
};

static struct app app;

void usage(void);
void *cmd(void *);
void *consumer(void *);
void *produce_video(void *);
void *produce_audio(void *);
void on_interrupt(int);

int
main(int argc, char **argv)
{
	if (argc < 2) {
		usage();
		return 0;
	}

	argc -= optind;
	argv += optind;

	/* THREADS
	 *
	 * vthread: video thread
	 * athread: audio thread
	 * ithread: input thread (cmd)
	 * cthread: consumer thread
	 */
	pthread_t vthread, athread, ithread;

	signal(SIGINT, on_interrupt);
	signal(SIGTERM, on_interrupt);
	signal(SIGSEGV, on_interrupt);

	app.hdl = ishio_open();
	app.cons = NULL;
	
	if (app.hdl == NULL) {
		fprintf(stderr, "%s: failed to open handle\n", __progname);
		exit(1);
	}

	pthread_cond_init(&app.capture_cond, NULL);
	pthread_mutex_init(&app.capture_lock, NULL);

	pthread_mutex_init(&app.video_lock, NULL);
	pthread_mutex_init(&app.audio_lock, NULL);
	pthread_mutex_init(&app.run_lock, NULL);
	pthread_mutex_init(&app.mode_lock, NULL);
	pthread_mutex_init(&app.hdl_lock, NULL);

	app.output = *argv;

	app.frame_interval = (struct timespec) { 0, 1000000000L / 30 };
	app.video = NULL;
	app.audio = NULL;
	app.running = 1;
	app.capture = 0;
	app.mode = 0;
	app.output_width = 0;
	app.output_height = 0;

	if (pthread_create(&vthread, NULL, produce_video, &app) != 0) {
		fprintf(stderr, "%s: failed to create video thread\n", __progname);
		ishio_close(app.hdl);
		return -1;
	}

	if (pthread_create(&athread, NULL, produce_audio, &app) != 0) {
		fprintf(stderr, "%s: failed to create audio thread\n", __progname);
		ishio_close(app.hdl);
		return -1;
	}

	if (pthread_create(&ithread, NULL, cmd, &app) != 0) {
		fprintf(stderr, "%s: failed to create input thread\n", __progname);
		ishio_close(app.hdl);
		return -1;
	}

	/*
	 * Starting the threads
	 */
	if (pthread_join(vthread, NULL) != 0) {
		fprintf(stderr, "%s: failed to join video thread\n", __progname);
		ishio_close(app.hdl);
		return -1;
	}

	if (pthread_join(athread, NULL) != 0) {
		fprintf(stderr, "%s: failed to join audio thread\n", __progname);
		ishio_close(app.hdl);
		return -1;
	}

	if (pthread_join(ithread, NULL) != 0) {
		fprintf(stderr, "%s: failed to join input thread\n", __progname);
		ishio_close(app.hdl);
		return -1;
	}

	ishio_close(app.hdl);
	if (app.cons != NULL)
		cons_free(app.cons); 

	fprintf(stdout, "%s: bye\n", __progname);

	return 0;
}

/*
 * -m mode (stands for recording(0) or streaming(1)
 * -o output file
 */
void
usage()
{
	fprintf(stderr, "usage: ish file\n");
}

void
on_interrupt(int s)
{
	printf("%s: unexpected exit (code %d)\n", __progname, s);
	ishio_close(app.hdl);
	cons_free(app.cons);
	exit(1);
}

void*
produce_video(void *args)
{
	struct app *app = (struct app *)args;
	struct ishio_video_buf *vbuf;

	pthread_mutex_lock(&app->capture_lock);
	while (app->capture == 0) {
		pthread_cond_wait(&app->capture_cond, &app->capture_lock);
	}
	pthread_mutex_unlock(&app->capture_lock);

	vbuf = ishio_video_buf_new(app->output_width, app->output_height, ISHIO_FMT_ARGB);

	while (app->running == 1) {
		struct timespec current;
		clock_gettime(CLOCK_MONOTONIC, &current);

		/*
		 * Making sure the framerate is stable
		 */
		if (timespeccmp(&current, &app->snext_time, <)) {
			struct timespec rem;
			timespecsub(&app->snext_time, &current, &rem);
			nanosleep(&rem, NULL);
		} else {
			app->snext_time = current;
		}

		pthread_mutex_lock(&app->video_lock);

		if (app->video != NULL) {
			ishio_fill_video_buf(app->hdl, app->video, vbuf);
		}

		timespecsub(&app->snext_time, &app->start_time, &vbuf->ts);
		timespecadd(&app->snext_time, &app->frame_interval, &app->snext_time);

		pthread_mutex_unlock(&app->video_lock);

		cons_write_video(app->cons, vbuf);
	}
	pthread_exit(NULL);
	return NULL;
}

void*
produce_audio(void *args)
{
	struct app *app = (struct app *)args;
	struct ishio_audio_buf *abuf;

	pthread_mutex_lock(&app->capture_lock);
	while (app->capture == 0) {
		pthread_cond_wait(&app->capture_cond, &app->capture_lock);
	}
	pthread_mutex_unlock(&app->capture_lock);

	abuf = ishio_audio_buf_new();

	while (app->running == 1) {
		memset(abuf->data, 0, abuf->count * 4);
		abuf->count = 0;

		if (app->audio != NULL)
			ishio_fill_audio_buf(app->hdl, abuf);

		cons_write_audio(app->cons, abuf);
	}
	return NULL;
}

/*
 * Handles command input
 */
void*
cmd(void *args)
{
	struct app *app = (struct app *)args;
	struct tk_queue cmd_queue;
	char buf[1024];

	cmd_queue = tk_queue_init();

	while (1) {
		pthread_mutex_lock(&app->run_lock);
		int running = app->running;
		pthread_mutex_unlock(&app->run_lock);

		if (running == 0)
			break;

		if (fgets(buf, sizeof(buf), stdin) != NULL) {
			tokenize(&cmd_queue, 0, buf, sizeof(buf));

			if (TAILQ_EMPTY(&cmd_queue))
				continue;

			struct tk_item *icmd = TAILQ_FIRST(&cmd_queue);
			switch (WHICH_COMMAND(icmd->literal)) {
				case CMD_MODE: {
				struct tk_item *mode = TAILQ_NEXT(icmd, entries);
				long long modeval;
				const char *errstr;

				if (mode == NULL || mode->type != TK_NUMBER || 
					((modeval = strtonum(mode->literal, 0, 1, &errstr)) == 0 && errstr != NULL)) {
					fprintf(stderr, "%s: mode must be video(0) or audio(1)\n", __progname);
					break;
				}

				pthread_mutex_lock(&app->mode_lock);
				app->mode = modeval;
				pthread_mutex_unlock(&app->mode_lock);

				printf("%s: mode changed to %lld\n", __progname, modeval);

				break;
			}
			case CMD_QUIT:
				app->running = 0;
				/*
				 * This is a hack
				 */
				if (app->capture == 0) {
					app->capture = 1;
					pthread_cond_broadcast(&app->capture_cond);
				}
				printf("%s: quitting\n", __progname);
				break;
			case CMD_LS: {
				pthread_mutex_lock(&app->mode_lock);
				int mode = app->mode;
				pthread_mutex_unlock(&app->mode_lock);

				if (mode == ISH_MODE_VIDEO) {
					ishio_ls_video(app->hdl);
				} else {
					/*
					 * You cannont list audio devices or control devices with sndio because
					 * you need to be a member of sndiop. You can't do that as regular user.
					 *
					 * The devices are there in /dev/audioN and /dev/audioctlN but its not
					 * possible.
					 */
					fprintf(stderr, "%s: `%s` command is not available in mode %d\n",
						__progname, icmd->literal, mode);
				}
				break;
			}
			case CMD_SET: {
				pthread_mutex_lock(&app->mode_lock);
				int mode = app->mode;
				pthread_mutex_unlock(&app->mode_lock);

				if (mode == ISH_MODE_VIDEO) {
					struct tk_item *setitm = TAILQ_NEXT(icmd, entries);
					const char *errstr;
					long long setval = 0;

					if (setitm == NULL || setitm->type != TK_NUMBER ||
						((setval = strtonum(setitm->literal, 0, 32456, &errstr)) == 0 && 
							errstr != NULL)) {
						fprintf(stderr, "%s: value must be a number from 0 to %u\n",
							__progname, UINT_MAX);
						break;
					}

					pthread_mutex_lock(&app->hdl_lock);
					pthread_mutex_lock(&app->video_lock);

					app->video = ishio_add_video(app->hdl, (int)setval);

					if (app->video == NULL) {
						fprintf(stderr, "%s: video %lld does not exist\n", __progname, setval);
					} else {
						printf("%s: select video number %lld\n", __progname, setval);
					}

					pthread_mutex_unlock(&app->video_lock);
					pthread_mutex_unlock(&app->hdl_lock);

				} else {
					if (app->capture == 1) {
						fprintf(stderr, "%s: cannot set audio devices while capturing\n", __progname);
						break;
					}
					struct tk_item *setitm = TAILQ_NEXT(icmd, entries);
					const char *errstr;
					
					if (setitm == NULL) {
						fprintf(stderr, "%s: value must be an audio device\n", __progname);
						break;
					}

					pthread_mutex_lock(&app->hdl_lock);
					pthread_mutex_lock(&app->audio_lock);

					app->audio = ishio_add_audio(app->hdl, setitm->literal);

					if (app->audio == NULL) {
						fprintf(stderr, "%s: failed to open audio device `%s`\n",
							__progname, setitm->literal);
					} else {
						printf("%s: audio device set to `%s`\n", __progname, setitm->literal);
					}

					pthread_mutex_unlock(&app->audio_lock);
					pthread_mutex_unlock(&app->hdl_lock);
				}

				break;
			}
			case CMD_CAPTURE:
				if (app->output_width == 0 || app->output_height == 0) {
					fprintf(stderr, "%s: please set width(%d) or height(%d)\n",
						__progname, app->output_width, app->output_height);
					break;
				}

				if (app->capture == 1) {
					fprintf(stderr, "%s: already capturing\n", __progname);
					return NULL;
				}
				clock_gettime(CLOCK_MONOTONIC, &app->start_time);
				clock_gettime(CLOCK_MONOTONIC, &app->snext_time);
				app->cons = cons_new(app->output_width, app->output_height, CONSUMMER_OUTPUT_FILE, app->output);

				if (app->cons == NULL) {
					exit(1);
				}
				app->capture = 1;
				pthread_cond_broadcast(&app->capture_cond);
				break;
			case CMD_WIDTH: {
				if (app->capture == 1) {
					fprintf(stderr, "%s: cannot set width while capturing\n", __progname);
					break;
				}
				struct tk_item *witem = TAILQ_NEXT(icmd, entries);
				const char *errstr;
				long long wval = 0;

				if (witem == NULL || witem->type != TK_NUMBER ||
					((wval = strtonum(witem->literal, 0, 1920, &errstr)) == 0 && 
						errstr != NULL)) {
					fprintf(stderr, "%s: value must be a number from 0 to %u\n",
						__progname, 1920);
					break;
				}

				printf("width: %d -> %lld\n", app->output_width, wval);
				app->output_width = wval;

				break;
			}
			case CMD_HEIGHT: {
				if (app->capture == 1) {
					fprintf(stderr, "%s: cannot set height while capturing\n", __progname);
					break;
				}
				struct tk_item *hitem = TAILQ_NEXT(icmd, entries);
				const char *errstr;
				long long hval = 0;

				if (hitem == NULL || hitem->type != TK_NUMBER ||
					((hval = strtonum(hitem->literal, 0, 1920, &errstr)) == 0 && 
						errstr != NULL)) {
					fprintf(stderr, "%s: value must be a number from 0 to %u\n",
						__progname, 1080);
					break;
				}

				printf("height: %d -> %lld\n", app->output_height, hval);
				app->output_height = hval;

				break;
			}
			case CMD_NONE:
				fprintf(stderr, "%s: uknown command `%s`\n", __progname,  icmd->literal);
				break;
			}

			tk_queue_free(&cmd_queue);
		}
	}
	return NULL;
}
