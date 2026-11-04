/*
 * mfc_http DIR [PORT]: a small file server for the phone's own browser (a test of the video decoder, with nothing but the phone): GET with Range, one
 * thread a connection; /log?TEXT prints TEXT on stdout, for a page to say what it sees on the console.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *root;

static const char *
type_of(const char *path)
{
	static const struct { const char *ext, *type; } t[] = {
		{ ".html", "text/html" }, { ".mp4", "video/mp4" }, { ".m3u8", "application/vnd.apple.mpegurl" }, { ".ts", "video/mp2t" },
		{ ".heic", "image/heic" }, { ".png", "image/png" }, { ".js", "text/javascript" },
	};
	const char *dot = strrchr(path, '.');

	for (size_t i = 0; dot != NULL && i < sizeof(t) / sizeof(t[0]); i++) {
		if (strcmp(dot, t[i].ext) == 0) {
			return t[i].type;
		}
	}
	return "application/octet-stream";
}

static void
unescape(char *s)
{
	char *to = s;

	for (; *s != 0; s++) {
		if (*s == '%' && s[1] != 0 && s[2] != 0) {
			char hex[3] = { s[1], s[2], 0 };

			*to++ = (char)strtol(hex, NULL, 16);
			s += 2;
		} else {
			*to++ = *s;
		}
	}
	*to = 0;
}

static void *
serve(void *arg)
{
	int c = (int)(intptr_t)arg;
	static char buffer_unused;
	char *request = malloc(8192), *block = malloc(1 << 16);
	size_t have = 0;

	(void)buffer_unused;
	for (;;) {
		char method[8], target[2048], path[2300], head[512];
		long long from = -1, to = -1;
		struct stat st;
		char *end;

		while ((end = strstr(request, "\r\n\r\n")) == NULL || have == 0) {
			ssize_t n = recv(c, request + have, 8191 - have, 0);

			if (n <= 0) {
				goto out;
			}
			have += (size_t)n;
			request[have] = 0;
		}
		if (sscanf(request, "%7s %2047s", method, target) != 2) {
			goto out;
		}
		char *range = strcasestr(request, "\nRange: bytes=");
		if (range != NULL) {
			sscanf(range + 14, "%lld-%lld", &from, &to);
		}
		size_t used = (size_t)(end + 4 - request);
		memmove(request, end + 4, have - used + 1);
		have -= used;
		if (strncmp(target, "/log?", 5) == 0) {
			unescape(target + 5);
			printf("mfc_http: %s\n", target + 5);
			fflush(stdout);
			static const char ok[] = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
			send(c, ok, sizeof(ok) - 1, 0);
			continue;
		}
		char *question = strchr(target, '?');
		if (question != NULL) {
			*question = 0;
		}
		snprintf(path, sizeof(path), "%s%s", root, target);
		int fd = strstr(target, "..") == NULL ? open(path, O_RDONLY) : -1;
		if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
			static const char no[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
			send(c, no, sizeof(no) - 1, 0);
			if (fd >= 0) {
				close(fd);
			}
			continue;
		}
		long long size = st.st_size, first = 0, last = size - 1;
		int partial = 0;

		if (from >= 0 && from < size) {
			first = from;
			last = (to >= from && to < size) ? to : size - 1;
			partial = 1;
		}
		int n = snprintf(head, sizeof(head), "HTTP/1.1 %s\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\nContent-Length: %lld\r\n", partial ? "206 Partial Content" : "200 OK", type_of(path), last - first + 1);
		if (partial) {
			n += snprintf(head + n, sizeof(head) - (size_t)n, "Content-Range: bytes %lld-%lld/%lld\r\n", first, last, size);
		}
		n += snprintf(head + n, sizeof(head) - (size_t)n, "Cache-Control: no-store\r\n\r\n");
		send(c, head, (size_t)n, 0);
		if (strcmp(method, "HEAD") != 0) {
			long long left = last - first + 1;

			lseek(fd, first, SEEK_SET);
			while (left > 0) {
				ssize_t got = read(fd, block, left < (1 << 16) ? (size_t)left : (1 << 16));

				if (got <= 0) {
					break;
				}
				ssize_t at = 0;
				while (at < got) {
					ssize_t sent = send(c, block + at, (size_t)(got - at), 0);

					if (sent <= 0) {
						close(fd);
						goto out;
					}
					at += sent;
				}
				left -= got;
			}
		}
		close(fd);
	}
out:
	close(c);
	free(request);
	free(block);
	return NULL;
}

int
main(int argc, char **argv)
{
	struct sockaddr_in a;
	int s = socket(AF_INET, SOCK_STREAM, 0), on = 1;

	if (argc < 2) {
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	root = argv[1];
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(argc > 2 ? (uint16_t)atoi(argv[2]) : 8080);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 32) != 0) {
		printf("mfc_http: cannot listen: %d\n", errno);
		return 1;
	}
	printf("mfc_http: serving %s\n", root);
	fflush(stdout);
	for (;;) {
		int c = accept(s, NULL, NULL);
		pthread_t t;

		if (c >= 0) {
			pthread_create(&t, NULL, serve, (void *)(intptr_t)c);
			pthread_detach(t);
		}
	}
}
