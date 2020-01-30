/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1995, 96, 97, 98, 99, 2000 by Ralf Baechle
 * Copyright (C) 1999, 2000 Silicon Graphics, Inc.
 *
 * Changed system calls macros _syscall5 - _syscall7 to push args 5 to 7 onto
 * the stack. Robin Farine for ACN S.A, Copyright (C) 1996 by ACN S.A
 */
#ifndef _ASM_UNISTD_H
#define _ASM_UNISTD_H

#include <asm/sgidefs.h>

#if _MIPS_SIM == _MIPS_SIM_ABI32

/*
 * Linux o32 style syscalls are in the range from 4000 to 4999.
 */
#define __NR_Linux			4000
#define __NR_syscall			(__NR_Linux +   0)
#define __NR_exit			(__NR_Linux +   1)
#define __NR_fork			(__NR_Linux +   2)
#define __NR_read			(__NR_Linux +   3)
#define __NR_write			(__NR_Linux +   4)
#define __NR_open			(__NR_Linux +   5)
#define __NR_close			(__NR_Linux +   6)
#define __NR_waitpid			(__NR_Linux +   7)
#define __NR_creat			(__NR_Linux +   8)
#define __NR_link			(__NR_Linux +   9)
#define __NR_unlink			(__NR_Linux +  10)
#define __NR_execve			(__NR_Linux +  11)
#define __NR_chdir			(__NR_Linux +  12)
#define __NR_time			(__NR_Linux +  13)
#define __NR_mknod			(__NR_Linux +  14)
#define __NR_chmod			(__NR_Linux +  15)
#define __NR_lchown			(__NR_Linux +  16)
#define __NR_break			(__NR_Linux +  17)
#define __NR_unused18			(__NR_Linux +  18)
#define __NR_lseek			(__NR_Linux +  19)
#define __NR_getpid			(__NR_Linux +  20)
#define __NR_mount			(__NR_Linux +  21)
#define __NR_umount			(__NR_Linux +  22)
#define __NR_setuid			(__NR_Linux +  23)
#define __NR_getuid			(__NR_Linux +  24)
#define __NR_stime			(__NR_Linux +  25)
#define __NR_ptrace			(__NR_Linux +  26)
#define __NR_alarm			(__NR_Linux +  27)
#define __NR_unused28			(__NR_Linux +  28)
#define __NR_pause			(__NR_Linux +  29)
#define __NR_utime			(__NR_Linux +  30)
#define __NR_stty			(__NR_Linux +  31)
#define __NR_gtty			(__NR_Linux +  32)
#define __NR_access			(__NR_Linux +  33)
#define __NR_nice			(__NR_Linux +  34)
#define __NR_ftime			(__NR_Linux +  35)
#define __NR_sync			(__NR_Linux +  36)
#define __NR_kill			(__NR_Linux +  37)
#define __NR_rename			(__NR_Linux +  38)
#define __NR_mkdir			(__NR_Linux +  39)
#define __NR_rmdir			(__NR_Linux +  40)
#define __NR_dup			(__NR_Linux +  41)
#define __NR_pipe			(__NR_Linux +  42)
#define __NR_times			(__NR_Linux +  43)
#define __NR_prof			(__NR_Linux +  44)
#define __NR_brk			(__NR_Linux +  45)
#define __NR_setgid			(__NR_Linux +  46)
#define __NR_getgid			(__NR_Linux +  47)
#define __NR_signal			(__NR_Linux +  48)
#define __NR_geteuid			(__NR_Linux +  49)
#define __NR_getegid			(__NR_Linux +  50)
#define __NR_acct			(__NR_Linux +  51)
#define __NR_umount2			(__NR_Linux +  52)
#define __NR_lock			(__NR_Linux +  53)
#define __NR_ioctl			(__NR_Linux +  54)
#define __NR_fcntl			(__NR_Linux +  55)
#define __NR_mpx			(__NR_Linux +  56)
#define __NR_setpgid			(__NR_Linux +  57)
#define __NR_ulimit			(__NR_Linux +  58)
#define __NR_unused59			(__NR_Linux +  59)
#define __NR_umask			(__NR_Linux +  60)
#define __NR_chroot			(__NR_Linux +  61)
#define __NR_ustat			(__NR_Linux +  62)
#define __NR_dup2			(__NR_Linux +  63)
#define __NR_getppid			(__NR_Linux +  64)
#define __NR_getpgrp			(__NR_Linux +  65)
#define __NR_setsid			(__NR_Linux +  66)
#define __NR_sigaction			(__NR_Linux +  67)
#define __NR_sgetmask			(__NR_Linux +  68)
#define __NR_ssetmask			(__NR_Linux +  69)
#define __NR_setreuid			(__NR_Linux +  70)
#define __NR_setregid			(__NR_Linux +  71)
#define __NR_sigsuspend			(__NR_Linux +  72)
#define __NR_sigpending			(__NR_Linux +  73)
#define __NR_sethostname		(__NR_Linux +  74)
#define __NR_setrlimit			(__NR_Linux +  75)
#define __NR_getrlimit			(__NR_Linux +  76)
#define __NR_getrusage			(__NR_Linux +  77)
#define __NR_gettimeofday		(__NR_Linux +  78)
#define __NR_settimeofday		(__NR_Linux +  79)
#define __NR_getgroups			(__NR_Linux +  80)
#define __NR_setgroups			(__NR_Linux +  81)
#define __NR_reserved82			(__NR_Linux +  82)
#define __NR_symlink			(__NR_Linux +  83)
#define __NR_unused84			(__NR_Linux +  84)
#define __NR_readlink			(__NR_Linux +  85)
#define __NR_uselib			(__NR_Linux +  86)
#define __NR_swapon			(__NR_Linux +  87)
#define __NR_reboot			(__NR_Linux +  88)
#define __NR_readdir			(__NR_Linux +  89)
#define __NR_mmap			(__NR_Linux +  90)
#define __NR_munmap			(__NR_Linux +  91)
#define __NR_truncate			(__NR_Linux +  92)
#define __NR_ftruncate			(__NR_Linux +  93)
#define __NR_fchmod			(__NR_Linux +  94)
#define __NR_fchown			(__NR_Linux +  95)
#define __NR_getpriority		(__NR_Linux +  96)
#define __NR_setpriority		(__NR_Linux +  97)
#define __NR_profil			(__NR_Linux +  98)
#define __NR_statfs			(__NR_Linux +  99)
#define __NR_fstatfs			(__NR_Linux + 100)
#define __NR_ioperm			(__NR_Linux + 101)
#define __NR_socketcall			(__NR_Linux + 102)
#define __NR_syslog			(__NR_Linux + 103)
#define __NR_setitimer			(__NR_Linux + 104)
#define __NR_getitimer			(__NR_Linux + 105)
#define __NR_stat			(__NR_Linux + 106)
#define __NR_lstat			(__NR_Linux + 107)
#define __NR_fstat			(__NR_Linux + 108)
#define __NR_unused109			(__NR_Linux + 109)
#define __NR_iopl			(__NR_Linux + 110)
#define __NR_vhangup			(__NR_Linux + 111)
#define __NR_idle			(__NR_Linux + 112)
#define __NR_vm86			(__NR_Linux + 113)
#define __NR_wait4			(__NR_Linux + 114)
#define __NR_swapoff			(__NR_Linux + 115)
#define __NR_sysinfo			(__NR_Linux + 116)
#define __NR_ipc			(__NR_Linux + 117)
#define __NR_fsync			(__NR_Linux + 118)
#define __NR_sigreturn			(__NR_Linux + 119)
#define __NR_clone			(__NR_Linux + 120)
#define __NR_setdomainname		(__NR_Linux + 121)
#define __NR_uname			(__NR_Linux + 122)
#define __NR_modify_ldt			(__NR_Linux + 123)
#define __NR_adjtimex			(__NR_Linux + 124)
#define __NR_mprotect			(__NR_Linux + 125)
#define __NR_sigprocmask		(__NR_Linux + 126)
#define __NR_create_module		(__NR_Linux + 127)
#define __NR_init_module		(__NR_Linux + 128)
#define __NR_delete_module		(__NR_Linux + 129)
#define __NR_get_kernel_syms		(__NR_Linux + 130)
#define __NR_quotactl			(__NR_Linux + 131)
#define __NR_getpgid			(__NR_Linux + 132)
#define __NR_fchdir			(__NR_Linux + 133)
#define __NR_bdflush			(__NR_Linux + 134)
#define __NR_sysfs			(__NR_Linux + 135)
#define __NR_personality		(__NR_Linux + 136)
#define __NR_afs_syscall		(__NR_Linux + 137) /* Syscall for Andrew File System */
#define __NR_setfsuid			(__NR_Linux + 138)
#define __NR_setfsgid			(__NR_Linux + 139)
#define __NR__llseek			(__NR_Linux + 140)
#define __NR_getdents			(__NR_Linux + 141)
#define __NR__newselect			(__NR_Linux + 142)
#define __NR_flock			(__NR_Linux + 143)
#define __NR_msync			(__NR_Linux + 144)
#define __NR_readv			(__NR_Linux + 145)
#define __NR_writev			(__NR_Linux + 146)
#define __NR_cacheflush			(__NR_Linux + 147)
#define __NR_cachectl			(__NR_Linux + 148)
#define __NR_sysmips			(__NR_Linux + 149)
#define __NR_unused150			(__NR_Linux + 150)
#define __NR_getsid			(__NR_Linux + 151)
#define __NR_fdatasync			(__NR_Linux + 152)
#define __NR__sysctl			(__NR_Linux + 153)
#define __NR_mlock			(__NR_Linux + 154)
#define __NR_munlock			(__NR_Linux + 155)
#define __NR_mlockall			(__NR_Linux + 156)
#define __NR_munlockall			(__NR_Linux + 157)
#define __NR_sched_setparam		(__NR_Linux + 158)
#define __NR_sched_getparam		(__NR_Linux + 159)
#define __NR_sched_setscheduler		(__NR_Linux + 160)
#define __NR_sched_getscheduler		(__NR_Linux + 161)
#define __NR_sched_yield		(__NR_Linux + 162)
#define __NR_sched_get_priority_max	(__NR_Linux + 163)
#define __NR_sched_get_priority_min	(__NR_Linux + 164)
#define __NR_sched_rr_get_interval	(__NR_Linux + 165)
#define __NR_nanosleep			(__NR_Linux + 166)
#define __NR_mremap			(__NR_Linux + 167)
#define __NR_accept			(__NR_Linux + 168)
#define __NR_bind			(__NR_Linux + 169)
#define __NR_connect			(__NR_Linux + 170)
#define __NR_getpeername		(__NR_Linux + 171)
#define __NR_getsockname		(__NR_Linux + 172)
#define __NR_getsockopt			(__NR_Linux + 173)
#define __NR_listen			(__NR_Linux + 174)
#define __NR_recv			(__NR_Linux + 175)
#define __NR_recvfrom			(__NR_Linux + 176)
#define __NR_recvmsg			(__NR_Linux + 177)
#define __NR_send			(__NR_Linux + 178)
#define __NR_sendmsg			(__NR_Linux + 179)
#define __NR_sendto			(__NR_Linux + 180)
#define __NR_setsockopt			(__NR_Linux + 181)
#define __NR_shutdown			(__NR_Linux + 182)
#define __NR_socket			(__NR_Linux + 183)
#define __NR_socketpair			(__NR_Linux + 184)
#define __NR_setresuid			(__NR_Linux + 185)
#define __NR_getresuid			(__NR_Linux + 186)
#define __NR_query_module		(__NR_Linux + 187)
#define __NR_poll			(__NR_Linux + 188)
#define __NR_nfsservctl			(__NR_Linux + 189)
#define __NR_setresgid			(__NR_Linux + 190)
#define __NR_getresgid			(__NR_Linux + 191)
#define __NR_prctl			(__NR_Linux + 192)
#define __NR_rt_sigreturn		(__NR_Linux + 193)
#define __NR_rt_sigaction		(__NR_Linux + 194)
#define __NR_rt_sigprocmask		(__NR_Linux + 195)
#define __NR_rt_sigpending		(__NR_Linux + 196)
#define __NR_rt_sigtimedwait		(__NR_Linux + 197)
#define __NR_rt_sigqueueinfo		(__NR_Linux + 198)
#define __NR_rt_sigsuspend		(__NR_Linux + 199)
#define __NR_pread64			(__NR_Linux + 200)
#define __NR_pwrite64			(__NR_Linux + 201)
#define __NR_chown			(__NR_Linux + 202)
#define __NR_getcwd			(__NR_Linux + 203)
#define __NR_capget			(__NR_Linux + 204)
#define __NR_capset			(__NR_Linux + 205)
#define __NR_sigaltstack		(__NR_Linux + 206)
#define __NR_sendfile			(__NR_Linux + 207)
#define __NR_getpmsg			(__NR_Linux + 208)
#define __NR_putpmsg			(__NR_Linux + 209)
#define __NR_mmap2			(__NR_Linux + 210)
#define __NR_truncate64			(__NR_Linux + 211)
#define __NR_ftruncate64		(__NR_Linux + 212)
#define __NR_stat64			(__NR_Linux + 213)
#define __NR_lstat64			(__NR_Linux + 214)
#define __NR_fstat64			(__NR_Linux + 215)
#define __NR_pivot_root			(__NR_Linux + 216)
#define __NR_mincore			(__NR_Linux + 217)
#define __NR_madvise			(__NR_Linux + 218)
#define __NR_getdents64			(__NR_Linux + 219)
#define __NR_fcntl64			(__NR_Linux + 220)
#define __NR_reserved221		(__NR_Linux + 221)
#define __NR_gettid			(__NR_Linux + 222)
#define __NR_readahead			(__NR_Linux + 223)
#define __NR_setxattr			(__NR_Linux + 224)
#define __NR_lsetxattr			(__NR_Linux + 225)
#define __NR_fsetxattr			(__NR_Linux + 226)
#define __NR_getxattr			(__NR_Linux + 227)
#define __NR_lgetxattr			(__NR_Linux + 228)
#define __NR_fgetxattr			(__NR_Linux + 229)
#define __NR_listxattr			(__NR_Linux + 230)
#define __NR_llistxattr			(__NR_Linux + 231)
#define __NR_flistxattr			(__NR_Linux + 232)
#define __NR_removexattr		(__NR_Linux + 233)
#define __NR_lremovexattr		(__NR_Linux + 234)
#define __NR_fremovexattr		(__NR_Linux + 235)
#define __NR_tkill			(__NR_Linux + 236)
#define __NR_sendfile64			(__NR_Linux + 237)
#define __NR_futex			(__NR_Linux + 238)
#define __NR_sched_setaffinity		(__NR_Linux + 239)
#define __NR_sched_getaffinity		(__NR_Linux + 240)
#define __NR_io_setup			(__NR_Linux + 241)
#define __NR_io_destroy			(__NR_Linux + 242)
#define __NR_io_getevents		(__NR_Linux + 243)
#define __NR_io_submit			(__NR_Linux + 244)
#define __NR_io_cancel			(__NR_Linux + 245)
#define __NR_exit_group			(__NR_Linux + 246)
#define __NR_lookup_dcookie		(__NR_Linux + 247)
#define __NR_epoll_create		(__NR_Linux + 248)
#define __NR_epoll_ctl			(__NR_Linux + 249)
#define __NR_epoll_wait			(__NR_Linux + 250)
#define __NR_remap_file_pages		(__NR_Linux + 251)
#define __NR_set_tid_address		(__NR_Linux + 252)
#define __NR_restart_syscall		(__NR_Linux + 253)
#define __NR_fadvise64			(__NR_Linux + 254)
#define __NR_statfs64			(__NR_Linux + 255)
#define __NR_fstatfs64			(__NR_Linux + 256)
#define __NR_timer_create		(__NR_Linux + 257)
#define __NR_timer_settime		(__NR_Linux + 258)
#define __NR_timer_gettime		(__NR_Linux + 259)
#define __NR_timer_getoverrun		(__NR_Linux + 260)
#define __NR_timer_delete		(__NR_Linux + 261)
#define __NR_clock_settime		(__NR_Linux + 262)
#define __NR_clock_gettime		(__NR_Linux + 263)
#define __NR_clock_getres		(__NR_Linux + 264)
#define __NR_clock_nanosleep		(__NR_Linux + 265)
#define __NR_tgkill			(__NR_Linux + 266)
#define __NR_utimes			(__NR_Linux + 267)
#define __NR_mbind			(__NR_Linux + 268)
#define __NR_get_mempolicy		(__NR_Linux + 269)
#define __NR_set_mempolicy		(__NR_Linux + 270)
#define __NR_mq_open			(__NR_Linux + 271)
#define __NR_mq_unlink			(__NR_Linux + 272)
#define __NR_mq_timedsend		(__NR_Linux + 273)
#define __NR_mq_timedreceive		(__NR_Linux + 274)
#define __NR_mq_notify			(__NR_Linux + 275)
#define __NR_mq_getsetattr		(__NR_Linux + 276)
#define __NR_vserver			(__NR_Linux + 277)

