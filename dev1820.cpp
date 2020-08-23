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

#define TTY_TIMEOUT 20		// seconds
//#define TTY_TIMEOUT 0

extern bool exitSignal;

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
	//fprintf(stderr, "%s\n", __func__);
	_tty_close();
}

/**
 * read exactly one temperature value
 * @param value: pointer to read value
 * @param channel: pointer to the channel number
 * @returns 0 if successful, -1 on failure
 */
int Dev1820::readSingle(int *channel, float *value) {
	struct stat sb;
	int result;
	// if serial device is not open ....
	if (fstat(this->_ttyFd, &sb) != 0) {
		// open serial device
		if (_tty_open() < 0)
			return -1;			// failed to open
	}

tryAgain:
	result = _tty_read(channel, value);
	if (result == 0) return 0;
	if (result == -2) goto tryAgain;

	// should never get here
	_tty_close();
	return -1;
}

int Dev1820::_tty_open() {
	this->_ttyFd = open(this->_ttyDevice.c_str(), O_RDONLY | O_NOCTTY | O_SYNC);
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

	// flush input queue
	tcflush(this->_ttyFd, TCIFLUSH);
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

    // setup for non-canonical mode
/*	tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;
	tty.c_cc[VMIN] = 1;		// maximum number of bytes (more than we need)
	tty.c_cc[VTIME] = 0;		// intercharcter timeout in 1/10 sec
*/

	// setup for canonical (line based) mode
	tty.c_lflag |= ICANON | ISIG;  /* canonical input */
	tty.c_lflag &= ~(ECHO | ECHOE | ECHONL | IEXTEN);	// No echo

	tty.c_iflag &= ~IGNCR;  // preserve carriage return /
	tty.c_iflag &= ~INPCK;	// diable parity checking
	tty.c_iflag &= ~(INLCR | ICRNL | IUCLC | IMAXBEL); // no translation
	tty.c_iflag &= ~(IXON | IXOFF | IXANY);	// no SW flowcontrol /

    tty.c_oflag &= ~OPOST;	// diable impementation define processing

    tty.c_cc[VEOL] = 0;		// additional EOL character
    tty.c_cc[VEOL2] = 0;	// additional EOL character
    tty.c_cc[VEOF] = 0x04;	// End of File character


    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        printf("%s: Error from tcsetattr: %s\n", __func__, strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * read one channel from the device
 * @param value: pointer to read value
 * @param channel: pointer to the channel number
 * @returns: zero on success, -1 on failure, -2 for non temp data
 * Note: the device will send startup data which causes a return
 * value of -2.
 */
int Dev1820::_tty_read(int *channel, float *value) {
	int rxlen = 0;
	int rdlen, result;
	char buf[65];
	int timeout = TTY_TIMEOUT;

	fd_set rfds;
	struct timeval tv;
	int select_result;

	FD_ZERO(&rfds);
	FD_SET(this->_ttyFd, &rfds);

	tv.tv_sec = 16;
	tv.tv_usec = 0;

	// break the total timout into 1s chunks
	// and respond to exitSignal
	do {
		FD_ZERO(&rfds);
		FD_SET(this->_ttyFd, &rfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		select_result = select(this->_ttyFd + 1, &rfds, NULL, NULL, &tv);
		//printf("%s: %d\n", __func__, select_result);
		if (select_result != 0) break;
		//printf("%s: timeout %d\n", __func__, timeout);
	} while ( (--timeout > 0) && (!exitSignal) );

	//printf("%s: select: %d\n", __func__, select_result);
	if (exitSignal) return -1;

	if (select_result == -1) {
		fprintf(stderr, "%s: error select()\n", __func__);
		return -1;
	}
	if (select_result == 0) {
		fprintf(stderr, "%s: No data within timeout.\n", __func__);
		return -1;
	}

	// read will return after a LF terminated line was received
	rdlen = read(this->_ttyFd, buf, sizeof(buf) - 1);
	if (rdlen > 0) {
		// mark end of string
		if (buf[rdlen-1] == 0x0A) { // this should be true at all times 
			buf[rdlen-1] = 0;
		} else {
			buf[rdlen] = 0;
		}
		//printf("%s: %d bytes: %s\n", __func__, rdlen, buf);
		//printf("%s\n", buf);
		// temp data always starts with T
		if (buf[0] == 'T') {
			result = sscanf(buf, "T%d %f", channel, value);
			if (result != 2) {
				fprintf(stderr, "%s: sscanf error %d <%s>\n", __func__, result, buf);
				return -1;
			}
		} else {
			return -2;
		}
	} else if (rdlen < 0) {
		fprintf(stderr, "%s: Error from read: %d: %s\n", __func__, rdlen, strerror(errno));
		return -1;
	} else {  /* rdlen == 0 */
		fprintf(stderr, "%s: Timeout from read\n", __func__);
		return -1;
	}
	return 0;
}
