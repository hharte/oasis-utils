/*
 * mm_serial_win32.c - WIN32 serial port library implementation
 *
 * Part of mm_manager.
 *
 * www.github.com/hharte/mm_manager
 * www.github.com/hharte/oasis-utils
 * Copyright (c) 2020-2025, Howard M. Harte
 * SPDX-License-Identifier: MIT
 */

 /* Windows Headers */
#include <windows.h>

/* Standard C Headers */
#include <stdio.h>	/* For fprintf */
#include <stdlib.h>	/* Standard library definitions */
#include <stdint.h>	/* Standard integer types */
#include <string.h>	/* String function definitions */
#include <errno.h>	/* POSIX errno constants */

/* Include the corresponding header file (assuming it's named mm_serial.h) */
/* This header should handle the ssize_t definition for _MSC_VER */
#include "mm_serial.h"

/* Define maximum concurrent serial connections */
#define MAX_SERIAL_FDS 32 /* Increased size */

/* Global table to map integer file descriptors to Windows HANDLES */
/* FIXME: This global table is not ideal (thread safety, fixed size) */
static HANDLE hHandleTable[MAX_SERIAL_FDS] = { NULL }; /* Initialize to NULL */

/**
 * map_win_error_to_errno() - Map common Windows API errors to POSIX errno values.
 * @win_error:	Windows error code obtained from GetLastError().
 *
 * Return: Corresponding negative POSIX errno value, or -EIO for unmapped errors.
 */
static int map_win_error_to_errno(DWORD win_error)
{
	switch (win_error) {
	case ERROR_SUCCESS:		 return 0; /* Should not happen if API failed */
	case ERROR_FILE_NOT_FOUND:	 return -ENOENT;
	case ERROR_PATH_NOT_FOUND:	 return -ENOENT;
	case ERROR_ACCESS_DENIED:	 return -EACCES;
	case ERROR_INVALID_HANDLE:	 return -EBADF;
	case ERROR_SHARING_VIOLATION:	 return -EBUSY; /* Or -EACCES */
	case ERROR_INVALID_PARAMETER:	 return -EINVAL;
	case ERROR_SEM_TIMEOUT:		 return -ETIMEDOUT;
	case ERROR_OPERATION_ABORTED:	 return -EINTR; /* Or a custom error */
	case ERROR_HANDLE_EOF:		 return 0; /* EOF is not an error for read */
		/* Add more mappings as needed */
	default:			 return -EIO; /* Generic I/O error */
	}
}

/**
 * open_serial() - Open and configure a serial port device on Windows.
 * @modem_dev:	Path to the serial device (e.g., "COM1", "\\\\.\\COM10").
 *
 * Opens the specified serial port using CreateFileA. Finds an empty slot
 * in the global handle table to store the handle and returns the index
 * as the file descriptor.
 *
 * Return: File descriptor (index) on success, negative errno code on failure.
 */
int open_serial(const char* modem_dev)
{
	HANDLE hComm;
	int fd = -1;
	int i;
	DWORD last_error;

	/* Find an empty slot in the handle table */
	for (i = 0; i < MAX_SERIAL_FDS; i++) {
		if (hHandleTable[i] == NULL) {
			fd = i;
			break;
		}
	}

	if (fd == -1) {
		fprintf(stderr, "%s: Too many open serial ports (max %d).\n",
			__func__, MAX_SERIAL_FDS);
		return -EMFILE; /* Too many open files */
	}

	/*
	 * Open the serial port device using CreateFileA.
	 * GENERIC_READ | GENERIC_WRITE: Read/Write access.
	 * 0: No sharing.
	 * NULL: Default security attributes.
	 * OPEN_EXISTING: Opens a file or device, only if it exists.
	 * 0: Default attributes (no FILE_FLAG_OVERLAPPED for sync IO).
	 * NULL: No template file.
	 */
	hComm = CreateFileA(modem_dev,
		GENERIC_READ | GENERIC_WRITE,
		0,
		NULL,
		OPEN_EXISTING,
		0, /* FILE_ATTRIBUTE_NORMAL? */
		NULL);

	if (hComm == INVALID_HANDLE_VALUE) {
		last_error = GetLastError();
		fprintf(stderr, "%s: Failed to open serial port %s: Error %lu\n",
			__func__, modem_dev, last_error);
		return map_win_error_to_errno(last_error);
	}

	/* Store the handle and return the file descriptor index */
	hHandleTable[fd] = hComm;

	return fd;
}

