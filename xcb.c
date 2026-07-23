#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/queue.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <xcb/xcb.h>
#include <xcb/shm.h>

#include "ishio.h"
#include "video.h"

struct client {
	struct ishio_dev device;
	xcb_window_t id;
	uint16_t width;
	uint16_t height;
	uint8_t bpp;
	uint8_t format;
	int stride;
	char *name;

	xcb_shm_seg_t shmseg;
	int shmid;
	uint8_t *pixels;

	TAILQ_ENTRY(client) entries;
};

struct video_xcb_hdl {
	struct video_hdl hdl;

	xcb_connection_t *conn;
	xcb_screen_t *screen;

	TAILQ_HEAD(client_head, client) clients;
};

static struct video_xcb_hdl *xhdl;

static void video_xcb_close(void);
static int video_xcb_ls(void);
static struct ishio_dev *video_xcb_add(int);
static int video_xcb_capture(struct ishio_dev *, struct ishio_video_buf *);

static struct video_ops video_xcb_ops = {
	video_xcb_close,
	video_xcb_ls,
	video_xcb_add,
	video_xcb_capture,
};

static void get_clients(struct client_head *, xcb_window_t);
static void free_clients(struct client_head *);

struct video_hdl*
video_open()
{
	int err;
	xhdl = calloc(1, sizeof(struct video_xcb_hdl));

	if (xhdl == NULL)
		return NULL;

	xhdl->conn = xcb_connect(NULL, NULL);
	err = xcb_connection_has_error(xhdl->conn);
	if (err) {
		free(xhdl);
		return NULL;
	}

	xhdl->screen = xcb_setup_roots_iterator(xcb_get_setup(xhdl->conn)).data;

	if (xhdl->screen == NULL) {
		free(xhdl);
		return NULL;
	}

	xhdl->hdl.ops = &video_xcb_ops;

	TAILQ_INIT(&xhdl->clients);
	get_clients(&xhdl->clients, xhdl->screen->root);

	return &(xhdl->hdl);
}

void
video_xcb_close()
{
	free_clients(&xhdl->clients);
	xcb_disconnect(xhdl->conn);
	free(xhdl);
}

/*
 * Check for resize, window creation or deletion
 */
void
video_xcb_update()
{

}

/*
 * SCREEN COMMANDS
 *
 * This is where each backend differs
 */

/*
 * Gives a list of existing windows
 */
int
video_xcb_ls()
{
	struct client *node;
	int i = 0;

	TAILQ_FOREACH(node, &xhdl->clients, entries) {
		printf("%d - %s\n", i, node->name);
		i++;
	}

	return 0;
}

/*
 * For now id is position in the queue
 */
static struct ishio_dev*
video_xcb_add(int id)
{
	struct client *c = NULL;	
	int index = 0;

	if (TAILQ_EMPTY(&xhdl->clients))
		return NULL;

	TAILQ_FOREACH(c, &xhdl->clients, entries) {
		if (id == index) {
			return &c->device;
		}
		index++;
	}

	return NULL;
}

static int
video_xcb_capture(struct ishio_dev *dev, struct ishio_video_buf *buf)
{
	struct client *c = (struct client *)dev;
	xcb_shm_get_image_cookie_t cookie = xcb_shm_get_image(
		xhdl->conn, 
		c->id, 
		0, 0, 
		c->width, c->height,
		~0, 
		XCB_IMAGE_FORMAT_Z_PIXMAP, 
		c->shmseg, 
		0
	);

	xcb_generic_error_t *err;
	xcb_shm_get_image_reply_t *reply = xcb_shm_get_image_reply(xhdl->conn, cookie, &err);

	if (err) {
		fprintf(stderr, "xcb error: %d\n", err->error_code);
		free(err);
		return -1;
	}

	if (reply == NULL) {
		return -1;
	}

	int height = (buf->height < c->height) ? buf->height : c->height;
	int width = (buf->width < c->width) ? buf->width : c->width;

	/*
	 * X Server default is BGR
	 */

	for (int y = 0; y < height; y++) {
		uint8_t *dst = buf->data + y * buf->width * 4;
		uint8_t *src = c->pixels + y * c->width * 4;

		for (int x = 0; x < width; x++) {
			dst[x * 4 + 0] = src[x * 4 + 3];
			dst[x * 4 + 1] = src[x * 4 + 2];
			dst[x * 4 + 2] = src[x * 4 + 1];
			dst[x * 4 + 3] = src[x * 4 + 0];
		}
	}

	free(reply);
	return 0;
}

