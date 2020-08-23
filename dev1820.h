/**
 * @file dev1820.h
-----------------------------------------------------------------------------
This class encapsulates then interface to the Arduino based
DS18B20 reader built by Ross Wheeler.
-----------------------------------------------------------------------------
*/

#ifndef _DEV1820_H_
#define _DEV1820_H_

/*********************
 *      INCLUDES
 *********************/
#include <stdint.h>
#include <termios.h>
#include <string>

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *      CLASS
 **********************/

class Dev1820 {
public:
	Dev1820();		// empty constructor throws error
	Dev1820(const char* ttyDeviceStr, int baud);
	~Dev1820();
	int readSingle(int *channel, float *value);

private:
	int _tty_open();
	void _tty_close(bool ignoreLock = false);
	int _tty_set_attribs(int fd, int speed);
	int _tty_read(int *channel, float *value);

	std::string _ttyDevice;
	int _ttyBaud;
	int _ttyFd;
};

#endif /* _DEV1820_H_ */
