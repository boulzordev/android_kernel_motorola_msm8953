/*
 * Add-ons to binder
 * David Wu
 *
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/pid.h>
#include <linux/syscalls.h>
#include <linux/reboot.h>
#include <linux/file.h>

#include "binder_filter.h"
#include "binder.h"

#define MAX_BUFFER_SIZE 500
#define BUFFER_LOG_SIZE 64
#define LARGE_BUFFER_SIZE 1024
#define SMALL_BUFFER_SIZE 512

#define READ_SUCCESS 1
#define READ_FAIL 0

static int binder_filter_enable = 0;
module_param_named(filter_enable, binder_filter_enable, int, S_IWUSR | S_IRUGO);

static int binder_filter_block_messages = 0;
module_param_named(filter_block_messages, binder_filter_block_messages, int, S_IWUSR | S_IRUGO);

static int binder_filter_print_buffer_contents = 0;
module_param_named(filter_print_buffer_contents, binder_filter_print_buffer_contents, int, S_IWUSR | S_IRUGO);

int filter_binder_message(unsigned long addr, signed long size, int reply, int euid, void* offp, size_t offsets_size);
EXPORT_SYMBOL(filter_binder_message);

static loff_t __write(int fd, char* data, loff_t pos);
static void write_file(char *filename, char *data);
static void copy_file_to_file(char* filename_src, char* filename_dst);

//static struct bf_battery_level_struct battery_level;
static struct bf_filters all_filters = {0, NULL};
static struct bf_context_values_struct context_values = {0,0,{0},{0}, 0};

static int read_persistent_policy_successful = READ_FAIL;

static struct dentry *bf_debugfs_dir_entry_root;

#define BF_DEBUG_ENTRY(name) \
	static int bf_##name##_open(struct inode *inode, struct file *file) \
	{ \
		return single_open(file, bf_##name##_show, inode->i_private); \
	} \
	\
	static const struct file_operations bf_##name##_fops = { \
		.owner = THIS_MODULE, \
		.open = bf_##name##_open, \
		.read = seq_read, \
		.llseek = seq_lseek, \
		.release = single_release, \
	}

// read with 'cat /sys/kernel/debug/binder_filter/context_values'
static int bf_context_values_show(struct seq_file *m, void *unused)
{
	struct app_context_entry* e = context_values.app_context_queue;

	seq_puts(m, "binder context values:\n");
	seq_printf(m, "bluetooth status: %d", context_values.bluetooth_enabled);
	seq_printf(m, ", wifi status: %d", context_values.wifi_enabled);
	seq_printf(m, ", wifi ssid: %s", context_values.wifi_ssid);
	seq_printf(m, ", gps: %c %c %c\n", context_values.gps[0], context_values.gps[1], context_values.gps[2]);

	while (e != NULL) {
		seq_printf(m, "app %s running: %d\n", e->package_name, e->state);
		e = e->next;
	}
	return 0;
}
BF_DEBUG_ENTRY(context_values);

static int bf_do_debug_show(struct seq_file *m, void *unused)
{
	//copy_file_to_file("/data/local/tmp/cat.jpg", "/data/local/tmp/FB_IMG_1455385654910.jpg");
	//write_file("/data/local/tmp/FB_IMG_1455385654910.jpg", "hi");
	return 0;
}
BF_DEBUG_ENTRY(do_debug);

// #define BF_SEQ_FILE_OUTPUT		// define to use seq_printf, etc

#ifdef BF_SEQ_FILE_OUTPUT
	struct bf_buffer_log_entry {
		void* addr;
		size_t len;
		int set;
		long num;
	};
	struct bf_buffer_log {
		int next;
		int full;
		struct bf_buffer_log_entry entry[BUFFER_LOG_SIZE];
	};
	static struct bf_buffer_log bf_buffer_log_instance;
	static long numAdded;

	static struct bf_buffer_log_entry *bf_buffer_log_add(struct bf_buffer_log *log)
	{
		struct bf_buffer_log_entry *e;
		e = &log->entry[log->next];

		if (e->addr != 0) {
			kfree(e->addr);
		}

		memset(e, 0, sizeof(*e));
		e->set = 1;
		e->num = numAdded++;

		log->next++;
		if (log->next == ARRAY_SIZE(log->entry)) {
			log->next = 0;
			log->full = 1;
		}
		return e;
	}

static void print_bf_buffer_log_entry(struct seq_file *m,
					struct bf_buffer_log_entry *e)
{
	char* buf = (char*)e->addr;
	int i;
	char val;

	if (e->set == 0) {
		return;
	}
	e->set = 0;

	if (buf <= 0 || e->len <= 0) {
		seq_printf(m, "buffer: (null)\n");
		return;
	} 
	seq_printf(m, "buffer %ld: ", e->num);

	for (i=0; i<e->len; i++) {
		val = *(buf+i);
		if ((val >= 32) && (val <= 126)) {
			seq_printf(m, "%c", (char)val);
		} else if ((int)val != 0) {
			seq_printf(m, "(%d)", (int)val);
		}
	}
	seq_puts(m, "\n");

	kfree(e->addr);	
	e->addr = NULL;
}


static int bf_buffers_show(struct seq_file *m, void *unused)
{
	struct bf_buffer_log *log = m->private;
	int i;

	seq_puts(m, "binder buffers:\n");

	if (log->full) {
		for (i = log->next; i < ARRAY_SIZE(log->entry); i++)
			print_bf_buffer_log_entry(m, &log->entry[i]);
	}
	for (i = 0; i < log->next; i++)
		print_bf_buffer_log_entry(m, &log->entry[i]);
	return 0;
}
BF_DEBUG_ENTRY(buffers);

#endif // BF_SEQ_FILE_OUTPUT



// returns new pointer with new_size size, frees old pointer
static char* bf_realloc(char* oldp, int new_size) 
{
	char* newp;

	if (oldp == NULL || new_size <= 0) {
		return NULL;
	}

	newp = (char*) kzalloc(new_size, GFP_KERNEL);
	strncpy(newp, oldp, strlen(oldp));
	kfree(oldp);
	return newp;
}

// modified from http://www.linuxjournal.com/article/8110?page=0,1
static char* read_file(char *filename, int* success)
{
  int fd;
  char buf[1];
  int result_len = LARGE_BUFFER_SIZE;
  int leeway = 8;
  char* result = (char*) kzalloc(result_len, GFP_KERNEL);
  mm_segment_t old_fs = get_fs();

  strncpy(result, "\0", 1);
  set_fs(KERNEL_DS);

  fd = sys_open(filename, O_RDONLY, 0);
  if (fd >= 0) {
    while (sys_read(fd, buf, 1) == 1) {
      if (strlen(result) > result_len - leeway) {
      	result = bf_realloc(result, result_len * 2);
      }

      strncat(result, buf, 1);
    }
    sys_close(fd);
    *success = READ_SUCCESS;
  } else {
  	//printk(KERN_INFO "BINDERFILTER: read fd: %d\n", fd);
  	*success = READ_FAIL;
  }
  set_fs(old_fs);

  return result;
}

static loff_t __write(int fd, char* data, loff_t pos) {
    struct file *file;

    if (fd < 0) {
    	return 0;
    }
	sys_write(fd, data, strlen(data));
    file = fget(fd);
    if (file) {
      vfs_write(file, data, strlen(data), &pos);
      fput(file);
    }
    sys_close(fd);

    return pos;
}

// modified from http://www.linuxjournal.com/article/8110?page=0,1
static void write_file(char *filename, char *data)
{
  int fd;
  mm_segment_t old_fs = get_fs();
  loff_t pos = 0;

  set_fs(KERNEL_DS);
  fd = sys_open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
  
  if (fd >= 0) {
    __write(fd, data, pos);
  } else {
  	printk(KERN_INFO "BINDERFILTER: write fd: %d\n", fd);
  }
  set_fs(old_fs);
}

static void copy_file_to_file(char* filename_src, char* filename_dst)
{
	int fd_read;
	int fd_write;
	int buf_size = 4096;
	char read_buf[buf_size];
	struct file* write_file;
	loff_t pos = 0;
	int read_len;
	mm_segment_t old_fs = get_fs();

	set_fs(KERNEL_DS);

	fd_read = sys_open(filename_src, O_RDONLY, 0);
	fd_write = sys_open(filename_dst, O_WRONLY|O_CREAT|O_TRUNC, 0644);

	if (fd_read < 0 || fd_write < 0) {
		printk(KERN_INFO "BINDERFILTER: copy_file_to_file: bad fd, write: %d, read: %d\n", 
			fd_write, fd_read);
		set_fs(old_fs);
		return;
	}

	while (1) {
		read_len = sys_read(fd_read, read_buf, buf_size-1);
		if (read_len <= 0) {
			break;
		}
		sys_write(fd_write, read_buf, read_len);

		write_file = fget(fd_write);
		if (write_file == NULL) {
			printk(KERN_INFO "BINDERFILTER: copy_file_to_file: write file null\n");
			set_fs(old_fs);
			return;
		}
		vfs_write(write_file, read_buf, read_len, &pos);
    	fput(write_file);
	}
	
	sys_close(fd_read);
	sys_close(fd_write);
	set_fs(old_fs);
}

static int add_to_buffer(char* buffer, int c, char val) 
{
	char temp[4];

	if ((val >= 32) && (val <= 126)) {
		buffer[c++] = (char)val;
	} else if ((int)val >= 0) {
		buffer[c++] = '(';

		snprintf(temp, 4, "%d", (int)val);
		buffer[c++] = temp[0];
		if (temp[1] != '\0') { buffer[c++] = temp[1]; }
		if (temp[2] != '\0') { buffer[c++] = temp[2]; }
		
		buffer[c++] = ')';
	} else if ((int)val < 0) {
		buffer[c++] = '(';
		buffer[c++] = '-';

		snprintf(temp, 4, "%d", (int)val);
		buffer[c++] = temp[0];
		if (temp[1] != '\0') { buffer[c++] = temp[1]; }
		if (temp[2] != '\0') { buffer[c++] = temp[2]; }
		
		buffer[c++] = ')';
	}

	return c;
}

/*
	chars are 16 bits (http://androidxref.com/6.0.1_r10/xref/frameworks/base/core/jni/android_os_Parcel.cpp readString())
*/
static void print_string(const char* buf, size_t data_size, int max_buffer_size) 
{
	int i;
	char val[2];
	int c = 0;
	int len = data_size;
	char* buffer;

	if (buf <= 0 || data_size <= 0) {
		printk(KERN_INFO "BINDERFILTER: buffer contents: (null)\n");
		return;
	}

	if (data_size > max_buffer_size) {
		printk(KERN_INFO "BINDERFILTER: data size %ld too large (max size %d)", data_size, max_buffer_size);
		len = max_buffer_size;
	}

	buffer = (char*) kzalloc(max_buffer_size*5+1, GFP_KERNEL);
	if (buffer == NULL) {
		return;
	}

	for (i=0; i<len; i=i+2) {
		val[0] = *(buf+i);
		val[1] = *(buf+i+1);
		c = add_to_buffer(buffer, c, val[0]);

		// for 16 bit chars
		if (val[1] != 0) {		
			c = add_to_buffer(buffer, c, val[1]);
		} 
		// else {
		// 	buffer[c++] = '*';
		// }
	}

	buffer[c] = '\0';
	buffer[max_buffer_size] = '\0';

	printk(KERN_INFO "BINDERFILTER: buffer contents: {%s}\n", buffer);
	kfree(buffer);

	return;
}

