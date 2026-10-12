BUILD_DIR ?= .build
CC ?= cc
CXX ?= c++
CFLAGS ?= -O2 -Wall -Wextra
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

.PHONY: all test
all: $(BUILD_DIR)/cec-xs-bridge $(BUILD_DIR)/cec-xs-client $(BUILD_DIR)/xs8_send $(BUILD_DIR)/xs8-service

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/xs8_gpio.o: xs8_gpio.c xs8_gpio.h xs8_rt.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -std=c11 -pthread -c xs8_gpio.c -o $@

$(BUILD_DIR)/xs8_rt.o: xs8_rt.c xs8_rt.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -std=c11 -pthread -c xs8_rt.c -o $@

$(BUILD_DIR)/xs8_decode.o: xs8_decode.c xs8_decode.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -std=c11 -c xs8_decode.c -o $@

$(BUILD_DIR)/xs8_monitor.o: xs8_monitor.c xs8_monitor.h xs8_decode.h xs8_gpio.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -std=c11 -pthread -c xs8_monitor.c -o $@

$(BUILD_DIR)/cec-xs-bridge: main.cpp volume_control.cpp volume_control.h xs8_gpio.h xs8_monitor.h $(BUILD_DIR)/xs8_gpio.o $(BUILD_DIR)/xs8_rt.o $(BUILD_DIR)/xs8_decode.o $(BUILD_DIR)/xs8_monitor.o
	$(CXX) $(CXXFLAGS) main.cpp volume_control.cpp $(BUILD_DIR)/xs8_gpio.o $(BUILD_DIR)/xs8_rt.o $(BUILD_DIR)/xs8_decode.o $(BUILD_DIR)/xs8_monitor.o -o $@ -lcec -ldl -pthread -lgpiod

$(BUILD_DIR)/cec-xs-client: cec_xs_client.cpp cec_client_core.cpp cec_client_core.h cec_service_socket.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) cec_xs_client.cpp cec_client_core.cpp -o $@ -lcec -ldl -pthread

$(BUILD_DIR)/xs8_send: xs8_send.c xs8_gpio.h $(BUILD_DIR)/xs8_gpio.o $(BUILD_DIR)/xs8_rt.o
	$(CC) $(CFLAGS) -std=c11 xs8_send.c $(BUILD_DIR)/xs8_gpio.o $(BUILD_DIR)/xs8_rt.o -o $@ -pthread -lgpiod

$(BUILD_DIR)/xs8-service: xs8_service.cpp xs8_service_core.cpp xs8_service_core.h xs8_query_status.cpp xs8_query_status.h xs8_receiver_state.cpp xs8_receiver_state.h xs8_research.cpp xs8_research.h volume_control.cpp volume_control.h xs8_gpio.h xs8_monitor.h $(BUILD_DIR)/xs8_gpio.o $(BUILD_DIR)/xs8_rt.o $(BUILD_DIR)/xs8_decode.o $(BUILD_DIR)/xs8_monitor.o
	$(CXX) $(CXXFLAGS) xs8_service.cpp xs8_service_core.cpp xs8_query_status.cpp xs8_receiver_state.cpp xs8_research.cpp volume_control.cpp $(BUILD_DIR)/xs8_gpio.o $(BUILD_DIR)/xs8_rt.o $(BUILD_DIR)/xs8_decode.o $(BUILD_DIR)/xs8_monitor.o -o $@ -pthread -lgpiod

$(BUILD_DIR)/volume_control_test: volume_control.cpp volume_control.h tests/volume_control_test.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Werror -I. volume_control.cpp tests/volume_control_test.cpp -o $@

test: $(BUILD_DIR)/volume_control_test $(BUILD_DIR)/xs8_decode_test $(BUILD_DIR)/xs8_rt_test $(BUILD_DIR)/xs8_service_core_test $(BUILD_DIR)/xs8_query_status_test $(BUILD_DIR)/xs8_receiver_state_test $(BUILD_DIR)/xs8_research_test $(BUILD_DIR)/cec_client_core_test $(BUILD_DIR)/cec_service_socket_test $(BUILD_DIR)/xs8_service_socket_test
	./$(BUILD_DIR)/volume_control_test
	./$(BUILD_DIR)/xs8_decode_test
	./$(BUILD_DIR)/xs8_rt_test
	./$(BUILD_DIR)/xs8_service_core_test
	./$(BUILD_DIR)/xs8_query_status_test
	./$(BUILD_DIR)/xs8_receiver_state_test
	./$(BUILD_DIR)/xs8_research_test
	./$(BUILD_DIR)/cec_client_core_test
	./$(BUILD_DIR)/cec_service_socket_test
	python3 -m unittest tests/test_xs8_repl.py
	python3 -m unittest tests/test_xs8_service_socket.py

$(BUILD_DIR)/xs8_decode_test: xs8_decode.c xs8_decode.h tests/xs8_decode_test.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -std=c11 -Werror -I. xs8_decode.c tests/xs8_decode_test.c -o $@

$(BUILD_DIR)/xs8_rt_test: xs8_rt.c xs8_rt.h tests/xs8_rt_test.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -std=c11 -Werror -pthread -I. xs8_rt.c tests/xs8_rt_test.c -o $@

$(BUILD_DIR)/xs8_service_core_test: xs8_service_core.cpp xs8_service_core.h volume_control.cpp volume_control.h tests/xs8_service_core_test.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Werror -I. xs8_service_core.cpp volume_control.cpp tests/xs8_service_core_test.cpp -o $@

$(BUILD_DIR)/xs8_query_status_test: xs8_query_status.cpp xs8_query_status.h xs8_monitor.h xs8_decode.h tests/xs8_query_status_test.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Werror -I. xs8_query_status.cpp tests/xs8_query_status_test.cpp -o $@

$(BUILD_DIR)/xs8_receiver_state_test: xs8_receiver_state.cpp xs8_receiver_state.h xs8_query_status.cpp xs8_query_status.h xs8_monitor.h xs8_decode.h tests/xs8_receiver_state_test.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Werror -I. xs8_receiver_state.cpp xs8_query_status.cpp tests/xs8_receiver_state_test.cpp -o $@

$(BUILD_DIR)/xs8_research_test: xs8_research.cpp xs8_research.h xs8_monitor.h xs8_decode.h tests/xs8_research_test.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Werror -I. xs8_research.cpp tests/xs8_research_test.cpp -o $@

$(BUILD_DIR)/cec_client_core_test: cec_client_core.cpp cec_client_core.h tests/cec_client_core_test.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Werror -I. cec_client_core.cpp tests/cec_client_core_test.cpp -o $@ -pthread

$(BUILD_DIR)/cec_service_socket_test: cec_client_core.cpp cec_client_core.h cec_service_socket.h tests/cec_service_socket_test.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Werror -I. cec_client_core.cpp tests/cec_service_socket_test.cpp -o $@ -pthread

$(BUILD_DIR)/xs8_service_socket_test: xs8_service.cpp xs8_service_core.cpp xs8_service_core.h xs8_query_status.cpp xs8_query_status.h xs8_receiver_state.cpp xs8_receiver_state.h xs8_research.cpp xs8_research.h volume_control.cpp volume_control.h xs8_gpio.h xs8_monitor.h tests/stubs/gpiod.h tests/xs8_service_fake_backend.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Werror -Itests/stubs -I. xs8_service.cpp xs8_service_core.cpp xs8_query_status.cpp xs8_receiver_state.cpp xs8_research.cpp volume_control.cpp tests/xs8_service_fake_backend.cpp -o $@ -pthread

.PHONY: pi-build
pi-build:
	sh deploy/pi-build/build.sh
