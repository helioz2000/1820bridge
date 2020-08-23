/**
 * @file 1820bridge.cpp
 *
 * https://github.com/helioz2000/1820bridge
 *
 * Author: Erwin Bejsta
 * August 2020
 *
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
#include <syslog.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <iostream>

#include <libconfig.h++>
#include <mosquitto.h>

#include "1820tag.h"
#include "mqtt.h"
#include "1820bridge.h"
#include "1820dev.h"

using namespace std;
using namespace libconfig;

const char *build_date_str = __DATE__ " " __TIME__;
const int version_major = 1;
const int version_minor = 0;

#define CFG_DEFAULT_FILENAME "1820bridge.cfg"
#define CFG_DEFAULT_FILEPATH "/etc/"

#define MAIN_LOOP_INTERVAL_MINIMUM 50     // milli seconds
#define MAIN_LOOP_INTERVAL_MAXIMUM 2000   // milli seconds

#define MQTT_BROKER_DEFAULT "127.0.0.1"
#define MQTT_CLIENT_ID "1820bridge"
#define MQTT_RECONNECT_INTERVAL 10			// seconds between reconnect attempts

static string cpu_temp_topic = "";
static string cfgFileName;
static string processName;
bool exitSignal = false;
bool debugEnabled = false;
bool runningAsDaemon = false;
char *info_label_text;

bool mqttDebugEnabled = false;
time_t mqtt_connect_time = 0;			// time the connection was initiated
time_t mqtt_next_connect_time = 0;		// time when next connect is scheduled
bool mqtt_connection_in_progress = false;
bool mqtt_retain_default = false;

useconds_t mainloopinterval = 250;	// milli seconds
struct timespec lastAccTime;		// last accumulation run
double accPwr;						// power readout accumulator (reset)
double accPwrChg, accPwrDsc;		// power accumulator (no reset)

updatecycle *updateCycles = NULL;	// array of update cycle definitions
Tag *tags = NULL;			// array of all 1820 tags

int tagCount = -1;
uint32_t i2cTransactionDelay = 0;	// delay between modbus transactions
#define I2C_DEVICEID_MAX 254		// highest permitted I2C device ID
#define I2C_DEVICEID_MIN 1			// lowest permitted I2C device ID

#pragma mark Proto types
void subscribe_tags(void);
void mqtt_connection_status(bool status);
void mqtt_topic_update(const struct mosquitto_message *message);
void mqtt_subscribe_tags(void);
void setMainLoopInterval(int newValue);
bool i2c_read_process();
bool mqtt_publish_tag(Tag *tag);
void mqtt_clear_tags(bool publish_noread, bool clear_retain);

MQTT mqtt(MQTT_CLIENT_ID);
Config cfg;			// config file
//Hardware hw(false);	// no screen

/**
 * log to console and syslog for daemon
 */
template<typename... Args> void log(int priority, const char * f, Args... args) {
	if (runningAsDaemon) {
		syslog(priority, f, args...);
	} else {
		fprintf(stderr, f, args...);
		fprintf(stderr, "\n");
	}
}

/**
 * Handle OS signals
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

	log(LOG_INFO, "Received %s", signame);
	exitSignal = true;
}

void timespec_diff(struct timespec *start, struct timespec *stop, struct timespec *result) {
	if ((stop->tv_nsec - start->tv_nsec) < 0) {
		result->tv_sec = stop->tv_sec - start->tv_sec - 1;
		result->tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
	} else {
		result->tv_sec = stop->tv_sec - start->tv_sec;
	result->tv_nsec = stop->tv_nsec - start->tv_nsec;
	}
	return;
}

void timespec_set(struct timespec *src, struct timespec *dst) {
	dst->tv_nsec = src->tv_nsec;
	dst->tv_sec = src->tv_sec;
}

#pragma mark -- Config File functions

/**
 * Read configuration file.
 * @return true if success
 */