/*
 * Offset of the last Linux o32 flavoured syscall
 */
#define __NR_Linux_syscalls		277

#endif /* _MIPS_SIM == _MIPS_SIM_ABI32 */

#define __NR_O32_Linux			4000
#define __NR_O32_Linux_syscalls		277

#if _MIPS_SIM == _MIPS_SIM_ABI64

/*
 * Linux 64-bit syscalls are in the range from 5000 to 5999.
 */
#define __NR_Linux			5000
#define __NR_read			(__NR_Linux +   0)
#define __NR_write			(__NR_Linux +   1)
#define __NR_open			(__NR_Linux +   2)
#define __NR_close			(__NR_Linux +   3)
#define __NR_stat			(__NR_Linux +   4)
#define __NR_fstat			(__NR_Linux +   5)
#define __NR_lstat			(__NR_Linux +   6)
#define __NR_poll			(__NR_Linux +   7)
#define __NR_lseek			(__NR_Linux +   8)
#define __NR_mmap			(__NR_Linux +   9)
#define __NR_mprotect			(__NR_Linux +  10)
#define __NR_munmap			(__NR_Linux +  11)
#define __NR_brk			(__NR_Linux +  12)
#define __NR_rt_sigaction		(__NR_Linux +  13)
#define __NR_rt_sigprocmask		(__NR_Linux +  14)
#define __NR_ioctl			(__NR_Linux +  15)
#define __NR_pread64			(__NR_Linux +  16)
#define __NR_pwrite64			(__NR_Linux +  17)
#define __NR_readv			(__NR_Linux +  18)
#define __NR_writev			(__NR_Linux +  19)
#define __NR_access			(__NR_Linux +  20)
#define __NR_pipe			(__NR_Linux +  21)
#define __NR__newselect			(__NR_Linux +  22)
#define __NR_sched_yield		(__NR_Linux +  23)
#define __NR_mremap			(__NR_Linux +  24)
#define __NR_msync			(__NR_Linux +  25)
#define __NR_mincore			(__NR_Linux +  26)
#define __NR_madvise			(__NR_Linux +  27)
#define __NR_shmget			(__NR_Linux +  28)
#define __NR_shmat			(__NR_Linux +  29)
#define __NR_shmctl			(__NR_Linux +  30)
#define __NR_dup			(__NR_Linux +  31)
#define __NR_dup2			(__NR_Linux +  32)
#define __NR_pause			(__NR_Linux +  33)
#define __NR_nanosleep			(__NR_Linux +  34)
#define __NR_getitimer			(__NR_Linux +  35)
#define __NR_setitimer			(__NR_Linux +  36)
#define __NR_alarm			(__NR_Linux +  37)
#define __NR_getpid			(__NR_Linux +  38)
#define __NR_sendfile			(__NR_Linux +  39)
#define __NR_socket			(__NR_Linux +  40)
#define __NR_connect			(__NR_Linux +  41)
#define __NR_accept			(__NR_Linux +  42)
#define __NR_sendto			(__NR_Linux +  43)
#define __NR_recvfrom			(__NR_Linux +  44)
#define __NR_sendmsg			(__NR_Linux +  45)
#define __NR_recvmsg			(__NR_Linux +  46)
#define __NR_shutdown			(__NR_Linux +  47)
#define __NR_bind			(__NR_Linux +  48)
#define __NR_listen			(__NR_Linux +  49)
#define __NR_getsockname		(__NR_Linux +  50)
#define __NR_getpeername		(__NR_Linux +  51)
#define __NR_socketpair			(__NR_Linux +  52)
#define __NR_setsockopt			(__NR_Linux +  53)
#define __NR_getsockopt			(__NR_Linux +  54)
#define __NR_clone			(__NR_Linux +  55)
#define __NR_fork			(__NR_Linux +  56)
#define __NR_execve			(__NR_Linux +  57)
#define __NR_exit			(__NR_Linux +  58)
#define __NR_wait4			(__NR_Linux +  59)
#define __NR_kill			(__NR_Linux +  60)
#define __NR_uname			(__NR_Linux +  61)
#define __NR_semget			(__NR_Linux +  62)
#define __NR_semop			(__NR_Linux +  63)
#define __NR_semctl			(__NR_Linux +  64)
#define __NR_shmdt			(__NR_Linux +  65)
#define __NR_msgget			(__NR_Linux +  66)
#define __NR_msgsnd			(__NR_Linux +  67)
#define __NR_msgrcv			(__NR_Linux +  68)
#define __NR_msgctl			(__NR_Linux +  69)
#define __NR_fcntl			(__NR_Linux +  70)
#define __NR_flock			(__NR_Linux +  71)
#define __NR_fsync			(__NR_Linux +  72)
#define __NR_fdatasync			(__NR_Linux +  73)
#define __NR_truncate			(__NR_Linux +  74)
#define __NR_ftruncate			(__NR_Linux +  75)
#define __NR_getdents			(__NR_Linux +  76)
#define __NR_getcwd			(__NR_Linux +  77)
#define __NR_chdir			(__NR_Linux +  78)
#define __NR_fchdir			(__NR_Linux +  79)
#define __NR_rename			(__NR_Linux +  80)
#define __NR_mkdir			(__NR_Linux +  81)
#define __NR_rmdir			(__NR_Linux +  82)
#define __NR_creat			(__NR_Linux +  83)
#define __NR_link			(__NR_Linux +  84)
#define __NR_unlink			(__NR_Linux +  85)
#define __NR_symlink			(__NR_Linux +  86)
#define __NR_readlink			(__NR_Linux +  87)
#define __NR_chmod			(__NR_Linux +  88)
#define __NR_fchmod			(__NR_Linux +  89)
#define __NR_chown			(__NR_Linux +  90)
#define __NR_fchown			(__NR_Linux +  91)
#define __NR_lchown			(__NR_Linux +  92)
#define __NR_umask			(__NR_Linux +  93)
#define __NR_gettimeofday		(__NR_Linux +  94)
#define __NR_getrlimit			(__NR_Linux +  95)
#define __NR_getrusage			(__NR_Linux +  96)
#define __NR_sysinfo			(__NR_Linux +  97)
#define __NR_times			(__NR_Linux +  98)
#define __NR_ptrace			(__NR_Linux +  99)
#define __NR_getuid			(__NR_Linux + 100)
#define __NR_syslog			(__NR_Linux + 101)
#define __NR_getgid			(__NR_Linux + 102)
#define __NR_setuid			(__NR_Linux + 103)
#define __NR_setgid			(__NR_Linux + 104)
#define __NR_geteuid			(__NR_Linux + 105)
#define __NR_getegid			(__NR_Linux + 106)
#define __NR_setpgid			(__NR_Linux + 107)
#define __NR_getppid			(__NR_Linux + 108)
#define __NR_getpgrp			(__NR_Linux + 109)
#define __NR_setsid			(__NR_Linux + 110)
#define __NR_setreuid			(__NR_Linux + 111)
#define __NR_setregid			(__NR_Linux + 112)
#define __NR_getgroups			(__NR_Linux + 113)
#define __NR_setgroups			(__NR_Linux + 114)
#define __NR_setresuid			(__NR_Linux + 115)
#define __NR_getresuid			(__NR_Linux + 116)
#define __NR_setresgid			(__NR_Linux + 117)
#define __NR_getresgid			(__NR_Linux + 118)
#define __NR_getpgid			(__NR_Linux + 119)
#define __NR_setfsuid			(__NR_Linux + 120)
#define __NR_setfsgid			(__NR_Linux + 121)
#define __NR_getsid			(__NR_Linux + 122)
#define __NR_capget			(__NR_Linux + 123)
#define __NR_capset			(__NR_Linux + 124)
#define __NR_rt_sigpending		(__NR_Linux + 125)
#define __NR_rt_sigtimedwait		(__NR_Linux + 126)
#define __NR_rt_sigqueueinfo		(__NR_Linux + 127)
#define __NR_rt_sigsuspend		(__NR_Linux + 128)
#define __NR_sigaltstack		(__NR_Linux + 129)
#define __NR_utime			(__NR_Linux + 130)
#define __NR_mknod			(__NR_Linux + 131)
#define __NR_personality		(__NR_Linux + 132)
#define __NR_ustat			(__NR_Linux + 133)
#define __NR_statfs			(__NR_Linux + 134)
#define __NR_fstatfs			(__NR_Linux + 135)
#define __NR_sysfs			(__NR_Linux + 136)
#define __NR_getpriority		(__NR_Linux + 137)
#define __NR_setpriority		(__NR_Linux + 138)
#define __NR_sched_setparam		(__NR_Linux + 139)
#define __NR_sched_getparam		(__NR_Linux + 140)
#define __NR_sched_setscheduler		(__NR_Linux + 141)
#define __NR_sched_getscheduler		(__NR_Linux + 142)
#define __NR_sched_get_priority_max	(__NR_Linux + 143)
#define __NR_sched_get_priority_min	(__NR_Linux + 144)
#define __NR_sched_rr_get_interval	(__NR_Linux + 145)
#define __NR_mlock			(__NR_Linux + 146)
#define __NR_munlock			(__NR_Linux + 147)
#define __NR_mlockall			(__NR_Linux + 148)
#define __NR_munlockall			(__NR_Linux + 149)
#define __NR_vhangup			(__NR_Linux + 150)
#define __NR_pivot_root			(__NR_Linux + 151)
#define __NR__sysctl			(__NR_Linux + 152)
#define __NR_prctl			(__NR_Linux + 153)
#define __NR_adjtimex			(__NR_Linux + 154)
#define __NR_setrlimit			(__NR_Linux + 155)
#define __NR_chroot			(__NR_Linux + 156)
#define __NR_sync			(__NR_Linux + 157)
#define __NR_acct			(__NR_Linux + 158)
#define __NR_settimeofday		(__NR_Linux + 159)
#define __NR_mount			(__NR_Linux + 160)
#define __NR_umount2			(__NR_Linux + 161)
#define __NR_swapon			(__NR_Linux + 162)
#define __NR_swapoff			(__NR_Linux + 163)
#define __NR_reboot			(__NR_Linux + 164)
#define __NR_sethostname		(__NR_Linux + 165)
#define __NR_setdomainname		(__NR_Linux + 166)
#define __NR_create_module		(__NR_Linux + 167)
#define __NR_init_module		(__NR_Linux + 168)
#define __NR_delete_module		(__NR_Linux + 169)
#define __NR_get_kernel_syms		(__NR_Linux + 170)
#define __NR_query_module		(__NR_Linux + 171)
#define __NR_quotactl			(__NR_Linux + 172)
#define __NR_nfsservctl			(__NR_Linux + 173)
#define __NR_getpmsg			(__NR_Linux + 174)
#define __NR_putpmsg			(__NR_Linux + 175)
#define __NR_afs_syscall		(__NR_Linux + 176)
#define __NR_reserved177		(__NR_Linux + 177)
#define __NR_gettid			(__NR_Linux + 178)
#define __NR_readahead			(__NR_Linux + 179)
#define __NR_setxattr			(__NR_Linux + 180)
#define __NR_lsetxattr			(__NR_Linux + 181)
#define __NR_fsetxattr			(__NR_Linux + 182)
#define __NR_getxattr			(__NR_Linux + 183)
#define __NR_lgetxattr			(__NR_Linux + 184)
#define __NR_fgetxattr			(__NR_Linux + 185)
#define __NR_listxattr			(__NR_Linux + 186)
#define __NR_llistxattr			(__NR_Linux + 187)
#define __NR_flistxattr			(__NR_Linux + 188)
#define __NR_removexattr		(__NR_Linux + 189)
#define __NR_lremovexattr		(__NR_Linux + 190)
#define __NR_fremovexattr		(__NR_Linux + 191)
#define __NR_tkill			(__NR_Linux + 192)
#define __NR_reserved193		(__NR_Linux + 193)
#define __NR_futex			(__NR_Linux + 194)
#define __NR_sched_setaffinity		(__NR_Linux + 195)
#define __NR_sched_getaffinity		(__NR_Linux + 196)
#define __NR_cacheflush			(__NR_Linux + 197)
#define __NR_cachectl			(__NR_Linux + 198)
#define __NR_sysmips			(__NR_Linux + 199)
#define __NR_io_setup			(__NR_Linux + 200)
#define __NR_io_destroy			(__NR_Linux + 201)
#define __NR_io_getevents		(__NR_Linux + 202)
#define __NR_io_submit			(__NR_Linux + 203)
#define __NR_io_cancel			(__NR_Linux + 204)
#define __NR_exit_group			(__NR_Linux + 205)
#define __NR_lookup_dcookie		(__NR_Linux + 206)
#define __NR_epoll_create		(__NR_Linux + 207)
#define __NR_epoll_ctl			(__NR_Linux + 208)
#define __NR_epoll_wait			(__NR_Linux + 209)
#define __NR_remap_file_pages		(__NR_Linux + 210)
#define __NR_rt_sigreturn		(__NR_Linux + 211)
#define __NR_set_tid_address		(__NR_Linux + 212)
#define __NR_restart_syscall		(__NR_Linux + 213)
#define __NR_semtimedop			(__NR_Linux + 214)
#define __NR_fadvise64			(__NR_Linux + 215)
#define __NR_timer_create		(__NR_Linux + 216)
#define __NR_timer_settime		(__NR_Linux + 217)
#define __NR_timer_gettime		(__NR_Linux + 218)
#define __NR_timer_getoverrun		(__NR_Linux + 219)
#define __NR_timer_delete		(__NR_Linux + 220)
#define __NR_clock_settime		(__NR_Linux + 221)
#define __NR_clock_gettime		(__NR_Linux + 222)
#define __NR_clock_getres		(__NR_Linux + 223)
#define __NR_clock_nanosleep		(__NR_Linux + 224)
#define __NR_tgkill			(__NR_Linux + 225)
#define __NR_utimes			(__NR_Linux + 226)
#define __NR_mbind			(__NR_Linux + 227)
#define __NR_get_mempolicy		(__NR_Linux + 228)
#define __NR_set_mempolicy		(__NR_Linux + 229)
#define __NR_mq_open			(__NR_Linux + 230)
#define __NR_mq_unlink			(__NR_Linux + 231)
#define __NR_mq_timedsend		(__NR_Linux + 232)
#define __NR_mq_timedreceive		(__NR_Linux + 233)
#define __NR_mq_notify			(__NR_Linux + 234)
#define __NR_mq_getsetattr		(__NR_Linux + 235)
#define __NR_vserver			(__NR_Linux + 236)

