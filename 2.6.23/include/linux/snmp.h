/*
 * Definitions for MIBs
 *
 * Author: Hideaki YOSHIFUJI <yoshfuji@linux-ipv6.org>
 */

#ifndef _LINUX_SNMP_H
#define _LINUX_SNMP_H

/* ipstats mib definitions */
/*
 * RFC 1213:  MIB-II
 * RFC 2011 (updates 1213):  SNMPv2-MIB-IP
 * RFC 2863:  Interfaces Group MIB
 * RFC 2465:  IPv6 MIB: General Group
 * draft-ietf-ipv6-rfc2011-update-10.txt: MIB for IP: IP Statistics Tables
 */
enum
{
	IPSTATS_MIB_NUM = 0,
	IPSTATS_MIB_INRECEIVES,			/* InReceives */
	IPSTATS_MIB_INHDRERRORS,		/* InHdrErrors */
	IPSTATS_MIB_INTOOBIGERRORS,		/* InTooBigErrors */
	IPSTATS_MIB_INNOROUTES,			/* InNoRoutes */
	IPSTATS_MIB_INADDRERRORS,		/* InAddrErrors */
	IPSTATS_MIB_INUNKNOWNPROTOS,		/* InUnknownProtos */
	IPSTATS_MIB_INTRUNCATEDPKTS,		/* InTruncatedPkts */
	IPSTATS_MIB_INDISCARDS,			/* InDiscards */
	IPSTATS_MIB_INDELIVERS,			/* InDelivers */
	IPSTATS_MIB_OUTFORWDATAGRAMS,		/* OutForwDatagrams */
	IPSTATS_MIB_OUTREQUESTS,		/* OutRequests */
	IPSTATS_MIB_OUTDISCARDS,		/* OutDiscards */
	IPSTATS_MIB_OUTNOROUTES,		/* OutNoRoutes */
	IPSTATS_MIB_REASMTIMEOUT,		/* ReasmTimeout */
	IPSTATS_MIB_REASMREQDS,			/* ReasmReqds */
	IPSTATS_MIB_REASMOKS,			/* ReasmOKs */
	IPSTATS_MIB_REASMFAILS,			/* ReasmFails */
	IPSTATS_MIB_FRAGOKS,			/* FragOKs */
	IPSTATS_MIB_FRAGFAILS,			/* FragFails */
	IPSTATS_MIB_FRAGCREATES,		/* FragCreates */
	IPSTATS_MIB_INMCASTPKTS,		/* InMcastPkts */
	IPSTATS_MIB_OUTMCASTPKTS,		/* OutMcastPkts */
	IPSTATS_MIB_INBCASTPKTS,		/* InBcastPkts */
	IPSTATS_MIB_OUTBCASTPKTS,		/* OutBcastPkts */
	__IPSTATS_MIB_MAX
};

/* icmp mib definitions */
/*
 * RFC 1213:  MIB-II ICMP Group
 * RFC 2011 (updates 1213):  SNMPv2 MIB for IP: ICMP group
 */
