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
std::string processName;
static string execName;
static string ttyDeviceStr = "/dev/ttyNANOTEMP";// default device
static int ttyBaudrate;							// default baudrate is 9600
int readCount = 10;			// default number of reads

Dev1820 *dev;

/* Handle OS signals
*/
void sigHandler(int signum)
{
	char signame[10];
	switch (signum) {
		case SIGTERM:
			strcpy(signame, "SIGTERM");
			break;
		case SIGHUP:
			strcpy(signame, "SIGHUP");
			break;
		case SIGINT:
			strcpy(signame, "SIGINT");
			break;

		default:
			break;
	}

	fprintf(stderr, "Received %s\n", signame);
	exitSignal = true;
}

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
	cout << execName << " -n10 -pSerialDevice -bBaudrate -h" << endl;
	cout << "c = Number of results to read (default is 10, -1 is endless)" << endl;
	cout << "s = Serial device (e.g. /dev/ttyUSB0)" << endl;
	cout << "b = Baudrate (e.g. 9600) [300|1200|2400|9600]" << endl;
	cout << "h = Display help" << endl;
	cout << "default device is " << ttyDeviceStr << endl;
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
				case 'c':
					str = std::string(&buffer[2]);
					readCount = std::stoi( str );
					break;
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
	int channel;
	float value;

	if (! parseArguments(argc, argv) ) goto exit_fail;

	//signal (SIGTERM, sigHandler);
	//signal (SIGHUP, sigHandler);
	signal (SIGINT, sigHandler);

	dev = new Dev1820(ttyDeviceStr.c_str(), getBaudrate(ttyBaudrate));

	do {
		if ( dev->readSingle(&channel, &value) < 0 ) {
			//goto exit_fail;
		} else {
			printf("CH%02d: %.1f\n", channel, value);
		}
		// endless run for negative values
		if (readCount < 0) readCount = -1;
	} while ( (--readCount != 0) && (!exitSignal) );

	delete(dev);
	//printf("Exit Success\n");
	exit(EXIT_SUCCESS);

exit_fail:
	delete(dev);
	//printf("exit with error\n");
	exit(EXIT_FAILURE);
}
