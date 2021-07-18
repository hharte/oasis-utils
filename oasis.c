/*
 * Utility to list directory contents of OASIS floppy disks,
 * and optionally extract the files.
 *
 * www.github.com/hharte/oasis
 *
 * (c) 2021, Howard M. Harte
 *
 * Reference: Directory Entry Block (DEB) http://bitsavers.org/pdf/phaseOneSystems/oasis/Macro_Assembler_Reference_Manual_2ed.pdf pp. 112
 *
 */

#define _CRT_SECURE_NO_DEPRECATE

#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BLOCK_SIZE		(256)
#define FNAME_LEN	8
#define FEXT_LEN	8

#define FILE_FORMAT_DELETED		(0xFF)
#define FILE_FORMAT_EMPTY		(0x00)
#define FILE_FORMAT_SYNONYM		(0x80)
#define FILE_FORMAT_RELOCATABLE	(0x01)
#define FILE_FORMAT_ABSOLUTE	(0x02)
#define FILE_FORMAT_SEQUENTIAL	(0x04)
#define FILE_FORMAT_DIRECT		(0x08)
#define FILE_FORMAT_INDEXED		(0x10)
#define FILE_FORMAT_KEYED		(0x18)

const char kPathSeparator =
#ifdef _WIN32
	'\\';
#else
	'/';
#endif

#pragma pack (push, 1)
/* OASIS Filesystem header */
typedef struct filesystem_block {
	char label[FNAME_LEN];
	uint8_t timestamp[3];
	uint8_t unknown[12];
	uint8_t num_heads;
	uint8_t num_cyl;
	uint8_t num_sectors;
	uint8_t dir_entries_max;
	uint8_t unknown2[2];
	uint8_t free_blocks;
} filesystem_block_t;


/* OASIS Directory Entry */
typedef struct directory_entry_block {
	uint8_t file_format;
	char file_name[FNAME_LEN];
	char file_type[FEXT_LEN];
	uint16_t record_count;
	uint16_t block_count;
	uint16_t start_sector;
	uint16_t file_format_dependent1;
	uint8_t timestamp[3];
	uint8_t owner_id;
	uint8_t shared_from_owner_id;
	uint16_t file_format_dependent2;
} directory_entry_block_t;
#pragma pack (pop)

typedef struct oasis_args {
	char image_filename[256];
	char output_path[256];
	char operation[8];
	int dir_block_cnt;
	int force;
	int quiet;
} oasis_args_t;

/* Function prototypes */
int parse_args(int argc, char* argv[], oasis_args_t* args);
int oasis_read_dir_entries(FILE* stream, directory_entry_block_t* dir_entries, int dir_block_cnt, int dir_entries_max);
void oasis_list_dir_entry(directory_entry_block_t* dir_entry);
int oasis_extract_file(directory_entry_block_t* dir_entry, FILE* instream, char *path, int quiet);

#if defined(_WIN32)
#define strncasecmp(x,y,z) _strnicmp(x,y,z)
#endif

