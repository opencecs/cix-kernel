Y4 fixed PCIe packet size
========================

Scope
-----

Only the Sky1 x8 host in sky1-y4.dts opts into sky1,fixed-mps = <256>.
It must also specify max-payload = <256>. The property fixes both MPS
and MRRS throughout that host's hierarchy. Other Y4 hosts continue using
the existing automatic policy.

This supports the PCIe switch topology with M50 and Optane endpoints, whose
maximum supported payload is 256 bytes. It does not change link speed,
link width, DMA masks, coherency, AER masks, or hotplug resource reservation.
The firmware/hardware reset value is outside the OS policy; the kernel
sets 256 as soon as it discovers a function, before publishing it to a
driver. It never deliberately configures or falls back to 128 in this
fabric.

Configuration lifetime
----------------------

* Controller setup and resume replace both DevCtl fields with masks and
  verify the write. The DevSta W1C half of the register is written as zero.
* pci_setup_device() applies the policy to initial enumeration and rescan.
  Bus mastering is disabled before initial configuration. Incompatible
  devices and failed configuration accesses are rejected.
* Bus-wide tuning verifies the fixed policy without using the hotplug
  minimum-size algorithm. The is_hotplug_bridge flag is preserved.
* Device enable and pci_set_master() check the upstream path. A mismatch
  is reported and DMA is refused; a live hierarchy is not retuned.
* State restore normalizes saved DevCtl and verifies the policy before
  restoring a saved bus-master-enable bit. A failed check leaves DMA off.
* The public MPS/MRRS setters reject values different from the host policy.

This is an opt-in platform policy, not a replacement for the generic
PCIE_BUS_SAFE algorithm. It does not protect against arbitrary privileged
configuration-space writes made outside the PCI core.

AER diagnostics
---------------

Sky1's three threaded AER IRQs share one mutex and are the only consumers
of Root Error Status. The host's custom_aer flag excludes the competing
standard port-service IRQ, while native device error reporting remains
enabled. Root Error Command is enabled after enumeration and on resume.

Every interrupt scans the entire host subtree, including switch ports and
endpoints, regardless of the Source ID. This covers multi-source events and
invalid/unavailable requesters. Status is filtered by the hardware masks;
masked startup latches are not counted as new active errors. Correctable
status is acknowledged without rewriting DevCtl. Uncorrectable status and
headers remain available as evidence.

The sky1,aer-uncor-panic property applies to root, endpoint and unresolved
uncorrectable events after the whole subtree has been inspected. Without
that property the driver disables DMA, marks the host hierarchy disconnected
and disables the link until reboot. It does not claim M50 firmware recovery.
Use Sky1 debugfs counters and captured status/logs for these events; the
standard port-service root counters no longer consume them.

IRQ request errors are returned immediately, with only successfully
requested IRQs unwound. Retry/removal drains PME work before freeing the
controller mappings. Missing IRQ resources, including probe deferral, are
propagated as signed error codes.

M50 runtime power management
----------------------------

Suspend requires a firmware sleep acknowledgment within 1000 ms. Resume
requires a wake acknowledgment within 2000 ms before probe_post or client
resume callbacks. Register-access, message-send and client errors propagate;
all-ones status reads are treated as an inaccessible device.

A failed transition latches a device error, stops HDMA and clears bus
mastering. Partially suspended/resumed clients are not declared healthy,
and subsequent PM callbacks refuse further transitions until device
reinitialization. EBUSY/EAGAIN from a client are converted to EIO in this
failed state, since the PM core otherwise assumes the device is operational.
This deliberately does not attempt an unvalidated firmware rollback.


Validation
----------

Use the same Image, DTB, modules, configuration, toolchain and source diff
throughout a test run. Record the kernel release and artifact SHA-256.
For every boot, check the host-relative topology, not hard-coded BDFs:
preserving hotplug bridge flags can change bus-number allocation.

The expected fixed tree is one CIX root port, five PCIe switch bridge
functions, one M50 and three Optane endpoints. Every function must read
back 256/256.

Check initial boot, repeated warm reboots, actual removal and restoration
of external power, NVMe concurrent access, and remove/rescan of an idle
non-system endpoint. Preserve boot IDs, dmesg, lspci, raw AER registers,
Linux AER counters and the Sky1 debugfs AER counters. A status-only snapshot
cannot establish stability; compare errors before and after workloads.

For read-only NVMe tests, use O_DIRECT and compare repeated-read hashes.
Do not overwrite raw devices merely because they have no mounted filesystem.
Model inference is outside the acceptance scope for this task.

Run tools/testing/selftests/pci/y4_fault_paths.py on the build host. It
compiles the actual C callbacks with fault-injecting hardware/API stubs to
check timeouts, read/message/client errors, IRQ unwind, multiple sources,
masked status, inaccessible sources, panic and isolation. These tests are
not hardware AER injection and do not establish model-level recovery.

The Y12 RTL8127 root may set myt,aer-log-ce-ratelimited. This limits
correctable-only diagnostics to one detailed sample per five seconds with a
count of omitted samples. Interrupt acknowledgment, status handling and AER
statistics still run for each event. Uncorrectable diagnostics remain visible.

Y4 declares a 4 MiB minimum non-prefetchable window per downstream hotplug
bridge in the PCIe switch hierarchy. The M50 uses three 1 MiB BARs and
may arrive after the initial bus scan. Other PCI roots and Y12 use the
normal PCI resource policy.
