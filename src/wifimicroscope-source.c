#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <obs.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "jfif.h"
#include "bmp.h"

static int singleton = 0;

struct wifimicroscope_source {
	obs_source_t *source;
	os_event_t *stop_signal;
	os_event_t *image_ready;
	pthread_t thread;
	pthread_t network;
	bool initialized;
	bool online;

	int udpSocket;
	struct sockaddr_in serverAddr;
	struct sockaddr_in cameraAddr;

	uint8_t bufferA[1310920]; // Max possible size plus some
	uint8_t bufferB[1310920]; // Max possible size plus some
	uint8_t *buffer;
	uint32_t bufferSize;
};

static const char *wm_get_name(void *unused) {
	UNUSED_PARAMETER(unused);
	return "WiFi Microscope";
}

static void wm_destroy(void *data) {
	struct wifimicroscope_source *wm = data;

	if (wm) {
		if (wm->initialized) {
			os_event_signal(wm->stop_signal);
		}

		if (wm->thread) {
			pthread_join(wm->thread, NULL);
		}
		if (wm->network) {
			pthread_join(wm->network, NULL);
		}

		if (wm->stop_signal) {
			os_event_destroy(wm->stop_signal);
		}

		if (wm->image_ready) {
			os_event_destroy(wm->image_ready);
		}

		if (wm) {
			bfree(wm);
		}
	}

	singleton = 0;
}

static void *video_thread(void *data) {
	struct wifimicroscope_source *wm = data;
	uint32_t pixels[1280 * 720];
	uint64_t cur_time = os_gettime_ns();

	struct obs_source_frame frame = {
		.data = {[0] = (uint8_t *)pixels},
		.linesize = {[0] = 1280 * 4},
		.width = 1280,
		.height = 720,
		.format = VIDEO_FORMAT_BGRA,
	};

	while (os_event_try(wm->stop_signal) == EAGAIN) {
		if (os_event_try(wm->image_ready) != EAGAIN) {
			if (wm->online) {
				uint8_t *buf;
				if (wm->buffer == wm->bufferA) {
					buf = wm->bufferB;
				} else {
					buf = wm->bufferA;
				}	

				if (buf[0] == 0xFF && buf[1] == 0xD8) {

					void *jfif = jfif_load(buf, wm->bufferSize);
					if (jfif) {
						//jfif_dump(jfif);
						BMP bmp = {0};
						if (jfif_decode(jfif, &bmp) < 0) {
							jfif_free(jfif);
						} else {
							jfif_free(jfif);

							int ip = 0;
							int op = 0;
		
							for (int y = 0; y < 720; y++) {
								for (int x = 0; x < 1280; x++) {
									pixels[op] = 0xFF000000;
									pixels[op] |= ((uint8_t *)bmp.pdata)[ip++] << 0;
									pixels[op] |= ((uint8_t *)bmp.pdata)[ip++] << 8;
									pixels[op] |= ((uint8_t *)bmp.pdata)[ip++] << 16;


//									pixels[op] |= ((uint8_t *)bmp.pdata)[ip++] << 8;
//									pixels[op] |= ((uint8_t *)bmp.pdata)[ip++] << 16;
									op++;
								}
							}
						}

					}

					frame.timestamp = cur_time;
					obs_source_output_video(wm->source, &frame);
				}
			} else {
				bzero(pixels, 1280 * 720 * 4);
				frame.timestamp = cur_time;
				obs_source_output_video(wm->source, &frame);
			}
		}
		os_sleepto_ns(cur_time += 30000000);
	}
	return NULL;
}

