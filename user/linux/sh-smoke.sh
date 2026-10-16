echo sh-product-start
x=parent
(x=child; echo $x)
echo $x
/bin/busybox-sh.elf echo sh-exec-ok
echo sh-pipe-ok | /bin/busybox-sh.elf cat
/bin/busybox-sh.elf cat /example.txt
echo sh-redirect-ok > /data/linux/sh-output.txt
/bin/busybox-sh.elf cat /data/linux/sh-output.txt
echo sh-product-ok
exit 0