int main(int argc, char *argv[])
{
	FILE* instream;
	directory_entry_block_t* dir_entry_list;
	filesystem_block_t fs_block;
	oasis_args_t args;
	int positional_arg_cnt;
	int dir_entries_max;
	int dir_entry_cnt;
	int i;
	int result;
	int extracted_file_count = 0;
	int status = 0;

	positional_arg_cnt = parse_args(argc, argv, &args);

	if (positional_arg_cnt == 0) {
		printf("OASIS File Utility (c) 2021 - Howard M. Harte\n");
		printf("https://github.com/hharte/oasis\n\n");

		printf("usage is: %s <filename.img> [command] [<filename>|<path>] [-q] [-b=n]\n", argv[0]);
		printf("\t<filename.img> OASIS Disk Image in .img format.\n");
		printf("\t[command]      LI - List files\n");
		printf("\t               EX - Extract files to <path>\n");
		printf("\tFlags:\n");
		printf("\t      -q       Quiet: Don't list file details during extraction.\n");
		printf("\n\tIf no command is given, LIst is assumed.\n");
		return (-1);
	}

	if (!(instream = fopen(args.image_filename, "rb"))) {
		fprintf(stderr, "Error Openening %s\n", argv[1]);
		return (-ENOENT);
	}

	fseek(instream, 256, SEEK_SET);

	fread(&fs_block, sizeof(filesystem_block_t), 1, instream);

	char label[FNAME_LEN + 1];

	snprintf(label, sizeof(label), "%s", fs_block.label);

	dir_entries_max = fs_block.dir_entries_max * 8;

	printf("Label: %s\n", label);
	printf("%d-%d-%d\n", fs_block.num_cyl, fs_block.num_heads >> 4, fs_block.num_sectors);
	printf("%d directory entries\n", dir_entries_max);
	printf("%dK free\n", fs_block.free_blocks);

	fseek(instream, 512, SEEK_SET);

	dir_entry_list = (directory_entry_block_t*)calloc(dir_entries_max, sizeof(directory_entry_block_t));

	if (dir_entry_list == NULL) {
		fprintf(stderr, "Memory allocation of %d bytes failed\n", (int)(dir_entries_max * sizeof(directory_entry_block_t)));
		status = -ENOMEM;
		goto exit_main;
	}

	dir_entry_cnt = oasis_read_dir_entries(instream, dir_entry_list, args.dir_block_cnt, dir_entries_max);

	if (dir_entry_cnt == 0) {
		fprintf(stderr, "File not found\n");
		status = -ENOENT;
		goto exit_main;
	}

	/* Parse the command, and perform the requested action. */
	if ((positional_arg_cnt == 1) | (!strncasecmp(args.operation, "LI", 2))) {
		printf("Fname--- Ftype--  --Date-- Time- -Recs Blks Format- -Sect Own SOw Other-\n");

		for (i = 0; i < dir_entry_cnt; i++) {
			oasis_list_dir_entry(&dir_entry_list[i]);
		}
	} else {
		if (positional_arg_cnt < 2) {
			fprintf(stderr, "filename required.\n");
			status = -EBADF;
			goto exit_main;
		} else if (!strncasecmp(args.operation, "EX", 2)) {
			for (i = 0; i < dir_entry_cnt; i++) {
				result = oasis_extract_file(&dir_entry_list[i], instream, args.output_path, args.quiet);
				if (result == 0) {
					extracted_file_count++;
				}
			}
			printf("Extracted %d files.\n", extracted_file_count);
		}
	}

exit_main:
	if (dir_entry_list) free(dir_entry_list);

	if (instream != NULL) fclose(instream);

	return status;
}

int parse_args(int argc, char* argv[], oasis_args_t *args)
{
	int positional_arg_cnt = 0;

	memset(args, 0, sizeof(oasis_args_t));

	for (int i = 1; i < argc; i++) {
		if (argv[i][0] != '-') {
			switch (positional_arg_cnt) {
			case 0:
				snprintf(args->image_filename, sizeof(args->image_filename), "%s", argv[i]);
				break;
			case 1:
				snprintf(args->operation, sizeof(args->operation), "%s", argv[i]);
				break;
			case 2:
				snprintf(args->output_path, sizeof(args->output_path), "%s", argv[i]);
				break;
			}
			positional_arg_cnt++;
		}
		else {
			char flag = argv[i][1];
			switch (flag) {
			case 'f':
				args->force = 1;
				break;
			case 'q':
				args->quiet = 1;
				break;
			default:
				printf("Unknown option '-%c'\n", flag);
				break;
			}
		}
	}
	return positional_arg_cnt;
}

int oasis_read_dir_entries(FILE* stream, directory_entry_block_t* dir_entries, int dir_block_cnt, int dir_entries_max)
{
	int dir_entry_index = 0;
	directory_entry_block_t* dir_entry;

	for (int i = 0; i < dir_entries_max; i++) {
		size_t readlen;
		dir_entry = &dir_entries[dir_entry_index];
		readlen = fread(dir_entry, sizeof(directory_entry_block_t), 1, stream);

		if (readlen == 1) {
			if (dir_entry->file_format > 0) {
				dir_entry_index++;
			}
		}
	}
	return dir_entry_index;
}

