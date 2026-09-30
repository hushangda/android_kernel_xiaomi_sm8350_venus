// SPDX-License-Identifier: GPL-2.0-only
/* Actual driver functions are appended by run_host_tests.sh. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;
typedef s64 ktime_t;

#define MAX_STR_LEN 128
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define min_t(t, a, b) min((t)(a), (t)(b))
#define clamp_val(v, lo, hi) min(max(v, lo), hi)
#define DIV_ROUND_CLOSEST(v, d) (((v) + (d) / 2) / (d))
#define div_s64(a, b) ((s64)(a) / (b))
#define div64_u64(a, b) ((u64)(a) / (b))
#define scnprintf snprintf
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
#define msecs_to_jiffies(x) (x)
#define system_wq NULL
#define container_of(p, type, member) \
	((type *)((char *)(p) - offsetof(type, member)))
#define to_delayed_work(p) container_of(p, struct delayed_work, work)

enum power_supply_property {
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX,
	PROP_COUNT,
};
enum {
	POWER_SUPPLY_STATUS_UNKNOWN,
	POWER_SUPPLY_STATUS_CHARGING,
	POWER_SUPPLY_STATUS_DISCHARGING,
	POWER_SUPPLY_STATUS_NOT_CHARGING,
	POWER_SUPPLY_STATUS_FULL,
};
enum { PSY_TYPE_BATTERY, PSY_TYPE_XM };
#define BATT_CAPACITY POWER_SUPPLY_PROP_CAPACITY
#define BATT_STATUS POWER_SUPPLY_PROP_STATUS
#define BATT_CURR_NOW POWER_SUPPLY_PROP_CURRENT_NOW
#define BATT_CHG_FULL POWER_SUPPLY_PROP_CHARGE_FULL
#define BATT_CHG_FULL_DESIGN POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN
#define XM_PROP_FG_RM 0

struct work_struct { int unused; };
struct delayed_work { struct work_struct work; u32 pending_ms; };
struct power_supply { void *data; };
union power_supply_propval { int intval; const char *strval; };
struct psy_state {
	u32 prop[PROP_COUNT], remote[PROP_COUNT];
	int error[PROP_COUNT], reads[PROP_COUNT];
	char *model;
	struct power_supply *psy;
};
struct battery_chg_dev {
	struct psy_state psy_list[2];
	u32 battery_design_uah, battery_full_uah;
	bool battery_profile_valid, battery_expanded, initialized;
	bool ui_soc_valid, ui_soc_from_gauge;
	int ui_soc, ui_soc_target;
	int ui_soc_lock, fake_soc, curr_thermal_level, num_thermal_levels;
	ktime_t ui_soc_update_time;
	struct delayed_work ui_soc_work;
};
static struct battery_chg_dev dev;
static struct power_supply battery = { .data = &dev };
static char model[MAX_STR_LEN];
static ktime_t now_ms;
static int notifications;

static ktime_t ktime_get_boottime(void) { return now_ms; }
static s64 ktime_ms_delta(ktime_t a, ktime_t b) { return a - b; }
static void cancel_delayed_work(struct delayed_work *w)
{
	w->pending_ms = 0;
}
static void mod_delayed_work(void *q, struct delayed_work *w, u32 ms)
{
	w->pending_ms = ms;
}
static void *power_supply_get_drvdata(struct power_supply *p) { return p->data; }
static void power_supply_changed(struct power_supply *p) { notifications++; }
static int read_property_id(struct battery_chg_dev *d, struct psy_state *p,
			    unsigned int id)
{
	assert(id < PROP_COUNT);
	p->reads[id]++;
	if (!p->error[id])
		p->prop[id] = p->remote[id];
	return p->error[id];
}
static int get_property_id(struct psy_state *p, enum power_supply_property id)
{
	return id;
}
static int battery_psy_get_prop(struct power_supply *, enum power_supply_property,
				union power_supply_propval *);
static void battery_chg_ui_soc_work(struct work_struct *);
static void battery_chg_restart_ui_soc(struct battery_chg_dev *);
static u32 battery_chg_ui_soc_step_ms(struct battery_chg_dev *, struct psy_state *);

static int get(enum power_supply_property p)
{
	union power_supply_propval value;

	assert(battery_psy_get_prop(&battery, p, &value) == 0);
	return value.intval;
}

static void reset(u32 full, int status, int initial_soc)
{
	struct psy_state *b;

	memset(&dev, 0, sizeof(dev));
	now_ms = 0;
	notifications = 0;
	dev.initialized = true;
	dev.fake_soc = -EINVAL;
	dev.battery_design_uah = VENUS_BATTERY_DESIGN_UAH;
	b = &dev.psy_list[PSY_TYPE_BATTERY];
	b->model = model;
	b->psy = &battery;
	b->remote[BATT_CHG_FULL_DESIGN] = VENUS_BATTERY_DESIGN_UAH;
	b->remote[BATT_CHG_FULL] = full;
	b->remote[BATT_CURR_NOW] = full; /* 1 C: 36 seconds per percent. */
	b->remote[BATT_STATUS] = status;
	b->remote[BATT_CAPACITY] = initial_soc * 100;
	b->remote[POWER_SUPPLY_PROP_CHARGE_COUNTER] = 123456;
	dev.psy_list[PSY_TYPE_XM].remote[XM_PROP_FG_RM] = full * initial_soc / 100;
}