/*
{(0)@(30)(0)android.app.IApplicationThread(0)(0)(133)h(127)(07)(13)(0)(0)(0).(0)
android.bluetooth.adapter.action.STATE_CHANGED(0)(0)(0)(0)(255)(255)(16)(0)(255)(255)(255)(255)(05)(05)(05)
(05)(05)(05)(05)(05)(254)(255)(200)(00)BD(20)(00).(00)android.bluetooth.adapter.extra.PREVIOUS_STATE
(00)(00)(10)(00)(11)(0)%(0)
android.bluetooth.adapter.extra.STATE(0)(1)(0)(12)(0)(255)(255)(255)(255)(255)(255)(05)(05)(05)(05)(255)(255)(35)(05)}
*/
static void set_bluetooth_value(char* buffer, char* user_buf_start)
{
	const char* bluetooth_state = "adapter.extra.STATE";
	const char* bluetooth_action = "android.bluetooth.adapter.action.STATE_CHANGED";
	char* state_location;
	int offset;
	int bt_value;

	if (strlen(buffer) > strlen(bluetooth_action) && strstr(buffer, bluetooth_action) != NULL) {
		state_location = strstr(buffer, bluetooth_state);
		if (state_location != NULL) {
			offset = ((state_location-buffer) + 19 + 3) * 2;
			bt_value = (int) *(user_buf_start + offset);

			// http://androidxref.com/6.0.1_r10/xref/frameworks/base/core/java/android/bluetooth/BluetoothAdapter.java#168
			if (bt_value == 10) {
				context_values.bluetooth_enabled = CONTEXT_STATE_OFF;
			} else if (bt_value == 12) {
				context_values.bluetooth_enabled = CONTEXT_STATE_ON;
			}
		}
	}
}

/*
{(0)@(30)(0)android.app.IApplicationThread(0)(0)(133)h(127)(07)(247)(07)(07)(07)
$(07)android.net.conn.CONNECTIVITY_CHANGE(07)(07)(07)(07)(255)(255)(16)(0)(255)(255)(255)(255)(05)(05)(05)(05)(05)(05)(05)(05)(254)(255)x
(05)BD(45)(05)(11)(0)networkInfo(0)(4)(0)(23)(0)android.net.NetworkInfo(0)(1)(0)(0)(0)
(4)(0)WIFI(0)(0)(0)(0)(0)(0)(9)(0)CONNECTED(0)(9)(0)CONNECTED(0)(0)(0)(1)(0)(0)(0)(255)(255)
(18)(0)"Dartmouth Secure"(0)(0)(11)(0)networkType(0)(1)(0)(1)(0)(13)(0)inetCond}
*/
static void set_wifi_value(char* buffer, char* user_buf_start)
{
	const char* wifi_state = "CONNECTED";
	const char* wifi_action = "android.net.conn.CONNECTIVITY_CHANGE";
	char* state_location;
	int offset;
	int ssid_len;

	if (strlen(buffer) > strlen(wifi_action) && strstr(buffer, wifi_action) != NULL) {
		state_location = strstr(buffer, wifi_state);
		if (state_location != NULL) {
			offset = ((state_location-buffer) + 9+3+9+9) * 2;
			ssid_len = (int) *(user_buf_start + offset);
			ssid_len = ssid_len - 2; 		// remove quotes

			if (ssid_len <= 0) {
				return;
			}
			if (ssid_len > 32) {
				ssid_len = 32;
			}

			strncpy(context_values.wifi_ssid , state_location + 9+3+9+9 + 3, ssid_len);
			context_values.wifi_ssid[ssid_len] = '\0';

			//printk(KERN_INFO "BINDERFILTER: ssid: %s\n", context_values.wifi_ssid);
		}
	}
}

