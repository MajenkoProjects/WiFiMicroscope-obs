#ifndef __FFJPEG_JFIF_H__
#define __FFJPEG_JFIF_H__

// ����ͷ�ļ�
#include "bmp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* �������� */
void* jfif_load(uint8_t *data, size_t len);
int   jfif_save(void *ctxt, char *file);
void  jfif_free(void *ctxt);
void  jfif_dump(void *jfif);

int   jfif_decode(void *ctxt, BMP *pb);
void* jfif_encode(BMP *pb);

#ifdef __cplusplus
}
#endif

#endif

