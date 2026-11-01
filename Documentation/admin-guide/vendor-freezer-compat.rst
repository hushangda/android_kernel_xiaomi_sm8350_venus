Android vendor freezer protocol compatibility
=============================================

Some OPlus HANS and Xiaomi MILLET userspace implementations both reserve raw
netlink protocol 29.  ``CONFIG_ANDROID_VENDOR_FREEZER_COMPAT`` gives the
protocol a single kernel owner and routes messages to the matching backend.

The default ``auto`` mode selects a backend from the first authenticated,
well-formed loopback handshake.  The HANS and MILLET wire formats have
different type and port fields, so ordinary messages cannot select a backend.
Selection is fixed for the rest of the boot; this prevents a second daemon
from taking over protocol 29 after Android has started.

The active state is visible at::

	cat /sys/kernel/vendor_freezer/backend

It reports ``auto`` before a handshake and then ``hans`` or ``millet``.
Devices with unusual startup ordering can force a backend on the kernel
command line::

	vendor_freezer=hans
	vendor_freezer=millet

Omitting the argument, or using ``vendor_freezer=auto``, keeps handshake-based
detection.  When one backend is selected, callbacks belonging to the other
backend are passive, including its Binder, signal, network, and cgroup paths.

MILLET notifications cover both the legacy freezer and the unified hierarchy's
``cgroup.freeze`` requests. The latter do not set ``PF_FROZEN`` or increment
``system_freezing_cnt``; checking only the legacy freezer would suppress Binder
and signal notifications for apps frozen through cgroup v2. The unified lookup
uses ``cgroup_task_freeze()``, including its RCU protection, so it also covers
the transition into the frozen state. Userspace still owns the thaw policy.

``/proc/millet/millet_stat`` counts a successful notification only after the
netlink send succeeds. Filtered notifications and an inactive backend are not
counted. A successful send confirms queueing to the userspace socket, not that
Greeze consumed the message or thawed the target.

The bounded host regressions are::

	python3 tools/testing/selftests/venus/millet_freezer_test.py
	python3 tools/testing/selftests/venus/millet_delivery_test.py

They exercise the production predicates and delivery functions with mock task
and netlink state. Booting the rebuilt kernel and observing the target cgroup,
Binder queue and Greeze behavior is still required for runtime validation.
