/*
 * OASIS Utility Functions
 *
 * www.github.com/hharte/oasis
 *
 * (c) 2021, Howard M. Harte
 *
 * Reference: http://bitsavers.org/pdf/phaseOneSystems/oasis/
 *
 */

#define _CRT_SECURE_NO_DEPRECATE

#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "oasis.h"

const char kPathSeparator =
#ifdef _WIN32
'\\';
#else
'/';
#endif


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

/*
 * Text messages include a Longitudinal Redundancy Check Code (LRCC) which is the
 * 8-bit sum of all characters transmitted in the record, including all
 * control characters. The sum is logically ORed with 0xC0 and transmitted at the
 * end of the message
 */
uint8_t oasis_lrcc(uint8_t* buf, uint16_t len)
{
	uint8_t lrcc = 0;
	for (uint16_t i = 0; i < len; i++) {
		lrcc += buf[i];
	}

	/* Not sure why OASIS didn't specify the LRCC as being ORed with 0x40, since the
	 * high bit will be masked on transmit anyway.
	 */
	lrcc |= 0xC0;
	lrcc &= 0x7F;

	return lrcc;
}

void dump_hex(uint8_t* data, int len)
{
	uint8_t ascii[32];
	uint8_t* pascii = ascii;

	printf("\n");
	if (len > 0) {
		uint16_t i;
		printf("\tData:");

		for (i = 0; i < len; i++) {
			if (i % 16 == 0) {
				if (i > 0) {
					*pascii++ = '\0';
					printf("%s", ascii);
				}
				printf("\n\t%03x: ", i);
				pascii = ascii;
			}
			printf("%02x, ", data[i]);
			if (((data[i]) >= 0x20) && (data[i] < 0x7F)) {
				*pascii++ = data[i];
			}
			else {
				*pascii++ = '.';
			}
		}
		*pascii++ = '\0';
		if (strnlen((char*)ascii, sizeof(ascii)) > 0) {
			for (i = 0; i < 16 - strnlen((char*)ascii, sizeof(ascii)); i++) {
				printf("    ");
			}
			printf("%s", ascii);
		}
	}
	printf("\n");
}
