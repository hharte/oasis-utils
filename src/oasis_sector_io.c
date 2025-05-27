/*
 * oasis_sector_io.c - Sector-based I/O Abstraction Layer Implementation
 *
 * Implements the sector I/O functions for raw disk image files and
 * ImageDisk (.IMD) files with OASIS-specific constraints.
 * Handles logical pairing of 128-byte IMD sectors for OASIS 256-byte sectors,
 * even with physical interleaving.
 *
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2021-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h> /* For tolower */

#include "oasis.h"   /* Main header, should include oasis_sector_io.h */

 /* Uncomment to enable debug prints */
 /* #define OASIS_SECTOR_IO_DEBUG */

#ifdef OASIS_SECTOR_IO_DEBUG
#define SIO_DEBUG_PRINTF(...) do { fprintf(stderr, "SIO_DEBUG: %s:%d:%s(): ", __FILE__, __LINE__, __func__); fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define SIO_DEBUG_PRINTF(...) do { /* Nothing */ } while (0)
#endif

/* Case-insensitive string comparison helper for file extensions */
static int strcasecmp_local(const char* s1, const char* s2) {
    int c1, c2;
    if (s1 == s2) return 0;
    if (s1 == NULL) return -1;
    if (s2 == NULL) return 1;

    do {
        c1 = tolower((unsigned char)*s1++);
        c2 = tolower((unsigned char)*s2++);
    } while (c1 == c2 && c1 != 0);
    return c1 - c2;
}

/*
 * @brief Opens a disk image file for sector-based I/O.
 */