static void *network_thread(void *data) {
	struct wifimicroscope_source *wm = data;
	uint8_t buffer[1450];

	int framepos = 0;
	int frameno = 0;
	int packetno = 0;

	wm->udpSocket = socket(PF_INET, SOCK_DGRAM, 0);

	int one = 1;
	setsockopt(wm->udpSocket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int));

	if (!wm->udpSocket) {
		wm_destroy(wm);
		return NULL;
	}

	wm->serverAddr.sin_family = AF_INET;
	wm->serverAddr.sin_port = htons(10900);
	wm->serverAddr.sin_addr.s_addr = inet_addr("0.0.0.0");
	memset(wm->serverAddr.sin_zero, '\0', sizeof(wm->serverAddr.sin_zero));

	wm->cameraAddr.sin_family = AF_INET;
	wm->cameraAddr.sin_port = htons(20000);
	wm->cameraAddr.sin_addr.s_addr = inet_addr("192.168.29.1");
	memset(wm->cameraAddr.sin_zero, '\0', sizeof(wm->cameraAddr.sin_zero));

	if (bind(wm->udpSocket, (struct sockaddr *)&wm->serverAddr, sizeof(wm->serverAddr)) != 0) {
		blog(LOG_ERROR, "Error binding socket: %s", strerror(errno));
		wm_destroy(wm);
		return NULL;
	}

	sendto(wm->udpSocket, "JHCMD\x10\x00", 7, 0, (struct sockaddr *)&wm->cameraAddr, sizeof(wm->cameraAddr));
	sendto(wm->udpSocket, "JHCMD\x20\x00", 7, 0, (struct sockaddr *)&wm->cameraAddr, sizeof(wm->cameraAddr));
	sendto(wm->udpSocket, "JHCMD\xd0\x01", 7, 0, (struct sockaddr *)&wm->cameraAddr, sizeof(wm->cameraAddr));
	sendto(wm->udpSocket, "JHCMD\xd0\x01", 7, 0, (struct sockaddr *)&wm->cameraAddr, sizeof(wm->cameraAddr));

	int olcount = 0;

	while (os_event_try(wm->stop_signal) == EAGAIN) {

		fd_set rfd;
		fd_set efd;
		fd_set wfd;
	
		struct timeval to;
		
		FD_ZERO(&rfd);
		FD_ZERO(&efd);
		FD_ZERO(&wfd);

		FD_SET(wm->udpSocket, &rfd);

		to.tv_sec = 1;
		to.tv_usec = 0;

		int t = select(wm->udpSocket + 1, &rfd, &wfd, &efd, &to);

		if (t > 0) {
			int nBytes = recvfrom(wm->udpSocket, buffer, sizeof(buffer), 0, NULL, NULL);

			if (nBytes > 8) {
				wm->online = 1;
				olcount = 0;
				frameno = buffer[0] | (buffer[1] << 8);
				packetno = buffer[3];

				if (packetno == 0) {
					if (framepos > 0) {
						if (wm->buffer == wm->bufferA) {
							wm->buffer = wm->bufferB;
						} else {
							wm->buffer = wm->bufferA;
						}
						wm->bufferSize = framepos;
						os_event_signal(wm->image_ready);
						// Image is ready
						framepos = 0;
					}
					if (frameno % 50 == 0) {
						sendto(wm->udpSocket, "JHCMD\xd0\x01", 7, 0, (struct sockaddr *)&wm->cameraAddr, sizeof(wm->cameraAddr));
					}
				}
				memcpy(wm->buffer + framepos, buffer + 8, nBytes - 8);
				framepos += (nBytes - 8);
			}
					
		} else {
			wm->online = 0;
			os_event_signal(wm->image_ready);
			olcount++;
			if (olcount > 3) {
				olcount = 0;
			    sendto(wm->udpSocket, "JHCMD\x10\x00", 7, 0, (struct sockaddr *)&wm->cameraAddr, sizeof(wm->cameraAddr));
			    sendto(wm->udpSocket, "JHCMD\x20\x00", 7, 0, (struct sockaddr *)&wm->cameraAddr, sizeof(wm->cameraAddr));
			    sendto(wm->udpSocket, "JHCMD\xd0\x01", 7, 0, (struct sockaddr *)&wm->cameraAddr, sizeof(wm->cameraAddr));
			    sendto(wm->udpSocket, "JHCMD\xd0\x01", 7, 0, (struct sockaddr *)&wm->cameraAddr, sizeof(wm->cameraAddr));
			}
		}
	}

	blog(LOG_INFO, "Closing socket");

	close(wm->udpSocket);
	
	return NULL;
}

static void *wm_create(obs_data_t *settings, obs_source_t *source) {

	if (singleton != 0) {
		return NULL;
	}

	singleton = 1;

	struct wifimicroscope_source *wm = bzalloc(sizeof(struct wifimicroscope_source));

	wm->source = source;

	wm->online = 0;
	wm->buffer = wm->bufferA;

	if (os_event_init(&wm->stop_signal, OS_EVENT_TYPE_MANUAL) != 0) {
		wm_destroy(wm);
		return NULL;
	}

	if (os_event_init(&wm->image_ready, OS_EVENT_TYPE_MANUAL) != 0) {
		wm_destroy(wm);
		return NULL;
	}

	if (pthread_create(&wm->thread, NULL, video_thread, wm) != 0) {
		wm_destroy(wm);
		return NULL;
	}

	if (pthread_create(&wm->network, NULL, network_thread, wm) != 0) {
		wm_destroy(wm);
		return NULL;
	}

	wm->initialized = true;

	UNUSED_PARAMETER(settings);

	return wm;
}

static uint32_t wm_get_width(void *data) {
	UNUSED_PARAMETER(data);
	return 1280;
}

static uint32_t wm_get_height(void *data) {
	UNUSED_PARAMETER(data);
	return 720;
}

struct obs_source_info wifimicroscope_source = {
	.id				= "wifimicroscope_source",
	.type			= OBS_SOURCE_TYPE_INPUT,
	.output_flags	= OBS_SOURCE_ASYNC_VIDEO,
	.get_name		= wm_get_name,
	.create			= wm_create,
	.destroy		= wm_destroy,
	.get_width		= wm_get_width,
	.get_height		= wm_get_height,
};