bool readConfig (void)
{
	//int ival;
	// Read the file. If there is an error, report it and exit.

	try
	{
		cfg.readFile(cfgFileName.c_str());
	}
	catch(const FileIOException &fioex)
	{
		std::cerr << "I/O error while reading file <" << cfgFileName << ">." << std::endl;
		return false;
	}
	catch(const ParseException &pex)
	{
		std::cerr << "Parse error at " << pex.getFile() << ":" << pex.getLine()
				<< " - " << pex.getError() << std::endl;
		return false;
	}

	//log (LOG_INFO, "CFG file read OK");
	//std::cerr << cfgFileName << " read OK" <<endl;

	try {
		setMainLoopInterval(cfg.lookup("mainloopinterval"));
	} catch (const SettingNotFoundException &excp) {
	;
	} catch (const SettingTypeException &excp) {
		std::cerr << "Error in config file <" << excp.getPath() << "> is not an integer" << std::endl;
		return false;
	}

	// Read MQTT broker from config
	try {
		mqtt.setBroker(cfg.lookup("mqtt.broker"));
	} catch (const SettingNotFoundException &excp) {
		mqtt.setBroker(MQTT_BROKER_DEFAULT);
	} catch (const SettingTypeException &excp) {
		std::cerr << "Error in config file <" << excp.getPath() << "> is not a string" << std::endl;
		return false;
	}
	return true;
}

/**
 * Get integer value from config file
 */
bool cfg_get_int(const std::string &path, int &value) {
	if (!cfg.lookupValue(path, value)) {
		std::cerr << "Error in config file <" << path << ">" << std::endl;
		return false;
	}
	return true;
}

/**
 * Get string value from config file
 */
bool cfg_get_str(const std::string &path, std::string &value) {
	if (!cfg.lookupValue(path, value)) {
		std::cerr << "Error in config file <" << path << ">" << std::endl;
		return false;
	}
	return true;
}

#pragma mark -- Processing

/**
 * Process all variables
 * @return true if at least one variable was processed
 * Note: the return value from this function is used
 * to measure processing time
 */
bool process() {
	bool retval = false;
	if (mqtt.isConnected()) {
		if (i2c_read_process()) retval = true;
	}
	return retval;
}

/** Process accumulator values
 * called at regulat intervals to integrate accumulator values
 * @param
 * @return true on success
 */
/*bool processAccumulators () {
	int readResult;
	float milliVolts, milliAmps;
	double Wh, W, elapsedseconds;
	struct timespec thistime, elapsedtime;

	// read Voltage and Current
	readResult = vimon.getMilliVolts(0, &milliVolts);
	if (readResult != 0) return false;
	readResult = vimon.getBipolarMilliAmps(&milliAmps);
	if (readResult != 0) return false;
	// calculate time since last call
	clock_gettime(CLOCK_MONOTONIC, &thistime);
	timespec_diff(&lastAccTime, &thistime, &elapsedtime);
	timespec_set(&thistime, &lastAccTime);		// update lassAccTime
	W = (milliVolts * milliAmps) * 1.0e-6;		// Watts
	// tiem since last measurement
	elapsedseconds = (double)elapsedtime.tv_sec + (double)elapsedtime.tv_nsec * 1.0e-9;
	Wh = (W / 3600) * elapsedseconds;
	// add Ws to accumulator
	accPwr += Wh;
	if (Wh > 0.0) {
		accPwrChg += Wh;
	} else {
		accPwrDsc += abs(Wh);
	}
//	printf("%s - %f %f %f\n", __func__, accPwr, accPwrCount, Ws);
	return true;
}
*/

/*
bool init_values(void)
{
	char info1[80], info2[80], info3[80], info4[80];

    // get hardware info
    hw.get_model_name(info1, sizeof(info1));
    hw.get_os_name(info2, sizeof(info2));
    hw.get_kernel_name(info3, sizeof(info3));
    hw.get_ip_address(info4, sizeof(info4));
    info_label_text = (char *)malloc(strlen(info1) +strlen(info2) +strlen(info3) +strlen(info4) +5);
    sprintf(info_label_text, "%s\n%s\n%s\n%s", info1, info2, info3, info4);
    if (!runningAsDaemon) {
	    printf(info_label_text);
        }
    //printf(info_label_text);
    return true;
}
*/

#pragma mark MQTT

/** Initialise the tag database (tagstore)
 * @return false on failure
 */
bool mqtt_init_tags(void) {
	std::string strValue;

	if (!cfg.exists("mqtt_tags")) {	// optional
		log(LOG_NOTICE,"configuration - parameter \"mqtt_tags\" does not exist");
		return false;
		}
	return true;
}

void mqtt_connect(void) {
	if (mqttDebugEnabled)
		printf("%s - attempting to connect to mqtt broker %s.\n", __func__, mqtt.broker());
	mqtt.connect();
	mqtt_connection_in_progress = true;
	mqtt_connect_time = time(NULL);
	mqtt_next_connect_time = 0;
	//printf("%s - Done\n", __func__);
}

/**
 * Initialise the MQTT broker and register callbacks
 */
