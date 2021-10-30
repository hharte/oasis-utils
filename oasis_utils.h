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
extern uint8_t oasis_lrcc(uint8_t* buf, uint16_t len);
extern void dump_hex(uint8_t* data, int len);