/**
 * init_serial() - Initialize Windows serial port settings.
 * @fd:		File descriptor (index into hHandleTable).
 * @baudrate:	Desired baud rate (e.g., 9600, 19200, 115200).
 * @enable_flow_control: Boolean to enable (true) or disable (false) RTS/CTS hardware flow control.
 *
 * Configures the serial port DCB (Device Control Block) for 8N1,
 * hardware flow control (optional, based on DCB flags), and sets
 * communication timeouts.
 *
 * Return: 0 on success, negative errno code on failure.
 */
int init_serial(int fd, int baudrate, bool enable_flow_control)
{
	HANDLE hComm;
	DCB dcbSerialParams = { 0 };
	COMMTIMEOUTS timeouts = { 0 };
	DWORD last_error;

	/* Validate fd and get handle */
	if (fd < 0 || fd >= MAX_SERIAL_FDS || hHandleTable[fd] == NULL) {
		return -EBADF; /* Bad file descriptor */
	}
	hComm = hHandleTable[fd];

	/* Initialize DCB structure */
	dcbSerialParams.DCBlength = sizeof(dcbSerialParams);

	if (!GetCommState(hComm, &dcbSerialParams)) {
		last_error = GetLastError();
		fprintf(stderr, "%s: Failed to get comm state: Error %lu\n",
			__func__, last_error);
		return map_win_error_to_errno(last_error);
	}

	/* Set baud rate */
	switch (baudrate) {
	case 1200:
	case 2400:
	case 4800:
	case 9600:
	case 19200:
	case 38400:
	case 57600:
	case 115200:
	case 230400:
		dcbSerialParams.BaudRate = baudrate;
		break;
	default:
		fprintf(stderr, "%s: Invalid baud rate %d, defaulting to 9600.\n",
			__func__, baudrate);
		dcbSerialParams.BaudRate = CBR_9600; /* Default */
		break;
	}

	/* Configure for 8N1, hardware flow control */
	dcbSerialParams.ByteSize = 8;		/* 8 data bits */
	dcbSerialParams.StopBits = ONESTOPBIT;	/* 1 stop bit */
	dcbSerialParams.Parity = NOPARITY;	/* No parity */

	/* Hardware Flow Control Settings */
	if (enable_flow_control) {
		dcbSerialParams.fOutxCtsFlow = TRUE;	/* Enable CTS output flow control */
		dcbSerialParams.fRtsControl = RTS_CONTROL_HANDSHAKE; /* Enable RTS handshaking */
	}
	else {
		dcbSerialParams.fOutxCtsFlow = FALSE; /* Disable CTS output flow control */
		dcbSerialParams.fRtsControl = RTS_CONTROL_DISABLE; /* Disable RTS */
	}
	/* Disable software flow control */
	dcbSerialParams.fOutX = FALSE;
	dcbSerialParams.fInX = FALSE;
	/* Other potentially useful flags */
	dcbSerialParams.fBinary = TRUE;		/* Must be TRUE for Win32 */
	dcbSerialParams.fParity = FALSE;	/* Disable parity checking */
	dcbSerialParams.fNull = FALSE;		/* Disable null stripping */
	dcbSerialParams.fAbortOnError = FALSE;	/* Don't abort reads/writes on error */

	/* Set the new DCB structure */
	if (!SetCommState(hComm, &dcbSerialParams)) {
		last_error = GetLastError();
		fprintf(stderr, "%s: Failed to set comm state: Error %lu\n",
			__func__, last_error);
		return map_win_error_to_errno(last_error);
	}

	/* Set communication timeouts */
	/* Read timeouts */
	timeouts.ReadIntervalTimeout = 50;	/* Max time between chars (ms) */
	timeouts.ReadTotalTimeoutMultiplier = 10;	/* Multiplier per byte (ms) */
	timeouts.ReadTotalTimeoutConstant = 500;	/* Constant timeout (ms) - 0.5 sec */
	/* Write timeouts */
	timeouts.WriteTotalTimeoutMultiplier = 10;	/* Multiplier per byte (ms) */
	timeouts.WriteTotalTimeoutConstant = 500;	/* Constant timeout (ms) */

	if (!SetCommTimeouts(hComm, &timeouts)) {
		last_error = GetLastError();
		fprintf(stderr, "%s: Failed to set comm timeouts: Error %lu\n",
			__func__, last_error);
		return map_win_error_to_errno(last_error);
	}

	/* Flush buffers after configuration */
	if (!PurgeComm(hComm, PURGE_RXCLEAR | PURGE_TXCLEAR)) {
		last_error = GetLastError();
		fprintf(stderr, "%s: Failed to purge comm buffers: Error %lu\n",
			__func__, last_error);
		/* Non-fatal, but log it */
	}

	return 0; /* Success */
}

