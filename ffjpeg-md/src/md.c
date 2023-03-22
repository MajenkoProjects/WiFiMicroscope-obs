#include <stdlib.h>
#include <string.h>

#include "md.h"


md *md_fopen(uint8_t *data, uint32_t len) {
	md *fd = malloc(sizeof(md));
	fd->data = data;
	fd->len = len;
	fd->pos = 0;
	return fd;
}

void md_fclose(md *fd) {
	free(fd);
}

size_t md_fseek(md *fd, long offset, int whence) {
	long p = fd->pos;
	switch (whence) {
		case SEEK_SET:
			p = offset;
			break;
		case SEEK_CUR:
			p += offset;
			break;
		case SEEK_END:
			p = (fd->len - 1) - offset;
			break;
	}
	if (p < 0) p = 0;
	fd->pos = p;
	return 0;
}

int md_fgetc(md *fd) {
	if (fd->pos >= fd->len) return EOF;
	return fd->data[fd->pos++];
}

size_t md_fread(void *ptr, size_t size, size_t nmemb, md *stream) {
	size_t req = size * nmemb;
	size_t avail = stream->len - stream->pos;
	if (req > avail) req = avail;
	memcpy(ptr, stream->data + stream->pos, req);
	stream->pos += req;
	return req;
}

long md_ftell(md *fd) {
	return fd->pos;
}
