#MultiROM for Droid Razr HD
MultiROM is multi-boot solution for the RAZR/Razr MAXX HD. It can boot android ROM while
keeping the one in internal memory intact or boot Ubuntu without formating
the whole device. MultiROM can boot either from internal memory of the device
or from USB flash drive.

More info in the XDA thread: http://forum.xda-developers.com/showthread.php?t=2011403

###Build
Clone repo to some folder inside Android 4.1.x source tree, I use `/system/extras/multirom`. Then just

    . build/envsetup.h
    lunch full_xt926-userdebug
    make multirom trampoline -j4
