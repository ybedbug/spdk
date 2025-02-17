#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

# Pci bus hotplug
cleanup() {
	[[ -e /proc/$hotplug_pid/status ]] || return 0
	kill "$hotplug_pid"
}

# Helper function to remove/attach cotrollers
remove_attach_helper() {
	local hotplug_events=$1
	local hotplug_wait=$2
	local use_bdev=$3
	local dev

	# We need to make sure we wait long enough for hotplug to initialize the devices
	# and start IO - if we start removing devices before that happens we will end up
	# stepping on hotplug's toes forcing it to fail to report proper count of given
	# events.
	sleep "$hotplug_wait"

	while ((hotplug_events--)); do
		for dev in "${nvmes[@]}"; do
			echo 1 > "/sys/bus/pci/devices/$dev/remove"
			if $use_bdev; then
				sleep "$hotplug_wait"
				rpc_cmd bdev_get_bdevs \
					| jq '.[] | .driver_specific | .nvme | .[] | .pci_address' \
					| NOT grep $dev
			fi
		done

		echo 1 > "/sys/bus/pci/rescan"

		if $use_bdev; then
			local retries_left=10

			"$rootdir/scripts/setup.sh"

			while [ "$(rpc_cmd bdev_get_bdevs \
				| jq '.[] | .driver_specific | .nvme | .[] | .pci_address' \
				| wc -l)" -lt "$nvme_count" ]; do
				[ $retries_left -gt 0 ] || break
				retries_left=$((retries_left - 1))
				sleep 1
			done

			# Bdev name has already changed, search for the bdev's PCI addresses
			for dev in "${nvmes[@]}"; do
				rpc_cmd bdev_get_bdevs \
					| jq '.[] | .driver_specific | .nvme | .[] | .pci_address' \
					| grep $dev
			done
		fi

		# setup.sh masks failures while writing to sysfs interfaces so let's do
		# this on our own to make sure there's anything for the hotplug to reattach
		# to.
		for dev in "${nvmes[@]}"; do
			echo "$dev" > "/sys/bus/pci/devices/$dev/driver/unbind"
			echo "$dev" > "/sys/bus/pci/drivers/${pci_bus_driver["$dev"]}/bind"
		done

		# Wait now for hotplug to reattach to the devices
		sleep "$hotplug_wait"
	done
}

run_hotplug() {
	trap "cleanup" EXIT

	remove_attach_helper "$hotplug_events" "$hotplug_wait" false &
	hotplug_pid=$!

	"$SPDK_EXAMPLE_DIR/hotplug" \
		-i 0 \
		-t $((hotplug_events * hotplug_wait + hotplug_wait * 3)) \
		-n $((hotplug_events * nvme_count)) \
		-r $((hotplug_events * nvme_count)) \
		-l warning
}

# SPDK target hotplug
tgt_run_hotplug() {
	local i=0
	local dev

	$SPDK_BIN_DIR/spdk_tgt &
	spdk_tgt_pid=$!

	trap 'killprocess ${spdk_tgt_pid}; echo 1 > /sys/bus/pci/rescan; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $spdk_tgt_pid

	for dev in "${nvmes[@]}"; do
		rpc_cmd bdev_nvme_attach_controller -b "Nvme0$i" -t PCIe -a $dev
		waitforbdev "Nvme0${i}n1" "$hotplug_wait"
		((i = i + 1))
	done

	rpc_cmd bdev_nvme_set_hotplug -e

	remove_attach_helper "$hotplug_events" "$hotplug_wait" true
	# Verify reregistering hotplug poller
	rpc_cmd bdev_nvme_set_hotplug -d
	rpc_cmd bdev_nvme_set_hotplug -e

	remove_attach_helper "$hotplug_events" "$hotplug_wait" true

	trap - SIGINT SIGTERM EXIT
	killprocess $spdk_tgt_pid
}

# Preparation
"$rootdir/scripts/setup.sh"

hotplug_wait=6
hotplug_events=3
nvmes=($(nvme_in_userspace))
nvme_count=${#nvmes[@]}

xtrace_disable
cache_pci_bus_sysfs
xtrace_restore

# Run pci bus hotplug test
run_hotplug

# Run SPDK target based hotplug
tgt_run_hotplug