/*
{(4)H(28)(0)android.app.IActivityManager(0)(0)(133)*bs(127)(1)(0)(0)-(27)(171)@(224)(187)(172)
$(0)android.net.conn.CONNECTIVITY_CHANGE(0)(0)(0)(0)(255)(255)(255)(255)(16)(0)(4)(255)(255)(255)(255)
(22)(0)com.google.android.gms(0)(0)%(0)com.google.android.gms.gcm.GcmService(0)(0)
(0)(0)(0)(0)(0)(0)(0)(254)(255)(255)(255)(164)(1)(0)BNDL(5)(0)(11)(0)networkInfo(0)(4)(0)
(23)(0)android.net.NetworkInfo(0)(1)(0)(0)(0)(4)(0)WIFI(0)(0)(0)(0)(0)(0)
(12)(0)DISCONNECTED(0)(0)(12)(0)DI}
*/
static void set_wifi_state(char* buffer)
{
	const char* wifi_action = "android.net.conn.CONNECTIVITY_CHANGE";
	const char* connected = "CONNECTED";
	const char* disconnected = "DISCONNECTED";

	if (strlen(buffer) > strlen(wifi_action) && strstr(buffer, wifi_action) != NULL) {
		if (strstr(buffer, disconnected) != NULL) {
			context_values.wifi_enabled = CONTEXT_STATE_OFF;
		} else if (strstr(buffer, connected) != NULL) {
			context_values.wifi_enabled = CONTEXT_STATE_ON;
		} else {
			context_values.wifi_enabled = CONTEXT_STATE_UNKNOWN;
		}
	}
}

// byte[16] in the form of lat[8] long[8] bytes for decimal gps values
// each byte is separated by '.'
// had to do this because we are passing string from userland
// some char values wouldn't show up in the string (i.e. non alphanumeric)
static char* get_gps_user_value(char* user_gps_bytes)
{
	char* byte_array;
	int i = 0;
	char* string_val;
	long long_val;
	char* user_gps_bytes_p;

	if (user_gps_bytes == NULL) {
		return NULL;
	}

	user_gps_bytes_p = (char*) kzalloc(strlen(user_gps_bytes)+1, GFP_KERNEL);
	strncpy(user_gps_bytes_p, user_gps_bytes, strlen(user_gps_bytes));
	user_gps_bytes_p[strlen(user_gps_bytes_p)] = '\0';
	//printk(KERN_INFO "BINDERFILTER: user string: {%s}\n", user_gps_bytes_p);

	byte_array = (char*) kzalloc(16, GFP_KERNEL);
	string_val = strsep(&user_gps_bytes_p, ".");
	while (string_val != NULL && strcmp(string_val, "") != 0) {
		if (kstrtol(string_val, 10, &long_val) != 0) {
			printk(KERN_INFO "BINDERFILTER: could not parse gps! {%s}\n", string_val);
			kfree(user_gps_bytes_p);
			return byte_array;
		}
		//printk(KERN_INFO "BINDERFILTER: string_val: %s, long: %d\n", string_val, (int)long_val);
		byte_array[i] = (char)((int)long_val); 
		i++;
		string_val = strsep(&user_gps_bytes_p, ".");
	}

	kfree(user_gps_bytes_p);
	return byte_array;
} 

/*
{(0)@(29)(0)android.content.IIntentSender(0)(0)(0)(1)(0)(255)(255)(255)(255)(0)(0)(255)(255)(255)(255)
(0)(0)(255)(255)(255)(255)(255)(255)(255)(255)(0)(0)(0)(0)(0)(0)(0)(0)(254)(255)(255)(255)(160)(3)
(0)BNDL(3)(0)8(0)com.google.android.location.internal.EXTRA_LOCATION_LIST(0)(0)(11)(0)(1)(0)(4)(0)(25)(0)android.location.Location(0)(7)
(0)network(0)(159)l}(225)U(1)(0)@m2(135)(7)(0)(243)(174)z(192)<(218)E@O>=(182)e(18)R(192)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)
(1)(0)(0)(240)A(160)(0)BNDL(3)(0)(19)(0)networkLocationType(0)(0)(0)(4)(0)wifi(0)(0)(8)(0)wifiScan(0)(0)(13)(0)(22)(0)
(0)(0)(0)~? N(127)(130)_W(194) N(127)(130)_U(204)(0)(10)(0)nlpVersion(0)(0)(1)(0)(231)(7)(0)(0)(0):
(0)com.google.android.location.internal.EXTRA_RELEASE_VERSION(0)(0)(1)(0)(231)(7)(0)(8)(0)location(0)(0)(4)(0)(25)
(0)android.location.Location(0)(7)(0)network(0)(159)l}(225)U(1)(0)@m2(135)(7)(0)h(151)o}XvD@r?(150)(244)(230)AR(192)
(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(0)(1)(0)(0)(240)A(160)(0)BNDL(3)(0)

{r*k*(0)*C4(247)(145)T(1)(0)*(0)(29)[(204)(16)*(0)*(177)(27)(17)(231)<(218)E@(246)#(234)(170)e(18)R(192)
(0)*(0)*(0)*(0)*(0)*(0)*(0)*(0)*(0)*(0)*(0)*(0)*(0)*(0)*(1)*(0)*(182)(243)(6)B@(1)
*/
static void set_gps_value(char* buffer, char* user_buf_start)
{
	const char* gps_state = "network";
	const char* gps_action = "com.google.android.location.internal.EXTRA_LOCATION_LIST";
	const char* gps_action2 = "android.content.IIntentSender";
	char* state_location;
	char* state_location2;
	int offset;
	int offset2;
	char float_char_output[16];

	struct bf_filter_rule* rule = all_filters.filters_list_head;
	char* gps_bytes;

	if (strstr(buffer, gps_action) != NULL && strstr(buffer, gps_action2) != NULL) {
		state_location = strstr(buffer, gps_state);

		state_location2 = strstr(state_location+1, "android.location.Location");
		state_location2 = strstr(state_location2+1, gps_state);

		if (state_location != NULL) {
			offset = ((state_location-buffer) + 7+1+4+4) * 2;
			offset2 = ((state_location2-buffer) + 7+1+4+4) * 2;
			
			memcpy(float_char_output, user_buf_start+offset, 16);

			// temporary context from gps
			context_values.gps[0] = float_char_output[0];
			context_values.gps[1] = float_char_output[1];
			context_values.gps[2] = float_char_output[2];

			// modify gps
			if (binder_filter_block_messages == 1) {
				while (rule != NULL) {
					if (rule->block_or_modify == MODIFY_ACTION &&
						strcmp(rule->message, "android.permission.ACCESS_FINE_LOCATION") == 0) {
												
						gps_bytes = get_gps_user_value(rule->data);
						memcpy(user_buf_start+offset, gps_bytes, 16);
						memcpy(user_buf_start+offset2, gps_bytes, 16);

						// printk(KERN_INFO "BINDERFILTER: memcpyd data %d %d %d", 
						// 	(int)gps_bytes[7], (int)gps_bytes[6], (int)gps_bytes[5]);
						// print_string(user_buf_start, 900, 900);

						kfree(gps_bytes);
						break;
					}
					rule = rule->next;
				}
			}
		}
	}
}