xcb_get_geometry_reply_t*
get_client_geometry(xcb_window_t window)
{
	xcb_get_geometry_cookie_t cookie;
	xcb_get_geometry_reply_t *reply;

	cookie = xcb_get_geometry(xhdl->conn, window);
	reply = xcb_get_geometry_reply(xhdl->conn, cookie, NULL);
	return reply;
}

xcb_visualtype_t*
get_client_visual(xcb_window_t id)
{
	xcb_get_window_attributes_cookie_t cookie;
	xcb_get_window_attributes_reply_t *reply;

	cookie = xcb_get_window_attributes(xhdl->conn, id);
	reply = xcb_get_window_attributes_reply(xhdl->conn, cookie, NULL);

	if (reply == NULL) {
		return NULL;
	}

	xcb_visualtype_t *vtype = NULL;
	xcb_depth_iterator_t diter = xcb_screen_allowed_depths_iterator(xhdl->screen);

	for (int i = 0; i < diter.rem; xcb_depth_next(&diter)) {
		xcb_visualtype_iterator_t viter = xcb_depth_visuals_iterator(diter.data);

		for (int j = 0; j < viter.rem; xcb_visualtype_next(&viter)) {
			if (viter.data->visual_id == reply->visual)
				vtype = viter.data;
		}

		if (vtype) {
			free(reply);
			return vtype;
		}
	}

	free(reply);
	return NULL;
}

int
get_client_format(int bpp, xcb_visualtype_t *vtype)
{
	if (vtype->red_mask == 0xff0000 &&
		vtype->green_mask == 0x00ff00 &&
		vtype->blue_mask == 0x0000ff) {

		if (bpp == 3) {
			return ISHIO_FMT_RGB;
		}

		if (bpp == 4) {
			return ISHIO_FMT_ARGB;
		}
	}
	return ISHIO_FMT_NONE;
}

char *
get_client_name(xcb_window_t id)
{
	xcb_intern_atom_cookie_t net_cookie;
	xcb_intern_atom_reply_t *net_reply;
	char *name = NULL;

	net_cookie = xcb_intern_atom(xhdl->conn, 1, strlen("_NET_WM_NAME"), "_NET_WM_NAME");
	net_reply = xcb_intern_atom_reply(xhdl->conn, net_cookie, NULL);

	if (net_reply) {
		xcb_get_property_cookie_t cookie;
		xcb_get_property_reply_t *reply;

		cookie = xcb_get_property(xhdl->conn, 0, id, net_reply->atom, XCB_ATOM_ANY, 0, 1024);
		reply = xcb_get_property_reply(xhdl->conn, cookie, NULL);

		if (reply) {
			if (reply->type != XCB_NONE) {
				int len = xcb_get_property_value_length(reply);

				name = malloc(len + 1);
				memcpy(name, xcb_get_property_value(reply), len);
				name[len] = '\0';

				free(reply);
			}
		}

		free(net_reply);
	}

	return name;
}

int
get_client_bpp(xcb_window_t id)
{
	xcb_get_geometry_cookie_t cookie;
	xcb_get_geometry_reply_t *reply;

	cookie = xcb_get_geometry(xhdl->conn, id);
	reply = xcb_get_geometry_reply(xhdl->conn, cookie, NULL);

	if (reply) {
		uint8_t bpp = 4; /* This is not true but understand 32-bit alignement */
		free(reply);
		return bpp;
	}
	return -1;
}


