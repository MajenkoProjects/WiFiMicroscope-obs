#ifndef _MD_H
#define _MD_H

#include <stdio.h>
#include <stdint.h>

typedef struct {
	uint8_t *data;
	size_t len;
	size_t pos;
} md;

md *md_fopen(uint8_t *data, uint32_t len);
void md_fclose(md *fd);
size_t md_fseek(md *fd, long offset, int whence);
int md_fgetc(md *fd);
size_t md_fread(void *ptr, size_t size, size_t nmemb, md *stream);
long md_ftell(md *fd);

#endif
