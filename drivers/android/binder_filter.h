/*
 * Add-ons to binder
 * David Wu
 *
 *
*/

#ifndef _LINUX_BINDER_FILTER_H
#define _LINUX_BINDER_FILTER_H

#include <linux/uidgid.h>

enum {
    BLOCK_ACTION = 1,
    UNBLOCK_ACTION = 2,
    MODIFY_ACTION = 3,
    UNMODIFY_ACTION = 4,
};

enum {
	CONTEXT_NONE = 0,
	CONTEXT_WIFI_STATE = 1,
	CONTEXT_WIFI_SSID = 2,
	CONTEXT_WIFI_NEARBY = 3,
	CONTEXT_BT_STATE = 4,
	CONTEXT_BT_CONNECTED_DEVICE = 5,
	CONTEXT_BT_NEARBY_DEVICE = 6,
	CONTEXT_LOCATION = 7,
	CONTEXT_APP_INSTALLED = 8,
	CONTEXT_APP_RUNNING = 9,
	CONTEXT_DATE_DAY = 10,
};

enum {
	CONTEXT_STATE_ON = 1,
	CONTEXT_STATE_OFF = 2,
	CONTEXT_STATE_UNKNOWN = 3,
};

enum {
	CONTEXT_TYPE_INT = 1,
    CONTEXT_TYPE_STRING = 2,
};

struct bf_filter_param {
	unsigned long addr;
	size_t size; 
	int reply; 
	int euid;
	void* offsets;
	size_t offsets_size;
};

// what the user passes in
struct bf_user_filter {
	int action;
    int uid;
    char* message;
    char* data;

    int context;
    int context_type;
    int context_int_value;
    char* context_string_value;
};

// what we use in the kernel
struct bf_filter_rule {
	int uid;
	char* message;
	int block_or_modify;
	char* data;

    int context;
    int context_type;
    int context_int_value;
    char* context_string_value;

	struct bf_filter_rule* next;
};

struct bf_filters {
	int num_filters;
	struct bf_filter_rule* filters_list_head;
};


struct bf_battery_level_struct {
	int level_value_no_BT;
	int level_value_with_BT;
};

struct app_context_entry {
	char* package_name;
	int state;
	struct app_context_entry* next;
};

/* current values for different enviornment or sensor data */
struct bf_context_values_struct {
	int bluetooth_enabled;
	int wifi_enabled;
	char wifi_ssid[33]; 			
	char gps[3];		// there are a lot of problems reducing doubles to 3 bytes... but it's what we're doing for now
	struct app_context_entry* app_context_queue;		// list of apps to check running state of
};

#endif /* _LINUX_BINDER_FILTER_H */
