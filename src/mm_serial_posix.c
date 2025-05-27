/*
 * mm_serial_posix.c - POSIX serial port library implementation
 *
 * Part of mm_manager.
 *
 * www.github.com/hharte/mm_manager
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2020-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

/* Standard C Headers */
#include <stdio.h>	/* For fprintf */
#include <stdlib.h>	/* Standard library definitions (might not be needed here) */
#include <stdint.h>	/* Standard integer types (might not be needed here) */
#include <string.h>	/* String function definitions (might not be needed here) */
#include <errno.h>	/* Error number definitions */

/* POSIX Headers */
#include <unistd.h>	/* UNIX standard function definitions */
#include <fcntl.h>	/* File control definitions */
#include <termios.h>	/* POSIX terminal control definitions */

/* Include the corresponding header file (assuming it's named mm_serial.h) */
#include "mm_serial.h"

/**
 * open_serial() - Open and configure a serial port device.
 * @modem_dev:	Path to the serial device (e.g., "/dev/ttyS0", "/dev/tty.usbserial").
 *
 * Opens the specified serial port with read/write access, non-blocking,
 * no controlling terminal, and synchronous writes. It then sets the port
 * back to blocking mode.
 *
 * Return: File descriptor on success, negative errno code on failure.
 */
int open_serial(const char* modem_dev)
{
	int fd;
	int ret;
	int saved_errno;

	/*
	 * Open the serial port device.
	 * O_RDWR: Read/Write access.
	 * O_NOCTTY: Don't make this the controlling terminal.
	 * O_NDELAY: Open in non-blocking mode initially.
	 * O_SYNC: Writes are synchronous (data is physically transmitted).
	 */
	fd = open(modem_dev, O_RDWR | O_NOCTTY | O_NDELAY | O_SYNC);

	if (fd < 0) {
		saved_errno = errno;
		fprintf(stderr, "%s: Failed to open serial port %s: %s\n",
			__func__, modem_dev, strerror(saved_errno));
		return -saved_errno;
	}

	/*
	 * Switch back to blocking mode. fcntl(fd, F_SETFL, 0) clears all
	 * flags set by open (like O_NDELAY) except O_RDWR, O_WRONLY, O_RDONLY.
	 */
	ret = fcntl(fd, F_SETFL, 0);
	if (ret < 0) {
		saved_errno = errno;
		fprintf(stderr, "%s: Failed to set blocking mode for %s: %s\n",
			__func__, modem_dev, strerror(saved_errno));
		close(fd); /* Clean up */
		return -saved_errno;
	}

	return fd; /* Return the file descriptor */
}

/**
 * init_serial() - Initialize serial port settings (baud rate, options).
 * @fd:		File descriptor of the open serial port.
 * @baudrate:	Desired baud rate (e.g., 9600, 19200, 115200).
 * @enable_flow_control: Boolean to enable (true) or disable (false) RTS/CTS hardware flow control.
 *
 * Configures the serial port for raw data transfer (8N1), enables receiver,
 * sets local mode, optionally enables hardware flow control (RTS/CTS),
 * and sets the specified baud rate. Ignores break and parity errors. Sets a read timeout.
 *
 * Return: 0 on success, negative errno code on failure.
 */