/**
 * close_serial() - Close the Windows serial port handle.
 * @fd:		File descriptor (index into hHandleTable).
 *
 * Return: 0 on success, negative errno code on failure.
 */
int close_serial(int fd)
{
	HANDLE hComm;
	DWORD last_error;

	/* Validate fd and get handle */
	if (fd < 0 || fd >= MAX_SERIAL_FDS || hHandleTable[fd] == NULL) {
		return -EBADF; /* Bad file descriptor */
	}
	hComm = hHandleTable[fd];

	/* Clear the entry in the table *before* closing */
	hHandleTable[fd] = NULL;

	if (!CloseHandle(hComm)) {
		last_error = GetLastError();
		fprintf(stderr, "%s: Failed to close serial handle (fd=%d): Error %lu\n",
			__func__, fd, last_error);
		/* Handle was already cleared, but report error */
		return map_win_error_to_errno(last_error);
	}

	return 0; /* Success */
}

/**
 * read_serial() - Read data from the Windows serial port.
 * @fd:		File descriptor (index into hHandleTable).
 * @buf:	Buffer to store read data.
 * @count:	Maximum number of bytes to read.
 *
 * Return: Number of bytes read on success (can be 0 on timeout),
 * or negative errno code on failure.
 */
ssize_t read_serial(int fd, void* buf, size_t count)
{
	HANDLE hComm;
	DWORD bytes_read = 0;
	DWORD dw_count;
	BOOL status;
	DWORD last_error;

	/* Validate fd and get handle */
	if (fd < 0 || fd >= MAX_SERIAL_FDS || hHandleTable[fd] == NULL) {
		return -EBADF; /* Bad file descriptor */
	}
	hComm = hHandleTable[fd];

	/* Check if count exceeds DWORD maximum */
	if (count > UINT32_MAX) {
		fprintf(stderr, "%s: Read count %zu exceeds DWORD_MAX.\n", __func__, count);
		return -EINVAL;
	}
	dw_count = (DWORD)count;

	/* Read from the serial port */
	status = ReadFile(hComm, buf, dw_count, &bytes_read, NULL);

	if (!status) {
		last_error = GetLastError();
		/* EOF is not an error in POSIX read, handle it here? */
		/* ReadFile returns FALSE on timeout if timeouts are set */
		if (last_error == ERROR_OPERATION_ABORTED || last_error == ERROR_SEM_TIMEOUT) {
			/* Treat timeout as 0 bytes read, not an error */
			return 0;
		}
		fprintf(stderr, "%s: ReadFile failed: Error %lu\n", __func__, last_error);
		return map_win_error_to_errno(last_error);
	}

	/* Return number of bytes actually read */
	return (ssize_t)bytes_read;
}

