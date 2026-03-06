/* trixiectl.c — command-line IPC client for trixie compositor */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static const char *socket_path(void) {
	static char buf[256];
	if (buf[0]) return buf;
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	if (xdg) snprintf(buf, sizeof(buf), "%s/trixie.sock", xdg);
	else      snprintf(buf, sizeof(buf), "/tmp/trixie-%d.sock", (int)getuid());
	return buf;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr, "Usage: trixiectl <command> [args...]\n");
		fprintf(stderr, "Commands: workspace, focus, layout, float, scratchpad,\n");
		fprintf(stderr, "          close, fullscreen, spawn, reload, quit, status, status_json\n");
		return 1;
	}

	/* Build command string from argv */
	char cmd[1024] = {0};
	for (int i = 1; i < argc; i++) {
		if (i > 1) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
		strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
	}
	strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) { perror("socket"); return 1; }

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, socket_path(), sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "trixiectl: could not connect to %s\n"
		                "(is trixie running?)\n", socket_path());
		close(fd);
		return 1;
	}

	write(fd, cmd, strlen(cmd));

	char reply[4096] = {0};
	ssize_t n = read(fd, reply, sizeof(reply) - 1);
	close(fd);

	if (n > 0) {
		fputs(reply, stdout);
		if (reply[n-1] != '\n') fputc('\n', stdout);
	}

	/* exit 0 on "ok:", 1 on "err:" */
	return strncmp(reply, "ok", 2) == 0 ? 0 : 1;
}
