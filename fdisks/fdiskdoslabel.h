#ifndef FDISK_DOS_LABEL_H
#define FDISK_DOS_LABEL_H

extern struct pte ptes[MAXIMUM_PARTS];

#define pt_offset(b, n)	((struct partition *)((b) + 0x1be + \
					      (n) * sizeof(struct partition)))

extern int ext_index; /* the prime extended partition */
extern sector_t sector_offset;

/* A valid partition table sector ends in 0x55 0xaa */
static inline unsigned int part_table_flag(unsigned char *b)
{
	return ((unsigned int) b[510]) + (((unsigned int) b[511]) << 8);
}

static inline sector_t get_partition_start(struct pte *pe)
{
	return pe->offset + get_start_sect(pe->part_table);
}

extern void dos_print_mbr_id(struct fdisk_context *cxt);
extern void dos_set_mbr_id(struct fdisk_context *cxt);
extern void dos_init(struct fdisk_context *cxt);

extern int mbr_is_valid_magic(unsigned char *b);

#endif