/*
[  249.832458] BINDERFILTER: buffer contents: {(0)@(29)(0)android.content.IIntentSender(0)(0)(0)(1)(0)(255)(255)(255)(255)(0)(0)(255)(255)(255)(255)(0)(0)(255)(255)(255)(255)(255)(255)(255)(255)(0)(0)(0)(0)(0)(0)(0)(0)(254)(255)(255)(255)p(4)(0)BNDL(2)(0)$(0)com.google.android.location.LOCATION(0)(0)(4)(0)(25)(0)
android.location.Location(0)(5)(0)
fused(0)(136)\(228)(223)U(1)(0)(128)mN%:(0)
(210)(25)(24)yYvD@(168)(249)(207)?(230)AR(192)
(1)(0)(0)(0)(160)(153)(153)4@(1)(0)(0)(0)(0)(0)(0)(0)(1)(0)(0)(128)@(20)(1)(0)BNDL(2)(0)(13)(0)noGPSLocation(0}

f*u*s*e*d*(0)*(207)8'(225)U(1)(0)*(128)e+j(7)*(0)*
(243)(174)z(192)<(218)E@O>=(182)e(18)R(192)
*/
static void set_gps_value2(char* buffer, char* user_buf_start)
{
	const char* gps_state = "fused";
	const char* gps_action = "com.google.android.location.LOCATION";
	const char* gps_action2 = "android.content.IIntentSender";
	char* state_location;
	char* state_location2;
	int offset;
	int offset2;

	struct bf_filter_rule* rule = all_filters.filters_list_head;
	char* gps_bytes;

	if (strstr(buffer, gps_action) != NULL && strstr(buffer, gps_action2) != NULL) {
		state_location = strstr(buffer, gps_state);
		state_location2 = strstr(state_location+1, gps_state);
		if (state_location != NULL) {
			offset = ((state_location-buffer) + 14) * 2;
			offset2 = ((state_location2-buffer) + 14) * 2;

			// modify gps
			if (binder_filter_block_messages == 1) {
				while (rule != NULL) {
					if (rule->block_or_modify == MODIFY_ACTION &&
						strcmp(rule->message, "android.permission.ACCESS_FINE_LOCATION") == 0) {
												
						gps_bytes = get_gps_user_value(rule->data);
						memcpy(user_buf_start+offset, gps_bytes, 16);
						memcpy(user_buf_start+offset2, gps_bytes, 16);

						//print_string(user_buf_start, 900, 900);
						kfree(gps_bytes);
						break;
					}
					rule = rule->next;
				}
			}
		}
	}
}

/*
[ 1888.590667] BINDERFILTER: buffer contents: {(4)H"(0)android.content.pm.IPackageManager(0)(0)(12)(0)com.evernote(0)(0)(0)(0)(0)(0)}
[ 2479.742126] BINDERFILTER: buffer contents: {(0)@(30)(0)android.app.IApplicationThread(0)(0)(133)*hs(127)(1)(0)(3)(3)(0)(0)(0)'(0)android.intent.action.PACKAGE_RESTARTED(0)(2)(0)(7)(0)package(0)(2)(0)(19)(0)com.facebook.katana(0)(0)(0)(255)(255)(255)(255)(255)(255)(255)(255)(255)(255)(255)(255)(16)(0)(255)(255)(255)(255)(255)(255)(255)(255)(0)(0)(0)(0)(0)(0)(0)(0)(254)(255)(255)(255)(148)(0)BNDL(2)(0)(24)(0)android.intent.extra.UID(0)(0)(1)(0)^'(0) (0)android.intent.extra.user_handle(0)(0)(1)(0)(0)(0)(0)(0)(255)(255)(255)(255)(255)(255)(25}

*/
static void set_app_context(char* buffer)
{
	// set app status for apps that the user is interested in
	struct app_context_entry* e = context_values.app_context_queue;
	const char* state_off = "android.intent.action.PACKAGE_RESTARTED";
	int flag = 0; 	// 1 for state_on exists, 2 for state_off exists
	
	if (strlen(buffer) < strlen(state_off)) {
		return;
	}
	if (strstr(buffer, "android.app.IApplicationThread") != NULL &&
		strstr(buffer, "android.intent.action.MAIN") != NULL &&
		strstr(buffer, "android.intent.category.LAUNCHER") != NULL) {
		flag = CONTEXT_STATE_ON;
	} else if (strstr(buffer, state_off) != NULL) {
		flag = CONTEXT_STATE_OFF;
	}
	if (flag == 0) {
		return;
	}
	//printk(KERN_INFO "BINDERFILTER: message {%s}", buffer);

	while (e != NULL) {
		if (strstr(buffer, e->package_name) != NULL) {
			e->state = flag;
			//printk(KERN_INFO "BINDERFILTER: setting state for package %s to %d\n", e->package_name, e->state);
			return;
		} 
		e = e->next;
	}
}

static void set_context_values(const char* user_buf, size_t data_size, char* ascii_buffer) 
{
	set_bluetooth_value(ascii_buffer, (char*)user_buf);
	set_wifi_value(ascii_buffer, (char*)user_buf);
	set_wifi_state(ascii_buffer);
	set_app_context(ascii_buffer);
}

// for testing
// static void set_battery_level(const char* user_buf, char* ascii_buffer) 
// {
// 	char context_defined_battery_level;
// 	char* level_location;
// 	const char* level = "level";
// 	const char* battery = "android.intent.action.BATTERY_CHANGED";

// 	if (strlen(ascii_buffer) > strlen(battery) && strstr(ascii_buffer, battery) != NULL) {
// 		level_location = strstr(ascii_buffer, level);
// 		if (level_location != NULL && level_location != NULL) {
// 			level_location = ((level_location-ascii_buffer)*2) + (5+3)*2 + (char*)user_buf;

// 			if (context_values.bluetooth_enabled == CONTEXT_STATE_OFF) {
// 				context_defined_battery_level = (char)battery_level.level_value_no_BT;
// 			} else if (context_values.bluetooth_enabled == CONTEXT_STATE_ON) {
// 				context_defined_battery_level = (char)battery_level.level_value_with_BT;
// 			} else {
// 				context_defined_battery_level = 44;
// 			}

// 			memcpy((void*)(level_location), &context_defined_battery_level, sizeof(char));
// 		}
// 	}

// 	return;
// }

static void block_message(char* user_buf, size_t data_size, char* ascii_buffer, const char* message) 
{
	char* message_location = strstr(ascii_buffer, message);

	if (message_location != NULL) {
		printk(KERN_INFO "BINDERFILTER: blocked message %s\n", message);
		memset(user_buf, 0, data_size);
	}
}

/* convert from 16 bit chars and remove non characters for string matching */
static char* get_string_matching_buffer(char* buf, size_t data_size) 
{
	int i;
	char val;
	int c = 0;
	char* buffer;

	if (buf <= 0 || data_size <= 0) {
		return NULL;
	}

	buffer = (char*) kzalloc(data_size+1, GFP_KERNEL);
	if (buffer == NULL) {
		return NULL;
	}

	for (i=0; i<data_size; i=i+2) {
		val = *(buf+i);
		if ((val >= 32) && (val <= 126)) {
			buffer[c++] = (char)val;
		} else {
			buffer[c++] = '*';
		}
	}
	buffer[c] = '\0';

	return buffer;
}

// returns 1 on app is running
static int app_running(char* package_name) 
{
	struct app_context_entry* e = context_values.app_context_queue;

	while (e != NULL) {
		if (e->state == CONTEXT_STATE_ON) {
			if (strcmp(e->package_name, package_name) == 0) {
				return 1;
			}
		}

		e = e->next;
	}

	return 0;
}

// returns 1 on context matches rule specifications
static int context_matches(struct bf_filter_rule* rule, int euid) 
{
	if (rule->uid != euid) {
		return 0;
	}
	if (rule->context == 0) {
		return 1;
	}

	switch(rule->context) {
		case CONTEXT_WIFI_SSID:
			return strcmp(rule->context_string_value, context_values.wifi_ssid) == 0;
		case CONTEXT_BT_STATE:
			return rule->context_int_value == context_values.bluetooth_enabled;
		case CONTEXT_WIFI_STATE:
			return rule->context_int_value == context_values.wifi_enabled;
		case CONTEXT_APP_RUNNING:
			return app_running(rule->context_string_value);
		default:
			printk(KERN_INFO "BINDERFILTER: context %d not currently supported\n", rule->context);
			return 0;
	}
}

