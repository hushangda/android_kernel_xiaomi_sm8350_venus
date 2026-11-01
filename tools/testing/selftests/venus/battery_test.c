// SPDX-License-Identifier: GPL-2.0-only
/* Actual current stock-gauge getter/charge-counter functions are appended. */
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
typedef uint32_t u32;
#define PAGE_SIZE 4096
#define scnprintf snprintf
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
enum power_supply_property {
 POWER_SUPPLY_PROP_CAPACITY, POWER_SUPPLY_PROP_STATUS,
 POWER_SUPPLY_PROP_CURRENT_NOW, POWER_SUPPLY_PROP_CHARGE_FULL,
 POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, POWER_SUPPLY_PROP_MODEL_NAME,
 POWER_SUPPLY_PROP_CHARGE_COUNTER, POWER_SUPPLY_PROP_TEMP,
 POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT,
 POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX, PROP_COUNT
};
enum { PSY_TYPE_BATTERY };
struct class { int unused; };
struct class_attribute { int unused; };
struct power_supply { void *data; };
union power_supply_propval { int intval; const char *strval; };
struct psy_state {
 u32 prop[PROP_COUNT], remote[PROP_COUNT];
 int error[PROP_COUNT], reads[PROP_COUNT];
 char *model;
 struct power_supply *psy;
};
struct battery_chg_dev {
 struct psy_state psy_list[1];
 struct class battery_class;
 int fake_soc, curr_thermal_level, num_thermal_levels;
};
static int battery_psy_get_prop(struct power_supply *, enum power_supply_property,
 union power_supply_propval *);
static ssize_t charge_counter_show(struct class *, struct class_attribute *, char *);
static void *power_supply_get_drvdata(struct power_supply *p) { return p->data; }
static int get_property_id(struct psy_state *p, enum power_supply_property id)
{
 (void)p; return (unsigned)id < PROP_COUNT ? (int)id : -EINVAL;
}
static int read_property_id(struct battery_chg_dev *d, struct psy_state *p, unsigned id)
{
 (void)d; assert(id < PROP_COUNT); p->reads[id]++;
 if (!p->error[id]) p->prop[id] = p->remote[id];
 return p->error[id];
}
static int power_supply_get_property(struct power_supply *p, enum power_supply_property id,
 union power_supply_propval *v) { return battery_psy_get_prop(p, id, v); }
int main(void)
{
 struct battery_chg_dev d = { .fake_soc = -EINVAL };
 struct power_supply battery = { .data = &d };
 struct psy_state *p = &d.psy_list[PSY_TYPE_BATTERY];
 union power_supply_propval v;
 char model[] = "actual-gauge-model", buf[PAGE_SIZE];
 p->psy = &battery; p->model = model;
 for (unsigned n = 0; n <= 10000; n += 25) {
  p->remote[POWER_SUPPLY_PROP_CAPACITY] = n;
  assert(!battery_psy_get_prop(&battery, POWER_SUPPLY_PROP_CAPACITY, &v));
  assert(v.intval == (int)n / 100);
 }
 for (int fake = 0; fake <= 100; fake += 20) {
  d.fake_soc = fake;
  assert(!battery_psy_get_prop(&battery, POWER_SUPPLY_PROP_CAPACITY, &v));
  assert(v.intval == fake);
 }
 d.fake_soc = 101;
 assert(!battery_psy_get_prop(&battery, POWER_SUPPLY_PROP_CAPACITY, &v) && v.intval == 100);
 d.fake_soc = -EINVAL;
 assert(!battery_psy_get_prop(&battery, POWER_SUPPLY_PROP_MODEL_NAME, &v) && v.strval == model);
 p->remote[POWER_SUPPLY_PROP_TEMP] = (u32)-123;
 assert(!battery_psy_get_prop(&battery, POWER_SUPPLY_PROP_TEMP, &v) && v.intval == -12);
 p->remote[POWER_SUPPLY_PROP_TEMP] = 345;
 assert(!battery_psy_get_prop(&battery, POWER_SUPPLY_PROP_TEMP, &v) && v.intval == 34);
 d.curr_thermal_level = 3; d.num_thermal_levels = 12;
 assert(!battery_psy_get_prop(&battery, POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT, &v) && v.intval == 3);
 assert(!battery_psy_get_prop(&battery, POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX, &v) && v.intval == 12);
 for (unsigned id = POWER_SUPPLY_PROP_CURRENT_NOW; id <= POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN; id++) {
  p->remote[id] = 6200123;
  assert(!battery_psy_get_prop(&battery, id, &v) && v.intval == 6200123);
 }
 for (unsigned n = 0; n < 2; n++) {
  p->remote[POWER_SUPPLY_PROP_CHARGE_COUNTER] = n ? 4752000 : 0;
  assert(!battery_psy_get_prop(&battery, POWER_SUPPLY_PROP_CHARGE_COUNTER, &v));
  int expected = v.intval;
  ssize_t length = charge_counter_show(&d.battery_class, NULL, buf);
  char want[32]; int want_length = snprintf(want, sizeof(want), "%d\n", expected);
  assert(expected == (int)p->remote[POWER_SUPPLY_PROP_CHARGE_COUNTER]);
  assert(length == want_length && !strcmp(buf, want));
 }
 p->error[POWER_SUPPLY_PROP_CHARGE_COUNTER] = -EIO;
 strcpy(buf, "unchanged");
 assert(charge_counter_show(&d.battery_class, NULL, buf) == -EIO && !strcmp(buf, "unchanged"));
 p->error[POWER_SUPPLY_PROP_CAPACITY] = -ETIMEDOUT;
 assert(battery_psy_get_prop(&battery, POWER_SUPPLY_PROP_CAPACITY, &v) == -ETIMEDOUT);
 assert(battery_psy_get_prop(&battery, PROP_COUNT, &v) == -EINVAL);
 puts("PASS battery: real gauge SOC/model/units, signed temperature, thermal limits, charge-counter class parity including zero and error propagation");
 return 0;
}