bool mqtt_init(void) {
	//if (!runningAsDaemon) printf("%s\n", __FUNCTION__);
	bool bValue;
	if (!runningAsDaemon) {
		if (cfg.lookupValue("mqtt.debug", bValue)) {
			mqttDebugEnabled = bValue;
			mqtt.setConsoleLog(mqttDebugEnabled);
			if (mqttDebugEnabled) printf("%s - mqtt debug enabled\n", __func__);
		}
	}
	if (cfg.lookupValue("mqtt.retain_default", bValue))
		mqtt_retain_default = bValue;
	mqtt.registerConnectionCallback(mqtt_connection_status);
	mqtt.registerTopicUpdateCallback(mqtt_topic_update);
	mqtt_connect();
	return true;
}

/**
 * Subscribe tags to MQTT broker
 * Iterate over tag store and process every "subscribe" tag
 */
void mqtt_subscribe_tags(void) {
	//mqtt.unsubscribe("vk2ray/pwr/pl20/batv");

	//printf("%s %s - Start\n", __FILE__, __func__);
/*	Tag* tp = ts.getFirstTag();
	while (tp != NULL) {
		if (tp->isSubscribe()) {
			//printf("%s %s: %s\n", __FILE__, __func__, tp->getTopic());
			mqtt.subscribe(tp->getTopic());
		}
		tp = ts.getNextTag();
	}
	//printf("%s - Done\n", __func__);
*/
}

/**
 * callback function for MQTT
 * MQTT notifies a change in connection status by calling this function
 * This function is registered with MQTT during initialisation
 */
void mqtt_connection_status(bool status) {
	//printf("%s %s - %d\n", __FILE__, __func__, status);
	// subscribe tags when connection is online
	if (status) {
		log(LOG_INFO, "Connected to MQTT broker [%s]", mqtt.broker());
		mqtt_next_connect_time = 0;
		mqtt_connection_in_progress = false;
		mqtt.setRetain(mqtt_retain_default);
		mqtt_subscribe_tags();
	} else {
		if (mqtt_connection_in_progress) {
			mqtt.disconnect();
			// Note: the timeout is determined by OS network stack
			unsigned long timeout = time(NULL) - mqtt_connect_time;
			log(LOG_INFO, "mqtt connection timeout after %lds", timeout);
			mqtt_connection_in_progress = false;
		} else {
			log(LOG_WARNING, "Disconnected from MQTT broker [%s]", mqtt.broker());
		}
		// trigger reconnect unless we are exiting
		if (!exitSignal) {
			mqtt_next_connect_time = time(NULL) + MQTT_RECONNECT_INTERVAL;	// current time
			log(LOG_INFO, "mqtt reconnect scheduled in %d seconds", MQTT_RECONNECT_INTERVAL);
		}
	}
	//printf("%s %s - done\n", __FILE__, __func__);
}

/**
 * callback function for MQTT
 * MQTT notifies when a subscribed topic has received an update
 * this function will udate the corresponding tag
 * @param message: mqtt message
 */
void mqtt_topic_update(const struct mosquitto_message *message) {
//	if(mqttDebugEnabled) {
//		printf("%s %s - %s %s\n", __FILE__, __func__, message->topic, (const char*)message->payload );
//	};
/*	Tag *tp = ts.getTag(message->topic);
	if (tp == NULL) {
		fprintf(stderr, "%s: <%s> not  in ts\n", __func__, message->topic);
	} else {
		tp->setValueIsRetained(message->retain);
		if (message->payload != NULL) {
			tp->setValue((const char*)message->payload);	// This will trigger a callback to modbus_write_request
		}
	}*/
}

/**
 * Publish tag to MQTT
 * @param tag: I2C tag to publish
 *
 */
bool mqtt_publish_tag(Tag *tag) {
	if(mqttDebugEnabled) {
		printf("%s %s - %s\n", __FILE__, __func__, tag->getTopic());
	}
	if (!mqtt.isConnected()) return false;
	if (tag->getTopicString().empty()) return true;	// don't publish if topic is empty
	// Publish value if read was OK
	if (!tag->isNoread()) {
		mqtt.publish(tag->getTopic(), tag->getFormat(), tag->getScaledValue(), tag->getPublishRetain());
		//printf("%s %s - %s \n", __FILE__, __FUNCTION__, tag->getTopic());
		return true;
	}
	//printf("%s - NoRead: %s \n", __FUNCTION__, tag->getTopic());
	// Handle Noread
	if (!tag->noReadIgnoreExceeded()) return true;		// ignore noread, do nothing
	// noreadignore is exceeded, need to take action
	switch (tag->getNoreadAction()) {
	case 0:	// publish null value
		mqtt.clear_retained_message(tag->getTopic());
		break;
	case 1:	// publish noread value
		mqtt.publish(tag->getTopic(), tag->getFormat(), tag->getNoreadValue(), tag->getPublishRetain());
		break;
	default:
		// do nothing (default, -1)
		break;
	}

	return true;
}