/*
 * Offset of the last Linux flavoured syscall
 */
#define __NR_Linux_syscalls		236

#endif /* _MIPS_SIM == _MIPS_SIM_ABI64 */

#define __NR_64_Linux			5000
#define __NR_64_Linux_syscalls		236

#if _MIPS_SIM == _MIPS_SIM_NABI32

/*
 * Linux N32 syscalls are in the range from 6000 to 6999.
 */
#define __NR_Linux			6000
#define __NR_read			(__NR_Linux +   0)
#define __NR_write			(__NR_Linux +   1)
#define __NR_open			(__NR_Linux +   2)
#define __NR_close			(__NR_Linux +   3)
#define __NR_stat			(__NR_Linux +   4)
#define __NR_fstat			(__NR_Linux +   5)
#define __NR_lstat			(__NR_Linux +   6)
#define __NR_poll			(__NR_Linux +   7)
#define __NR_lseek			(__NR_Linux +   8)
#define __NR_mmap			(__NR_Linux +   9)
#define __NR_mprotect			(__NR_Linux +  10)
#define __NR_munmap			(__NR_Linux +  11)
#define __NR_brk			(__NR_Linux +  12)
#define __NR_rt_sigaction		(__NR_Linux +  13)
#define __NR_rt_sigprocmask		(__NR_Linux +  14)
#define __NR_ioctl			(__NR_Linux +  15)
#define __NR_pread64			(__NR_Linux +  16)
#define __NR_pwrite64			(__NR_Linux +  17)
#define __NR_readv			(__NR_Linux +  18)
#define __NR_writev			(__NR_Linux +  19)
#define __NR_access			(__NR_Linux +  20)
#define __NR_pipe			(__NR_Linux +  21)
#define __NR__newselect			(__NR_Linux +  22)
#define __NR_sched_yield		(__NR_Linux +  23)
#define __NR_mremap			(__NR_Linux +  24)
#define __NR_msync			(__NR_Linux +  25)
#define __NR_mincore			(__NR_Linux +  26)
#define __NR_madvise			(__NR_Linux +  27)
#define __NR_shmget			(__NR_Linux +  28)
#define __NR_shmat			(__NR_Linux +  29)
#define __NR_shmctl			(__NR_Linux +  30)
#define __NR_dup			(__NR_Linux +  31)
#define __NR_dup2			(__NR_Linux +  32)
#define __NR_pause			(__NR_Linux +  33)
#define __NR_nanosleep			(__NR_Linux +  34)
#define __NR_getitimer			(__NR_Linux +  35)
#define __NR_setitimer			(__NR_Linux +  36)
#define __NR_alarm			(__NR_Linux +  37)
#define __NR_getpid			(__NR_Linux +  38)
#define __NR_sendfile			(__NR_Linux +  39)
#define __NR_socket			(__NR_Linux +  40)
#define __NR_connect			(__NR_Linux +  41)
#define __NR_accept			(__NR_Linux +  42)
#define __NR_sendto			(__NR_Linux +  43)
#define __NR_recvfrom			(__NR_Linux +  44)
#define __NR_sendmsg			(__NR_Linux +  45)
#define __NR_recvmsg			(__NR_Linux +  46)
#define __NR_shutdown			(__NR_Linux +  47)
#define __NR_bind			(__NR_Linux +  48)
#define __NR_listen			(__NR_Linux +  49)
#define __NR_getsockname		(__NR_Linux +  50)
#define __NR_getpeername		(__NR_Linux +  51)
#define __NR_socketpair			(__NR_Linux +  52)
#define __NR_setsockopt			(__NR_Linux +  53)
#define __NR_getsockopt			(__NR_Linux +  54)
#define __NR_clone			(__NR_Linux +  55)
#define __NR_fork			(__NR_Linux +  56)
#define __NR_execve			(__NR_Linux +  57)
#define __NR_exit			(__NR_Linux +  58)
#define __NR_wait4			(__NR_Linux +  59)
#define __NR_kill			(__NR_Linux +  60)
#define __NR_uname			(__NR_Linux +  61)
#define __NR_semget			(__NR_Linux +  62)
#define __NR_semop			(__NR_Linux +  63)
#define __NR_semctl			(__NR_Linux +  64)
#define __NR_shmdt			(__NR_Linux +  65)
#define __NR_msgget			(__NR_Linux +  66)
#define __NR_msgsnd			(__NR_Linux +  67)
#define __NR_msgrcv			(__NR_Linux +  68)
#define __NR_msgctl			(__NR_Linux +  69)
#define __NR_fcntl			(__NR_Linux +  70)
#define __NR_flock			(__NR_Linux +  71)
#define __NR_fsync			(__NR_Linux +  72)
#define __NR_fdatasync			(__NR_Linux +  73)
#define __NR_truncate			(__NR_Linux +  74)
#define __NR_ftruncate			(__NR_Linux +  75)
#define __NR_getdents			(__NR_Linux +  76)
#define __NR_getcwd			(__NR_Linux +  77)
#define __NR_chdir			(__NR_Linux +  78)
#define __NR_fchdir			(__NR_Linux +  79)
#define __NR_rename			(__NR_Linux +  80)
#define __NR_mkdir			(__NR_Linux +  81)
#define __NR_rmdir			(__NR_Linux +  82)
#define __NR_creat			(__NR_Linux +  83)
#define __NR_link			(__NR_Linux +  84)
#define __NR_unlink			(__NR_Linux +  85)
#define __NR_symlink			(__NR_Linux +  86)
#define __NR_readlink			(__NR_Linux +  87)
#define __NR_chmod			(__NR_Linux +  88)
#define __NR_fchmod			(__NR_Linux +  89)
#define __NR_chown			(__NR_Linux +  90)
#define __NR_fchown			(__NR_Linux +  91)
#define __NR_lchown			(__NR_Linux +  92)
#define __NR_umask			(__NR_Linux +  93)
#define __NR_gettimeofday		(__NR_Linux +  94)
#define __NR_getrlimit			(__NR_Linux +  95)
#define __NR_getrusage			(__NR_Linux +  96)
#define __NR_sysinfo			(__NR_Linux +  97)
#define __NR_times			(__NR_Linux +  98)
#define __NR_ptrace			(__NR_Linux +  99)
#define __NR_getuid			(__NR_Linux + 100)
#define __NR_syslog			(__NR_Linux + 101)
#define __NR_getgid			(__NR_Linux + 102)
#define __NR_setuid			(__NR_Linux + 103)
#define __NR_setgid			(__NR_Linux + 104)
#define __NR_geteuid			(__NR_Linux + 105)
#define __NR_getegid			(__NR_Linux + 106)
#define __NR_setpgid			(__NR_Linux + 107)
#define __NR_getppid			(__NR_Linux + 108)
#define __NR_getpgrp			(__NR_Linux + 109)
#define __NR_setsid			(__NR_Linux + 110)
#define __NR_setreuid			(__NR_Linux + 111)
#define __NR_setregid			(__NR_Linux + 112)
#define __NR_getgroups			(__NR_Linux + 113)
#define __NR_setgroups			(__NR_Linux + 114)
#define __NR_setresuid			(__NR_Linux + 115)
#define __NR_getresuid			(__NR_Linux + 116)
#define __NR_setresgid			(__NR_Linux + 117)
#define __NR_getresgid			(__NR_Linux + 118)
#define __NR_getpgid			(__NR_Linux + 119)
#define __NR_setfsuid			(__NR_Linux + 120)
#define __NR_setfsgid			(__NR_Linux + 121)
#define __NR_getsid			(__NR_Linux + 122)
#define __NR_capget			(__NR_Linux + 123)
#define __NR_capset			(__NR_Linux + 124)
#define __NR_rt_sigpending		(__NR_Linux + 125)
#define __NR_rt_sigtimedwait		(__NR_Linux + 126)
#define __NR_rt_sigqueueinfo		(__NR_Linux + 127)
#define __NR_rt_sigsuspend		(__NR_Linux + 128)
#define __NR_sigaltstack		(__NR_Linux + 129)
#define __NR_utime			(__NR_Linux + 130)
#define __NR_mknod			(__NR_Linux + 131)
#define __NR_personality		(__NR_Linux + 132)
#define __NR_ustat			(__NR_Linux + 133)
#define __NR_statfs			(__NR_Linux + 134)
#define __NR_fstatfs			(__NR_Linux + 135)
#define __NR_sysfs			(__NR_Linux + 136)
#define __NR_getpriority		(__NR_Linux + 137)
#define __NR_setpriority		(__NR_Linux + 138)
#define __NR_sched_setparam		(__NR_Linux + 139)
#define __NR_sched_getparam		(__NR_Linux + 140)
#define __NR_sched_setscheduler		(__NR_Linux + 141)
#define __NR_sched_getscheduler		(__NR_Linux + 142)
#define __NR_sched_get_priority_max	(__NR_Linux + 143)
#define __NR_sched_get_priority_min	(__NR_Linux + 144)
#define __NR_sched_rr_get_interval	(__NR_Linux + 145)
#define __NR_mlock			(__NR_Linux + 146)
#define __NR_munlock			(__NR_Linux + 147)
#define __NR_mlockall			(__NR_Linux + 148)
#define __NR_munlockall			(__NR_Linux + 149)
#define __NR_vhangup			(__NR_Linux + 150)
#define __NR_pivot_root			(__NR_Linux + 151)
#define __NR__sysctl			(__NR_Linux + 152)
#define __NR_prctl			(__NR_Linux + 153)
#define __NR_adjtimex			(__NR_Linux + 154)
#define __NR_setrlimit			(__NR_Linux + 155)
#define __NR_chroot			(__NR_Linux + 156)
#define __NR_sync			(__NR_Linux + 157)
#define __NR_acct			(__NR_Linux + 158)
#define __NR_settimeofday		(__NR_Linux + 159)
#define __NR_mount			(__NR_Linux + 160)
#define __NR_umount2			(__NR_Linux + 161)
#define __NR_swapon			(__NR_Linux + 162)
#define __NR_swapoff			(__NR_Linux + 163)
#define __NR_reboot			(__NR_Linux + 164)
#define __NR_sethostname		(__NR_Linux + 165)
#define __NR_setdomainname		(__NR_Linux + 166)
#define __NR_create_module		(__NR_Linux + 167)
#define __NR_init_module		(__NR_Linux + 168)
#define __NR_delete_module		(__NR_Linux + 169)
#define __NR_get_kernel_syms		(__NR_Linux + 170)
#define __NR_query_module		(__NR_Linux + 171)
#define __NR_quotactl			(__NR_Linux + 172)
#define __NR_nfsservctl			(__NR_Linux + 173)
#define __NR_getpmsg			(__NR_Linux + 174)
#define __NR_putpmsg			(__NR_Linux + 175)
#define __NR_afs_syscall		(__NR_Linux + 176)
#define __NR_reserved177		(__NR_Linux + 177)
#define __NR_gettid			(__NR_Linux + 178)
#define __NR_readahead			(__NR_Linux + 179)
#define __NR_setxattr			(__NR_Linux + 180)
#define __NR_lsetxattr			(__NR_Linux + 181)
#define __NR_fsetxattr			(__NR_Linux + 182)
#define __NR_getxattr			(__NR_Linux + 183)
#define __NR_lgetxattr			(__NR_Linux + 184)
#define __NR_fgetxattr			(__NR_Linux + 185)
#define __NR_listxattr			(__NR_Linux + 186)
#define __NR_llistxattr			(__NR_Linux + 187)
#define __NR_flistxattr			(__NR_Linux + 188)
#define __NR_removexattr		(__NR_Linux + 189)
#define __NR_lremovexattr		(__NR_Linux + 190)
#define __NR_fremovexattr		(__NR_Linux + 191)
#define __NR_tkill			(__NR_Linux + 192)
#define __NR_reserved193		(__NR_Linux + 193)
#define __NR_futex			(__NR_Linux + 194)
#define __NR_sched_setaffinity		(__NR_Linux + 195)
#define __NR_sched_getaffinity		(__NR_Linux + 196)
#define __NR_cacheflush			(__NR_Linux + 197)
#define __NR_cachectl			(__NR_Linux + 198)
#define __NR_sysmips			(__NR_Linux + 199)
#define __NR_io_setup			(__NR_Linux + 200)
#define __NR_io_destroy			(__NR_Linux + 201)
#define __NR_io_getevents		(__NR_Linux + 202)
#define __NR_io_submit			(__NR_Linux + 203)
#define __NR_io_cancel			(__NR_Linux + 204)
#define __NR_exit_group			(__NR_Linux + 205)
#define __NR_lookup_dcookie		(__NR_Linux + 206)
#define __NR_epoll_create		(__NR_Linux + 207)
#define __NR_epoll_ctl			(__NR_Linux + 208)
#define __NR_epoll_wait			(__NR_Linux + 209)
#define __NR_remap_file_pages		(__NR_Linux + 210)
#define __NR_rt_sigreturn		(__NR_Linux + 211)
#define __NR_fcntl64			(__NR_Linux + 212)
#define __NR_set_tid_address		(__NR_Linux + 213)
#define __NR_restart_syscall		(__NR_Linux + 214)
#define __NR_semtimedop			(__NR_Linux + 215)
#define __NR_fadvise64			(__NR_Linux + 216)
#define __NR_statfs64			(__NR_Linux + 217)
#define __NR_fstatfs64			(__NR_Linux + 218)
#define __NR_sendfile64			(__NR_Linux + 219)
#define __NR_timer_create		(__NR_Linux + 220)
#define __NR_timer_settime		(__NR_Linux + 221)
#define __NR_timer_gettime		(__NR_Linux + 222)
#define __NR_timer_getoverrun		(__NR_Linux + 223)
#define __NR_timer_delete		(__NR_Linux + 224)
#define __NR_clock_settime		(__NR_Linux + 225)
#define __NR_clock_gettime		(__NR_Linux + 226)
#define __NR_clock_getres		(__NR_Linux + 227)
#define __NR_clock_nanosleep		(__NR_Linux + 228)
#define __NR_tgkill			(__NR_Linux + 229)
#define __NR_utimes			(__NR_Linux + 230)
#define __NR_mbind			(__NR_Linux + 231)
#define __NR_get_mempolicy		(__NR_Linux + 232)
#define __NR_set_mempolicy		(__NR_Linux + 233)
#define __NR_mq_open			(__NR_Linux + 234)
#define __NR_mq_unlink			(__NR_Linux + 235)
#define __NR_mq_timedsend		(__NR_Linux + 236)
#define __NR_mq_timedreceive		(__NR_Linux + 237)
#define __NR_mq_notify			(__NR_Linux + 238)
#define __NR_mq_getsetattr		(__NR_Linux + 239)
#define __NR_vserver			(__NR_Linux + 240)