static void test_pack(u32 full)
{
	struct psy_state *b = &dev.psy_list[PSY_TYPE_BATTERY];
	struct psy_state *xm = &dev.psy_list[PSY_TYPE_XM];
	union power_supply_propval value;
	u32 initial_rm = full * 80 / 100;
	char expected_model[MAX_STR_LEN];

	reset(full, POWER_SUPPLY_STATUS_DISCHARGING, 80);
	assert(get(POWER_SUPPLY_PROP_CHARGE_FULL) == (int)full);
	assert(get(POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN) == (int)full);
	assert(dev.battery_expanded == (full >= 4700000));
	snprintf(expected_model, sizeof(expected_model), "k2_%umah", full / 1000);
	assert(!battery_psy_get_prop(&battery, POWER_SUPPLY_PROP_MODEL_NAME, &value));
	assert(!strcmp(value.strval, expected_model));
	assert(get(POWER_SUPPLY_PROP_CHARGE_COUNTER) == (int)initial_rm);
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 80);
	assert(xm->reads[XM_PROP_FG_RM] > 0);

	/* Ignore a remote-SOC recalculation when measured remaining charge holds. */
	b->remote[BATT_CAPACITY] = 3000;
	now_ms = 36000;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 80);
	xm->remote[XM_PROP_FG_RM] = initial_rm - full / 100;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 79);

	/* A one-percent update must wait, including for the stock 4600 mAh pack. */
	b->remote[BATT_CAPACITY] = 7800;
	xm->remote[XM_PROP_FG_RM] = initial_rm - 2 * (full / 100);
	now_ms = 36001;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 79);
	assert(dev.ui_soc_work.pending_ms == 35999);
	now_ms = 72000;
	notifications = 0;
	battery_chg_ui_soc_work(&dev.ui_soc_work.work);
	assert(dev.ui_soc == 78 && notifications == 1);

	/* A gauge correction must converge even while status says discharging. */
	xm->remote[XM_PROP_FG_RM] = initial_rm;
	now_ms = 108000;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 79);
	assert(dev.ui_soc_target == 80);
	now_ms = 144000;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 80);

	/* Failed/out-of-range RM uses the existing guarded remote fallback. */
	xm->error[XM_PROP_FG_RM] = -EIO;
	b->remote[BATT_CAPACITY] = 7700;
	assert(get(POWER_SUPPLY_PROP_CHARGE_COUNTER) == 123456);
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 80);
	now_ms = 180000;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 79);
	xm->error[XM_PROP_FG_RM] = 0;
	xm->remote[XM_PROP_FG_RM] = VENUS_BATTERY_MAX_RM_UAH + 1;
	assert(get(POWER_SUPPLY_PROP_CHARGE_COUNTER) == 123456);
	xm->remote[XM_PROP_FG_RM] = full / 2;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 79);
	assert(dev.ui_soc_target == 50);

	/* A remote restart preserves the display but cannot preserve an offset. */
	battery_chg_restart_ui_soc(&dev);
	b->remote[BATT_CAPACITY] = 2000;
	xm->remote[XM_PROP_FG_RM] = full / 3;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 79);
	assert(dev.ui_soc_target == 33);
	now_ms += 36000;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 78);

	/* Charging status cannot suppress a real decrease or gauge correction. */
	reset(full, POWER_SUPPLY_STATUS_DISCHARGING, 50);
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 50);
	now_ms = 120000;
	b->remote[BATT_STATUS] = POWER_SUPPLY_STATUS_CHARGING;
	b->remote[BATT_CAPACITY] = 5100;
	xm->remote[XM_PROP_FG_RM] += full / 100;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 51);
	now_ms += 36000;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 51);
	xm->remote[XM_PROP_FG_RM] -= 2 * (full / 100);
	now_ms += 36000;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 50);
	now_ms += 36000;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 49);

	/* Charger-confirmed full reaches 100 for either type of pack. */
	reset(full, POWER_SUPPLY_STATUS_CHARGING, 99);
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 99);
	b->remote[BATT_STATUS] = POWER_SUPPLY_STATUS_FULL;
	xm->remote[XM_PROP_FG_RM] = full;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 100);
	assert(dev.ui_soc_target == 100);
	printf("PASS battery: %u mAh identity, RM, smoothing, counter, fallback, SSR, full\n",
	       full / 1000);
}

