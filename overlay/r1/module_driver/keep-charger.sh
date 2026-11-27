#!/bin/sh
#
# Loads the AXP2101 driver without changing how the battery is charged.
#
# The R1's: driver_default_init_script.sh (recovery/overlay/r1) runs it in
# place of `sh axp2101.sh`, with module_driver/ as the current folder.
#
# The driver's probe writes the charger's registers every time it loads: the
# end of charge voltage from axp2101.sh's charge_voltage_limit (4400 mV in
# the stock script), its own charge current, input current limit, precharge
# and termination. On the main system Sonix Player puts its own back a few
# seconds later (axpcharge.c: 4.20 V and at most 500 mA when the user chose
# them); in the recovery nothing would, and the battery would charge to
# 4.40 V for as long as the player sits here.
#
# The PMIC keeps its registers over a reboot, so what is in them before the
# driver loads is the configuration the main system left. This reads them
# over I2C first, loads the driver, and writes them back through the
# driver's own /sys/class/axp/axp_reg (its register cache stays right), then
# tells the driver the charge current, which its monitor thread applies
# again after every plug-in (usb/constant_charge_current_max).
#
#   16  input current limit
#   61  precharge current
#   62  constant charge current
#   63  termination
#   64  end of charge voltage
#
# A charge current of 0 is not kept: the recovery has to be able to charge.
# It creates nothing: what it did goes to the console, and a register it
# cannot read (no /dev/i2c-0 yet) is left as the driver sets it.

BUS=0
ADDR=0x34
REGS="16 61 62 63 64"
# overridable only to try the script on a PC against a made-up PMIC
AXP_REG=${KC_AXP_REG:-/sys/class/axp/axp_reg}
USB_ICC=${KC_USB_ICC:-/sys/class/power_supply/usb/constant_charge_current_max}
CONSOLE=${KC_CONSOLE:-/dev/console}

log() {
	echo "keep-charger: $*" >$CONSOLE
}

saved=""
for r in $REGS; do
	v=$(i2cget -f -y $BUS $ADDR 0x$r 2>/dev/null)
	case $v in
	0x??) saved="$saved $r=${v#0x}" ;;
	*) log "cannot read register 0x$r before the driver: it keeps the driver's value" ;;
	esac
done
log "before the driver:$saved"

sh axp2101.sh

# the class appears with the driver; give it a moment
i=0
while [ ! -w $AXP_REG ] && [ $i -lt 50 ]; do
	sleep 0.02
	i=$((i + 1))
done

restored=""
for kv in $saved; do
	r=${kv%=*}
	v=${kv#*=}
	if [ "$r" = 62 ] && [ $((0x$v & 0x1f)) -eq 0 ]; then
		log "charge current 0 not kept"
		continue
	fi
	if [ -w $AXP_REG ]; then
		echo "0x$r$v" >$AXP_REG 2>/dev/null || { log "the driver refused 0x$v for register 0x$r"; continue; }
	else
		# a driver without axp_reg (HiBy's own): straight to the chip
		i2cset -f -y $BUS $ADDR 0x$r 0x$v || { log "cannot write register 0x$r"; continue; }
	fi
	restored="$restored $r=$v"
	if [ "$r" = 62 ] && [ -w $USB_ICC ]; then
		sel=$((0x$v & 0x1f))
		if [ $sel -le 8 ]; then ma=$((sel * 25)); else ma=$(((sel - 8) * 100 + 200)); fi
		# in mA in this driver (axp210x_usb_set_property), not the usual uA
		echo $ma >$USB_ICC 2>/dev/null || log "cannot set $USB_ICC"
	fi
done
log "written back:$restored"