/**
 * Publish noread value to all tags (normally done on program exit)
 * @param publish_noread: publish the "noread" value of the tag
 * @param clear_retain: clear retained value from broker's persistance store
 */
void mqtt_clear_tags(bool publish_noread = true, bool clear_retain = true) {

	int index = 0, tagIndex = 0;
	int *tagArray;
	I2Ctag i2cTag;
	//printf("%s %s", __FILE__, __func__);

	// Iterate over all update cycles
	//mqtt.setRetain(false);
	while (updateCycles[index].ident >= 0) {
		// ignore if cycle has no tags to process
		if (updateCycles[index].tagArray == NULL) {
			index++; continue;
		}
		// get array for tags
		tagArray = updateCycles[index].tagArray;
		// read each tag in the array
		tagIndex = 0;
		while (tagArray[tagIndex] >= 0) {
			i2cTag = i2cReadTags[tagArray[tagIndex]];
			if (publish_noread) {}
				mqtt.publish(i2cTag.getTopic(), i2cTag.getFormat(), i2cTag.getNoreadValue(), i2cTag.getPublishRetain());
				//mqtt_publish_tag(mbTag, true);			// publish noread value
			if (clear_retain) {}
				mqtt.clear_retained_message(i2cTag.getTopic());	// clear retained status
			tagIndex++;
		}
		index++;
	}	// while 

}

#pragma mark I2C

/**
 * Read single tag from I2C device
 * @returns: true if successful read
 */
/*bool i2c_read_tag(I2Ctag *tag) {
	uint16_t registers[4];
	bool retVal = true;
	float value;
	int readResult = 0;
	int16_t rawValue;

	uint8_t slaveId = tag->getSlaveId();

	//printf("%s %s - %s\n", __FILE__, __func__, tag->getTopic());

	switch(tag->getAddress()) {
		case 101:
			value = tmp_env.readTempC();
			break;
		case 301:
			value = hw.read_cpu_temp();
			break;
		case 401:		// vimon battery voltage
			readResult = vimon.getMilliVolts(0, &value);
			break;
		case 402:		// vimon battery current
			readResult = vimon.getBipolarMilliAmps(&value);
			break;
		case 403:		// vimon battery temperature
			readResult = vimon.getPT100temp(&value);
			//printf("%s - temp: %.2f\n", __func__, readValue);
			break;
		case 1001:		// Power accumulator
			value = accPwr * 3600;		// convert from Ws to Wh
			//readValue = accPwr * tag->getMultiplier();
			accPwr = 0.0;	// clear accumulator
			break;
		case 1002:		// Power accumulator charge
			value = accPwrChg;
			break;
		case 1003:		// Power accumulator discharge
			value = accPwrDsc;
			break;
		default:
			printf("%s %s - unknown address %d\n", __FILE__, __func__, tag->getAddress());
			retVal = false;
			break;
	}

	if (readResult == 0) {
		tag->setValue(value);
	} else {
		printf("%s %s - read error on tag address %d\n", __FILE__, __func__, tag->getAddress());
	}

	if (retVal) {
		mqtt_publish_tag(tag);
	}
	return retVal;
}
*/

/**
 * process I2C cyclic read update
 * @return false if there was nothing to process, otherwise true
 */
/*bool i2c_read_process() {
	int index = 0;
	int tagIndex = 0;
	int *tagArray;
	bool retval = false;
	time_t now = time(NULL);
	time_t refTime;
	while (updateCycles[index].ident >= 0) {
		// ignore if cycle has no tags to process
		if (updateCycles[index].tagArray == NULL) {
			index++; continue;
		}
		// new reference time for each read cycle
		refTime = time(NULL);		// used for group reads
		if (now >= updateCycles[index].nextUpdateTime) {
			// set next update cycle time
			updateCycles[index].nextUpdateTime = now + updateCycles[index].interval;
			// get array for tags
			tagArray = updateCycles[index].tagArray;
			// read each tag in the array
			tagIndex = 0;
			while (tagArray[tagIndex] >= 0) {
				i2c_read_tag(&i2cReadTags[tagArray[tagIndex]]);
				tagIndex++;
			}
			retval = true;
			//cout << now << " Update Cycle: " << updateCycles[index].ident << " - " << updateCycles[index].tagArraySize << " tags" << endl;
		}
		index++;
	}

	return retval;
}
*/