/*
 * Offset of the last N32 flavoured syscall
 */
#define __NR_Linux_syscalls		240

#endif /* _MIPS_SIM == _MIPS_SIM_NABI32 */

#define __NR_N32_Linux			6000
#define __NR_N32_Linux_syscalls		240

#ifndef __ASSEMBLY__

/* XXX - _foo needs to be __foo, while __NR_bar could be _NR_bar. */
#define _syscall0(type,name) \
type name(void) \
{ \
	register unsigned long __a3 asm("$7"); \
	unsigned long __v0; \
	\
	__asm__ volatile ( \
	".set\tnoreorder\n\t" \
	"li\t$2, %2\t\t\t# " #name "\n\t" \
	"syscall\n\t" \
	"move\t%0, $2\n\t" \
	".set\treorder" \
	: "=&r" (__v0), "=r" (__a3) \
	: "i" (__NR_##name) \
	: "$2", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24"); \
	\
	if (__a3 == 0) \
		return (type) __v0; \
	errno = __v0; \
	return -1; \
}

/*
 * DANGER: This macro isn't usable for the pipe(2) call
 * which has a unusual return convention.
 */
#define _syscall1(type,name,atype,a) \
type name(atype a) \
{ \
	register unsigned long __a0 asm("$4") = (unsigned long) a; \
	register unsigned long __a3 asm("$7"); \
	unsigned long __v0; \
	\
	__asm__ volatile ( \
	".set\tnoreorder\n\t" \
	"li\t$2, %3\t\t\t# " #name "\n\t" \
	"syscall\n\t" \
	"move\t%0, $2\n\t" \
	".set\treorder" \
	: "=&r" (__v0), "=r" (__a3) \
	: "r" (__a0), "i" (__NR_##name) \
	: "$2", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24"); \
	\
	if (__a3 == 0) \
		return (type) __v0; \
	errno = __v0; \
	return -1; \
}

#define _syscall2(type,name,atype,a,btype,b) \
type name(atype a, btype b) \
{ \
	register unsigned long __a0 asm("$4") = (unsigned long) a; \
	register unsigned long __a1 asm("$5") = (unsigned long) b; \
	register unsigned long __a3 asm("$7"); \
	unsigned long __v0; \
	\
	__asm__ volatile ( \
	".set\tnoreorder\n\t" \
	"li\t$2, %4\t\t\t# " #name "\n\t" \
	"syscall\n\t" \
	"move\t%0, $2\n\t" \
	".set\treorder" \
	: "=&r" (__v0), "=r" (__a3) \
	: "r" (__a0), "r" (__a1), "i" (__NR_##name) \
	: "$2", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24"); \
	\
	if (__a3 == 0) \
		return (type) __v0; \
	errno = __v0; \
	return -1; \
}

#define _syscall3(type,name,atype,a,btype,b,ctype,c) \
type name(atype a, btype b, ctype c) \
{ \
	register unsigned long __a0 asm("$4") = (unsigned long) a; \
	register unsigned long __a1 asm("$5") = (unsigned long) b; \
	register unsigned long __a2 asm("$6") = (unsigned long) c; \
	register unsigned long __a3 asm("$7"); \
	unsigned long __v0; \
	\
	__asm__ volatile ( \
	".set\tnoreorder\n\t" \
	"li\t$2, %5\t\t\t# " #name "\n\t" \
	"syscall\n\t" \
	"move\t%0, $2\n\t" \
	".set\treorder" \
	: "=&r" (__v0), "=r" (__a3) \
	: "r" (__a0), "r" (__a1), "r" (__a2), "i" (__NR_##name) \
	: "$2", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24"); \
	\
	if (__a3 == 0) \
		return (type) __v0; \
	errno = __v0; \
	return -1; \
}

#define _syscall4(type,name,atype,a,btype,b,ctype,c,dtype,d) \
type name(atype a, btype b, ctype c, dtype d) \
{ \
	register unsigned long __a0 asm("$4") = (unsigned long) a; \
	register unsigned long __a1 asm("$5") = (unsigned long) b; \
	register unsigned long __a2 asm("$6") = (unsigned long) c; \
	register unsigned long __a3 asm("$7") = (unsigned long) d; \
	unsigned long __v0; \
	\
	__asm__ volatile ( \
	".set\tnoreorder\n\t" \
	"li\t$2, %5\t\t\t# " #name "\n\t" \
	"syscall\n\t" \
	"move\t%0, $2\n\t" \
	".set\treorder" \
	: "=&r" (__v0), "+r" (__a3) \
	: "r" (__a0), "r" (__a1), "r" (__a2), "i" (__NR_##name) \
	: "$2", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24"); \
	\
	if (__a3 == 0) \
		return (type) __v0; \
	errno = __v0; \
	return -1; \
}

#if (_MIPS_SIM == _MIPS_SIM_ABI32)

/*
 * Using those means your brain needs more than an oil change ;-)
 */

#define _syscall5(type,name,atype,a,btype,b,ctype,c,dtype,d,etype,e) \
type name(atype a, btype b, ctype c, dtype d, etype e) \
{ \
	register unsigned long __a0 asm("$4") = (unsigned long) a; \
	register unsigned long __a1 asm("$5") = (unsigned long) b; \
	register unsigned long __a2 asm("$6") = (unsigned long) c; \
	register unsigned long __a3 asm("$7") = (unsigned long) d; \
	unsigned long __v0; \
	\
	__asm__ volatile ( \
	".set\tnoreorder\n\t" \
	"lw\t$2, %6\n\t" \
	"subu\t$29, 32\n\t" \
	"sw\t$2, 16($29)\n\t" \
	"li\t$2, %5\t\t\t# " #name "\n\t" \
	"syscall\n\t" \
	"move\t%0, $2\n\t" \
	"addiu\t$29, 32\n\t" \
	".set\treorder" \
	: "=&r" (__v0), "+r" (__a3) \
	: "r" (__a0), "r" (__a1), "r" (__a2), "i" (__NR_##name), \
	  "m" ((unsigned long)e) \
	: "$2", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24"); \
	\
	if (__a3 == 0) \
		return (type) __v0; \
	errno = __v0; \
	return -1; \
}

#define _syscall6(type,name,atype,a,btype,b,ctype,c,dtype,d,etype,e,ftype,f) \
type name(atype a, btype b, ctype c, dtype d, etype e, ftype f) \
{ \
	register unsigned long __a0 asm("$4") = (unsigned long) a; \
	register unsigned long __a1 asm("$5") = (unsigned long) b; \
	register unsigned long __a2 asm("$6") = (unsigned long) c; \
	register unsigned long __a3 asm("$7") = (unsigned long) d; \
	unsigned long __v0; \
	\
	__asm__ volatile ( \
	".set\tnoreorder\n\t" \
	"lw\t$2, %6\n\t" \
	"lw\t$8, %7\n\t" \
	"subu\t$29, 32\n\t" \
	"sw\t$2, 16($29)\n\t" \
	"sw\t$8, 20($29)\n\t" \
	"li\t$2, %5\t\t\t# " #name "\n\t" \
	"syscall\n\t" \
	"move\t%0, $2\n\t" \
	"addiu\t$29, 32\n\t" \
	".set\treorder" \
	: "=&r" (__v0), "+r" (__a3) \
	: "r" (__a0), "r" (__a1), "r" (__a2), "i" (__NR_##name), \
	  "m" ((unsigned long)e), "m" ((unsigned long)f) \
	: "$2", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24"); \
	\
	if (__a3 == 0) \
		return (type) __v0; \
	errno = __v0; \
	return -1; \
}

#endif /* (_MIPS_SIM == _MIPS_SIM_ABI32) */

#if (_MIPS_SIM == _MIPS_SIM_NABI32) || (_MIPS_SIM == _MIPS_SIM_ABI64)

#define _syscall5(type,name,atype,a,btype,b,ctype,c,dtype,d,etype,e) \
type name (atype a,btype b,ctype c,dtype d,etype e) \
{ \
	register unsigned long __a0 asm("$4") = (unsigned long) a; \
	register unsigned long __a1 asm("$5") = (unsigned long) b; \
	register unsigned long __a2 asm("$6") = (unsigned long) c; \
	register unsigned long __a3 asm("$7") = (unsigned long) d; \
	register unsigned long __a4 asm("$8") = (unsigned long) e; \
	unsigned long __v0; \
	\
	__asm__ volatile ( \
	".set\tnoreorder\n\t" \
	"li\t$2, %6\t\t\t# " #name "\n\t" \
	"syscall\n\t" \
	"move\t%0, $2\n\t" \
	".set\treorder" \
	: "=&r" (__v0), "+r" (__a3), "+r" (__a4) \
	: "r" (__a0), "r" (__a1), "r" (__a2), "i" (__NR_##name) \
	: "$2","$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24"); \
	\
	if (__a3 == 0) \
		return (type) __v0; \
	errno = __v0; \
	return -1; \
}

#define _syscall6(type,name,atype,a,btype,b,ctype,c,dtype,d,etype,e,ftype,f) \
type name (atype a,btype b,ctype c,dtype d,etype e,ftype f) \
{ \
	register unsigned long __a0 asm("$4") = (unsigned long) a; \
	register unsigned long __a1 asm("$5") = (unsigned long) b; \
	register unsigned long __a2 asm("$6") = (unsigned long) c; \
	register unsigned long __a3 asm("$7") = (unsigned long) d; \
	register unsigned long __a4 asm("$8") = (unsigned long) e; \
	register unsigned long __a5 asm("$9") = (unsigned long) f; \
	unsigned long __v0; \
	\
	__asm__ volatile ( \
	".set\tnoreorder\n\t" \
	"li\t$2, %7\t\t\t# " #name "\n\t" \
	"syscall\n\t" \
	"move\t%0, $2\n\t" \
	".set\treorder" \
	: "=&r" (__v0), "+r" (__a3) \
	: "r" (__a0), "r" (__a1), "r" (__a2), "r" (__a4), "r" (__a5), \
	  "i" (__NR_##name) \
	: "$2","$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24"); \
	\
	if (__a3 == 0) \
		return (type) __v0; \
	errno = __v0; \
	return -1; \
}

#endif /* (_MIPS_SIM == _MIPS_SIM_NABI32) || (_MIPS_SIM == _MIPS_SIM_ABI64) */

#ifdef __KERNEL__
#define __ARCH_WANT_IPC_PARSE_VERSION
#define __ARCH_WANT_OLD_READDIR
#define __ARCH_WANT_SYS_ALARM
#define __ARCH_WANT_SYS_GETHOSTNAME
#define __ARCH_WANT_SYS_PAUSE
#define __ARCH_WANT_SYS_SGETMASK
#define __ARCH_WANT_SYS_TIME
#define __ARCH_WANT_SYS_UTIME
#define __ARCH_WANT_SYS_WAITPID
#define __ARCH_WANT_SYS_SOCKETCALL
#define __ARCH_WANT_SYS_FADVISE64
#define __ARCH_WANT_SYS_GETPGRP
#define __ARCH_WANT_SYS_LLSEEK
#define __ARCH_WANT_SYS_NICE
#define __ARCH_WANT_SYS_OLD_GETRLIMIT
#define __ARCH_WANT_SYS_OLDUMOUNT
#define __ARCH_WANT_SYS_SIGPENDING
#define __ARCH_WANT_SYS_SIGPROCMASK
#define __ARCH_WANT_SYS_RT_SIGACTION
# ifndef __mips64
#  define __ARCH_WANT_STAT64
# endif
#endif

#ifdef __KERNEL_SYSCALLS__

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/linkage.h>
#include <asm/ptrace.h>
#include <asm/sim.h>

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */
static inline _syscall0(pid_t,setsid)
static inline _syscall3(int,write,int,fd,const char *,buf,off_t,count)
static inline _syscall3(int,read,int,fd,char *,buf,off_t,count)
static inline _syscall3(off_t,lseek,int,fd,off_t,offset,int,count)
static inline _syscall1(int,dup,int,fd)
static inline _syscall3(int,execve,const char *,file,char **,argv,char **,envp)
static inline _syscall3(int,open,const char *,file,int,flag,int,mode)
static inline _syscall1(int,close,int,fd)
struct rusage;
static inline _syscall4(pid_t,wait4,pid_t,pid,int *,stat_addr,int,options,struct rusage *,ru)

static inline pid_t waitpid(int pid, int * wait_stat, int flags)
{
	return wait4(pid, wait_stat, flags, NULL);
}

asmlinkage unsigned long sys_mmap(
				unsigned long addr, size_t len,
				int prot, int flags,
				int fd, off_t offset);
asmlinkage long sys_mmap2(
			unsigned long addr, unsigned long len,
			unsigned long prot, unsigned long flags,
			unsigned long fd, unsigned long pgoff);
asmlinkage int sys_execve(nabi_no_regargs struct pt_regs regs);
asmlinkage int sys_pipe(nabi_no_regargs struct pt_regs regs);
asmlinkage int sys_ptrace(long request, long pid, long addr, long data);
struct sigaction;
asmlinkage long sys_rt_sigaction(int sig,
				const struct sigaction __user *act,
				struct sigaction __user *oact,
				size_t sigsetsize);

#endif /* __KERNEL_SYSCALLS__ */
#endif /* !__ASSEMBLY__ */

/*
 * "Conditional" syscalls
 *
 * What we want is __attribute__((weak,alias("sys_ni_syscall"))),
 * but it doesn't work on all toolchains, so we just do it by hand
 */
#define cond_syscall(x) asm(".weak\t" #x "\n" #x "\t=\tsys_ni_syscall");

#endif /* _ASM_UNISTD_H */
