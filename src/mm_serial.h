/*
 * mm_serial.h - POSIX/Win32 Serial Port Library Interface
 *
 * This header file declares functions for cross-platform serial port communication,
 * supporting basic operations like opening, initializing, reading, writing,
 * and closing serial ports on POSIX and Windows systems.
 * Part of the mm_manager, used by oasis-utils.
 *
 * www.github.com/hharte/mm_manager
 * Copyright (c) 2020-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

#ifndef MM_SERIAL_H_
#define MM_SERIAL_H_

#include <stddef.h> /* Provides size_t */
#include <stdbool.h> /* For bool type */

 /*
  * Define ssize_t for MSVC, as it's not standard C but POSIX.
  * On POSIX systems (Linux, macOS), unistd.h provides it.
  */
#ifdef _MSC_VER
#ifndef _SSIZE_T_DEFINED      // <--- MODIFIED: Add guard
# include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED    // <--- MODIFIED: Define guard
#endif // _SSIZE_T_DEFINED
#else /* _MSC_VER */
# include <unistd.h> /* Provides ssize_t for POSIX */
#endif /* _MSC_VER */

  /* Function Prototypes */

  /**
   * open_serial() - Open a serial port device.
   * @modem_dev:	Path to the serial device (e.g., "/dev/ttyS0", "COM1").
   *
   * Return: File descriptor on success, negative errno code on failure.
   */
extern int open_serial(const char* modem_dev);

/**
 * init_serial() - Initialize serial port settings.
 * @fd:		File descriptor of the open serial port.
 * @baudrate:	Desired baud rate (e.g., 9600, 19200, 115200).
 * @enable_flow_control: Boolean to enable (true) or disable (false) RTS/CTS hardware flow control.
 *
 * Return: 0 on success, negative errno code on failure.
 */
extern int init_serial(int fd, int baudrate, bool enable_flow_control);

/**
 * close_serial() - Close the serial port.
 * @fd:		File descriptor of the open serial port.
 *
 * Return: 0 on success, negative errno code on failure.
 */
extern int close_serial(int fd);

/**
 * read_serial() - Read data from the serial port.
 * @fd:		File descriptor of the open serial port.
 * @buf:	Buffer to store read data.
 * @count:	Maximum number of bytes to read.
 *
 * Return: Number of bytes read on success, or negative errno code on failure.
 */
extern ssize_t read_serial(int fd, void* buf, size_t count);

/**
 * write_serial() - Write data to the serial port.
 * @fd:		File descriptor of the open serial port.
 * @buf:	Buffer containing data to write.
 * @count:	Number of bytes to write.
 *
 * Return: Number of bytes written on success, or negative errno code on failure.
 */
extern ssize_t write_serial(int fd, const void* buf, size_t count);

/**
 * drain_serial() - Wait for all output to be transmitted.
 * @fd:		File descriptor of the open serial port.
 *
 * Return: 0 on success, negative errno code on failure.
 */
extern int drain_serial(int fd);

/**
 * flush_serial() - Discard pending input and/or output data.
 * @fd:		File descriptor of the open serial port.
 *
 * Return: 0 on success, negative errno code on failure.
 */
extern int flush_serial(int fd);

#endif /* MM_SERIAL_H_ */