/**
 * assign tags to update cycles
 * generate arrays of tags assigned ot the same updatecycle
 * 1) iterate over update cycles
 * 2) count tags which refer to update cycle
 * 3) allocate array for those tags
 * 4) fill array with index of tags that match update cycle
 * 5) assign array to update cycle
 * 6) go back to 1) until all update cycles have been matched
 */
bool assign_updatecycles () {
	int updidx = 0;
	int i2cTagIdx = 0;
	int cycleIdent = 0;
	int matchCount = 0;
	int *intArray = NULL;
	int arIndex = 0;
	// iterate over updatecycle array
	while (updateCycles[updidx].ident >= 0) {
		cycleIdent = updateCycles[updidx].ident;
		updateCycles[updidx].tagArray = NULL;
		updateCycles[updidx].tagArraySize = 0;
		// iterate over mbReadTags array
		i2cTagIdx = 0;
		matchCount = 0;
		while (i2cReadTags[i2cTagIdx].updateCycleId() >= 0) {
			// count tags with cycle id match
			if (i2cReadTags[i2cTagIdx].updateCycleId() == cycleIdent) {
				matchCount++;
				//cout << cycleIdent <<" " << mbReadTags[mbTagIdx].getAddress() << endl;
			}
			i2cTagIdx++;
		}
		// skip to next cycle update if we have no matching tags
		if (matchCount < 1) {
			updidx++;
			continue;
		}
		// -- We have some matching tags
		// allocate array for tags in this cycleupdate
		intArray = new int[matchCount+1];			// +1 to allow for end marker
		// fill array with matching tag indexes
		i2cTagIdx = 0;
		arIndex = 0;
		while (i2cReadTags[i2cTagIdx].updateCycleId() >= 0) {
			// count tags with cycle id match
			if (i2cReadTags[i2cTagIdx].updateCycleId() == cycleIdent) {
				intArray[arIndex] = i2cTagIdx;
				arIndex++;
			}
			i2cTagIdx++;
		}
		// mark end of array
		intArray[arIndex] = -1;
		// add the array to the update cycles
		updateCycles[updidx].tagArray = intArray;
		updateCycles[updidx].tagArraySize = arIndex;
		// next update index
		updidx++;
	}
	return true;
}

/**
 * read tag configuration for one I2C device from config file
 */
bool i2c_config_tags(Setting& tagsSettings, uint8_t deviceId) {
	int tagIndex;
	int tagAddress;
	int tagUpdateCycle;
	string strValue;
	float fValue;
	int intValue;
	bool bValue;

	int numTags = tagsSettings.getLength();
	if (numTags < 1) {
		cout << "No tags Found " << endl;
		return true;		// permissible condition
	}

	for (tagIndex = 0; tagIndex < numTags; tagIndex++) {
		if (tagsSettings[tagIndex].lookupValue("address", tagAddress)) {
			i2cReadTags[i2cTagCount].setAddress(tagAddress);
			i2cReadTags[i2cTagCount].setSlaveId(deviceId);
		} else {
			log(LOG_WARNING, "Error in config file, tag address missing");
			continue;		// skip to next tag
		}
		if (i2cTagsSettings[tagIndex].lookupValue("update_cycle", tagUpdateCycle)) {
			i2cReadTags[i2cTagCount].setUpdateCycleId(tagUpdateCycle);
		}
		// is topic present? -> read mqtt related parametrs
		if (tagsSettings[tagIndex].lookupValue("topic", strValue)) {
			i2cReadTags[i2cTagCount].setTopic(strValue.c_str());
			i2cReadTags[i2cTagCount].setPublishRetain(mqtt_retain_default);		// set to default
			if (tagsSettings[tagIndex].lookupValue("retain", bValue))		// override default if required
				i2cReadTags[i2cTagCount].setPublishRetain(bValue);
			if (tagsSettings[tagIndex].lookupValue("format", strValue))
				i2cReadTags[i2cTagCount].setFormat(strValue.c_str());
			if (tagsSettings[tagIndex].lookupValue("multiplier", fValue))
				i2cReadTags[i2cTagCount].setMultiplier(fValue);
			if (tagsSettings[tagIndex].lookupValue("offset", fValue))
				i2cReadTags[i2cTagCount].setOffset(fValue);
			if (tagsSettings[tagIndex].lookupValue("noreadvalue", fValue))
				i2cReadTags[i2cTagCount].setNoreadValue(fValue);
			if (tagsSettings[tagIndex].lookupValue("noreadaction", intValue))
				i2cReadTags[i2cTagCount].setNoreadAction(intValue);
			if (tagsSettings[tagIndex].lookupValue("noreadignore", intValue))
				i2cReadTags[i2cTagCount].setNoreadIgnore(intValue);
		}
		cout << "Tag " << i2cTagCount << " addr: " << tagAddress << " cycle: " << tagUpdateCycle;
		cout << " Topic: " << i2cReadTags[i2cTagCount].getTopicString() << endl;
		i2cTagCount++;
	}
	return true;
}

