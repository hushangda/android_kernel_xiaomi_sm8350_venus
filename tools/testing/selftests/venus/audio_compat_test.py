#!/usr/bin/env python3
"""Exercise the real ADM event repacker; optionally compile platform variants."""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[4]
ADM = Path("techpack/audio/dsp/q6adm.c")
PCM = Path("techpack/audio/asoc/msm-pcm-q6-v2.c")
CHARGER = Path("drivers/power/supply/qti_battery_charger_venus.c")


def event_test(source, out, name, should_fail=False):
    start = source.index("\t\t\tpayload_size = data->payload_size;")
    end = source.index("\t\t\tif (this_adm.copp.cb)", start)
    fragment = source[start:end]
    header = (ROOT / "techpack/audio/include/uapi/audio/linux/msm_audio.h").read_text()
    event = re.search(r"struct msm_adsp_event_data \{.*?\n\};", header, re.S)[0]
    header = (ROOT / "techpack/audio/include/dsp/apr_audio-v2.h").read_text()
    user_info = re.search(r"struct adm_usr_info \{.*?\n\};", header, re.S)[0]
    code = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t __u32;
typedef uint8_t __u8;
typedef uint32_t u32;
#define GFP_ATOMIC 0
#define ADSP_ADM_SERVICE_ID 3
#define kzalloc(n, flags) calloc(1, (n))
''' + event + "\n" + user_info + r'''
struct apr_client_data {
    uint16_t payload_size;
    uint32_t token;
    void *payload;
};
static int check_packet(struct apr_client_data *data)
{
    int payload_size, ret = 0;
    struct msm_adsp_event_data *pp_event_package;
    struct adm_usr_info usr_data = {0}, copied;
''' + fragment + r'''
    assert(pp_event_package->event_type == ADSP_ADM_SERVICE_ID);
    assert(pp_event_package->payload_len == data->payload_size + sizeof(usr_data));
    memcpy(&copied, pp_event_package->payload, sizeof(copied));
    assert(copied.service_id == ADSP_ADM_SERVICE_ID);
    assert(copied.token_coppidx == data->token);
    assert(copied.reserved == 0);
    assert(!memcmp(pp_event_package->payload + sizeof(copied),
                   data->payload, data->payload_size));
    free(pp_event_package);
    return 0;
}
int main(void)
{
    const uint16_t sizes[] = {8, 16, 512, 4096, UINT16_MAX};
    unsigned char payload[UINT16_MAX];
    struct apr_client_data data = {.token = 0x120003, .payload = payload};
    for (unsigned int i = 0; i < sizeof(payload); i++)
        payload[i] = i;
    for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        data.payload_size = sizes[i];
        assert(check_packet(&data) == 0);
    }
    return 0;
}
'''
    cfile = out / (name + ".c")
    binary = out / name
    cfile.write_text(code)
    subprocess.run(["clang", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", str(cfile), "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    (out / (name + ".log")).write_text(result.stdout + result.stderr)
    if should_fail:
        assert result.returncode and "heap-buffer-overflow" in result.stderr
        print("PASS: original ADM repacker reproduces heap-buffer-overflow")
    else:
        assert result.returncode == 0, result.stderr
        print("PASS: ADM repacker preserves header/payload for 8..65535 bytes under ASan/UBSan")


def compile_objects(build, out):
    # Replay Kbuild's recorded command with separate outputs/dependencies. The
    # original build remains untouched, including its manifest and objects.
    for source, variant, defines in [
        (PCM, "phone", []),
        (PCM, "auto", ["-DCONFIG_PLATFORM_AUTO=1"]),
        (PCM, "gvm", ["-DCONFIG_QTI_QUIN_GVM=1"]),
        (ADM, "adm", []),
        (CHARGER, "charger", []),
    ]:
        obj = source.with_suffix(".o")
        recorded = build / obj.parent / ("." + obj.name + ".cmd")
        command = shlex.split(recorded.read_text().splitlines()[0].split(":=", 1)[1])
        target = out / (variant + ".o")
        command[command.index("-o") + 1] = str(target)
        command = [arg for arg in command if not arg.startswith("-Wp,-MMD,")]
        command[1:1] = defines
        (out / (variant + "-command.json")).write_text(json.dumps(command, indent=2))
        result = subprocess.run(command, cwd=build, capture_output=True, text=True)
        (out / (variant + "-compile.log")).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stderr
        if source == PCM:
            symbols = subprocess.check_output(["llvm-nm", "--undefined-only", str(target)], text=True)
            (out / (variant + "-undefined.txt")).write_text(symbols)
            for verb in ("init", "clean"):
                assert f"msm_adsp_{verb}_mixer_ctl_pp_event_queue" in symbols
                adm = f"msm_adsp_{verb}_mixer_ctl_adm_pp_event_queue"
                assert (adm in symbols) == bool(defines), (variant, adm)
        print(f"PASS: ARM64 {variant} object and platform-specific event calls")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--old-revision", help="also reproduce the old overflow from this git revision")
    parser.add_argument("--build-dir", type=Path, help="configured Venus output with existing Kbuild .cmd files")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        out = args.output_dir.resolve()
    else:
        out = Path(tempfile.mkdtemp(prefix="venus-audio-test-"))
    event_test((ROOT / ADM).read_text(), out, "adm-fixed")
    if args.old_revision:
        old = subprocess.check_output(["git", "show", f"{args.old_revision}:{ADM}"], cwd=ROOT, text=True)
        event_test(old, out, "adm-original", should_fail=True)
    if args.build_dir:
        compile_objects(args.build_dir.resolve(), out)
    print(f"Evidence: {out}")


if __name__ == "__main__":
    main()