int init_serial(int fd, int baudrate, bool enable_flow_control)
{
	struct termios	options;
	speed_t		speed;
	int		ret;
	int		saved_errno;

	/* Get current serial port options */
	ret = tcgetattr(fd, &options);
	if (ret < 0) {
		saved_errno = errno;
		fprintf(stderr, "%s: Failed to get termios attributes: %s\n",
			__func__, strerror(saved_errno));
		return -saved_errno;
	}

	/* Select baud rate */
	switch (baudrate) {
	case 1200:	speed = B1200;	 break;
	case 2400:	speed = B2400;	 break;
	case 4800:	speed = B4800;	 break;
	case 9600:	speed = B9600;	 break;
	case 19200:	speed = B19200;	 break;
	case 38400:	speed = B38400;	 break;
	case 57600:	speed = B57600;	 break;
	case 115200:	speed = B115200; break;
	case 230400:	speed = B230400; break;
		/* Add other baud rates as needed */
	default:
		fprintf(stderr, "%s: Invalid baud rate %d, defaulting to 9600.\n",
			__func__, baudrate);
		speed = B9600; /* Default to a common rate */
		break;
	}

	/* Set input and output speeds */
	ret = cfsetispeed(&options, speed);
	if (ret < 0) {
		saved_errno = errno;
		fprintf(stderr, "%s: Failed to set input speed: %s\n",
			__func__, strerror(saved_errno));
		return -saved_errno;
	}

	ret = cfsetospeed(&options, speed);
	if (ret < 0) {
		saved_errno = errno;
		fprintf(stderr, "%s: Failed to set output speed: %s\n",
			__func__, strerror(saved_errno));
		return -saved_errno;
	}

	/*
	 * Set terminal attributes for raw communication (8N1, flow control).
	 * cfmakeraw() sets flags for raw mode (non-canonical, no echo, etc.)
	 */
	cfmakeraw(&options);

	/* Control Modes (c_cflag) */
	options.c_cflag |= (CLOCAL | CREAD); /* Enable receiver, ignore modem status lines */
	options.c_cflag &= ~CSIZE;	     /* Clear data size bits */
	options.c_cflag |= CS8;		     /* 8 data bits */
	options.c_cflag &= ~PARENB;	     /* No parity */
	options.c_cflag &= ~CSTOPB;	     /* 1 stop bit */
	/* options.c_cflag |= HUPCL; */	     /* Optional: Hang up on last close */
	if (enable_flow_control) {
		options.c_cflag |= CRTSCTS; /* Enable hardware flow control (RTS/CTS) */
	}
	else {
		options.c_cflag &= ~CRTSCTS; /* Disable hardware flow control */
	}


	/* Input Modes (c_iflag) */
	options.c_iflag |= (IGNPAR | IGNBRK); /* Ignore parity errors and break conditions */
	options.c_iflag &= ~(IXON | IXOFF | IXANY); /* Disable software flow control */

	/* Output Modes (c_oflag) - cfmakeraw handles most */
	/* options.c_oflag &= ~OPOST; */ /* Example: Disable output processing if needed */

	/* Local Modes (c_lflag) - cfmakeraw handles most */
	/* options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); */

	/* Special Characters (c_cc) */
	options.c_cc[VMIN] = 0;		/* Read doesn't block */
	options.c_cc[VTIME] = 5;	/* Read timeout in 0.1s units (0.5 seconds) */

	/* Apply the modified options */
	ret = tcsetattr(fd, TCSANOW, &options);
	if (ret < 0) {
		saved_errno = errno;
		fprintf(stderr, "%s: Failed to set termios attributes: %s\n",
			__func__, strerror(saved_errno));
		return -saved_errno;
	}

	/* Flush any pending input/output */
	ret = tcflush(fd, TCIOFLUSH);
	if (ret < 0) {
		saved_errno = errno;
		fprintf(stderr, "%s: Failed to flush terminal: %s\n",
			__func__, strerror(saved_errno));
		/* Non-fatal, but log it */
	}


	return 0; /* Success */
}

/**
 * close_serial() - Close the serial port.
 * @fd:		File descriptor of the open serial port.
 *
 * Return: 0 on success, negative errno code on failure.
 */
int close_serial(int fd)
{
	int ret;

	ret = close(fd);
	if (ret < 0) {
		return -errno;
	}

	return 0;
}

/**
 * read_serial() - Read data from the serial port.
 * @fd:		File descriptor of the open serial port.
 * @buf:	Buffer to store read data.
 * @count:	Maximum number of bytes to read.
 *
 * Return: Number of bytes read on success, or negative errno code on failure.
 * Returns 0 if no data is read before timeout (if VTIME is set).
 */
ssize_t read_serial(int fd, void* buf, size_t count)
{
	ssize_t bytes_read;

	bytes_read = read(fd, buf, count);

	if (bytes_read < 0) {
		return -errno; /* Return negative errno on error */
	}

	return bytes_read; /* Return bytes read (can be 0 on timeout) */
}

/**
 * write_serial() - Write data to the serial port.
 * @fd:		File descriptor of the open serial port.
 * @buf:	Buffer containing data to write.
 * @count:	Number of bytes to write.
 *
 * Return: Number of bytes written on success, or negative errno code on failure.
 */
ssize_t write_serial(int fd, const void* buf, size_t count)
{
	ssize_t bytes_written;

	bytes_written = write(fd, buf, count);

	if (bytes_written < 0) {
		return -errno; /* Return negative errno on error */
	}

	return bytes_written; /* Return bytes written */
}

/**
 * drain_serial() - Wait for all output to be transmitted.
 * @fd:		File descriptor of the open serial port.
 *
 * Blocks until all output written to the object referred to by fd
 * has been transmitted.
 *
 * Return: 0 on success, negative errno code on failure.
 */
int drain_serial(int fd)
{
	int ret;

	ret = tcdrain(fd);
	if (ret < 0) {
		return -errno;
	}

	return 0;
}

/**
 * flush_serial() - Discard pending input and/or output data.
 * @fd:		File descriptor of the open serial port.
 *
 * Discards data written to the object referred to by fd but not transmitted,
 * and data received but not read (TCIOFLUSH).
 *
 * Return: 0 on success, negative errno code on failure.
 */
int flush_serial(int fd)
{
	int ret;

	ret = tcflush(fd, TCIOFLUSH);
	if (ret < 0) {
		return -errno;
	}

	return 0;
}