enum
{
	ICMP_MIB_NUM = 0,
	ICMP_MIB_INMSGS,			/* InMsgs */
	ICMP_MIB_INERRORS,			/* InErrors */
	ICMP_MIB_INDESTUNREACHS,		/* InDestUnreachs */
	ICMP_MIB_INTIMEEXCDS,			/* InTimeExcds */
	ICMP_MIB_INPARMPROBS,			/* InParmProbs */
	ICMP_MIB_INSRCQUENCHS,			/* InSrcQuenchs */
	ICMP_MIB_INREDIRECTS,			/* InRedirects */
	ICMP_MIB_INECHOS,			/* InEchos */
	ICMP_MIB_INECHOREPS,			/* InEchoReps */
	ICMP_MIB_INTIMESTAMPS,			/* InTimestamps */
	ICMP_MIB_INTIMESTAMPREPS,		/* InTimestampReps */
	ICMP_MIB_INADDRMASKS,			/* InAddrMasks */
	ICMP_MIB_INADDRMASKREPS,		/* InAddrMaskReps */
	ICMP_MIB_OUTMSGS,			/* OutMsgs */
	ICMP_MIB_OUTERRORS,			/* OutErrors */
	ICMP_MIB_OUTDESTUNREACHS,		/* OutDestUnreachs */
	ICMP_MIB_OUTTIMEEXCDS,			/* OutTimeExcds */
	ICMP_MIB_OUTPARMPROBS,			/* OutParmProbs */
	ICMP_MIB_OUTSRCQUENCHS,			/* OutSrcQuenchs */
	ICMP_MIB_OUTREDIRECTS,			/* OutRedirects */
	ICMP_MIB_OUTECHOS,			/* OutEchos */
	ICMP_MIB_OUTECHOREPS,			/* OutEchoReps */
	ICMP_MIB_OUTTIMESTAMPS,			/* OutTimestamps */
	ICMP_MIB_OUTTIMESTAMPREPS,		/* OutTimestampReps */
	ICMP_MIB_OUTADDRMASKS,			/* OutAddrMasks */
	ICMP_MIB_OUTADDRMASKREPS,		/* OutAddrMaskReps */
	__ICMP_MIB_MAX
};

/* icmp6 mib definitions */
/*
 * RFC 2466:  ICMPv6-MIB
 */
enum
{
	ICMP6_MIB_NUM = 0,
	ICMP6_MIB_INMSGS,			/* InMsgs */
	ICMP6_MIB_INERRORS,			/* InErrors */
	ICMP6_MIB_INDESTUNREACHS,		/* InDestUnreachs */
	ICMP6_MIB_INPKTTOOBIGS,			/* InPktTooBigs */
	ICMP6_MIB_INTIMEEXCDS,			/* InTimeExcds */
	ICMP6_MIB_INPARMPROBLEMS,		/* InParmProblems */
	ICMP6_MIB_INECHOS,			/* InEchos */
	ICMP6_MIB_INECHOREPLIES,		/* InEchoReplies */
	ICMP6_MIB_INGROUPMEMBQUERIES,		/* InGroupMembQueries */
	ICMP6_MIB_INGROUPMEMBRESPONSES,		/* InGroupMembResponses */
	ICMP6_MIB_INGROUPMEMBREDUCTIONS,	/* InGroupMembReductions */
	ICMP6_MIB_INROUTERSOLICITS,		/* InRouterSolicits */
	ICMP6_MIB_INROUTERADVERTISEMENTS,	/* InRouterAdvertisements */
	ICMP6_MIB_INNEIGHBORSOLICITS,		/* InNeighborSolicits */
	ICMP6_MIB_INNEIGHBORADVERTISEMENTS,	/* InNeighborAdvertisements */
	ICMP6_MIB_INREDIRECTS,			/* InRedirects */
	ICMP6_MIB_OUTMSGS,			/* OutMsgs */
	ICMP6_MIB_OUTDESTUNREACHS,		/* OutDestUnreachs */
	ICMP6_MIB_OUTPKTTOOBIGS,		/* OutPktTooBigs */
	ICMP6_MIB_OUTTIMEEXCDS,			/* OutTimeExcds */
	ICMP6_MIB_OUTPARMPROBLEMS,		/* OutParmProblems */
	ICMP6_MIB_OUTECHOREPLIES,		/* OutEchoReplies */
	ICMP6_MIB_OUTROUTERSOLICITS,		/* OutRouterSolicits */
	ICMP6_MIB_OUTNEIGHBORSOLICITS,		/* OutNeighborSolicits */
	ICMP6_MIB_OUTNEIGHBORADVERTISEMENTS,	/* OutNeighborAdvertisements */
	ICMP6_MIB_OUTREDIRECTS,			/* OutRedirects */
	ICMP6_MIB_OUTGROUPMEMBRESPONSES,	/* OutGroupMembResponses */
	ICMP6_MIB_OUTGROUPMEMBREDUCTIONS,	/* OutGroupMembReductions */
	__ICMP6_MIB_MAX
};

/* tcp mib definitions */
/*
 * RFC 1213:  MIB-II TCP group
 * RFC 2012 (updates 1213):  SNMPv2-MIB-TCP
 */
