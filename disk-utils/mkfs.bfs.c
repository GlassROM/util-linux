/*
 *  mkfs.bfs - Create SCO BFS filesystem - aeb, 1999-09-07
 *
 *  Usage: mkfs.bfs [-N nr-of-inodes] [-V volume-name] [-F fsname] device
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#include "c.h"
#include "nls.h"
#include "blkdev.h"

#define BFS_ROOT_INO		2
#define BFS_NAMELEN		14
#define BFS_BLOCKSIZE		512
#define BFS_SUPER_MAGIC		0x1badface

/* superblock - 512 bytes */
struct bfssb {
        unsigned int s_magic;
        unsigned int s_start;	  /* byte offset of start of data */
        unsigned int s_end;       /* sizeof(slice)-1 */

        /* for recovery during compaction */
        int s_from, s_to;         /* src and dest block of current transfer */
        int s_backup_from, s_backup_to;

        /* labels - may well contain garbage */
        char s_fsname[6];
        char s_volume[6];
        char s_pad[472];
};

/* inode - 64 bytes */
struct bfsi {
        unsigned short i_ino;
        unsigned char i_pad1[2];
        unsigned long i_first_block;
        unsigned long i_last_block;
        unsigned long i_bytes_to_end;
        unsigned long i_type;           /* 1: file, 2: the unique dir */
        unsigned long i_mode;
        unsigned long i_uid, i_gid;
        unsigned long i_nlinks;
        unsigned long i_atime, i_mtime, i_ctime;
        unsigned char i_pad2[16];
};

#define BFS_DIR_TYPE	2

/* directory entry - 16 bytes */
struct bfsde {
        unsigned short d_ino;
        char d_name[BFS_NAMELEN];
};


static char *progname;

static void
usage(void) {
	fprintf(stderr, _(
		"Usage: %s [-v] [-N nr-of-inodes] [-V volume-name]\n"
		"       [-F fsname] device [block-count]\n"),
		progname);
	exit(1);
}