sector_io_stream_t* sector_io_open(const char* image_path, const char* mode) {
    sector_io_stream_t* stream = NULL;
    const char* dot;
    size_t i_track; /* Use size_t for loop counters with size_t bounds */

    if (image_path == NULL || mode == NULL) {
        fprintf(stderr, "sector_io_open: Error - NULL image_path or mode.\n");
        return NULL;
    }

    stream = (sector_io_stream_t*)malloc(sizeof(sector_io_stream_t));
    if (stream == NULL) {
        perror("sector_io_open: Error allocating memory for stream structure");
        return NULL;
    }
    memset(stream, 0, sizeof(sector_io_stream_t)); /* Initialize all to 0/NULL */

    dot = strrchr(image_path, '.');
    if (dot && (strcasecmp_local(dot, ".IMD") == 0)) {
        int is_read_only = 1;
        int imdf_err;
        size_t num_img_tracks = 0;
        uint32_t current_total_oasis_sectors = 0;

        SIO_DEBUG_PRINTF("Attempting to open '%s' as IMD image.\n", image_path);

        if (strchr(mode, 'w') != NULL || strchr(mode, '+') != NULL) {
            is_read_only = 0;
        }

        imdf_err = imdf_open(image_path, is_read_only, &stream->imdf_handle);
        if (imdf_err == IMDF_ERR_OK) {
            strcpy(stream->image_type, "IMD");
            stream->file_ptr = NULL; /* Not used for IMD via libimdf */

            /* Validate IMD tracks and calculate total_sectors for oasis_sector_io (256-byte view) */
            imdf_get_num_tracks(stream->imdf_handle, &num_img_tracks);
            SIO_DEBUG_PRINTF("IMD: Found %zu tracks.\n", num_img_tracks);

            for (i_track = 0; i_track < num_img_tracks; ++i_track) {
                const ImdTrackInfo* track = imdf_get_track_info(stream->imdf_handle, i_track);
                int count128 = 0;
                int count256 = 0;
                int has_128 = 0;
                int has_256 = 0;
                uint8_t s_idx; /* Use uint8_t for sector index on track */

                if (!track || !track->loaded) {
                    fprintf(stderr, "sector_io_open (IMD): Error getting info for track %zu.\n", i_track);
                    imdf_close(stream->imdf_handle);
                    free(stream);
                    return NULL;
                }

                SIO_DEBUG_PRINTF("IMD Track C:%u H:%u - IMDSectors:%u, IMDSectorSizeCode:%u (ActualSize:%u bytes)\n",
                    track->cyl, track->head, track->num_sectors, track->sector_size_code, track->sector_size);

                for (s_idx = 0; s_idx < track->num_sectors; ++s_idx) {
                    if (track->sector_size != 128 && track->sector_size != 256) {
                        fprintf(stderr, "sector_io_open (IMD): Error - Track %zu (C:%u H:%u) has unsupported sector size %u. Only 128 or 256 are allowed for OASIS use.\n",
                            i_track, track->cyl, track->head, track->sector_size);
                        imdf_close(stream->imdf_handle);
                        free(stream);
                        return NULL;
                    }

                    if (track->sector_size == 128) {
                        if (has_256) { /* 128-byte sector found after a 256-byte sector on this track */
                            fprintf(stderr, "sector_io_open (IMD): Error - Track %zu (C:%u H:%u) has 128-byte sectors after 256-byte sectors. This is not supported for OASIS use.\n",
                                i_track, track->cyl, track->head);
                            imdf_close(stream->imdf_handle);
                            free(stream);
                            return NULL;
                        }
                        count128++;
                        has_128 = 1;
                    }
                    else { /* track->sector_size == 256 */
                        count256++;
                        has_256 = 1;
                    }
                }

                if (has_128 && (count128 % 2 != 0)) {
                    fprintf(stderr, "sector_io_open (IMD): Error - Track %zu (C:%u H:%u) has an odd number of 128-byte sectors (%d). Required to be even for OASIS 256-byte logical sectors.\n",
                        i_track, track->cyl, track->head, count128);
                    imdf_close(stream->imdf_handle);
                    free(stream);
                    return NULL;
                }
                current_total_oasis_sectors += (count128 / 2) + count256;
            }
            stream->total_sectors = current_total_oasis_sectors;
            SIO_DEBUG_PRINTF("IMD: Opened and validated successfully. Total 256-byte equivalent OASIS sectors: %u\n", stream->total_sectors);
            return stream;
        }
        else {
            fprintf(stderr, "sector_io_open: Failed to open '%s' as IMD image (libimdf error: %d). Ensure it's a valid IMD file.\n", image_path, imdf_err);
            free(stream);
            return NULL; /* Failed to open as IMD */
        }
    }
    else { /* RAW file handling */
        long file_size_raw;
        SIO_DEBUG_PRINTF("Attempting to open '%s' as RAW image.\n", image_path);
        stream->file_ptr = fopen(image_path, mode);
        if (stream->file_ptr == NULL) {
            perror("sector_io_open: Error opening raw image file");
            free(stream);
            return NULL;
        }
        strcpy(stream->image_type, "RAW");
        stream->imdf_handle = NULL;

        if (fseek(stream->file_ptr, 0, SEEK_END) != 0) {
            perror("sector_io_open (RAW): Error seeking to end of file");
            fclose(stream->file_ptr);
            free(stream);
            return NULL;
        }
        file_size_raw = ftell(stream->file_ptr);
        if (file_size_raw < 0) {
            perror("sector_io_open (RAW): Error getting file size");
            fclose(stream->file_ptr);
            free(stream);
            return NULL;
        }
        if (fseek(stream->file_ptr, 0, SEEK_SET) != 0) { /* Rewind to beginning */
            perror("sector_io_open (RAW): Error seeking to beginning of file");
            fclose(stream->file_ptr);
            free(stream);
            return NULL;
        }
        if (file_size_raw > 0 && file_size_raw % SECTOR_SIZE != 0) {
            fprintf(stderr, "sector_io_open (RAW): Warning - Image file size (%ld bytes) is not a multiple of SECTOR_SIZE (%d bytes).\n", file_size_raw, SECTOR_SIZE);
        }
        stream->total_sectors = (uint32_t)(file_size_raw / SECTOR_SIZE);
        SIO_DEBUG_PRINTF("RAW: Opened successfully. Total 256-byte sectors: %u\n", stream->total_sectors);
        return stream;
    }
}

/*
 * @brief Closes a disk image file previously opened with sector_io_open.
 */
int sector_io_close(sector_io_stream_t* stream) {
    int ret = 0; /* Success by default */

    if (stream == NULL) {
        SIO_DEBUG_PRINTF("Called with NULL stream, no action taken.\n");
        return 0; /* Or an error if NULL stream is invalid */
    }

    SIO_DEBUG_PRINTF("Closing stream, type: %s\n", stream->image_type);

    if (stream->imdf_handle != NULL) {
        SIO_DEBUG_PRINTF("Closing IMD handle.\n");
        imdf_close(stream->imdf_handle); /* libimdf's close doesn't return a value */
        stream->imdf_handle = NULL;
    }

    if (stream->file_ptr != NULL) {
        SIO_DEBUG_PRINTF("Closing RAW file pointer.\n");
        if (fflush(stream->file_ptr) != 0) {
            perror("sector_io_close: fflush failed for file_ptr");
            /* Consider this a non-fatal warning for close, but log it. */
        }
        if (fclose(stream->file_ptr) != 0) {
            perror("sector_io_close: Error closing file_ptr");
            ret = EOF; /* Indicate fclose error */
        }
        stream->file_ptr = NULL;
    }
    free(stream);
    return ret;
}

