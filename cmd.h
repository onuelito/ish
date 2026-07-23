#include <string.h>

struct cmd_map {
	const char *name;
	int id;
};

#define CMD_NONE		0	
#define CMD_QUIT		1
#define CMD_MODE		2
#define CMD_LS			3
#define CMD_SET			4
#define CMD_CAPTURE		5
#define CMD_WIDTH		6
#define CMD_HEIGHT		7

#define MAP_QUIT		(struct cmd_map) { "quit", CMD_QUIT }
#define MAP_MODE		(struct cmd_map) { "mode", CMD_MODE }
#define MAP_LS			(struct cmd_map) { "ls", CMD_LS }
#define MAP_SET			(struct cmd_map) { "set", CMD_SET }
#define MAP_CAPTURE		(struct cmd_map) { "capture", CMD_CAPTURE }
#define MAP_WIDTH		(struct cmd_map) { "width", CMD_WIDTH }
#define MAP_HEIGHT		(struct cmd_map) { "height", CMD_HEIGHT }


#define WHICH_COMMAND(cmd) \
	strcmp(cmd, MAP_QUIT.name) == 0 ? MAP_QUIT.id : \
	strcmp(cmd, MAP_MODE.name) == 0 ? MAP_MODE.id : \
	strcmp(cmd, MAP_MODE.name) == 0 ? MAP_MODE.id : \
	strcmp(cmd, MAP_LS.name) == 0 ? MAP_LS.id : \
	strcmp(cmd, MAP_SET.name) == 0 ? MAP_SET.id : \
	strcmp(cmd, MAP_CAPTURE.name) == 0 ? MAP_CAPTURE.id : \
	strcmp(cmd, MAP_WIDTH.name) == 0 ? MAP_WIDTH.id : \
	strcmp(cmd, MAP_HEIGHT.name) == 0 ? MAP_HEIGHT.id : \
	CMD_NONE