int
main(int argc, char *argv[]) {
	char *device, *volume, *fsname;
	int inodes;
	unsigned long long total_blocks, ino_bytes, ino_blocks, data_blocks;
	unsigned long long user_specified_total_blocks = 0;
	int verbose = 0;
	int fd;
	struct bfssb sb;
	struct bfsi ri;
	struct bfsde de;
	struct stat statbuf;
	time_t now;
	int c, i, len;
	char *p;

	progname = argv[0];
	if ((p = strrchr(progname, '/')) != NULL)
		progname = p+1;

	if (argc < 2)
		usage();

	if (argc == 2 &&
	    (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version"))) {
		printf(_("%s (%s)\n"), progname, PACKAGE_STRING);
		exit(0);
	}

	volume = fsname = "      ";	/* is there a default? */
	inodes = 0;

	while ((c = getopt(argc, argv, "vF:N:V:cl:")) != -1) {
		switch (c) {
		case 'N':
			inodes = atol(optarg);
			break;

		case 'V':
			len = strlen(optarg);
			if (len <= 0 || len > 6)
				errx(EXIT_FAILURE, _("volume name too long"));
			volume = strdup(optarg);
			break;

		case 'F':
			len = strlen(optarg);
			if (len <= 0 || len > 6)
				errx(EXIT_FAILURE, _("fsname name too long"));
			fsname = strdup(optarg);
			break;

		case 'v':
			verbose = 1;
			break;

			/* when called via mkfs we may get options c,l,v */
		case 'c':
		case 'l':
			break;

		default:
			usage();
		}
	}

	if (optind == argc)
		usage();

	device = argv[optind++];

	if (stat(device, &statbuf) == -1)
		err(EXIT_FAILURE, _("cannot stat device %s"), device);

	if (!S_ISBLK(statbuf.st_mode))
		errx(EXIT_FAILURE, _("%s is not a block special device"), device);

	fd = open(device, O_RDWR | O_EXCL);
	if (fd == -1)
		err(EXIT_FAILURE, _("cannot open %s"), device);

	if (optind == argc-1)
		user_specified_total_blocks = atoll(argv[optind]);
	else if (optind != argc)
		usage();

	if (blkdev_get_sectors(fd, &total_blocks) == -1) {
		if (!user_specified_total_blocks)
			err(EXIT_FAILURE, _("cannot get size of %s"), device);
		total_blocks = user_specified_total_blocks;
	} else if (user_specified_total_blocks) {
		if (user_specified_total_blocks > total_blocks)
			errx(EXIT_FAILURE, _("blocks argument too large, max is %llu"),
			      total_blocks);
		total_blocks = user_specified_total_blocks;
	}

	if (!inodes) {
		/* pick some reasonable default */
		inodes = 8*(total_blocks/800);
		if (inodes < 48)
			inodes = 48;
		if (inodes > 512)
			inodes = 512;
	} else {
		/* believe the user */
		if (inodes > 512)
			errx(EXIT_FAILURE, _("too many inodes - max is 512"));
	}

	ino_bytes = inodes * sizeof(struct bfsi);
	ino_blocks = (ino_bytes + BFS_BLOCKSIZE - 1) / BFS_BLOCKSIZE;
	data_blocks = total_blocks - ino_blocks - 1;

	/* mimic the behaviour of SCO's mkfs - maybe this limit is needed */
	if (data_blocks < 32)
		errx(EXIT_FAILURE, _("not enough space, need at least %llu blocks"),
		      ino_blocks + 33);

	memset(&sb, 0, sizeof(sb));
	sb.s_magic = BFS_SUPER_MAGIC;
	sb.s_start = ino_bytes + sizeof(struct bfssb);
	sb.s_end = total_blocks * BFS_BLOCKSIZE - 1;
	sb.s_from = sb.s_to = sb.s_backup_from = sb.s_backup_to = -1;
	memcpy(sb.s_fsname, fsname, 6);
	memcpy(sb.s_volume, volume, 6);

	if (verbose) {
		fprintf(stderr, _("Device: %s\n"), device);
		fprintf(stderr, _("Volume: <%-6s>\n"), volume);
		fprintf(stderr, _("FSname: <%-6s>\n"), fsname);
		fprintf(stderr, _("BlockSize: %d\n"), BFS_BLOCKSIZE);
		if (ino_blocks==1)
			fprintf(stderr, _("Inodes: %d (in 1 block)\n"),
				inodes);
		else
			fprintf(stderr, _("Inodes: %d (in %lld blocks)\n"),
				inodes, ino_blocks);
		fprintf(stderr, _("Blocks: %lld\n"), total_blocks);
		fprintf(stderr, _("Inode end: %d, Data end: %d\n"),
			sb.s_start-1, sb.s_end);
	}

	if (write(fd, &sb, sizeof(sb)) != sizeof(sb))
		errx(EXIT_FAILURE, _("error writing superblock"));

	memset(&ri, 0, sizeof(ri));
	ri.i_ino = BFS_ROOT_INO;
	ri.i_first_block = 1 + ino_blocks;
	ri.i_last_block = ri.i_first_block +
		(inodes * sizeof(de) - 1) / BFS_BLOCKSIZE;
	ri.i_bytes_to_end = ri.i_first_block * BFS_BLOCKSIZE
		+ 2 * sizeof(struct bfsde) - 1;
	ri.i_type = BFS_DIR_TYPE;
	ri.i_mode = S_IFDIR | 0755; 	/* or just 0755 */
	ri.i_uid = 0;
	ri.i_gid = 1;			/* random */
	ri.i_nlinks = 2;
	time(&now);
	ri.i_atime = now;
	ri.i_mtime = now;
	ri.i_ctime = now;

	if (write(fd, &ri, sizeof(ri)) != sizeof(ri))
		errx(EXIT_FAILURE, _("error writing root inode"));

	memset(&ri, 0, sizeof(ri));
	for (i=1; i<inodes; i++)
		if (write(fd, &ri, sizeof(ri)) != sizeof(ri))
			errx(EXIT_FAILURE, _("error writing inode"));

	if (lseek(fd, (1 + ino_blocks)*BFS_BLOCKSIZE, SEEK_SET) == -1)
		errx(EXIT_FAILURE, _("seek error"));

	memset(&de, 0, sizeof(de));
	de.d_ino = BFS_ROOT_INO;
	memcpy(de.d_name, ".", 1);
	if (write(fd, &de, sizeof(de)) != sizeof(de))
		errx(EXIT_FAILURE, _("error writing . entry"));

	memcpy(de.d_name, "..", 2);
	if (write(fd, &de, sizeof(de)) != sizeof(de))
		errx(EXIT_FAILURE, _("error writing .. entry"));

	if (close(fd) == -1)
		err(EXIT_FAILURE, _("error closing %s"), device);

	return EXIT_SUCCESS;
}