static void test_stale_soc(void)
{
	struct psy_state *b = &dev.psy_list[PSY_TYPE_BATTERY];
	struct psy_state *xm = &dev.psy_list[PSY_TYPE_XM];
	int status, expected;

	/* Live regression: nearly full RM must not inherit a stale 75% anchor. */
	reset(4800000, POWER_SUPPLY_STATUS_CHARGING, 75);
	xm->remote[XM_PROP_FG_RM] = 4739000;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 99);
	assert(dev.ui_soc_target == 99 && dev.ui_soc_from_gauge);
	xm->remote[XM_PROP_FG_RM] = 4736000;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 99);
	b->remote[BATT_STATUS] = POWER_SUPPLY_STATUS_FULL;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 100);

	/* Late gauge initialization replaces a boot-time remote-SOC fallback. */
	reset(4800000, POWER_SUPPLY_STATUS_CHARGING, 75);
	xm->remote[XM_PROP_FG_RM] = 0;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 75);
	assert(!dev.ui_soc_from_gauge);
	xm->remote[XM_PROP_FG_RM] = 4739000;
	now_ms = 1;
	battery_chg_ui_soc_work(&dev.ui_soc_work.work);
	assert(dev.ui_soc == 99 && notifications == 1);

	/* A later measurement correction converges, in either charger state. */
	for (status = POWER_SUPPLY_STATUS_CHARGING;
	     status <= POWER_SUPPLY_STATUS_DISCHARGING; status++) {
		reset(4800000, status, 75);
		b->remote[BATT_CURR_NOW] = 101000;
		assert(get(POWER_SUPPLY_PROP_CAPACITY) == 75);
		xm->remote[XM_PROP_FG_RM] = 4739000;
		now_ms = 1;
		assert(get(POWER_SUPPLY_PROP_CAPACITY) == 75);
		assert(dev.ui_soc_target == 99);
		for (expected = 76; expected <= 99; expected++) {
			now_ms += 120000;
			battery_chg_ui_soc_work(&dev.ui_soc_work.work);
			assert(dev.ui_soc == expected);
		}
		assert(dev.ui_soc_work.pending_ms == 0);
	}

	/* The worker must refresh FULL without requiring a separate sysfs read. */
	b->remote[BATT_CHG_FULL] = 6000000;
	now_ms += 120000;
	battery_chg_ui_soc_work(&dev.ui_soc_work.work);
	assert(dev.battery_full_uah == 6000000);
	assert(dev.ui_soc_target == 79 && dev.ui_soc == 98);

	reset(4800000, POWER_SUPPLY_STATUS_DISCHARGING, 0);
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 0 && dev.ui_soc_from_gauge);
	reset(4800000, POWER_SUPPLY_STATUS_CHARGING, 75);
	xm->remote[XM_PROP_FG_RM] = 4800001;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 100);
	reset(4800000, POWER_SUPPLY_STATUS_CHARGING, 75);
	xm->remote[XM_PROP_FG_RM] = VENUS_BATTERY_MAX_RM_UAH + 1;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 75 && !dev.ui_soc_from_gauge);
	puts("PASS battery: live 75/99 regression, late gauge, convergence, fresh FULL, bounds");
}

int main(void)
{
	struct psy_state *b = &dev.psy_list[PSY_TYPE_BATTERY];

	test_stale_soc();
	test_pack(4600000);
	test_pack(4800000);
	test_pack(6000000);
	/* An aged stock pack retains its design capacity and uses its learned full. */
	reset(4300000, POWER_SUPPLY_STATUS_DISCHARGING, 80);
	assert(get(POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN) == 4600000);
	assert(get(POWER_SUPPLY_PROP_CHARGE_FULL) == 4300000);
	assert(!dev.battery_expanded);
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 80);
	dev.psy_list[PSY_TYPE_XM].remote[XM_PROP_FG_RM] -= 43000;
	now_ms = 36000;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 79);
	b->prop[BATT_CURR_NOW] = (u32)-4300000;
	assert(battery_chg_ui_soc_step_ms(&dev, b) == 36000);
	b->prop[BATT_CURR_NOW] = 0;
	assert(battery_chg_ui_soc_step_ms(&dev, b) == 120000);
	b->prop[BATT_CURR_NOW] = 100000000;
	assert(battery_chg_ui_soc_step_ms(&dev, b) == 5000);
	dev.fake_soc = 42;
	assert(get(POWER_SUPPLY_PROP_CAPACITY) == 42 && !dev.ui_soc_valid);
	puts("PASS battery: stock aging, signed current, rate bounds and fake-SOC reset");
	return 0;
}