void oasis_list_dir_entry(directory_entry_block_t* dir_entry)
{
	char fname[FNAME_LEN + 1];
	char fext[FEXT_LEN + 1];
	char format[8];
	char other_str[8];
	uint8_t day, month, year;
	uint8_t hours, minutes;
	 
	if (dir_entry->file_format == 0) {
		return;
	}

	snprintf(fname, sizeof(fname), "%s", dir_entry->file_name);
	snprintf(fext, sizeof(fext), "%s", dir_entry->file_type);
	snprintf(other_str, sizeof(other_str), "       ");

	/* Packed timestamp:
	 * +-------------------------------------------------+
	 * | BYTE  |      0      |      1      |      2      |
	 * | FIELD | MMMM | DDDD | DYYY | YHHH | HHMM | MMMM |
	 * | BIT   | 7         0 | 7         0 | 7         0 |
	 * +-------------------------------------------------+ 
	 */
	month = (dir_entry->timestamp[0] & 0xF0) >> 4;
	day = ((dir_entry->timestamp[0] & 0x0F) << 1) | ((dir_entry->timestamp[1] & 0x80) >> 7);
	year = ((dir_entry->timestamp[1] & 0x78) >> 3) + 77; /* 0 = 1977, highest valid year is 1992. */
	hours = ((dir_entry->timestamp[1] & 0x07) << 2) | ((dir_entry->timestamp[2] & 0xc0) >> 6);
	minutes = dir_entry->timestamp[2] & 0x3F;
	
	switch (dir_entry->file_format) {
	case FILE_FORMAT_DELETED:
		snprintf(format, sizeof(format), "Deleted");
		break;
	case FILE_FORMAT_EMPTY:
		snprintf(format, sizeof(format), "Empty  ");
		break;
	case FILE_FORMAT_SYNONYM:
		snprintf(format, sizeof(format), "Synonym");
		break;
	default:
		switch (dir_entry->file_format & 0x1F) {
		case FILE_FORMAT_RELOCATABLE:
			snprintf(format, sizeof(format), "R %5d", dir_entry->file_format_dependent1);
			if (dir_entry->file_format_dependent2 > 0) {
				snprintf(other_str, sizeof(other_str), "%5d L", dir_entry->file_format_dependent2);
			}
			break;
		case FILE_FORMAT_ABSOLUTE:
			snprintf(format, sizeof(format), "A %5d", dir_entry->file_format_dependent1);
			snprintf(other_str, sizeof(other_str), "%5d O", dir_entry->file_format_dependent2);
			break;
		case FILE_FORMAT_SEQUENTIAL:
			snprintf(format, sizeof(format), "S %5d", dir_entry->file_format_dependent1);
			if (dir_entry->file_format_dependent2 > 0) {
				snprintf(other_str, sizeof(other_str), "%5d E", dir_entry->file_format_dependent2);
			}
			break;
		case FILE_FORMAT_DIRECT:
			snprintf(format, sizeof(format), "D %5d", dir_entry->file_format_dependent1);
			break;
		case FILE_FORMAT_INDEXED:
			snprintf(format, sizeof(format), "I%3d/%3d", dir_entry->file_format_dependent1 & 0x1F, (dir_entry->file_format_dependent1 & 0xFE) >> 9);
			break;
		case FILE_FORMAT_KEYED:
			snprintf(format, sizeof(format), "K%3d/%3d", dir_entry->file_format_dependent1 & 0x1F, (dir_entry->file_format_dependent1 & 0xFE) >> 9);
			break;
		default:
			printf("Unsupported file type %x\n", dir_entry->file_format);
			break;
		}
	}

	printf("%s %s %02d/%02d/%02d %02d:%02d %5d %4d %s %5d %3d %3d  %s\n",
		fname,
		fext,
		month,
		day,
		year,
		hours, minutes,
		dir_entry->record_count,
		dir_entry->block_count,
		format,
		dir_entry->start_sector,
		dir_entry->owner_id,
		dir_entry->shared_from_owner_id,
		other_str);
}