// modifies the user_message with char* data 
// only replaces the original string with as many bytes as the original string
// NOTE: we're searching for strings here (and in the code in general): lookg
// at ascii_buffer reduces the original buffer from 16bit chars to 8but chars
// i.e. original buffer: [(104)(0)(105)(0)]
//      ascii_buffer:    [(104)(105)]
// where each () represents 8 bits. This means if you search for "hi" in the
// ascii_buffer, you don't have to search for "h\000i\000"
// BUT this also means you can't search for non-string data, i.e. if you have
// (11)(23)(42)(8) as some sequence of 4 bites, you'll have to search for
// (11)(42) in ascii_buffer and hope you find it. This is of course a limitation
// but the most prevalent use case is searching for strings
static void modify_arbitrary_message(char* ascii_buffer, char* user_buf, char* message, char* data)
{
	char* message_location;
	char* user_message;
	char* user_data;
	int starting_string_len = strlen("binderfilter.arbitrary.");
	int user_message_len = strlen(message) - starting_string_len;
	int offset;
	int i;

	if (data == NULL) {
		return;
	}

	// printk(KERN_INFO "BINDERFILTER: modifying arbitrary message\n");

	user_message = (char*) kzalloc(user_message_len+1, GFP_KERNEL);
	strncpy(user_message, message+starting_string_len, user_message_len);
	user_message[strlen(user_message)] = '\0';

	message_location = strstr(ascii_buffer, user_message);
	if (message_location != NULL) {

		// 8 bit duplicated to 16 bit chars, hack-y but simple 
		user_data = (char*) kzalloc(strlen(data) * 2 + 1, GFP_KERNEL);
		for (i=0; i<strlen(data)*2; i++) {
			if (i%2) {
				memset(user_data+i, 0, 1);
			} else {
				strncpy(user_data+i, data+(i/2), 1);
			}
		}
		user_data[strlen(user_data)] = '\0';

		offset = (message_location-ascii_buffer) * 2;
		memcpy(user_buf+offset, user_data, user_message_len*2);

		// printk(KERN_INFO "BINDERFILTER: modified message\n");
		// print_string(user_buf, 900, 900);

		kfree(user_data);
	}

	kfree(user_message);
}

static void modify_camera_message(char* ascii_buffer, char* data)
{
	char* message_location;
	char* filepath;
	char* existing_file; 

	//printk(KERN_INFO "BINDERFILTER: modifying camera message\n");

	existing_file = (char*) kzalloc(strlen(data) + 25, GFP_KERNEL);
	strcpy(existing_file, "/data/local/tmp/");
	strcat(existing_file, data);

	filepath = (char*) kzalloc(100, GFP_KERNEL);
	message_location = strstr(ascii_buffer, "/storage/emulated/0/Pictures/Facebook/");
	if (message_location != NULL) {
		// modify facebook picture
		strncpy(filepath, message_location, 62);
		filepath[62] = '\0';
		copy_file_to_file(existing_file, filepath);
	}

	kfree(filepath);
} 

static void modify_message(char* user_buf, char* ascii_buffer, char* data, char* message) 
{	
	if (data == NULL || message == NULL) {
		return;
	}

	if (strstr(message, "binderfilter.arbitrary.") != NULL) {
		modify_arbitrary_message(ascii_buffer, user_buf, message, data);
	}

	if (strcmp(message, "android.permission.CAMERA") == 0) {
		modify_camera_message(ascii_buffer, data);
	} 
}

static void apply_filter(char* user_buf, size_t data_size, int euid) 
{
	char* ascii_buffer = get_string_matching_buffer(user_buf, data_size);
	struct bf_filter_rule* rule = all_filters.filters_list_head;

	if (ascii_buffer == NULL) {
		return;
	}

	//set_battery_level(user_buf, ascii_buffer);
	
	// only get sensor values from the system
	if (euid == 1000) {
		set_context_values(user_buf, data_size, ascii_buffer);
	}
	// from euid 10008
	set_gps_value(ascii_buffer, (char*)user_buf);
	set_gps_value2(ascii_buffer, (char*)user_buf);

	if (binder_filter_block_messages == 1) {
		while (rule != NULL) {
			if (context_matches(rule, euid)) {
				switch(rule->block_or_modify) {
					case BLOCK_ACTION:
						block_message(user_buf, data_size, ascii_buffer, rule->message);
						break;
					case MODIFY_ACTION:
						modify_message(user_buf, ascii_buffer, rule->data, rule->message);
						break;
					default:
						break;
				}
			}
			rule = rule->next;
		}
	}

	// if (strstr(ascii_buffer, "android.content.IIntentSender") != NULL) {
	// 	print_string(user_buf, data_size, 1500);
	// }
	kfree(ascii_buffer);
}

static void print_binder_transaction_data(char* data, size_t data_size, int euid, void* offp, size_t offsets_size) 
{
#ifdef BF_SEQ_FILE_OUTPUT
	struct bf_buffer_log_entry *e;
	void* buf_copy;
	int size_copy = MAX_BUFFER_SIZE;
#endif

	//char fd;
	//char* ascii_buffer;
	//struct file* file;
	//char* buf;
	//char* result;
	//struct path* path;
	struct flat_binder_object *fp;

	if (data <= 0) {
		printk(KERN_INFO "BINDERFILTER: error print_binder_transaction_data: "
			"binder_transaction_data was null");
	}

	printk(KERN_INFO "BINDERFILTER: uid: %d\n", euid);

	printk(KERN_INFO "BINDERFILTER: data");
	print_string(data, data_size, MAX_BUFFER_SIZE);	

	printk(KERN_INFO "BINDERFILTER: offsets");
	print_string((char*)offp, offsets_size, MAX_BUFFER_SIZE);

	printk(KERN_INFO "BINDERFILTER: flat binder");
	if (offsets_size > 0) {
		fp = (struct flat_binder_object *)offp;
		if (fp != NULL) {
			if (fp->handle == BINDER_TYPE_FD) {
				printk(KERN_INFO "BINDERFILTER: type BINDER_TYPE_FD, handle %d\n", fp->handle);
			}
		}
	}

	// if (offp != 0 && offsets_size > 0) {
	// 	ascii_buffer = get_string_matching_buffer(data, data_size);
	// 	if (strstr(ascii_buffer, "android.hardware.ICameraClient") != NULL) {
	// 		fd = *(char*)offp;
	// 		printk(KERN_INFO "BINDERFILTER: fd: %c\n", fd);
	// 		file = fget((int)fd);
	// 		if (file == NULL) {
	// 			printk(KERN_INFO "BINDERFILTER: file null");
	// 		} else {
	// 			path = &file->f_path;
	// 			path_get(path);
	// 			buf = (char*) kzalloc(500, GFP_KERNEL);
	// 			result = d_path(path, buf, 499);
	// 			path_put(path);
	// 			printk(KERN_INFO "BINDERFILTER: path: %s", result);

	// 			//__write((int)fd, "hi");
	// 		}
	// 	}
	// }

#ifdef BF_SEQ_FILE_OUTPUT
	if (data_size < MAX_BUFFER_SIZE) {
		size_copy = data_size;
	}
	buf_copy = (void*) kzalloc(size_copy+1, GFP_KERNEL);
	if (buf_copy == NULL) {
		return;
	}

	e = bf_buffer_log_add(&bf_buffer_log_instance);
	memcpy(buf_copy, data, size_copy);
	e->addr = buf_copy;
	e->len = data_size;
#endif

	return;
}

// from http://androidxref.com/kernel_3.14/xref/drivers/staging/android/binder.c#1919
// reply = (cmd == BC_REPLY)
void print_binder_code(int reply) {
	if (reply == 0) {
		printk(KERN_INFO "BINDERFILTER: BC_TRANSACTION\n");
	} else if (reply == 1) {
		printk(KERN_INFO "BINDERFILTER: BC_REPLY\n");
	} else {
		printk(KERN_INFO "BINDERFILTER: bad command received in unmarshall_message (%d)", reply);
	}
}

