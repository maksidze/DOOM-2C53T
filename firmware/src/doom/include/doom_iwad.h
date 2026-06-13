#ifndef DOOM_IWAD_H
#define DOOM_IWAD_H

#include <stdbool.h>

extern unsigned char *doom_iwad;
extern unsigned int *p_doom_iwad_len;
extern unsigned int doom_iwad_flash_offset;
extern unsigned int doom_iwad_size;

void doom_iwad_configure(unsigned int flash_offset, unsigned int size);
void doom_iwad_configure_fat12(unsigned int volume_base,
                               unsigned int fat_offset,
                               unsigned int data_offset,
                               unsigned int cluster_size,
                               unsigned int first_cluster,
                               unsigned int size);
bool doom_iwad_read(unsigned int offset, void *dest, unsigned int length);
bool doom_iwad_validate(void);

#endif // DOOM_IWAD_H
