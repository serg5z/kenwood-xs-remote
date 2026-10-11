CC ?= cc
CXX ?= c++
CFLAGS ?= -O2 -Wall -Wextra
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

.PHONY: all test
all: .build/cec-xs-bridge .build/xs8_send

.build:
	mkdir -p .build

.build/xs8_gpio.o: xs8_gpio.c xs8_gpio.h | .build
	$(CC) $(CFLAGS) -std=c11 -pthread -c xs8_gpio.c -o $@

.build/xs8_decode.o: xs8_decode.c xs8_decode.h | .build
	$(CC) $(CFLAGS) -std=c11 -c xs8_decode.c -o $@

.build/xs8_monitor.o: xs8_monitor.c xs8_monitor.h xs8_decode.h xs8_gpio.h | .build
	$(CC) $(CFLAGS) -std=c11 -pthread -c xs8_monitor.c -o $@

.build/cec-xs-bridge: main.cpp volume_control.cpp volume_control.h xs8_gpio.h xs8_monitor.h .build/xs8_gpio.o .build/xs8_decode.o .build/xs8_monitor.o
	$(CXX) $(CXXFLAGS) main.cpp volume_control.cpp .build/xs8_gpio.o .build/xs8_decode.o .build/xs8_monitor.o -o $@ -lcec -ldl -pthread -lgpiod

.build/xs8_send: xs8_send.c xs8_gpio.h .build/xs8_gpio.o
	$(CC) $(CFLAGS) -std=c11 xs8_send.c .build/xs8_gpio.o -o $@ -pthread -lgpiod

.build/volume_control_test: volume_control.cpp volume_control.h tests/volume_control_test.cpp | .build
	$(CXX) $(CXXFLAGS) -Werror -I. volume_control.cpp tests/volume_control_test.cpp -o $@

test: .build/volume_control_test .build/xs8_decode_test
	./.build/volume_control_test
	./.build/xs8_decode_test

.build/xs8_decode_test: xs8_decode.c xs8_decode.h tests/xs8_decode_test.c | .build
	$(CC) $(CFLAGS) -std=c11 -Werror -I. xs8_decode.c tests/xs8_decode_test.c -o $@