enum
{
	TCP_MIB_NUM = 0,
	TCP_MIB_RTOALGORITHM,			/* RtoAlgorithm */
	TCP_MIB_RTOMIN,				/* RtoMin */
	TCP_MIB_RTOMAX,				/* RtoMax */
	TCP_MIB_MAXCONN,			/* MaxConn */
	TCP_MIB_ACTIVEOPENS,			/* ActiveOpens */
	TCP_MIB_PASSIVEOPENS,			/* PassiveOpens */
	TCP_MIB_ATTEMPTFAILS,			/* AttemptFails */
	TCP_MIB_ESTABRESETS,			/* EstabResets */
	TCP_MIB_CURRESTAB,			/* CurrEstab */
	TCP_MIB_INSEGS,				/* InSegs */
	TCP_MIB_OUTSEGS,			/* OutSegs */
	TCP_MIB_RETRANSSEGS,			/* RetransSegs */
	TCP_MIB_INERRS,				/* InErrs */
	TCP_MIB_OUTRSTS,			/* OutRsts */
	__TCP_MIB_MAX
};

/* udp mib definitions */
/*
 * RFC 1213:  MIB-II UDP group
 * RFC 2013 (updates 1213):  SNMPv2-MIB-UDP
 */
enum
{
	UDP_MIB_NUM = 0,
	UDP_MIB_INDATAGRAMS,			/* InDatagrams */
	UDP_MIB_NOPORTS,			/* NoPorts */
	UDP_MIB_INERRORS,			/* InErrors */
	UDP_MIB_OUTDATAGRAMS,			/* OutDatagrams */
	UDP_MIB_RCVBUFERRORS,			/* RcvbufErrors */
	UDP_MIB_SNDBUFERRORS,			/* SndbufErrors */
	__UDP_MIB_MAX
};