/**
 * read device configuration from config file
 */

bool i2c_config_devices(Setting& i2cDeviceSettings) {
	int deviceId, numTags;
	string deviceName;
	bool deviceEnabled;

	// we need at least one slave in config file
	int numDevices = i2cDeviceSettings.getLength();
	if (numDevices < 1) {
		log(LOG_ERR, "Error in config file, no Modbus slaves found");
		return false;
	}

	// calculate the total number of tags for all configured slaves
	numTags = 0;
	for (int deviceIdx = 0; deviceIdx < numDevices; deviceIdx++) {
		if (i2cDeviceSettings[deviceIdx].exists("tags")) {
			if (!i2cDeviceSettings[deviceIdx].lookupValue("enabled", deviceEnabled)) {
				deviceEnabled = true;	// true is assumed if there is no entry in config file
			}
			if (deviceEnabled) {
				Setting& i2cTagsSettings = i2cDeviceSettings[deviceIdx].lookup("tags");
				numTags += i2cTagsSettings.getLength();
			}
		}
	}

	i2cReadTags = new I2Ctag[numTags+1];

	i2cTagCount = 0;
	// iterate through devices
	for (int deviceIdx = 0; deviceIdx < numDevices; deviceIdx++) {
		i2cDeviceSettings[deviceIdx].lookupValue("name", deviceName);
		if (i2cDeviceSettings[deviceIdx].lookupValue("id", deviceId)) {
			if (i2cDebugLevel > 0)
				printf("%s - processing Device %d (%s)\n", __func__, deviceId, deviceName.c_str());
		} else {
			log(LOG_ERR, "Config error - PL device ID missing in entry %d", deviceId+1);
			return false;
		}

		// get list of tags
		if (i2cDeviceSettings[deviceIdx].exists("tags")) {
			if (!i2cDeviceSettings[deviceIdx].lookupValue("enabled", deviceEnabled)) {
				deviceEnabled = true;	// true is assumed if there is no entry in config file
			}
			if (deviceEnabled) {
				Setting& i2cTagsSettings = i2cDeviceSettings[deviceIdx].lookup("tags");
				if (!i2c_config_tags(i2cTagsSettings, deviceId)) {
					return false; }
			} else {
				log(LOG_NOTICE, "I2C device %d (%s) disabled in config", deviceId, deviceName.c_str());
			}
		} else {
			log(LOG_NOTICE, "No tags defined for I2C device %d", deviceId);
			// this is a permissible condition
		}
	}
	// mark end of array
	i2cReadTags[i2cTagCount].setUpdateCycleId(-1);
	i2cReadTags[i2cTagCount].setSlaveId(I2C_DEVICEID_MAX +1);
	return true;
}

/**
 * read update cycles from config file
 */
bool i2c_config_updatecycles(Setting& updateCyclesSettings) {
	int idValue, interval, index;
	int numUpdateCycles = updateCyclesSettings.getLength();

	if (numUpdateCycles < 1) {
		log(LOG_ERR, "Error in config file, \"updatecycles\" missing");
		return false;
	}

	// allocate array
	updateCycles = new updatecycle[numUpdateCycles+1];

	for (index = 0; index < numUpdateCycles; index++) {
		if (updateCyclesSettings[index].lookupValue("id", idValue)) {
		} else {
			log(LOG_ERR, "Config error - cycleupdate ID missing in entry %d", index+1);
			return false;
		}
		if (updateCyclesSettings[index].lookupValue("interval", interval)) {
		} else {
			log(LOG_ERR, "Config error - cycleupdate interval missing in entry %d", index+1);
			return false;
		}
		updateCycles[index].ident = idValue;
		updateCycles[index].interval = interval;
		updateCycles[index].nextUpdateTime = time(0) + interval;
		//cout << "Update " << index << " ID " << idValue << " Interval: " << interval << " t:" << updateCycles[index].nextUpdateTime << endl;
	}
	// mark end of data
	updateCycles[index].ident = -1;
	updateCycles[index].interval = -1;

	return true;
}


/**
 * read I2C configuration from config file
 */
