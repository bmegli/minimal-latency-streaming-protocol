/*
 * MLSP Minimal Latency Streaming Protocol C library header
 *
 * Copyright 2019-2020 (C) Bartosz Meglicki <meglickib@gmail.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

#ifndef MLSP_H
#define MLSP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

enum MLSP_COMPILE_TIME_CONSTANTS
{
	MLSP_MAX_SUBFRAMES = 3, //!< max number of logical subframes in a single MLSP frame
};

struct mlsp;

struct mlsp_config
{
	const char *ip; //!< IP (send to or listen on) or NULL and "\0" for server (listen on any)
	uint16_t port; //!< port to listen on (server) or send to (client)
	int timeout_ms; //!< 0 or positive number of ms
	int subframes; //!< number of logical subframes carried by single frame, 0 is treated as 1
};

enum mlsp_retval_enum
{
	MLSP_TIMEOUT=-2, //!< timeout on receive
	MLSP_ERROR=-1, //!< error occured
	MLSP_OK=0, //!< succesfull execution
};

//user level logical frame to send
struct mlsp_frame
{
	uint8_t *data;
	uint32_t size;
};

struct mlsp *mlsp_init_client(const struct mlsp_config *config);
struct mlsp *mlsp_init_server(const struct mlsp_config *config);
void mlsp_close(struct mlsp *m);

int mlsp_send(struct mlsp *m, const struct mlsp_frame *frame, uint8_t subframe);

//non NULL on success, NULL on failure or timeout
//the ownership of mlsp_packet remains with library
const struct mlsp_frame *mlsp_receive(struct mlsp *m, int *error);

#ifdef __cplusplus
}
#endif

#endif