/* linux mib definitions */
enum
{
	LINUX_MIB_NUM = 0,
	LINUX_MIB_SYNCOOKIESSENT,		/* SyncookiesSent */
	LINUX_MIB_SYNCOOKIESRECV,		/* SyncookiesRecv */
	LINUX_MIB_SYNCOOKIESFAILED,		/* SyncookiesFailed */
	LINUX_MIB_EMBRYONICRSTS,		/* EmbryonicRsts */
	LINUX_MIB_PRUNECALLED,			/* PruneCalled */
	LINUX_MIB_RCVPRUNED,			/* RcvPruned */
	LINUX_MIB_OFOPRUNED,			/* OfoPruned */
	LINUX_MIB_OUTOFWINDOWICMPS,		/* OutOfWindowIcmps */
	LINUX_MIB_LOCKDROPPEDICMPS,		/* LockDroppedIcmps */
	LINUX_MIB_ARPFILTER,			/* ArpFilter */
	LINUX_MIB_TIMEWAITED,			/* TimeWaited */
	LINUX_MIB_TIMEWAITRECYCLED,		/* TimeWaitRecycled */
	LINUX_MIB_TIMEWAITKILLED,		/* TimeWaitKilled */
	LINUX_MIB_PAWSPASSIVEREJECTED,		/* PAWSPassiveRejected */
	LINUX_MIB_PAWSACTIVEREJECTED,		/* PAWSActiveRejected */
	LINUX_MIB_PAWSESTABREJECTED,		/* PAWSEstabRejected */
	LINUX_MIB_DELAYEDACKS,			/* DelayedACKs */
	LINUX_MIB_DELAYEDACKLOCKED,		/* DelayedACKLocked */
	LINUX_MIB_DELAYEDACKLOST,		/* DelayedACKLost */
	LINUX_MIB_LISTENOVERFLOWS,		/* ListenOverflows */
	LINUX_MIB_LISTENDROPS,			/* ListenDrops */
	LINUX_MIB_TCPPREQUEUED,			/* TCPPrequeued */
	LINUX_MIB_TCPDIRECTCOPYFROMBACKLOG,	/* TCPDirectCopyFromBacklog */
	LINUX_MIB_TCPDIRECTCOPYFROMPREQUEUE,	/* TCPDirectCopyFromPrequeue */
	LINUX_MIB_TCPPREQUEUEDROPPED,		/* TCPPrequeueDropped */
	LINUX_MIB_TCPHPHITS,			/* TCPHPHits */
	LINUX_MIB_TCPHPHITSTOUSER,		/* TCPHPHitsToUser */
	LINUX_MIB_TCPPUREACKS,			/* TCPPureAcks */
	LINUX_MIB_TCPHPACKS,			/* TCPHPAcks */
	LINUX_MIB_TCPRENORECOVERY,		/* TCPRenoRecovery */
	LINUX_MIB_TCPSACKRECOVERY,		/* TCPSackRecovery */
	LINUX_MIB_TCPSACKRENEGING,		/* TCPSACKReneging */
	LINUX_MIB_TCPFACKREORDER,		/* TCPFACKReorder */
	LINUX_MIB_TCPSACKREORDER,		/* TCPSACKReorder */
	LINUX_MIB_TCPRENOREORDER,		/* TCPRenoReorder */
	LINUX_MIB_TCPTSREORDER,			/* TCPTSReorder */
	LINUX_MIB_TCPFULLUNDO,			/* TCPFullUndo */
	LINUX_MIB_TCPPARTIALUNDO,		/* TCPPartialUndo */
	LINUX_MIB_TCPDSACKUNDO,			/* TCPDSACKUndo */
	LINUX_MIB_TCPLOSSUNDO,			/* TCPLossUndo */
	LINUX_MIB_TCPLOSS,			/* TCPLoss */
	LINUX_MIB_TCPLOSTRETRANSMIT,		/* TCPLostRetransmit */
	LINUX_MIB_TCPRENOFAILURES,		/* TCPRenoFailures */
	LINUX_MIB_TCPSACKFAILURES,		/* TCPSackFailures */
	LINUX_MIB_TCPLOSSFAILURES,		/* TCPLossFailures */
	LINUX_MIB_TCPFASTRETRANS,		/* TCPFastRetrans */
	LINUX_MIB_TCPFORWARDRETRANS,		/* TCPForwardRetrans */
	LINUX_MIB_TCPSLOWSTARTRETRANS,		/* TCPSlowStartRetrans */
	LINUX_MIB_TCPTIMEOUTS,			/* TCPTimeouts */
	LINUX_MIB_TCPRENORECOVERYFAIL,		/* TCPRenoRecoveryFail */
	LINUX_MIB_TCPSACKRECOVERYFAIL,		/* TCPSackRecoveryFail */
	LINUX_MIB_TCPSCHEDULERFAILED,		/* TCPSchedulerFailed */
	LINUX_MIB_TCPRCVCOLLAPSED,		/* TCPRcvCollapsed */
	LINUX_MIB_TCPDSACKOLDSENT,		/* TCPDSACKOldSent */
	LINUX_MIB_TCPDSACKOFOSENT,		/* TCPDSACKOfoSent */
	LINUX_MIB_TCPDSACKRECV,			/* TCPDSACKRecv */
	LINUX_MIB_TCPDSACKOFORECV,		/* TCPDSACKOfoRecv */
	LINUX_MIB_TCPABORTONSYN,		/* TCPAbortOnSyn */
	LINUX_MIB_TCPABORTONDATA,		/* TCPAbortOnData */
	LINUX_MIB_TCPABORTONCLOSE,		/* TCPAbortOnClose */
	LINUX_MIB_TCPABORTONMEMORY,		/* TCPAbortOnMemory */
	LINUX_MIB_TCPABORTONTIMEOUT,		/* TCPAbortOnTimeout */
	LINUX_MIB_TCPABORTONLINGER,		/* TCPAbortOnLinger */
	LINUX_MIB_TCPABORTFAILED,		/* TCPAbortFailed */
	LINUX_MIB_TCPMEMORYPRESSURES,		/* TCPMemoryPressures */
	__LINUX_MIB_MAX
};

#endif	/* _LINUX_SNMP_H */