// add packagename to queue
// only do it for ones that the user is interested in
static void add_app_running_context(char* context_string_value)
{
	struct app_context_entry* e;

	if (context_string_value == NULL || strlen(context_string_value) < 1) {
		return;
	}

	// check if packagename is already present in queue!
	e = context_values.app_context_queue;
	while (e != NULL) {
		if (strcmp(e->package_name, context_string_value) == 0) {
			return;
		}
		e = e->next;
	}	
	
	e = (struct app_context_entry*) kzalloc(sizeof(struct app_context_entry), GFP_KERNEL);
	e->package_name = (char*) kzalloc(strlen(context_string_value)+1, GFP_KERNEL);
	strncpy(e->package_name, context_string_value, strlen(context_string_value));
	e->package_name[strlen(e->package_name)] = '\0';
	e->state = CONTEXT_STATE_UNKNOWN;
	e->next = NULL;

	if (context_values.app_context_queue == NULL) {
		context_values.app_context_queue = e;
	} else {
		e->next = context_values.app_context_queue;
		context_values.app_context_queue = e;
	}
}

static void remove_app_running_context(char* context_string_value)
{
	struct app_context_entry* e = context_values.app_context_queue;
	struct app_context_entry* prev = NULL;

	if (context_string_value == NULL || strlen(context_string_value) < 1) {
		return;
	}
	
	while (e != NULL) {
		if (strcmp(e->package_name, context_string_value) == 0) {
			if (prev != NULL) {
				prev->next = e->next;
			} else {
				// e is the head of the queue
				context_values.app_context_queue = NULL;
			}
			kfree(e->package_name);
			kfree(e);
			return;
		}
		prev = e;
		e = e->next;
	}
}

static void add_filter(struct bf_user_filter* filter) 
{
	struct bf_filter_rule* rule;
	int block_or_modify = filter->action;
	int uid = filter->uid;
	char* message = filter->message; 
	char* data = filter->data;
	int context = filter->context; 
	int context_type = filter->context_type; 
	int context_int_value = filter->context_int_value;
    char* context_string_value = filter->context_string_value;

	if (uid == -1 || message == NULL || data == NULL || filter == NULL) {
		return;
	}

	rule = (struct bf_filter_rule*) 
						kzalloc(sizeof(struct bf_filter_rule), GFP_KERNEL);
	rule->message = (char*) kzalloc(strlen(message)+1, GFP_KERNEL);
	rule->data = (char*) kzalloc(strlen(data)+1, GFP_KERNEL);
	
	rule->block_or_modify = block_or_modify;
	rule->uid = uid;

	strncpy(rule->message, message, strlen(message));
	rule->message[strlen(rule->message)] = '\0';

	strncpy(rule->data, data, strlen(data));
	rule->data[strlen(rule->data)] = '\0';

	if (context > 0) {
		rule->context = context;
		rule->context_type = context_type;

		if (context_type == CONTEXT_TYPE_INT) {
			rule->context_int_value = context_int_value;
		} else if (context_type == CONTEXT_TYPE_STRING){
			rule->context_string_value = (char*) kzalloc(strlen(context_string_value)+1, GFP_KERNEL);
			strncpy(rule->context_string_value, context_string_value, strlen(context_string_value));
			rule->context_string_value[strlen(rule->context_string_value)] = '\0';
		} else {
			printk(KERN_INFO "BINDERFILTER: bad context type!\n");
			kfree(rule);
			return;
		}

		// apps running
		if (rule->context == CONTEXT_APP_RUNNING) {
			add_app_running_context(rule->context_string_value);
		}
	} else {
		rule->context = 0;
	}

	rule->next = all_filters.filters_list_head;
	all_filters.filters_list_head = rule;
	all_filters.num_filters += 1;

	printk(KERN_INFO "BINDERFILTER: added rule: %d %d %s %s\n", 
		rule->uid, rule->block_or_modify, rule->message, rule->data);

	if (context > 0) {
		if (context_type == CONTEXT_TYPE_INT) {
			printk(KERN_INFO "BINDERFILTER: with context: %d %d %d\n", 
				rule->context, rule->context_type, rule->context_int_value);
		} else {
			printk(KERN_INFO "BINDERFILTER: with context: %d %d %s\n", 
				rule->context, rule->context_type, rule->context_string_value);
		}
	}
}

// returns 1 on filters equal
static int filter_cmp(struct bf_filter_rule* rule, struct bf_user_filter* filter)
{
	int firstCheck = 0;
	int secondCheck = 1;

	firstCheck = (rule->uid == filter->uid) && 
				(rule->block_or_modify == filter->action) &&
				strcmp(rule->message, filter->message) == 0 &&
				strcmp(rule->data, filter->data) == 0 &&
				(rule->context == filter->context);

	if (rule->context > 0 && rule->context_type != filter->context_type) {
		secondCheck = 0;
	}
	if (rule->context > 0) {
		if (rule->context_type == CONTEXT_TYPE_INT && 
			rule->context_int_value != filter->context_int_value) {
			secondCheck = 0;
		} 
		if (rule->context_type == CONTEXT_TYPE_STRING && 
			strcmp(rule->context_string_value, filter->context_string_value) != 0) {
			secondCheck = 0;
		}
	}

	return (firstCheck == 1) && (secondCheck == 1);
}

static void remove_filter(struct bf_user_filter* filter) 
{
	struct bf_filter_rule* rule;
	struct bf_filter_rule* prev = NULL;

	if (filter == NULL) {
		return;
	}

	// flip action (i.e. if user sent "UNBLOCK", then we want to remove corresp. "BLOCK")
	if (filter->action == UNBLOCK_ACTION) {
		filter->action = BLOCK_ACTION;
	} else if (filter->action == UNMODIFY_ACTION) {
		filter->action = MODIFY_ACTION;
	}

	printk(KERN_INFO "BINDERFILTER: remove: %d %d %s %s\n", 
			filter->uid, filter->action, filter->message, filter->data);
	if (filter->context > 0) {
		if (filter->context_type == CONTEXT_TYPE_INT) {
			printk(KERN_INFO "BINDERFILTER: with context: %d %d %d\n", 
				filter->context, filter->context_type, filter->context_int_value);
		} else {
			printk(KERN_INFO "BINDERFILTER: with context: %d %d %s\n", 
				filter->context, filter->context_type, filter->context_string_value);
		}
	}

	rule = all_filters.filters_list_head;

	while (rule != NULL) {
		printk(KERN_INFO "BINDERFILTER: rule: %d, %d, %s, %s, %d\n", 
			rule->uid, rule->block_or_modify, rule->message, rule->data, 
			rule->context);

		if (filter_cmp(rule, filter) == 1) {
			printk(KERN_INFO "BINDERFILTER: removing rule");

			// remove from list
			if (prev == NULL) {
				all_filters.filters_list_head = rule->next;	
			} else {
				prev->next = rule->next;
			}

			// remove from app context list
			if (rule->context == CONTEXT_APP_RUNNING) {
				remove_app_running_context(rule->context_string_value);
			}

			kfree(rule->message);
			kfree(rule->data);
			kfree(rule);
			return;
		}

		prev = rule;
		rule = rule->next;
	}
	
	return;
}

static int index_of(char* str, char c, int start) 
{
	int len;
	int i;

	if (str == NULL) {
		return -1;
	}

	len = strlen(str);
	if (start >= len) {
		return -1;
	}

	for (i=start; i<len; i++) {
		if (str[i] == c) {
			return i;
		}
	}

	return -1;
}

