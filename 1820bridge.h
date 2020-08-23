/**
 * @file i2cbridge.h
 *
 */

#ifndef I2CBRIDGE_H
#define I2CBRIDGE_H

//#include <time.h>

struct updatecycle {
	int	ident;
	int interval;	// seconds
	int *tagArray = NULL;
	int tagArraySize = 0;
	time_t nextUpdateTime;			// next update time 
};


#endif /* I2CBRIDGE_H */
