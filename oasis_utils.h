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
extern const char kPathSeparator;

extern void oasis_list_dir_entry(directory_entry_block_t* dir_entry);
extern void oasis_convert_timestamp_to_tm(oasis_tm_t* timestamp, struct tm* tmout);
extern void oasis_convert_tm_to_timestamp(struct tm* tmin, oasis_tm_t* timestamp);
extern uint8_t oasis_lrcc(uint8_t* buf, uint16_t len);
extern void set_file_time(char* output_filename, struct tm* tmin);
extern void dump_hex(uint8_t* data, int len);
