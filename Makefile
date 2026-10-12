CC ?= cc
CXX ?= c++
CFLAGS ?= -O2 -Wall -Wextra
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

.PHONY: all test
all: .build/cec-xs-bridge .build/cec-xs-client .build/xs8_send .build/xs8-service

.build:
	mkdir -p .build

.build/xs8_gpio.o: xs8_gpio.c xs8_gpio.h xs8_rt.h | .build
	$(CC) $(CFLAGS) -std=c11 -pthread -c xs8_gpio.c -o $@

.build/xs8_rt.o: xs8_rt.c xs8_rt.h | .build
	$(CC) $(CFLAGS) -std=c11 -pthread -c xs8_rt.c -o $@

.build/xs8_decode.o: xs8_decode.c xs8_decode.h | .build
	$(CC) $(CFLAGS) -std=c11 -c xs8_decode.c -o $@

.build/xs8_monitor.o: xs8_monitor.c xs8_monitor.h xs8_decode.h xs8_gpio.h | .build
	$(CC) $(CFLAGS) -std=c11 -pthread -c xs8_monitor.c -o $@

.build/cec-xs-bridge: main.cpp volume_control.cpp volume_control.h xs8_gpio.h xs8_monitor.h .build/xs8_gpio.o .build/xs8_rt.o .build/xs8_decode.o .build/xs8_monitor.o
	$(CXX) $(CXXFLAGS) main.cpp volume_control.cpp .build/xs8_gpio.o .build/xs8_rt.o .build/xs8_decode.o .build/xs8_monitor.o -o $@ -lcec -ldl -pthread -lgpiod

.build/cec-xs-client: cec_xs_client.cpp cec_client_core.cpp cec_client_core.h cec_service_socket.h | .build
	$(CXX) $(CXXFLAGS) cec_xs_client.cpp cec_client_core.cpp -o $@ -lcec -ldl -pthread

.build/xs8_send: xs8_send.c xs8_gpio.h .build/xs8_gpio.o .build/xs8_rt.o
	$(CC) $(CFLAGS) -std=c11 xs8_send.c .build/xs8_gpio.o .build/xs8_rt.o -o $@ -pthread -lgpiod

.build/xs8-service: xs8_service.cpp xs8_service_core.cpp xs8_service_core.h xs8_query_status.cpp xs8_query_status.h xs8_receiver_state.cpp xs8_receiver_state.h xs8_research.cpp xs8_research.h volume_control.cpp volume_control.h xs8_gpio.h xs8_monitor.h .build/xs8_gpio.o .build/xs8_rt.o .build/xs8_decode.o .build/xs8_monitor.o
	$(CXX) $(CXXFLAGS) xs8_service.cpp xs8_service_core.cpp xs8_query_status.cpp xs8_receiver_state.cpp xs8_research.cpp volume_control.cpp .build/xs8_gpio.o .build/xs8_rt.o .build/xs8_decode.o .build/xs8_monitor.o -o $@ -pthread -lgpiod

.build/volume_control_test: volume_control.cpp volume_control.h tests/volume_control_test.cpp | .build
	$(CXX) $(CXXFLAGS) -Werror -I. volume_control.cpp tests/volume_control_test.cpp -o $@

test: .build/volume_control_test .build/xs8_decode_test .build/xs8_rt_test .build/xs8_service_core_test .build/xs8_query_status_test .build/xs8_receiver_state_test .build/xs8_research_test .build/cec_client_core_test .build/cec_service_socket_test .build/xs8_service_socket_test
	./.build/volume_control_test
	./.build/xs8_decode_test
	./.build/xs8_rt_test
	./.build/xs8_service_core_test
	./.build/xs8_query_status_test
	./.build/xs8_receiver_state_test
	./.build/xs8_research_test
	./.build/cec_client_core_test
	./.build/cec_service_socket_test
	python3 -m unittest tests/test_xs8_repl.py
	python3 -m unittest tests/test_xs8_service_socket.py

.build/xs8_decode_test: xs8_decode.c xs8_decode.h tests/xs8_decode_test.c | .build
	$(CC) $(CFLAGS) -std=c11 -Werror -I. xs8_decode.c tests/xs8_decode_test.c -o $@

.build/xs8_rt_test: xs8_rt.c xs8_rt.h tests/xs8_rt_test.c | .build
	$(CC) $(CFLAGS) -std=c11 -Werror -pthread -I. xs8_rt.c tests/xs8_rt_test.c -o $@

.build/xs8_service_core_test: xs8_service_core.cpp xs8_service_core.h volume_control.cpp volume_control.h tests/xs8_service_core_test.cpp | .build
	$(CXX) $(CXXFLAGS) -Werror -I. xs8_service_core.cpp volume_control.cpp tests/xs8_service_core_test.cpp -o $@

.build/xs8_query_status_test: xs8_query_status.cpp xs8_query_status.h xs8_monitor.h xs8_decode.h tests/xs8_query_status_test.cpp | .build
	$(CXX) $(CXXFLAGS) -Werror -I. xs8_query_status.cpp tests/xs8_query_status_test.cpp -o $@

.build/xs8_receiver_state_test: xs8_receiver_state.cpp xs8_receiver_state.h xs8_query_status.cpp xs8_query_status.h xs8_monitor.h xs8_decode.h tests/xs8_receiver_state_test.cpp | .build
	$(CXX) $(CXXFLAGS) -Werror -I. xs8_receiver_state.cpp xs8_query_status.cpp tests/xs8_receiver_state_test.cpp -o $@

.build/xs8_research_test: xs8_research.cpp xs8_research.h xs8_monitor.h xs8_decode.h tests/xs8_research_test.cpp | .build
	$(CXX) $(CXXFLAGS) -Werror -I. xs8_research.cpp tests/xs8_research_test.cpp -o $@

.build/cec_client_core_test: cec_client_core.cpp cec_client_core.h tests/cec_client_core_test.cpp | .build
	$(CXX) $(CXXFLAGS) -Werror -I. cec_client_core.cpp tests/cec_client_core_test.cpp -o $@ -pthread

.build/cec_service_socket_test: cec_client_core.cpp cec_client_core.h cec_service_socket.h tests/cec_service_socket_test.cpp | .build
	$(CXX) $(CXXFLAGS) -Werror -I. cec_client_core.cpp tests/cec_service_socket_test.cpp -o $@ -pthread

.build/xs8_service_socket_test: xs8_service.cpp xs8_service_core.cpp xs8_service_core.h xs8_query_status.cpp xs8_query_status.h xs8_receiver_state.cpp xs8_receiver_state.h xs8_research.cpp xs8_research.h volume_control.cpp volume_control.h xs8_gpio.h xs8_monitor.h tests/stubs/gpiod.h tests/xs8_service_fake_backend.cpp | .build
	$(CXX) $(CXXFLAGS) -Werror -Itests/stubs -I. xs8_service.cpp xs8_service_core.cpp xs8_query_status.cpp xs8_receiver_state.cpp xs8_research.cpp volume_control.cpp tests/xs8_service_fake_backend.cpp -o $@ -pthread
