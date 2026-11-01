#!/usr/bin/env python3
"""Actual thermal getter/MiCharge show: backend data and errors, not fake zero."""
import pathlib
import subprocess
import tempfile
from scoped_reclaim_test import function

ROOT = pathlib.Path(__file__).resolve().parents[4]
thermal = (ROOT / "drivers/thermal/thermal_core.c").read_text()
thermal = thermal.replace("static ssize_t\nthermal_", "static ssize_t thermal_")
battery = (ROOT / "drivers/power/supply/qti_battery_charger_venus.c").read_text()
prelude = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/types.h>
#include <stdint.h>
#define PAGE_SIZE 4096
#define scnprintf snprintf
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
struct class {int unused;};struct class_attribute {int unused;};
struct device {int unused;};struct device_attribute {int unused;};
static pthread_mutex_t thermal_message_lock=PTHREAD_MUTEX_INITIALIZER;
static char board_sensor_temp[128];
static uint64_t now,board_sensor_updated_ns;
static int board_sensor_error=-ENODATA;
#define BOARD_SENSOR_MAX_AGE_NS 30000000000ULL
static uint64_t ktime_get_boottime_ns(void) {return now;}
/* Linux's independently provided integer parser is an environmental stub. */
static int kstrtoint(const char *s,int base,int *out) {
 char *end;errno=0;long value=strtol(s,&end,base);
 if(s==end || (*end && !(*end=='\n' && !end[1])))return -EINVAL;
 if(errno || value<INT_MIN || value>INT_MAX)return -ERANGE;
 *out=value;return 0;
}
'''
tests = r'''
int main(void) {
 char buf[PAGE_SIZE];int temp=123;
 assert(thermal_message_get_board_temp(NULL)==-EINVAL);
 assert(thermal_message_get_board_temp(&temp)==-ENODATA && temp==123);
 assert(board_sensor_temp_show(NULL,NULL,buf)==-ENODATA);
 assert(thermal_board_sensor_temp_store(NULL,NULL,"29765\n",6)==6);
 assert(thermal_message_get_board_temp(&temp)==0 && temp==29765);
 assert(board_sensor_temp_show(NULL,NULL,buf)==6 && !strcmp(buf,"29765\n"));
 assert(thermal_board_sensor_temp_show(NULL,NULL,buf)==6 && !strcmp(buf,"29765\n"));
 now=BOARD_SENSOR_MAX_AGE_NS;assert(!thermal_message_get_board_temp(&temp));
 now++;temp=999;assert(thermal_message_get_board_temp(&temp)==-ENODATA && temp==999);
 assert(thermal_board_sensor_temp_show(NULL,NULL,buf)==-ENODATA);
 assert(board_sensor_temp_show(NULL,NULL,buf)==-ENODATA);
 assert(thermal_board_sensor_temp_store(NULL,NULL,"-1250\n",6)==6);
 assert(board_sensor_temp_show(NULL,NULL,buf)==6 && !strcmp(buf,"-1250\n"));
 assert(thermal_board_sensor_temp_store(NULL,NULL,"0\n",2)==2);
 assert(board_sensor_temp_show(NULL,NULL,buf)==2 && !strcmp(buf,"0\n"));
 assert(thermal_board_sensor_temp_store(NULL,NULL,"bad",3)==3);
 assert(board_sensor_temp_show(NULL,NULL,buf)==-EINVAL);
 assert(thermal_board_sensor_temp_store(NULL,NULL,"99999999999999999999",20)==20);
 assert(board_sensor_temp_show(NULL,NULL,buf)==-ERANGE);
 assert(thermal_board_sensor_temp_store(NULL,NULL,"1\0x",3)==3);
 assert(board_sensor_temp_show(NULL,NULL,buf)==-EINVAL);
 assert(thermal_board_sensor_temp_store(NULL,NULL,"",0)==0);
 assert(board_sensor_temp_show(NULL,NULL,buf)==-ENODATA);
 assert(thermal_board_sensor_temp_store(NULL,NULL,buf,128)==-E2BIG);
 puts("PASS actual board sample store/read and MiCharge relay: errors, real zero, mC, 30s expiry, resume and recovery");
}
'''
with tempfile.TemporaryDirectory(prefix="venus-board-relay-") as temporary:
    path = pathlib.Path(temporary)
    (path / "test.c").write_text(prelude + function(thermal, "thermal_board_sensor_temp_store") +
                                function(thermal, "thermal_message_get_board_temp") +
                                function(thermal, "thermal_board_sensor_temp_show") +
                                function(battery, "board_sensor_temp_show") + tests)
    subprocess.run(["clang", "-std=gnu11", "-g", "-fsanitize=address,undefined", "-no-pie", "-pthread",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