void
get_clients(struct client_head *head, xcb_window_t window)
{
	char *name = NULL;
	xcb_intern_atom_cookie_t wms_cookie;
	xcb_intern_atom_reply_t *wms_reply;
	xcb_query_tree_cookie_t tree_cookie;
	xcb_query_tree_reply_t *tree_reply;

	xcb_get_window_attributes_cookie_t gwa_cookie;
	xcb_get_window_attributes_reply_t *gwa_reply;

	xcb_get_property_cookie_t gprop_cookie;
	xcb_get_property_reply_t *gprop_reply;

	wms_cookie = xcb_intern_atom(xhdl->conn, 0, strlen("WM_STATE"), "WM_STATE");
	wms_reply = xcb_intern_atom_reply(xhdl->conn, wms_cookie, NULL);

	if (window != xhdl->screen->root) {
		if (wms_reply == NULL)
			return;

		gprop_cookie = xcb_get_property(xhdl->conn, 
										0, 
										window, 
										wms_reply->atom, 
										XCB_GET_PROPERTY_TYPE_ANY, 
										0, 
										2);

		gprop_reply = xcb_get_property_reply(xhdl->conn, gprop_cookie, NULL);

		free(wms_reply);

		if (gprop_reply == NULL)
			return;

		if (gprop_reply->type == XCB_NONE) {
			free(gprop_reply);
			return;
		}

		free(gprop_reply);
	} else {
		const char *copy = "ROOT";
		size_t len = strlen(copy);

		name = malloc(len + 1);
		strlcpy(name, copy, len+1);
	}

	tree_cookie = xcb_query_tree(xhdl->conn, window);
	tree_reply = xcb_query_tree_reply(xhdl->conn, tree_cookie, NULL);

	if (tree_reply == NULL) {
		fprintf(stderr, "ish: failed to get xcb\n");
		exit(1);
	}

	gwa_cookie = xcb_get_window_attributes(xhdl->conn, window);
	gwa_reply = xcb_get_window_attributes_reply(xhdl->conn, gwa_cookie, NULL);

	if (gwa_reply == NULL) {
		free(tree_reply);
		fprintf(stderr, "ish: get window attributes\n");
		exit(1);
	}

	if (gwa_reply->_class != XCB_WINDOW_CLASS_INPUT_OUTPUT ||
		gwa_reply->map_state != XCB_MAP_STATE_VIEWABLE) {
		return;
	}

	struct client *info = calloc(1, sizeof(struct client));
	info->name = (name == NULL) ? get_client_name(window) : name;
	info->id = window;
	info->bpp = get_client_bpp(info->id);
	info->device.type = ISHIODEV_VIDEO;

	xcb_get_geometry_reply_t *geom_reply = get_client_geometry(info->id);
	if (geom_reply == NULL) {
		free(gwa_reply);
		free(tree_reply);
		free(info);
		free(info);
		return;
	}

	info->width = geom_reply->width;
	info->height = geom_reply->height;
	free(geom_reply);

	info->format = get_client_format(info->bpp, get_client_visual(info->id));

	size_t size = info->width * info->height * info->bpp;
	info->shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0666);

	if (info->shmid == -1) {
		perror("shmget");
		free(info->name);
		free(info);
		return;
	}

	info->pixels = shmat(info->shmid, NULL, 0);
	if (info->pixels == (void *)-1) {
		shmctl(info->shmid, IPC_RMID, NULL);
		free(info->name);
		free(info);
		return;
	}

	info->shmseg = xcb_generate_id(xhdl->conn);
	xcb_void_cookie_t acookie = xcb_shm_attach(xhdl->conn, info->shmseg, info->shmid, 0);
	xcb_generic_error_t *err = xcb_request_check(xhdl->conn, acookie);

	if (err) {
		shmctl(info->shmid, IPC_RMID, NULL);
		free(info->name);
		free(info);
		return;
	}

	if (TAILQ_EMPTY(head))
		TAILQ_INSERT_HEAD(head, info, entries);
	else
		TAILQ_INSERT_TAIL(head, info, entries);

	xcb_window_t *children = xcb_query_tree_children(tree_reply);
	int nchildren = tree_reply->children_len;

	for (int i = 0; i < nchildren; i++) {
		get_clients(head, children[i]);
	}

	free(tree_reply);
}

void
free_client(struct client *c)
{
	xcb_shm_detach(xhdl->conn, c->shmseg);
	shmdt(c->pixels);
	shmctl(c->shmid, IPC_RMID, NULL);
	if (c->name)
		free(c->name);
	free(c);
}

void
free_clients(struct client_head *head)
{
	struct client *node;
	while ((node = TAILQ_FIRST(head))) {
		TAILQ_REMOVE(head, node, entries);
		free_client(node);
	}
}
