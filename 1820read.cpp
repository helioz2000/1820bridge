/**
 * @file 1820read.cpp
 *
 * https://github.com/helioz2000/1820bridge
 *
 * Author: Erwin Bejsta
 * August 2020
 */

/*********************
 *      INCLUDES
 *********************/

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <syslog.h>
#include <sys/utsname.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include <iostream>
#include <string>

#include "dev1820.h"

using namespace std;
//using namespace libconfig;

bool exitSignal = false;
bool runningAsDaemon = false;
std::string processName;
static string execName;
static string ttyDeviceStr = "/dev/ttyUSB0";	// default device
static int ttyBaudrate;							// default baudrate is 9600

Dev1820 *dev;

int getBaudrate(int baud) {
	switch (baud) {
		case 300: return B300;
			break;
		case 1200: return B1200;
			break;
		case 2400: return B2400;
			break;
		default: return B9600;
	}
}

static void showUsage(void) {
	cout << "usage:" << endl;
	cout << execName << " -pSerialDevice -bBaudrate -h" << endl;
//	cout << "a = Address to read from PL device (e.g 50)[0-255]" << endl;
	cout << "s = Serial device (e.g. /dev/ttyUSB0)" << endl;
	cout << "b = Baudrate (e.g. 9600) [300|1200|2400|9600]" << endl;
	cout << "h = Display help" << endl;
	cout << "default device is /etc/ttyUSB0" << endl;
	cout << "default baudrate is 9600" << endl;
//	cout << "default address is 50 (Battery Voltage)" << endl;
}


bool parseArguments(int argc, char *argv[]) {
	char buffer[64];
	int i, buflen;
	int retval = true;
	string str;
	execName = std::string(basename(argv[0]));

	if (argc > 1) {
		for (i = 1; i < argc; i++) {
			strcpy(buffer, argv[i]);
			buflen = strlen(buffer);
			if ((buffer[0] == '-') && (buflen >=2)) {
				switch (buffer[1]) {
//				case 'a':
//					str = std::string(&buffer[2]);
//					address = std::stoi( str );
//					break;
				case 's':
					ttyDeviceStr = std::string(&buffer[2]);
					break;
				case 'b':
					str = std::string(&buffer[2]);
					ttyBaudrate = std::stoi( str );
					break;
				case 'h':
					showUsage();
					retval = false;
					break;
				default:
//					log(LOG_NOTICE, "unknown parameter: %s", argv[i]);
					showUsage();
					retval = false;
					break;
				} // switch
				;
			} // if
		}  // for (i)
	}  // if (argc >1)
	// add config file extension

	return retval;
}


int main (int argc, char *argv[])
{

	if (! parseArguments(argc, argv) ) goto exit_fail;

	dev = new Dev1820(ttyDeviceStr.c_str(), getBaudrate(ttyBaudrate));

	if ( dev->readSingle() < 0 )
		goto exit_fail;

	//printf("Address %d contains %d\n", address, value);
	delete(dev);
	printf("Exit Success\n");
	exit(EXIT_SUCCESS);

exit_fail:
	delete(dev);
	printf("exit with error\n");
	exit(EXIT_FAILURE);
}
