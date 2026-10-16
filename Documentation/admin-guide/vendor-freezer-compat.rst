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
