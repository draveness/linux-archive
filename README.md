# linux-archive 

Linux archive for studying the process scheduler

| Version | Scheduler| 
|:-:|:-:|
|0.01 | Initial Scheduler |
| 2.4.0 | O(n) Scheduler |
| 2.6.8.1 | O(1) Scheduler |
| 2.6.23 | CFS Scheduler|

# [调度系统设计精要](https://draveness.me/system-design-scheduler)

> 系统设计精要是一系列深入研究系统设计方法的系列文章，文中不仅会分析系统设计的理论，还会分析多个实际场景下的具体实现。这是一个季更或者半年更的系列，如果你有想要了解的问题，可以在文章下面留言。

调度是一个非常广泛的概念，很多领域都会使用调度这个术语，在计算机科学中，调度就是一种将任务（Work）分配给资源的方法[^1]。任务可能是虚拟的计算任务，例如线程、进程或者数据流，这些任务会被调度到硬件资源上执行，例如：处理器 CPU 等设备。

![system-design-and-scheduler](https://img.draveness.me/2020-02-02-15805807759135-system-design-and-scheduler.png)

**图 1 - 调度系统设计精要**

本文会介绍调度系统的常见场景以及设计过程中的一些关键问题，调度器的设计最终都会归结到一个问题上 — 如何对资源高效的分配和调度以达到我们的目的，可能包括对资源的合理利用、最小化成本、快速匹配供给和需求。


![mind-node](https://img.draveness.me/2020-02-02-15805826614612-mind-node.png)

**图 2 - 文章脉络和内容**

除了介绍调度系统设计时会遇到的常见问题之外，本文还会深入分析几种常见的调度器的设计、演进与实现原理，包括操作系统的进程调度器，Go 语言的运行时调度器以及 Kubernetes 的工作负载调度器，帮助我们理解调度器设计的核心原理。