/**
 * write_serial() - Write data to the Windows serial port.
 * @fd:		File descriptor (index into hHandleTable).
 * @buf:	Buffer containing data to write.
 * @count:	Number of bytes to write.
 *
 * Return: Number of bytes written on success, or negative errno code on failure.
 */
ssize_t write_serial(int fd, const void* buf, size_t count)
{
	HANDLE hComm;
	DWORD bytes_written = 0;
	DWORD dw_count;
	BOOL status;
	DWORD last_error;

	/* Validate fd and get handle */
	if (fd < 0 || fd >= MAX_SERIAL_FDS || hHandleTable[fd] == NULL) {
		return -EBADF; /* Bad file descriptor */
	}
	hComm = hHandleTable[fd];

	/* Check if count exceeds DWORD maximum */
	if (count > UINT32_MAX) {
		fprintf(stderr, "%s: Write count %zu exceeds DWORD_MAX.\n", __func__, count);
		return -EINVAL;
	}
	dw_count = (DWORD)count;

	/* Write to the serial port */
	status = WriteFile(hComm, buf, dw_count, &bytes_written, NULL);

	if (!status) {
		last_error = GetLastError();
		fprintf(stderr, "%s: WriteFile failed: Error %lu\n", __func__, last_error);
		return map_win_error_to_errno(last_error);
	}

	/* Sanity check: Did Windows write fewer bytes than requested without error? */
	if (bytes_written != dw_count) {
/*		fprintf(stderr, "%s: WriteFile wrote %lu bytes instead of %lu.\n",
			__func__, bytes_written, dw_count); */
	}

	/* Return number of bytes actually written */
	return (ssize_t)bytes_written;
}

/**
 * drain_serial() - Wait for Windows serial output buffer to empty.
 * @fd:		File descriptor (index into hHandleTable).
 *
 * Uses FlushFileBuffers which waits for write operations to complete.
 * Note: This might behave differently from tcdrain on POSIX.
 *
 * Return: 0 on success, negative errno code on failure.
 */
int drain_serial(int fd)
{
	HANDLE hComm;
	DWORD last_error;

	/* Validate fd and get handle */
	if (fd < 0 || fd >= MAX_SERIAL_FDS || hHandleTable[fd] == NULL) {
		return -EBADF; /* Bad file descriptor */
	}
	hComm = hHandleTable[fd];

	/* FlushFileBuffers waits for writes to complete */
	if (!FlushFileBuffers(hComm)) {
		last_error = GetLastError();
		fprintf(stderr, "%s: FlushFileBuffers failed: Error %lu\n", __func__, last_error);
		return map_win_error_to_errno(last_error);
	}

	return 0; /* Success */
}

/**
 * flush_serial() - Discard Windows serial port input/output buffers.
 * @fd:		File descriptor (index into hHandleTable).
 *
 * Uses PurgeComm to clear receive and transmit buffers.
 *
 * Return: 0 on success, negative errno code on failure.
 */
int flush_serial(int fd)
{
	HANDLE hComm;
	DWORD last_error;

	/* Validate fd and get handle */
	if (fd < 0 || fd >= MAX_SERIAL_FDS || hHandleTable[fd] == NULL) {
		return -EBADF; /* Bad file descriptor */
	}
	hComm = hHandleTable[fd];

	/* Purge both RX and TX buffers */
	if (!PurgeComm(hComm, PURGE_RXCLEAR | PURGE_TXCLEAR)) {
		last_error = GetLastError();
		fprintf(stderr, "%s: PurgeComm failed: Error %lu\n", __func__, last_error);
		return map_win_error_to_errno(last_error);
	}

	return 0; /* Success */
}
