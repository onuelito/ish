BIN=		ish
CC=			cc
SRCS=		ish.c \
			ishio.c \
			token.c \
			ffmpeg.c \
			sndio.c \
			xcb.c
OBJS=		${SRCS:.c=.o}
CFLAGS=		-O0 -Werror -std=c99
INCS=		-I /usr/X11R6/include -I /usr/local/include
LIBS=		-L /usr/X11R6/lib -lxcb -lxcb-shm -lsndio \
			-L /usr/local/lib -lavformat -lavutil -lavcodec -lpthread -lswscale -lswresample

all: ${BIN}

${BIN}: ${OBJS}
	${CC} ${OBJS} -o ${BIN} ${LIBS}

.c.o:
	${CC} -fPIE -c $< -o $@ ${INCS} ${CFLAGS}

clean:
	rm -rf ${BIN} ${BIN}.core ${OBJS}
