

CC     = gcc
CFLAGS = -Wall -Wextra -g
LIBS   = -lpthread

all: main sensor controller logger alert

main: main.c common.h
	$(CC) $(CFLAGS) -o main main.c $(LIBS)

sensor: sensor.c common.h
	$(CC) $(CFLAGS) -o sensor sensor.c $(LIBS)

controller: controller.c common.h
	$(CC) $(CFLAGS) -o controller controller.c $(LIBS)

logger: logger.c common.h
	$(CC) $(CFLAGS) -o logger logger.c $(LIBS)

alert: alert.c common.h
	$(CC) $(CFLAGS) -o alert alert.c $(LIBS)

clean:
	rm -f main sensor controller logger alert
	rm -f system.log daily_report.txt alerts.log alert_report.txt
	rm -f /tmp/log_fifo
	ipcrm -M 0x1234 2>/dev/null || true
	ipcrm -Q 0x5678 2>/dev/null || true