bool i2c_config() {
	// Configure update cycles
	try {
		Setting& updateCyclesSettings = cfg.lookup("updatecycles");
		if (!i2c_config_updatecycles(updateCyclesSettings)) {
			return false; }
	} catch (const SettingNotFoundException &excp) {
		log(LOG_ERR, "Error in config file <%s> not found", excp.getPath());
		return false;
	} catch (const SettingTypeException &excp) {
		log(LOG_ERR, "Error in config file <%s> is wrong type", excp.getPath());
		return false;
	}


	// Configure i2c devices
	try {
		Setting& i2cDeviceSettings = cfg.lookup("i2cdevices");
		if (!i2c_config_devices(i2cDeviceSettings)) {
			return false; }
	} catch (const SettingNotFoundException &excp) {
		log(LOG_ERR, "Error in config file <%s> not found", excp.getPath());
		return false;
	} catch (const SettingTypeException &excp) {
		log(LOG_ERR, "Error in config file <%s> is not a string", excp.getPath());
		return false;
	} catch (const ParseException &excp) {
		log(LOG_ERR, "Error in config file - Parse Exception");
		return false;
	} catch (...) {
		log(LOG_ERR, "pl_config <pldevices> Error in config file (exception)");
		return false;
	}
	return true;
}


/**
 * initialize I2C interface devices
 * @returns false for configuration error, otherwise true
 */
bool i2c_init() {
    // sequence is important, the I2C setup also calls
    // WiringPiSetupSys() which is required for pin IO functions

    // initialize environment temp sensor
//	tmp_env.initialize();

    // initialize rack temp sensor
    //tmp_rack.initialize();

    // Power Management board ADC's
/*    pwr_adc1.initialize();
    if (!pwr_adc1.testConnection()) {
        if (!runAsDaemon) {
            printf("ADS1115 #1 on power management board not found \n");
            // exit(0);
        }
    }
    pwr_adc1.setGain(ADS1115_PGA_4P096);
    if (!runAsDaemon) {
        pwr_adc1.showConfigRegister();
    }
    pwr_adc2.initialize();
    if (!pwr_adc2.testConnection()) {
        if (!runAsDaemon) {
            printf("ADS1115 #2 on power management board not found \n");
            // exit(0);
        }
    }
    pwr_adc2.setGain(ADS1115_PGA_4P096);
    if (!runAsDaemon) {
        pwr_adc2.showConfigRegister();
	} */

	// VI-Monitor board
	if (!vimon.initialize( ADS1115_ADDRESS_ADDR_SDA )) {
		return false;
	}

	if (!i2c_config()) return false;
	if (!assign_updatecycles()) return false;
	return true;
}

#pragma mark Loops

/**
 * set main loop interval to a valid setting
 * @param newValue the new main loop interval in ms
 */
void setMainLoopInterval(int newValue)
{
	int val = newValue;
	if (newValue < MAIN_LOOP_INTERVAL_MINIMUM) {
		val = MAIN_LOOP_INTERVAL_MINIMUM;
	}
	if (newValue > MAIN_LOOP_INTERVAL_MAXIMUM) {
		val = MAIN_LOOP_INTERVAL_MAXIMUM;
	}
	mainloopinterval = val;

	log(LOG_INFO, "Main Loop interval is %dms", mainloopinterval);
}

/**
 * called on program exit
 */
void exit_loop(void) 
{
	bool bValue, clearonexit = false, noreadonexit = false;

	// how to handle mqtt broker published tags 
	// clear retain status for all tags?
	if (cfg.lookupValue("mqtt.clearonexit", bValue))
		clearonexit = bValue;
	// publish noread value for all tags?
	if (cfg.lookupValue("mqtt.noreadonexit", bValue)) 
		noreadonexit = bValue;
	if (noreadonexit || clearonexit)
		mqtt_clear_tags(noreadonexit, clearonexit);
	// free allocated memory
	// arrays of tags in cycleupdates
	int *ar, idx=0;
	while (updateCycles[idx].ident >= 0) {
		ar = updateCycles[idx].tagArray;
		if (ar != NULL) delete [] ar;		// delete array if one exists
		idx++;
	}

	delete [] updateCycles;
}

/**
 * Main program loop
 */
