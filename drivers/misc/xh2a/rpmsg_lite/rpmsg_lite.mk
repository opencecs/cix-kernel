ccflags-y	+= -I${PWD}/rpmsg_lite
ccflags-y	+= -I${PWD}/rpmsg_lite/rpmsg-lite/lib/include
ccflags-y	+= -I${PWD}/rpmsg_lite/rpmsg-lite/lib/include/environment/xh2a_host
ccflags-y	+= -I${PWD}/rpmsg_lite/rpmsg-lite/lib/include/platform/xh2a_host
# Y4: rename virtqueue functions to avoid symbol collision with the kernel's
# drivers/virtio/virtio_ring.o when xh2a is built-in (=y). Same as the 6.1 tree.
ccflags-y	+= -Dvirtqueue_enable_cb=xh2a_virtqueue_enable_cb
ccflags-y	+= -Dvirtqueue_disable_cb=xh2a_virtqueue_disable_cb
ccflags-y	+= -Dvirtqueue_kick=xh2a_virtqueue_kick
xh2a_drv-objs += rpmsg_lite/rpmsg-lite/lib/common/llist.o \
		 rpmsg_lite/rpmsg-lite/lib/virtio/virtqueue.o \
		 rpmsg_lite/rpmsg-lite/lib/rpmsg_lite/rpmsg_ns.o \
		 rpmsg_lite/rpmsg-lite/lib/rpmsg_lite/rpmsg_queue.o \
		 rpmsg_lite/rpmsg-lite/lib/rpmsg_lite/rpmsg_lite.o \
		 rpmsg_lite/rpmsg-lite/lib/rpmsg_lite/porting/environment/rpmsg_env_xh2a_host.o \
		 rpmsg_lite/rpmsg-lite/lib/rpmsg_lite/porting/platform/xh2a_host/rpmsg_platform.o \
		 rpmsg_lite/xh2a_rpmsg_core.o \
		 rpmsg_lite/xh2a_rpmsg_lite.o \
		 rpmsg_lite/xh2a_rpmsg_lsm.o \
		 rpmsg_lite/xh2a_rpmsg_lite_ext.o