/*
 * @brief Helper function to find an IMD sector by its logical ID on a given track.
 * @param track Pointer to the ImdTrackInfo structure.
 * @param target_id The logical sector ID to find.
 * @param found_sflag Pointer to store the sflag of the found sector. Can be NULL.
 * @return The index in track->smap if found, -1 otherwise.
 */
static int find_imd_sector_index_by_id(const ImdTrackInfo* track, uint8_t target_id, uint8_t* found_sflag) {
    uint8_t s_idx;
    if (!track) return -1;
    for (s_idx = 0; s_idx < track->num_sectors; ++s_idx) {
        if (track->smap[s_idx] == target_id) {
            if (found_sflag) {
                *found_sflag = track->sflag[s_idx];
            }
            return s_idx; /* Return the physical index in smap */
        }
    }
    return -1; /* Not found */
}


/*
 * @brief Reads one or more 256-byte logical sectors from the disk image.
 */
ssize_t sector_io_read(sector_io_stream_t* stream, uint32_t sector_lba_oasis, uint32_t num_sectors_oasis, uint8_t* buffer) {
    uint32_t i_oasis_sector; /* Loop counter for OASIS sectors */

    if (stream == NULL || (!stream->file_ptr && !stream->imdf_handle) || buffer == NULL) {
        fprintf(stderr, "sector_io_read: Error - Invalid arguments (NULL stream, internal handle, or buffer).\n");
        return -1;
    }
    if (num_sectors_oasis == 0) {
        return 0;
    }

    SIO_DEBUG_PRINTF("Read request: OASIS LBA %u, Count %u, Type: %s\n", sector_lba_oasis, num_sectors_oasis, stream->image_type);

    if (strcmp(stream->image_type, "IMD") == 0) {
        ssize_t total_oasis_sectors_read = 0;
        uint8_t* current_buffer_ptr = buffer; /* Pointer to current position in output buffer */
        uint32_t cumulative_oasis_lba_offset = 0; /* Tracks 256-byte equivalent LBAs processed so far across all tracks */
        size_t num_img_tracks = 0;

        if (stream->imdf_handle == NULL) {
            fprintf(stderr, "sector_io_read (IMD): Error - IMD handle is NULL.\n");
            return -1;
        }
        imdf_get_num_tracks(stream->imdf_handle, &num_img_tracks);

        for (i_oasis_sector = 0; i_oasis_sector < num_sectors_oasis; ++i_oasis_sector) {
            uint32_t target_oasis_lba = sector_lba_oasis + i_oasis_sector;
            int found_lba_for_current_oasis_sector = 0;

            if (target_oasis_lba >= stream->total_sectors) {
                SIO_DEBUG_PRINTF("IMD Read: Target OASIS LBA %u is beyond total sectors %u. Ending read.\n", target_oasis_lba, stream->total_sectors);
                break; /* Requested LBA is out of bounds for the disk image */
            }

            cumulative_oasis_lba_offset = 0; /* Reset for each target_oasis_lba search */
            for (size_t i_track = 0; i_track < num_img_tracks; ++i_track) {
                const ImdTrackInfo* track = imdf_get_track_info(stream->imdf_handle, i_track);
                uint32_t oasis_lbas_on_this_track = 0;

                if (!track || !track->loaded) continue;

                if (track->sector_size == 256) {
                    oasis_lbas_on_this_track = track->num_sectors;
                }
                else if (track->sector_size == 128) {
                    oasis_lbas_on_this_track = track->num_sectors / 2; /* Assumes even number, validated in open */
                }

                if (target_oasis_lba >= cumulative_oasis_lba_offset &&
                    target_oasis_lba < cumulative_oasis_lba_offset + oasis_lbas_on_this_track) {
                    /* The target_oasis_lba falls on this track */
                    uint32_t oasis_lba_offset_on_track = target_oasis_lba - cumulative_oasis_lba_offset;

                    if (track->sector_size == 256) {
                        uint8_t imd_sector_id_to_read = track->smap[oasis_lba_offset_on_track];
                        uint8_t sflag_of_sector = track->sflag[oasis_lba_offset_on_track];
                        SIO_DEBUG_PRINTF("IMD Read: OASIS LBA %u -> Track C:%u H:%u, IMD Sector ID %u (256-byte)\n",
                            target_oasis_lba, track->cyl, track->head, imd_sector_id_to_read);

                        if (sflag_of_sector == IMD_SDR_UNAVAILABLE || IMD_SDR_HAS_ERR(sflag_of_sector)) {
                            SIO_DEBUG_PRINTF("IMD Read: Sector C:%u H:%u ID:%u (OASIS LBA %u) unavailable/error. Padding.\n",
                                track->cyl, track->head, imd_sector_id_to_read, target_oasis_lba);
                            memset(current_buffer_ptr, 0x00, SECTOR_SIZE);
                        }
                        else {
                            if (imdf_read_sector(stream->imdf_handle, track->cyl, track->head, imd_sector_id_to_read, current_buffer_ptr, SECTOR_SIZE) != IMDF_ERR_OK) {
                                fprintf(stderr, "sector_io_read (IMD): imdf_read_sector failed for 256-byte sector ID %u at OASIS LBA %u.\n", imd_sector_id_to_read, target_oasis_lba);
                                return total_oasis_sectors_read > 0 ? total_oasis_sectors_read : -1;
                            }
                        }
                    }
                    else { /* track->sector_size == 128 */
                        /* Assuming 1-based sector IDs for pairing on the track */
                        uint8_t target_id1 = (uint8_t)((oasis_lba_offset_on_track * 2) + 1);
                        uint8_t target_id2 = (uint8_t)((oasis_lba_offset_on_track * 2) + 2);
                        uint8_t sflag1 = 0, sflag2 = 0;
                        int smap_idx1, smap_idx2;

                        SIO_DEBUG_PRINTF("IMD Read: OASIS LBA %u -> Track C:%u H:%u, Target IMD Log.IDs %u & %u (128-byte pair)\n",
                            target_oasis_lba, track->cyl, track->head, target_id1, target_id2);

                        smap_idx1 = find_imd_sector_index_by_id(track, target_id1, &sflag1);
                        smap_idx2 = find_imd_sector_index_by_id(track, target_id2, &sflag2);

                        if (smap_idx1 == -1 || smap_idx2 == -1) {
                            fprintf(stderr, "sector_io_read (IMD): Could not find logical pair IDs %u or %u on track C:%u H:%u for OASIS LBA %u.\n",
                                target_id1, target_id2, track->cyl, track->head, target_oasis_lba);
                            return total_oasis_sectors_read > 0 ? total_oasis_sectors_read : -1;
                        }

                        if (sflag1 == IMD_SDR_UNAVAILABLE || IMD_SDR_HAS_ERR(sflag1) || sflag2 == IMD_SDR_UNAVAILABLE || IMD_SDR_HAS_ERR(sflag2)) {
                            SIO_DEBUG_PRINTF("IMD Read: One or both 128-byte sectors (IDs %u, %u) for OASIS LBA %u unavailable/error. Padding.\n",
                                target_id1, target_id2, target_oasis_lba);
                            memset(current_buffer_ptr, 0x00, SECTOR_SIZE);
                        }
                        else {
                            if (imdf_read_sector(stream->imdf_handle, track->cyl, track->head, target_id1, current_buffer_ptr, 128) != IMDF_ERR_OK) {
                                fprintf(stderr, "sector_io_read (IMD): imdf_read_sector failed for 1st 128-byte sector ID %u at OASIS LBA %u.\n", target_id1, target_oasis_lba);
                                return total_oasis_sectors_read > 0 ? total_oasis_sectors_read : -1;
                            }
                            if (imdf_read_sector(stream->imdf_handle, track->cyl, track->head, target_id2, current_buffer_ptr + 128, 128) != IMDF_ERR_OK) {
                                fprintf(stderr, "sector_io_read (IMD): imdf_read_sector failed for 2nd 128-byte sector ID %u at OASIS LBA %u.\n", target_id2, target_oasis_lba);
                                return total_oasis_sectors_read > 0 ? total_oasis_sectors_read : -1;
                            }
                        }
                    }
                    current_buffer_ptr += SECTOR_SIZE;
                    total_oasis_sectors_read++;
                    found_lba_for_current_oasis_sector = 1;
                    break; /* Found the track for target_oasis_lba, break from track loop */
                }
                cumulative_oasis_lba_offset += oasis_lbas_on_this_track;
            } /* End i_track loop */

            if (!found_lba_for_current_oasis_sector) {
                /* This should ideally not be reached if target_oasis_lba < stream->total_sectors,
                   as stream->total_sectors is calculated by summing oasis_lbas_on_this_track. */
                fprintf(stderr, "sector_io_read (IMD): Target OASIS LBA %u not found in image (logic error or inconsistent total_sectors). Read %zd sectors so far.\n", target_oasis_lba, total_oasis_sectors_read);
                return total_oasis_sectors_read;
            }
        } /* End i_oasis_sector loop */
        return total_oasis_sectors_read;

    }
    else if (strcmp(stream->image_type, "RAW") == 0) {
        long offset;
        size_t items_read;

        if (stream->total_sectors > 0 && sector_lba_oasis >= stream->total_sectors) {
            SIO_DEBUG_PRINTF("RAW Read: Attempt to read starting at or beyond EOF (LBA %u, total %u).\n", sector_lba_oasis, stream->total_sectors);
            return 0;
        }
        if (stream->total_sectors > 0 && sector_lba_oasis + num_sectors_oasis > stream->total_sectors) {
            SIO_DEBUG_PRINTF("RAW Read: Attempt to read beyond EOF (LBA %u, num %u, total %u). Adjusting num_sectors_oasis.\n", sector_lba_oasis, num_sectors_oasis, stream->total_sectors);
            num_sectors_oasis = stream->total_sectors - sector_lba_oasis;
            if (num_sectors_oasis == 0 && (stream->total_sectors - sector_lba_oasis > 0)) {
                return 0;
            }
            else if (stream->total_sectors - sector_lba_oasis == 0 && num_sectors_oasis > 0) {
                return 0;
            }
        }
        if (num_sectors_oasis == 0) return 0;


        offset = (long)sector_lba_oasis * SECTOR_SIZE;
        if (fseek(stream->file_ptr, offset, SEEK_SET) != 0) {
            perror("sector_io_read (RAW): Error seeking to sector offset");
            return -1;
        }
        items_read = fread(buffer, SECTOR_SIZE, num_sectors_oasis, stream->file_ptr);
        if (items_read < num_sectors_oasis) {
            if (feof(stream->file_ptr)) SIO_DEBUG_PRINTF("RAW Read: EOF reached. Expected %u, read %zu.\n", num_sectors_oasis, items_read);
            else if (ferror(stream->file_ptr)) perror("sector_io_read (RAW): Error reading sectors");
            else SIO_DEBUG_PRINTF("RAW Read: Short read. Expected %u, read %zu.\n", num_sectors_oasis, items_read);
        }
        return (ssize_t)items_read;
    }
    else {
        fprintf(stderr, "sector_io_read: Error - Unknown image type '%s'.\n", stream->image_type);
        return -1;
    }
}

