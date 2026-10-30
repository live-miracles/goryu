# Cap libsrt's log output at 20 lines per second for the whole process

On 2026-09-29 a receive-queue backlog on a bonded leg made libsrt log about 35 lines per second for 16 minutes, which loaded a small host enough to starve `systemd-journald` and take down its SSH and networking. Goryu sets libsrt's log level to errors only, and a process-wide handler caps what is left at 20 lines per second with a one-line summary when it trims. Lowering the level alone was not enough: the cap makes the failure impossible whatever triggers the next burst, at the price of dropping messages during one.

Details are in [Reliability](../3-operations/1-reliability.md#log-volume).