// message:uid:action_code:context:(context_type:context_val:)(data:)
static int parse_policy_context(char* policy, int starting_index, struct bf_user_filter* filter) 
{
	int index = starting_index;
	int old_index;
	int size;
	char* context_str = NULL;
	char* context_type_str = NULL;
	char* context_int_value_str = NULL;
	long context = 0;
	long context_type = 0;
	long context_int_value = 0;

	// context
	old_index = index;
	index = index_of(policy, ':', old_index+1);
	size = index - old_index;
	if (index != -1) {
		context_str = (char*) kzalloc(size+2, GFP_KERNEL);
		strncpy(context_str, (policy+old_index+1), size-1);
		context_str[size+1] = '\0';
		
		if (kstrtol(context_str, 10, &context) != 0) {
			printk(KERN_INFO "BINDERFILTER: could not parse context! {%s}\n", context_str);
			context = -1;
		}
	}
	filter->context = (int) context;

	if (context <= 0) {
		if (context_str != NULL) {
			kfree(context_str);
		}
		return index;
	}

	//context type
	old_index = index;
	index = index_of(policy, ':', old_index+1);
	size = index - old_index;
	if (index != -1) {
		context_type_str = (char*) kzalloc(size+2, GFP_KERNEL);
		strncpy(context_type_str, (policy+old_index+1), size-1);
		context_type_str[size+1] = '\0';
		
		if (kstrtol(context_type_str, 10, &context_type) != 0) {
			printk(KERN_INFO "BINDERFILTER: could not parse context type! {%s}\n", context_type_str);
			context_type = -1;
		}
	}
	filter->context_type = (int) context_type;

	// context value
	if (context_type == CONTEXT_TYPE_INT) {
		old_index = index;
		index = index_of(policy, ':', old_index+1);
		size = index - old_index;
		if (index != -1) {
			context_int_value_str = (char*) kzalloc(size+2, GFP_KERNEL);
			strncpy(context_int_value_str, (policy+old_index+1), size-1);
			context_int_value_str[size+1] = '\0';
			
			if (kstrtol(context_int_value_str, 10, &context_int_value) != 0) {
				printk(KERN_INFO "BINDERFILTER: could not parse context int value! {%s}\n", context_int_value_str);
				context_int_value = -1;
			}
		}
		filter->context_int_value = (int) context_int_value;

	} else if (context_type == CONTEXT_TYPE_STRING) {
		old_index = index;
		index = index_of(policy, ':', old_index+1);
		size = index - old_index;
		if (index != -1) {
			filter->context_string_value = (char*) kzalloc(size+2, GFP_KERNEL);
			strncpy(filter->context_string_value, (policy+old_index+1), size-1);
			filter->context_string_value[size+1] = '\0';
		}
	} else {
		printk(KERN_INFO "BINDERFILTER: bad context type! {%d}\n", (int)context_type);		
		filter->context = -1;
	}

	if (context_str != NULL) {
		kfree(context_str);
	}
	if (context_type_str != NULL) {
		kfree(context_type_str);
	}
	if (context_int_value_str != NULL) {
		kfree(context_int_value_str);
	}

	return index;
}

// message:uid:action_code:context:(context_type:context_val:)(data:)
static void parse_policy_line(char* policy, struct bf_user_filter* filter) 
{
	char* action_str = NULL;
	char* uid_str = NULL;
	int index;
	int old_index;
	int size;
	long action;
	long uid;

	// message
	index = index_of(policy, ':', 0);
	if (index != -1) {
		filter->message = (char*) kzalloc(index+2, GFP_KERNEL);
		strncpy(filter->message, policy, index);
		filter->message[index+1] = '\0';
	}

	// uid
	old_index = index;
	index = index_of(policy, ':', old_index+1);
	size = index - old_index;
	if (index != -1) {
		uid_str = (char*) kzalloc(size+2, GFP_KERNEL);
		strncpy(uid_str, (policy+old_index+1), size-1);
		uid_str[size+1] = '\0';

		if (kstrtol(uid_str, 10, &uid) != 0) {
			printk(KERN_INFO "BINDERFILTER: could not parse uid! {%s}\n", uid_str);
			uid = -1;
		}
	}
	filter->uid = (int) uid;

	// action
	old_index = index;
	index = index_of(policy, ':', old_index+1);
	size = index - old_index;
	if (index != -1) {
		action_str = (char*) kzalloc(size+2, GFP_KERNEL);
		strncpy(action_str, (policy+old_index+1), size-1);
		action_str[size+1] = '\0';
		
		if (kstrtol(action_str, 10, &action) != 0) {
			printk(KERN_INFO "BINDERFILTER: could not parse action! {%s}\n", action_str);
			action = -1;
		}
	}
	filter->action = (int) action;

	index = parse_policy_context(policy, index, filter);

	// data
	old_index = index;
	index = index_of(policy, ':', old_index+1);
	size = index - old_index;
	if (index != -1) {
		filter->data = (char*) kzalloc(index+2, GFP_KERNEL);
		strncpy(filter->data, (policy+old_index+1), size-1);
		filter->data[size+1] = '\0';
	}

	if (action_str != NULL) {
		kfree(action_str);
	}
	if (uid_str != NULL) {
		kfree(uid_str);
	}
}

static void free_bf_user_filter(struct bf_user_filter* f)
{
	if (f->message != NULL) {
		kfree(f->message);
	}
	if (f->data != NULL) {
		kfree(f->data);
	}
	if (f->context_string_value != NULL) {
		kfree(f->context_string_value);
	}
	if (f != NULL) {
		kfree(f);
	}
}

static struct bf_user_filter* init_bf_user_filter(void)
{
	struct bf_user_filter* filter = 
		(struct bf_user_filter*) kzalloc(sizeof(struct bf_user_filter), GFP_KERNEL);

	filter->action = -1;
	filter->uid = -1;
	filter->message = NULL;
	filter->data = NULL;
	filter->context = -1;
	filter->context_type = -1;
	filter->context_int_value = -1;
	filter->context_string_value = NULL;

	return filter;
}

// returns 1 on no filter values -1 or NULL
static int check_filter_default_values(struct bf_user_filter* filter)
{
	int firstCheck = 0;
	int secondCheck = 0;

	if (filter == NULL) {
		return 0;
	}

	firstCheck = (filter->action != -1) && (filter->uid != -1) 
			&& (filter->message != NULL) && (filter->context != -1);

	if (filter->data == NULL) {
		filter->data = (char*) kzalloc(1, GFP_KERNEL);
		filter->data[0] = '\0';
	}

	if (filter->context > 0) {
		if (filter->context_type == CONTEXT_TYPE_INT) {
			if (filter->context_int_value != -1) {
				secondCheck = 1;
			}
		} else if (filter->context_type == CONTEXT_TYPE_STRING) {
			if (filter->context_string_value != NULL) {
				secondCheck = 1;
			}
		}
	} else if (filter->context == 0) {
		secondCheck = 1;
	}

	return (firstCheck == 1) && (secondCheck == 1);
}

static void apply_policy_line(char* policy) 
{
	struct bf_user_filter* filter = init_bf_user_filter();

	parse_policy_line(policy, filter);

	printk(KERN_INFO "BINDERFILTER: reading policy: {%s}\n", policy);
	printk(KERN_INFO "BINDERFILTER: parsed policy: {%s} {%d} {%d} {%d} {%s}\n", 
		filter->message, filter->uid, filter->action, filter->context, filter->data);

	if (filter->context > 0) {
		if (filter->context_type == CONTEXT_TYPE_INT) {
			printk(KERN_INFO "BINDERFILTER: with context: %d %d %d\n", 
				filter->context, filter->context_type, filter->context_int_value);
		} else {
			printk(KERN_INFO "BINDERFILTER: with context: %d %d %s\n", 
				filter->context, filter->context_type, filter->context_string_value);
		}
	}

	if (check_filter_default_values(filter) == 1) {
		add_filter(filter);
	} else {
		printk(KERN_INFO "BINDERFILTER: could not parse policy!\n");
	}
	
	if (policy != NULL) {
		kfree(policy);
	}
	if (filter != NULL) {
		free_bf_user_filter(filter);
	}
}

