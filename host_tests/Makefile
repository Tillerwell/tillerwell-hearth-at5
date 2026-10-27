# Host-side tests for the protocol, bridge core and thermostat logic.
#   make -C host_tests          build and run
#   make -C host_tests vectors BASE=/path/to/autoterm-heater-control/autoterm
CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined -fno-omit-frame-pointer \
         -I../components/at_proto/include -I../components/at_core/include
SRCS = test_main.c ../components/at_proto/at_proto.c ../components/at_core/at_core.c ../components/at_core/at_ctl.c ../components/at_core/at_fuel.c ../components/at_core/at_timers.c ../components/at_core/at_tz.c ../components/at_core/at_version.c

test: test_main
	./test_main

test_main: $(SRCS) vectors.h
	$(CC) $(CFLAGS) $(SRCS) -o $@ -lm

vectors:
	python3 gen_vectors.py $(BASE) > vectors.h

clean:
	rm -f test_main
