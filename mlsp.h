#ifndef MLSP_H
#define MLSP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct mlsp;

struct mlsp_config
{
	const char *ip;
	uint16_t port;
};

enum mlsp_retval_enum
{
	MLSP_ERROR=-1, //!< error occured 
	MLSP_OK=0, //!< succesfull execution
};

struct mlsp *mlsp_init(const struct mlsp_config *config);
void mlsp_close(struct mlsp *m);

int mlsp_send(struct mlsp *m, uint16_t framenumber, uint8_t *data, int size);


#ifdef __cplusplus
}
#endif

#endif