/*
 * @brief Writes one or more 256-byte logical sectors to the disk image.
 */
ssize_t sector_io_write(sector_io_stream_t* stream, uint32_t sector_lba_oasis, uint32_t num_sectors_oasis, const uint8_t* buffer) {
    uint32_t i_oasis_sector;

    if (stream == NULL || (!stream->file_ptr && !stream->imdf_handle) || buffer == NULL) {
        fprintf(stderr, "sector_io_write: Error - Invalid arguments.\n");
        return -1;
    }
    if (num_sectors_oasis == 0) {
        return 0;
    }

    SIO_DEBUG_PRINTF("Write request: OASIS LBA %u, Count %u, Type: %s\n", sector_lba_oasis, num_sectors_oasis, stream->image_type);

    if (strcmp(stream->image_type, "IMD") == 0) {
        ssize_t total_oasis_sectors_written = 0;
        const uint8_t* current_buffer_ptr = buffer;
        int ro_status = 0;
        uint32_t cumulative_oasis_lba_offset = 0;
        size_t num_img_tracks = 0;

        if (stream->imdf_handle == NULL) {
            fprintf(stderr, "sector_io_write (IMD): Error - IMD handle is NULL.\n");
            return -1;
        }
        imdf_get_write_protect(stream->imdf_handle, &ro_status);
        if (ro_status) {
            fprintf(stderr, "sector_io_write (IMD): Error - Image is write-protected.\n");
            return -1;
        }
        imdf_get_num_tracks(stream->imdf_handle, &num_img_tracks);

        for (i_oasis_sector = 0; i_oasis_sector < num_sectors_oasis; ++i_oasis_sector) {
            uint32_t target_oasis_lba = sector_lba_oasis + i_oasis_sector;
            int found_lba = 0;
            int imdf_err;

            if (target_oasis_lba >= stream->total_sectors) {
                SIO_DEBUG_PRINTF("IMD Write: Target OASIS LBA %u is beyond total sectors %u. Ending write.\n", target_oasis_lba, stream->total_sectors);
                break;
            }

            cumulative_oasis_lba_offset = 0;
            for (size_t i_track = 0; i_track < num_img_tracks; ++i_track) {
                const ImdTrackInfo* track = imdf_get_track_info(stream->imdf_handle, i_track);
                uint32_t oasis_lbas_on_this_track = 0;

                if (!track || !track->loaded) continue;

                if (track->sector_size == 256) {
                    oasis_lbas_on_this_track = track->num_sectors;
                }
                else if (track->sector_size == 128) {
                    oasis_lbas_on_this_track = track->num_sectors / 2;
                }

                if (target_oasis_lba >= cumulative_oasis_lba_offset &&
                    target_oasis_lba < cumulative_oasis_lba_offset + oasis_lbas_on_this_track) {
                    uint32_t oasis_lba_offset_on_track = target_oasis_lba - cumulative_oasis_lba_offset;

                    if (track->sector_size == 256) {
                        uint8_t imd_sector_id_to_write = track->smap[oasis_lba_offset_on_track];
                        SIO_DEBUG_PRINTF("IMD Write: OASIS LBA %u -> Track C:%u H:%u, IMD Sector ID %u (256-byte)\n",
                            target_oasis_lba, track->cyl, track->head, imd_sector_id_to_write);
                        imdf_err = imdf_write_sector(stream->imdf_handle, track->cyl, track->head, imd_sector_id_to_write, current_buffer_ptr, SECTOR_SIZE);
                        if (imdf_err != IMDF_ERR_OK) {
                            fprintf(stderr, "sector_io_write (IMD): imdf_write_sector failed for 256-byte sector ID %u (err %d) at OASIS LBA %u.\n", imd_sector_id_to_write, imdf_err, target_oasis_lba);
                            return total_oasis_sectors_written > 0 ? total_oasis_sectors_written : -1;
                        }
                    }
                    else { /* track->sector_size == 128 */
                        uint8_t target_id1 = (uint8_t)((oasis_lba_offset_on_track * 2) + 1);
                        uint8_t target_id2 = (uint8_t)((oasis_lba_offset_on_track * 2) + 2);
                        int smap_idx1, smap_idx2; /* Not strictly needed for write by ID, but good for debug */

                        SIO_DEBUG_PRINTF("IMD Write: OASIS LBA %u -> Track C:%u H:%u, Target IMD Log.IDs %u & %u (128-byte pair)\n",
                            target_oasis_lba, track->cyl, track->head, target_id1, target_id2);

                        smap_idx1 = find_imd_sector_index_by_id(track, target_id1, NULL); /* Find to confirm existence for debug */
                        smap_idx2 = find_imd_sector_index_by_id(track, target_id2, NULL);
                        if (smap_idx1 == -1 || smap_idx2 == -1) {
                            fprintf(stderr, "sector_io_write (IMD): Could not find logical pair IDs %u or %u on track C:%u H:%u for OASIS LBA %u for writing.\n",
                                target_id1, target_id2, track->cyl, track->head, target_oasis_lba);
                            return total_oasis_sectors_written > 0 ? total_oasis_sectors_written : -1;
                        }

                        imdf_err = imdf_write_sector(stream->imdf_handle, track->cyl, track->head, target_id1, current_buffer_ptr, 128);
                        if (imdf_err != IMDF_ERR_OK) {
                            fprintf(stderr, "sector_io_write (IMD): imdf_write_sector failed for 1st 128-byte sector ID %u (err %d) at OASIS LBA %u.\n", target_id1, imdf_err, target_oasis_lba);
                            return total_oasis_sectors_written > 0 ? total_oasis_sectors_written : -1;
                        }
                        imdf_err = imdf_write_sector(stream->imdf_handle, track->cyl, track->head, target_id2, current_buffer_ptr + 128, 128);
                        if (imdf_err != IMDF_ERR_OK) {
                            fprintf(stderr, "sector_io_write (IMD): imdf_write_sector failed for 2nd 128-byte sector ID %u (err %d) at OASIS LBA %u.\n", target_id2, imdf_err, target_oasis_lba);
                            return total_oasis_sectors_written > 0 ? total_oasis_sectors_written : -1;
                        }
                    }
                    current_buffer_ptr += SECTOR_SIZE;
                    total_oasis_sectors_written++;
                    found_lba = 1;
                    break; /* Found the track for target_oasis_lba */
                }
                cumulative_oasis_lba_offset += oasis_lbas_on_this_track;
            } /* End i_track loop */

            if (!found_lba) {
                fprintf(stderr, "sector_io_write (IMD): Target OASIS LBA %u not found in image for writing (logic error or inconsistent total_sectors). Wrote %zd sectors so far.\n", target_oasis_lba, total_oasis_sectors_written);
                return total_oasis_sectors_written;
            }
        } /* End i_oasis_sector loop */
        return total_oasis_sectors_written;

    }
    else if (strcmp(stream->image_type, "RAW") == 0) {
        long offset;
        size_t items_written;
        uint32_t end_sector_exclusive_raw = sector_lba_oasis + num_sectors_oasis;

        offset = (long)sector_lba_oasis * SECTOR_SIZE;
        if (fseek(stream->file_ptr, offset, SEEK_SET) != 0) {
            perror("sector_io_write (RAW): Error seeking to sector offset");
            return -1;
        }
        items_written = fwrite(buffer, SECTOR_SIZE, num_sectors_oasis, stream->file_ptr);

        /*
         * Always attempt to flush after any fwrite operation.
         * This is crucial for tests that reopen the file immediately to verify writes.
         */
        if (fflush(stream->file_ptr) != 0) {
            perror("sector_io_write (RAW): fflush failed after fwrite");
            /* If fflush fails, the data might not have reached the OS buffers or disk.
             * This is a critical error for the write operation's integrity.
             */
            return -1; /* Indicate error */
        }

        if (items_written < num_sectors_oasis) {
            if (ferror(stream->file_ptr)) {
                perror("sector_io_write (RAW): Error reported by ferror after short write");
                /* Consider clearing the error: clearerr(stream->file_ptr); */
                return -1; /* ferror indicates a stream error, treat as write failure */
            }
            /* If not ferror, it was a short write (e.g., disk full but fwrite managed some part). */
            SIO_DEBUG_PRINTF("RAW Write: Short write. Expected %u, wrote %zu.\n", num_sectors_oasis, items_written);
            /* Update total_sectors if the partial write extended the known file size. */
            if (stream->total_sectors > 0 && sector_lba_oasis + items_written > stream->total_sectors) {
                stream->total_sectors = sector_lba_oasis + (uint32_t)items_written;
            } else if (stream->total_sectors == 0 && items_written > 0) { /* If file was initially empty */
                 stream->total_sectors = sector_lba_oasis + (uint32_t)items_written;
            }
            return (ssize_t)items_written; /* Return actual number of sectors written */
        }

        /* Full write was successful (items_written == num_sectors_oasis) */
        if (stream->total_sectors > 0 && end_sector_exclusive_raw > stream->total_sectors) {
            stream->total_sectors = end_sector_exclusive_raw;
        } else if (stream->total_sectors == 0 && num_sectors_oasis > 0) { /* If file was empty and sectors were written */
            stream->total_sectors = end_sector_exclusive_raw;
        }
        return (ssize_t)items_written;
    }
    else {
        fprintf(stderr, "sector_io_write: Error - Unknown image type '%s'.\n", stream->image_type);
        return -1;
    }
}


/*
 * @brief Gets the total number of 256-byte logical sectors in the opened disk image.
 */
uint32_t sector_io_get_total_sectors(sector_io_stream_t* stream) {
    if (stream == NULL) {
        fprintf(stderr, "sector_io_get_total_sectors: Error - Stream pointer is NULL.\n");
        return 0;
    }
    /* The stream->total_sectors should have been correctly calculated in sector_io_open
       for both RAW and validated IMD types (as 256-byte equivalent sectors). */
    SIO_DEBUG_PRINTF("Get total sectors for type '%s': %u\n", stream->image_type, stream->total_sectors);
    return stream->total_sectors;
}
