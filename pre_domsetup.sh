#!/bin/bash

# check if the user is sudo or not

#  mkdir -p /mnt/vmdisk
#  cd /mnt/vmdisk
#  echo "Current Directory: $(pwd)"

if [ $(whoami) != "root" ]; then
	echo "Only root user access is allowed"
	exit 1
else
    mkdir -p /mnt/vmdisk
    cd /mnt/vmdisk
    echo "Current Directory: $(pwd)"
    echo "$(ls)"
    if [ $(ls) == "mydisk1" ]; then
	echo "Has mydisk1: Corrupt: Lets Erase it"
	rm mydisk1
        
    else
	echo "ls returned nothing, proceeding further..."
    fi
    dd if=/dev/zero of=./mydisk1 bs=1024M count=16
    if [ $? -eq 0 ]; then
	echo "Records created and copied successfully"
    else
	echo "redo rest of domain configuration steps manually"
    fi
    CHECKER=(file mydisk1)
    echo " mydisk1 has $CHECKER"
    losetup /dev/loop1 ./mydisk1
    pvcreate /dev/loop1
    echo "file mydisk1 now has:$(file mydisk1)"
    vgcreate -s 8192 myvolumegroup1 /dev/loop1
    lvcreate -L 8192 -n mylv1 myvolumegroup1
    echo "$(ls /dev/myvolumegroup1)"
    mkdir -p /var/lib/xen/images/ubuntu-netboot
    cd /var/lib/xen/images/ubuntu-netboot
    rm ./*
    wget http://archive.ubuntu.com/ubuntu/dists/precise-updates/main/installer-amd64/current/images/netboot/xen/vmlinuz
    wget http://archive.ubuntu.com/ubuntu/dists/precise-updates/main/installer-amd64/current/images/netboot/xen/initrd.gz
    echo "$(ls)"
    cd /etc/xen
    echo "All controls checked, Ready to launch the domain NOW!!! - Pavan!!"
    xl create -c domu1.cfg
    if [ $? -eq 1 ]; then
	echo "vif- Ethernet Connection failure, Probably"
	echo "Dont panic!!, Pavan's script can fix that..yipeee"
        brctl addbr xenbr0
	brctl addif xenbr0 eth0
	ip link set dev xenbr0 up
	xl create -c domu1.cfg
   else
	echo "Activation begins, sit tight! Show's about to begin!!"
    fi

fi