int oasis_extract_file(directory_entry_block_t* dir_entry, FILE* instream, char *path, int quiet)
{
	char oasis_fname[FNAME_LEN + 1];
	char fname[FNAME_LEN + 1];
	char oasis_ftype[FEXT_LEN + 1];
	char ftype[FEXT_LEN + 1];

	if (dir_entry->file_format == 0) {
		return (-ENOENT);
	}

	snprintf(oasis_fname, sizeof(oasis_fname), "%s", dir_entry->file_name);
	snprintf(oasis_ftype, sizeof(oasis_ftype), "%s", dir_entry->file_type);

	/* Truncate the filename if a space is encountered. */
	for (unsigned int j = 0; j < strnlen(oasis_fname, sizeof(oasis_fname)); j++) {
		if (oasis_fname[j] == ' ') oasis_fname[j] = '\0';
	}

	snprintf(fname, sizeof(fname), "%s", oasis_fname);

	/* Truncate the type if a space is encountered. */
	for (unsigned int j = 0; j < strnlen(oasis_ftype, sizeof(oasis_ftype)); j++) {
		if (oasis_ftype[j] == ' ') oasis_ftype[j] = '\0';
	}

	snprintf(ftype, sizeof(fname), "%s", oasis_ftype);

	FILE* ostream;
	int file_offset;
	uint8_t* file_buf;
	int file_len = 0;
	char output_filename[256];

	switch (dir_entry->file_format & 0x1f) {
	case FILE_FORMAT_RELOCATABLE:
		file_len = dir_entry->file_format_dependent2;
		snprintf(output_filename, sizeof(output_filename), "%s%c%s.%s_R_%d", path, kPathSeparator, fname, ftype, dir_entry->file_format_dependent1);
		break;
	case FILE_FORMAT_ABSOLUTE:
		file_len = dir_entry->record_count * dir_entry->file_format_dependent1;
		snprintf(output_filename, sizeof(output_filename), "%s%c%s.%s_A_%d", path, kPathSeparator, fname, ftype, dir_entry->file_format_dependent2);
		break;
	case FILE_FORMAT_SEQUENTIAL:
		file_len = (dir_entry->file_format_dependent2 + 1 - dir_entry->start_sector) * BLOCK_SIZE;
		snprintf(output_filename, sizeof(output_filename), "%s%c%s.%s_S_%d", path, kPathSeparator, fname, ftype, dir_entry->file_format_dependent1);
		break;
	case FILE_FORMAT_DIRECT:
		file_len = dir_entry->block_count * 1024;
		snprintf(output_filename, sizeof(output_filename), "%s%c%s.%s_D_%d", path, kPathSeparator, fname, ftype, dir_entry->file_format_dependent1);
		break;
	case FILE_FORMAT_INDEXED:
		printf("Skipping INDEXED file: %s.%s\n", oasis_fname, oasis_ftype);
		break;
	case FILE_FORMAT_KEYED:
		printf("Skipping KEYED file: %s.%s\n", oasis_fname, oasis_ftype);
		break;
	default:
		break;
	}

	if (file_len == 0) {
		return (-ENOENT);
	}

	file_offset = dir_entry->start_sector * BLOCK_SIZE;

	output_filename[sizeof(output_filename) - 1] = '\0';
	if (!(ostream = fopen(output_filename, "wb"))) {
		printf("Error Openening %s\n", output_filename);
		return (-ENOENT);
	} else if ((file_buf = (uint8_t*)calloc(1, file_len))) {
		if (!quiet) printf("%s.%s -> %s (%d bytes)\n", oasis_fname, oasis_ftype, output_filename, file_len);

		fseek(instream, file_offset, SEEK_SET);
		fread(file_buf, file_len, 1, instream);
		fwrite(file_buf, file_len, 1, ostream);
		free(file_buf);
		fclose(ostream);
		return (0);
	}

	printf("Memory allocation of %d bytes failed\n", file_len);
	return (-ENOMEM);
}