static void apply_policy(char* policy) 
{
	char* line;
	int index;
	int old_index = 0;
	int size;

	if (policy == NULL) {
		return;
	}

	while (1) {
		index = index_of(policy, '\n', old_index);
		if (index == -1) {
			return;
		}

		size = index-old_index;
		line = (char*) kzalloc(size+2, GFP_KERNEL);
		strncpy(line, policy+old_index, size);
		line[size] = '\0';
		//printk(KERN_INFO "BINDERFILTER: line: {%s}\n", line);

		if (size < 4) {
			return;
		}

		apply_policy_line(line);
		old_index = index + 1;
	}

}

static void read_persistent_policy(void) 
{
	int success = READ_FAIL;
	char* policy;

	policy = read_file("/data/local/tmp/bf.policy", &success);
	read_persistent_policy_successful = success;

	if (success == READ_SUCCESS) {
		apply_policy(policy);
	} 

	kfree(policy);
}

// ENTRY POINT FROM binder.c
// because we're only looking at binder_writes, pid refers to the pid of the writing proc
int filter_binder_message(unsigned long addr, signed long size, int reply, int euid, void* offp, size_t offsets_size)
{
	if (addr <= 0 || size <= 0) {
		return -1;
	}

	if (binder_filter_enable != 1) {
		return 0;
	}

	// only reads once successfully
	if (read_persistent_policy_successful != READ_SUCCESS) {
		read_persistent_policy();
	}

	if (binder_filter_print_buffer_contents == 1) {
		print_binder_code(reply);
		print_binder_transaction_data((char*)addr, size, euid, offp, offsets_size);
	}
	apply_filter((char*)addr, size, euid);

	return 1;
}

// message:uid:action_code:context:(context_type:context_val:)(data:)
static char* get_policy_string(void)
{
	int policy_str_len_max = LARGE_BUFFER_SIZE;
	int temp_len_max = SMALL_BUFFER_SIZE;
	char *policy_str = (char*) kzalloc(policy_str_len_max, GFP_KERNEL);
	char *temp = (char*) kzalloc(temp_len_max,GFP_KERNEL);
	int policy_str_len;
	int temp_len;
	struct bf_filter_rule* rule;

	rule = all_filters.filters_list_head;
	while (rule != NULL) {
		temp_len = strlen(rule->message) + strlen(rule->data);
		if (temp_len > temp_len_max) {
			temp = bf_realloc(temp, temp_len * 2);
			temp_len_max = temp_len * 2;
		}

		if (rule->context > 0) {
			if (rule->context_type == CONTEXT_TYPE_INT) {
				sprintf(temp, "%s:%d:%d:%d:%d:%d:", 
					rule->message, rule->uid, rule->block_or_modify, 
					rule->context, rule->context_type, rule->context_int_value);
			} else {
				sprintf(temp, "%s:%d:%d:%d:%d:%s:", 
					rule->message, rule->uid, rule->block_or_modify, 
					rule->context, rule->context_type, rule->context_string_value);
			}
		} else {
			sprintf(temp, "%s:%d:%d:%d:", 
				rule->message, rule->uid, rule->block_or_modify, rule->context);
		}

		if (strlen(rule->data) <= 0) {
			strcat(temp, "\n");
		} else {
			strcat(temp, rule->data);
			strcat(temp, ":\n");
		}

		policy_str_len = strlen(temp) + strlen(policy_str);
		if (policy_str_len > policy_str_len_max) {
			policy_str = bf_realloc(policy_str, policy_str_len * 2);
			policy_str_len_max = policy_str_len * 2;
		}

		strcat(policy_str, temp);
		rule = rule->next;
	}

	kfree(temp);

	return policy_str;
}

static void init_context_values(void) 
{
	context_values.bluetooth_enabled = CONTEXT_STATE_UNKNOWN;	
	context_values.wifi_enabled = CONTEXT_STATE_UNKNOWN;	
}

static void write_persistent_policy(void) 
{
	char* policy = get_policy_string();
	//printk(KERN_INFO "BINDERFILTER: writing policy: {%s}\n", policy);
	write_file("/data/local/tmp/bf.policy", policy);
	kfree(policy);
}

static int bf_open(struct inode *nodp, struct file *filp)
{
	printk(KERN_INFO "BINDERFILTER: opened driver\n");
	return 0;
}

// reports policy info
static ssize_t bf_read(struct file * file, char * buf, size_t count, loff_t *ppos)
{	
	int len;
	char* ret_str;

	ret_str = get_policy_string();
	len = strlen(ret_str); /* Don't include the null byte. */
    
    if (count < len) {
        return -EINVAL;
    }

    if (*ppos != 0) {
        return 0;
    }

    if (copy_to_user(buf, ret_str, len)) {
        return -EINVAL;
    }

    kfree(ret_str);

    *ppos = len;
    return len;
}

static ssize_t bf_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
	struct bf_user_filter* user_filter;

	if (len < 0 || len > 4096 || buf == NULL) {
		printk(KERN_INFO "BINDERFILTER: bf_write bad arguments\n");
		return 0;
	}

	user_filter = init_bf_user_filter();
	if (copy_from_user(user_filter, buf, sizeof(struct bf_user_filter))) {
		printk(KERN_INFO "BINDERFILTER: bf_write copy_from_user failed\n");
		return 0;
	}

	switch (user_filter->action) {
		case BLOCK_ACTION:
			add_filter(user_filter);
			break;
		case UNBLOCK_ACTION:
			remove_filter(user_filter);
			break;
		case MODIFY_ACTION:
			add_filter(user_filter);
			break;
		case UNMODIFY_ACTION:
			remove_filter(user_filter);
			break;
		default:
			printk(KERN_INFO "BINDERFILTER: bf_write bad action %d\n", 
				user_filter->action);
			return 0;
	}

	write_persistent_policy();
	kfree(user_filter);
    return sizeof(struct bf_user_filter);
}

static const struct file_operations bf_fops = {
	.owner = THIS_MODULE,
	.open = bf_open,
	.read = bf_read,
	.write = bf_write,
};

static struct miscdevice bf_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "binderfilter",
	.fops = &bf_fops
};

static int __init binder_filter_init(void)
{
	int ret;

#ifdef BF_SEQ_FILE_OUTPUT
	int i;
#endif

	bf_debugfs_dir_entry_root = debugfs_create_dir("binder_filter", NULL);
	if (bf_debugfs_dir_entry_root == NULL) {
		printk(KERN_INFO "BINDERFILTER: could not create debugfs entry!");
		return 1;
	}
	debugfs_create_file("context_values",
			    S_IRUGO,
			    bf_debugfs_dir_entry_root,
			    NULL,
			    &bf_context_values_fops);
	debugfs_create_file("do_debug",
			    S_IRUGO,
			    bf_debugfs_dir_entry_root,
			    NULL,
			    &bf_do_debug_fops);

#ifdef BF_SEQ_FILE_OUTPUT
	debugfs_create_file("buffers",
			    S_IRUGO,
			    bf_debugfs_dir_entry_root,
			    &bf_buffer_log_instance,
			    &bf_buffers_fops);

	for (i=0; i<BUFFER_LOG_SIZE; i++) {
		bf_buffer_log_instance.entry[i].addr = 0;
		bf_buffer_log_instance.entry[i].set = 0;
	}

	numAdded = 0;
#endif

	ret = misc_register(&bf_miscdev);
	if (ret) {
		printk(KERN_INFO "BINDERFILTER: unable to register device, ret %d\n", ret);
		return ret;
	}

	// battery_level.level_value_no_BT = 42;
	// battery_level.level_value_with_BT = 43;

	init_context_values();

	printk(KERN_INFO "BINDERFILTER: started.\n");
	return 0;
}

device_initcall(binder_filter_init);

MODULE_AUTHOR("David Wu <davidxiaohanwu@gmail.com>");
MODULE_DESCRIPTION("binder filter");
MODULE_LICENSE("GPL v2");