void main_loop()
{
	bool processing_success = false;
	//clock_t start, end;
	struct timespec starttime, endtime, difftime;
	useconds_t sleep_usec;
	//double delta_time;
	useconds_t processing_time;
	useconds_t min_time = 99999999, max_time = 0;
	useconds_t interval = mainloopinterval * 1000;	// convert ms to us

	// intiate accumulator timing
	clock_gettime(CLOCK_MONOTONIC, &lastAccTime);
	// reset accumulator values
	accPwr = 0;
	accPwrChg = 0; accPwrDsc = 0;

	// first call takes a long time (10ms)
	while (!exitSignal) {
	// run processing and record start/stop time
		clock_gettime(CLOCK_MONOTONIC, &starttime);
		processing_success = process();
		clock_gettime(CLOCK_MONOTONIC, &endtime);
		// calculate cpu time used [us]
		timespec_diff(&starttime, &endtime, &difftime);
		processing_time = (difftime.tv_nsec / 1000) + (difftime.tv_sec * 1000000);

		// store min/max times if any processing was done
		if (processing_success) {
			// calculate cpu time used [us]
			if (debugEnabled)
				printf("%s - process() took %dus\n", __func__, processing_time);
			if (processing_time > max_time) {
				max_time = processing_time;
			}
			if (processing_time < min_time) {
				min_time = processing_time;
			}
			//printf("%s - success (%dus)\n", __func__, processing_time);
		}
		// enter loop delay if needed
		// if cpu_time_used exceeds the mainLoopInterval
		// then bypass the loop delay
		if (interval > processing_time) {
			sleep_usec = interval - processing_time;  // sleep time in us
			//printf("%s - sleeping for %dus (%dus)\n", __func__, sleep_usec, processing_time);
			usleep(sleep_usec);
		}

		if (mqtt_next_connect_time > 0) {
			if (time(NULL) >= mqtt_next_connect_time) {
				mqtt_connect();
			}
		}
	}
	if (!runningAsDaemon)
		printf("CPU time for variable processing: %dus - %dus\n", min_time, max_time);
}

/** Display program usage instructions.
 * @param
 * @return
 */
static void showUsage(void) {
	cout << "usage:" << endl;
	cout << processName << "-cCfgFileName -d -h" << endl;
	cout << "c = name of config file" << endl;
	cout << "d = enable debug mode" << endl;
	cout << "h = show help" << endl;
}

/** Parse command line arguments.
 * @param argc argument count
 * @param argv array of arguments
 * @return false to indicate program needs to abort
 */
bool parseArguments(int argc, char *argv[]) {
	char buffer[64];
	int i, buflen;
	int retval = true;


	if (runningAsDaemon) {
		cfgFileName = std::string(CFG_DEFAULT_FILEPATH) + std::string(CFG_DEFAULT_FILENAME);
	} else {
		cfgFileName = std::string(CFG_DEFAULT_FILENAME);
	}

	if (argc > 1) {
		for (i = 1; i < argc; i++) {
			strcpy(buffer, argv[i]);
			buflen = strlen(buffer);
			if ((buffer[0] == '-') && (buflen >=2)) {
				switch (buffer[1]) {
				case 'c':
					cfgFileName = std::string(&buffer[2]);
					break;
				case 'd':
					debugEnabled = true;
					printf("Debug enabled\n");
					break;
				case 'h':
					showUsage();
					retval = false;
					break;
				default:
					log(LOG_NOTICE, "unknown parameter: %s", argv[i]);
					showUsage();
					retval = false;
					break;
				} // switch
				;
			} // if
		}  // for (i)
	}  // if (argc >1)
	return retval;
}

int main (int argc, char *argv[])
{
	if ( getppid() == 1) runningAsDaemon = true;

	processName = std::string(basename(argv[0]));

	if (! parseArguments(argc, argv) ) goto exit_fail;

	log(LOG_INFO,"[%s] PID: %d PPID: %d", argv[0], getpid(), getppid());
	log(LOG_INFO,"Version %d.%02d [%s] ", version_major, version_minor, build_date_str);

	// catch SIGTERM only if running as daemon (started via systemctl)
	// when run from command line SIGTERM provides a last resort method
	// of killing the process regardless of any programming errors.
	if (runningAsDaemon) {
		signal (SIGTERM, sigHandler);
	}

	// read config file
	if (! readConfig()) {
		log(LOG_ERR, "Error reading config file <%s>", cfgFileName.c_str());
		goto exit_fail;
	}

	if (!mqtt_init()) goto exit_fail;
	if (!init_values()) goto exit_fail;
//	if (!i2c_init()) goto exit_fail;
	usleep(100000);
	main_loop();

	exit_loop();
	log(LOG_INFO, "exiting");
	exit(EXIT_SUCCESS);

exit_fail:
	log(LOG_INFO, "exit with error");
	exit(EXIT_FAILURE);
}
