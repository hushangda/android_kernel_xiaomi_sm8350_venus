Venus board-temperature publisher
================================

The installed mi_thermald on the tested ROM is a charging loop and does not
publish board temperature. This producer restores only the missing measured
temperature path. It never writes charging, CPU/GPU, brightness or cooling
controls and must not run alongside a restored stock temperature publisher.

The model is the VIRTUAL-SENSOR section of the stock Venus thermal-normal.conf.
The encrypted stock and current configurations are byte-identical (SHA-256
31cd8c441b336679da164ad17669f0813b11d77922ea0bfa89e21f1211fa7a00).
Six thermal zones are located by their type, not fixed zone numbers. Missing,
duplicate or invalid sensor readings are errors; a real failed sample is never
replaced by a zero, battery temperature or modem ambient reading. Computation
uses signed 64-bit intermediates and preserves millidegrees Celsius.

Build with the Android NDK aarch64-linux-android30-clang compiler::

    $CC -std=c11 -Wall -Wextra -Werror -Os -fPIE -pie \
      -Wl,-z,relro,-z,now venus-board-temp.c -o venus-board-temp

ROM paths and metadata
----------------------

* venus-board-temp -> /vendor/bin/venus-board-temp, root:root 0755,
  u:object_r:mi_thermald_exec:s0.
* venus-board-temp.conf -> /vendor/etc/venus-board-temp.conf, root:root 0644,
  u:object_r:vendor_configs_file:s0.
* venus-board-temp.rc -> /vendor/etc/init/venus-board-temp.rc, root:root 0644,
  u:object_r:vendor_configs_file:s0.

Merge file_contexts and venus-board-temp.te into the existing policy sources;
compile and preserve all existing policy. The extra permission is only
mi_thermald -> thermal_data_file:file lock. Existing Venus policy already
permits the thermal reads, relay write and lock-file creation. The lock file is
/data/vendor/thermal/venus-board-temp.lock; the existing thermal directory is
root:system 0771 and thermal_data_file. Enforcing stays enabled.

Modes and error handling
-----------------------

--sample [config] reads the actual sensors and prints one mC result, without
writing a temperature or taking the publisher lock. Both --publish-once and
--publish take the same exclusive lock; the former is for bounded diagnostics.
The daemon invalidates the old sample before configuration/sensor discovery,
retries a large temperature step up to three times as the stock implementation
does, and publishes every 10 seconds. Input failure and orderly termination
invalidate the relay. The kernel expires an otherwise valid board sample after
30 seconds of boottime, including suspend, so SIGKILL does not leave a permanent
old value. Configurable intervals are limited to 1..20 seconds to leave retry
margin. A missing optional board_sensor_temp_comp means zero runtime offset,
as in the stock initialization; an existing invalid offset is an error.

On-device validation on 2026-11-01 used the real mi_thermald domain with
SELinux Enforcing: --sample, one-shot publish/readback, two periodic updates,
SIGTERM invalidation and lock permission denial/restoration all passed.
The temporary allow rule, process and files were removed and the prior empty
relay restored. This proves the producer path on the current kernel; the new
qcom-battery alias and 30-second kernel expiry still require the new image.
This is not proof of installation, cold boot or long-duration thermal behavior.

This program does not implement ambient temperature. The old two-coefficient
ABT configuration does not match the stock ambient publisher's 27-coefficient
model, and deci-C output is not interchangeable with the new framework's mC.
