#!/usr/bin/env python3
"""Run the real board producer against temporary sensor files under sanitizers."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[4]
source = root / "tools/thermal/venus-board-temp/venus-board-temp.c"
config = root / "tools/thermal/venus-board-temp/venus-board-temp.conf"
test = r'''
#define VENUS_BOARD_TEMP_TEST
#include "PRODUCER_SOURCE"
#include <assert.h>
#include <sys/stat.h>
static void file(const char *path,const char *text) {
 FILE *f=fopen(path,"w");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));
}
int main(int argc,char **argv) {
 assert(argc==2);assert(!chdir(argv[1]));
 int n=9;assert(!parse_int("-123\n",&n)&&n==-123);
 assert(parse_int("",&n)&&parse_int("1x",&n)&&parse_int("99999999999999",&n));
 struct model m;assert(!read_model("MODEL_CONFIG",&m));assert(m.count==6&&m.divisor==1000&&m.offset==100&&m.interval==10000);
 const int temps[]={31882,32160,30955,32396,33454,27000};
 char p[PATH_MAX],v[80];
 for(int i=0;i<6;i++) {
  snprintf(p,sizeof(p),"thermal_zone%d",i);assert(!mkdir(p,0700));
  snprintf(p,sizeof(p),"thermal_zone%d/type",i);file(p,m.sensors[i].name);
  snprintf(p,sizeof(p),"thermal_zone%d/temp",i);snprintf(v,sizeof(v),"%d\n",temps[i]);file(p,v);
 }
 assert(!discover(&m,"."));
 assert(!sample(&m,"missing-comp",&n));
 long long sum=0;for(int i=0;i<6;i++)sum+=(long long)temps[i]*m.sensors[i].weight;
 assert(n==sum/1000+100);printf("real model sample: %d mC\n",n);
 file("comp","300\n");assert(!sample(&m,"comp",&n)&&n==sum/1000+400);
 for(int i=0;i<6;i++) {
  snprintf(p,sizeof(p),"thermal_zone%d/temp",i);
  const char *bad[]={"0\n","-40000\n","125000\n","garbage","99999999999999999\n",""};
  for(unsigned j=0;j<sizeof(bad)/sizeof(bad[0]);j++){file(p,bad[j]);n=987;assert(sample(&m,"comp",&n)<0&&n==987);}
  snprintf(v,sizeof(v),"%d\n",temps[i]);file(p,v);
 }
 file("comp","invalid");assert(sample(&m,"comp",&n)<0);
 file("comp","50000");assert(sample(&m,"comp",&n)==-ERANGE);
 assert(!mkdir("thermal_zone99",0700));file("thermal_zone99/type",m.sensors[0].name);
 assert(discover(&m,".")==-EEXIST);assert(!unlink("thermal_zone99/type"));assert(!rmdir("thermal_zone99"));
 assert(!unlink("thermal_zone0/type"));assert(discover(&m,".")==-ENOENT);
 const char *badmodel[]={
  "divisor 0\noffset_mc 0\ninterval_ms 1000\nsensor a 1\n",
  "divisor 1000\noffset_mc 0\ninterval_ms 1000\nsensor a 1 bad\n",
  "divisor 1000\noffset_mc 0\ninterval_ms 1000\nsensor a 1\nsensor a 2\n",
  "divisor 1000\noffset_mc 0\ninterval_ms 1000\nsensor ../a 1\n",
  "divisor 1000\noffset_mc 0\ninterval_ms 1000\nsensor a 10001\n",
  "divisor 1000\ndivisor 1000\noffset_mc 0\ninterval_ms 1000\nsensor a 1\n",
  "divisor 1000\noffset_mc 0\ninterval_ms 999\nsensor a 1\n",
  "divisor 1000\noffset_mc 0\ninterval_ms 60000\nsensor a 1\n",
  "offset_mc 0\ninterval_ms 1000\nsensor a 1\n"};
 for(unsigned j=0;j<sizeof(badmodel)/sizeof(badmodel[0]);j++){file("bad-model",badmodel[j]);assert(read_model("bad-model",&m)<0);}
 file("output","");assert(!publish("output","12345\n"));assert(!read_int("output",&n)&&n==12345);
 assert(publish("absent-dir/output","123\n")==-ENOENT);
 stopping=1;delay_ms(1000);stop_handler(SIGTERM);
 puts("PASS actual thermal producer: stock formula, discovery, duplicate/missing sensors, invalid readings, optional offsets, strict config, write errors");
 return 0;
}
'''.replace("PRODUCER_SOURCE", str(source)).replace("MODEL_CONFIG", str(config))
with tempfile.TemporaryDirectory(prefix="venus-thermal-producer-") as temporary:
    path = pathlib.Path(temporary)
    (path / "test.c").write_text(test)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                    "-fsanitize=address,undefined", "-no-pie", str(path / "test.c"),
                    "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test"), str(path)], check=True)
