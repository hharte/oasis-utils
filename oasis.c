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

#include "oasis.h"
#include "oasis_utils.h"

#define LF      0x0A
#define CR      0x0D
#define SUB		0x1A

typedef struct oasis_args {
	char image_filename[256];
	char output_path[256];
	char operation[8];
	int dir_block_cnt;
	int ascii;
	int quiet;
} oasis_args_t;


/* Function prototypes */
int parse_args(int argc, char* argv[], oasis_args_t* args);
int oasis_read_dir_entries(FILE* stream, directory_entry_block_t* dir_entries, int dir_block_cnt, int dir_entries_max);
int oasis_extract_file(directory_entry_block_t* dir_entry, FILE* instream, char *path, int quiet, int ascii);

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
		printf("https://github.com/hharte/oasis-utils\n\n");

		printf("usage is: %s <filename.img> [command] [<filename>|<path>] [-q] [-a]\n", argv[0]);
		printf("\t<filename.img> OASIS Disk Image in .img format.\n");
		printf("\t[command]      LI - List files\n");
		printf("\t               EX - Extract files to <path>\n");
		printf("\tFlags:\n");
		printf("\t      -q       Quiet: Don't list file details during extraction.\n");
		printf("\t      -a       ASCII: Convert line endings and truncate output file at EOF.\n");
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
				result = oasis_extract_file(&dir_entry_list[i], instream, args.output_path, args.quiet, args.ascii);
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
			case 'a':
				args->ascii = 1;
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


int oasis_extract_file(directory_entry_block_t* dir_entry, FILE* instream, char *path, int quiet, int ascii)
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
	uint8_t file_format;
	int file_len = 0;
	int current_block = 0;
	char output_filename[256];

	file_format = dir_entry->file_format & 0x1f;
	switch (file_format) {
	case FILE_FORMAT_RELOCATABLE:
		file_len = dir_entry->record_count * dir_entry->file_format_dependent1;
		snprintf(output_filename, sizeof(output_filename), "%s%c%s.%s_R_%d", path, kPathSeparator, fname, ftype, dir_entry->file_format_dependent1);
		break;
	case FILE_FORMAT_ABSOLUTE:
		file_len = dir_entry->record_count * dir_entry->file_format_dependent1;
		snprintf(output_filename, sizeof(output_filename), "%s%c%s.%s_A_%d", path, kPathSeparator, fname, ftype, dir_entry->file_format_dependent2);
		break;
	case FILE_FORMAT_SEQUENTIAL:
		file_len = BLOCK_SIZE;	/* Determined by walking through the file block by block. */
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
		fseek(instream, file_offset, SEEK_SET);

		if (file_format != FILE_FORMAT_SEQUENTIAL) {
			/* For all file types except sequential, copy the entire file in one shot (the file is contiguous) */
			fread(file_buf, file_len, 1, instream);
			fwrite(file_buf, file_len, 1, ostream);
		}
		else {
			uint16_t link;

			/* For sequential, copy the file block by block (the file may not be contiguous) */
			do {
				char text_buf[BLOCK_SIZE * 2];
				char *text_ptr = text_buf;
				int text_len;
				int i;
				fread(file_buf, BLOCK_SIZE, 1, instream);
				link  = file_buf[BLOCK_SIZE - 1] << 8;
				link |= file_buf[BLOCK_SIZE - 2];

				for (i = 0; i < BLOCK_SIZE - 2; i++) {
					if (ascii && (file_buf[i] == SUB)) {
						/* Text file EOF */
						break;
					}
					if (ascii && (file_buf[i] == CR)) {
						/* OASIS uses CR for line endings: convert CR to CR/LF (for Windows) or LF (for UNIX) */
#if defined(_WIN32)
						*text_ptr++ = file_buf[i];
#endif
						*text_ptr++ = LF;	/* Add LF */
					} else {
						*text_ptr++ = file_buf[i];
					}
				}

				text_len = (int)(text_ptr - text_buf);
				fwrite(text_buf, text_len, 1, ostream);

				current_block++;
				if (current_block > dir_entry->block_count * 4) {
					printf("Corrupted link: ");
					break;
				}

				file_offset = link * BLOCK_SIZE;
				fseek(instream, file_offset, SEEK_SET);
			} while (link != 0);
		}
		if (!quiet) printf("%s.%s -> %s (%ld bytes)\n", oasis_fname, oasis_ftype, output_filename, ftell(ostream));

		free(file_buf);
		fclose(ostream);
		return (0);
	}

	printf("Memory allocation of %d bytes failed\n", file_len);
	return (-ENOMEM);
}
