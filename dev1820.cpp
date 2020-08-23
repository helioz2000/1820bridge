/**
 * @file dev1820.cpp
 *
 * https://github.com/helioz2000/1820bridge
 *
 * Author: Erwin Bejsta
 * August 2020
 */

/*********************
 *      INCLUDES
 *********************/

#include "dev1820.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <stdexcept>
#include <iostream>

using namespace std;

#define TTY_TIMEOUT_S 12
#define TTY_TIMEOUT_US 0

/*********************
 * MEMBER FUNCTIONS
 *********************/

Dev1820::Dev1820() {
	printf("%s\n", __func__);
	throw runtime_error("Class 1820Dev - forbidden constructor");
}

Dev1820::Dev1820(const char* ttyDeviceStr, int baud) {
	if (ttyDeviceStr == NULL) {
		throw invalid_argument("Class 1820Dev - ttyDeviceStr is NULL");
	}
	this->_ttyDevice = ttyDeviceStr;
	this->_ttyBaud = baud;
	this->_ttyFd = -1;
}

Dev1820::~Dev1820() {
	//printf("%s\n", __func__);
	_tty_close();
}

/**
 * read single byte value from RAM address
 * @param address: RAM address of the requested value
 * @param readValue: pointer to a byte which will hold the value
 * @returns 0 if successful, -1 on failure
 */
int Dev1820::readSingle() {
	unsigned char value;
	struct stat sb;

	// if serial device is not open ....
	if (fstat(this->_ttyFd, &sb) != 0) {
		// open serial device
		if (_tty_open() < 0)
			return -1;			// failed to open
	}

	if (_tty_read() < 0)
		goto return_fail;

	return 0;

return_fail:
	_tty_close();
	return -1;
}

int Dev1820::_tty_open() {
	this->_ttyFd = open(this->_ttyDevice.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
	if (_ttyFd < 0) {
		printf("%s: Error opening %s: %s\n", __func__, this->_ttyDevice.c_str(), strerror(errno));
		return -1;
	}
	/*baudrate 8 bits, no parity, 1 stop bit */
	if (_tty_set_attribs(this->_ttyFd, this->_ttyBaud) < 0){
		this->_tty_close();
		return -1;
	}
	if (flock(_ttyFd, LOCK_EX | LOCK_NB) != 0) {	// lock for exclusive access
		printf("%s: error locking %s: %s\n", __func__, this->_ttyDevice.c_str(), strerror(errno));
		_tty_close(true);
		return -1;
	}
	//set_mincount(_tty_Fd, 0);                /* set to pure timed read */

	//printf("%s: OK\n", __func__);
	return 0;
}

void Dev1820::_tty_close(bool ignoreLock) {
	if (this->_ttyFd < 0)
		return;
	if (!ignoreLock) {
		if (flock(_ttyFd, LOCK_UN) != 0) {	// remove file lock
			printf("%s: flock failed [%s]\n", __func__, strerror(errno));
		}
	}
	close(this->_ttyFd);

	this->_ttyFd = -1;
	//printf("%s: Done\n", __func__);
}


int Dev1820::_tty_set_attribs(int fd, int speed)
{
    struct termios tty;

	// read termios attributes
	if (tcgetattr(fd, &tty) < 0) {
        printf("%s: Error from tcgetattr: %s\n", __func__, strerror(errno));
        return -1;
    }

	// set baudrate
    cfsetospeed(&tty, (speed_t)speed);
    cfsetispeed(&tty, (speed_t)speed);

	// set port control flags
    tty.c_cflag |= (CLOCAL | CREAD);    /* ignore modem controls */
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;         /* 8-bit characters */
    tty.c_cflag &= ~PARENB;     /* no parity bit */
    tty.c_cflag &= ~CSTOPB;     /* only need 1 stop bit */
    tty.c_cflag &= ~CRTSCTS;    /* no hardware flowcontrol */

    /* setup for non-canonical mode */
	tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;

	tty.c_cc[VMIN] = 1;		// maximum number of bytes (more than we need)
	tty.c_cc[VTIME] = 0;		// intercharcter timeout in 1/10 sec

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        printf("%s: Error from tcsetattr: %s\n", __func__, strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * read PLxx double byte response
 * @param value: pointer to read value
 */
int Dev1820::_tty_read(void) {
	int rxlen = 0;
	int rdlen;
	unsigned char buf[65];
	char c;

	fd_set rfds;
	struct timeval tv;
	int select_result;

	FD_ZERO(&rfds);
	FD_SET(this->_ttyFd, &rfds);
	tv.tv_sec = TTY_TIMEOUT_S;
	tv.tv_usec = TTY_TIMEOUT_US;

	//printf("%s: select()\n", __func__);
/*
	select_result = select(this->_ttyFd + 1, &rfds, NULL, NULL, &tv);

	if (select_result == -1) {
		fprintf(stderr, "%s: error select()\n", __func__);
		return -1;
	}
	if (!select_result) {
		fprintf(stderr, "%s: No data within timeout.\n", __func__);
		return -1;
	}
*/
	do {
	// read will return after the first byte is received and the specified
	// inter char delay expires. Which is the end of a batch of temps
		rdlen = read(this->_ttyFd, buf, sizeof(buf) - 1);
		if (rdlen > 0) {
				printf("%s: received %d bytes\n", __func__, rdlen);
				printf("%s\n", buf);
				rxlen += rdlen;
/*
				for (rxlen = 0; rxlen < rdlen; rxlen++) {
					c = buf[rxlen];
					if (c == 'T') printf("\n");
					printf ("%d:", rxlen);
					if (c < 31) {
						printf("[%2.0d] ", c);
					} else {
						printf("%c ",c);
					}
				}
				printf("\n");
*/
				//return 0;
		} else if (rdlen < 0) {
			fprintf(stderr, "%s: Error from read: %d: %s\n", __func__, rdlen, strerror(errno));
			return -1;
		} else {  /* rdlen == 0 */
			fprintf(stderr, "%s: Timeout from read\n", __func__);
			return -1;
		}
	} while (rxlen < 64);
	printf("rxlen: %d \n", rxlen);
	return 0;
}
