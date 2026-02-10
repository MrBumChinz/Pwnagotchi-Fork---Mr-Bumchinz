# PwnaUI Makefile
# Build the high-performance C UI daemon for Pwnagotchi

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE
LDFLAGS = -lm -lutil -lpthread -lsqlite3

# Optional: libwebsockets for bettercap WebSocket client
# Set HAVE_LIBWEBSOCKETS=1 to enable
ifdef HAVE_LIBWEBSOCKETS
CFLAGS += -DHAVE_LIBWEBSOCKETS
LDFLAGS += -lwebsockets
endif

# For ARM optimization (Raspberry Pi Zero)
ARMFLAGS = -march=armv6 -mfpu=vfp -mfloat-abi=hard

# Source files
SRCDIR = src
OBJDIR = obj
BINDIR = bin
TESTDIR = tests

# Core sources (always built)
SOURCES = $(SRCDIR)/pwnaui.c \
          $(SRCDIR)/ipc.c \
          $(SRCDIR)/renderer.c \
          $(SRCDIR)/display.c \
          $(SRCDIR)/font.c \
          $(SRCDIR)/icons.c \
          $(SRCDIR)/plugins.c \
          $(SRCDIR)/gps.c \
          $(SRCDIR)/faces.c \
          $(SRCDIR)/themes.c \
          $(SRCDIR)/lodepng.c \
          $(SRCDIR)/cJSON.c \
          $(SRCDIR)/bcap_ws.c \
          $(SRCDIR)/health_monitor.c \
          $(SRCDIR)/thompson.c \
          $(SRCDIR)/brain.c $(SRCDIR)/brain_attacks.c $(SRCDIR)/brain_handshake.c \
          $(SRCDIR)/channel_bandit.c \
          $(SRCDIR)/stealth.c \
          $(SRCDIR)/wifi_recovery.c \
          $(SRCDIR)/crack_manager.c \
          $(SRCDIR)/pcap_check.c \
          $(SRCDIR)/pcapng_gps.c \
          $(SRCDIR)/hc22000.c \
          $(SRCDIR)/attack_log.c \
          $(SRCDIR)/pisugar.c \
          $(SRCDIR)/webserver.c \
          $(SRCDIR)/gps_refine.c \
          $(SRCDIR)/ap_database.c \
          $(SRCDIR)/hash_sync.c
# Test sources (modules without main())
TEST_MODULES = $(SRCDIR)/ipc.c \
               $(SRCDIR)/renderer.c \
               $(SRCDIR)/display.c \
               $(SRCDIR)/font.c \
               $(SRCDIR)/icons.c \
               $(SRCDIR)/plugins.c \
               $(SRCDIR)/gps.c

OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
TARGET = $(BINDIR)/pwnaui
TEST_TARGET = $(BINDIR)/test_runner

# Install paths
PREFIX = /usr/local
BINPREFIX = $(PREFIX)/bin
LIBPREFIX = /usr/lib/python3/dist-packages
SYSTEMD_DIR = /etc/systemd/system

.PHONY: all clean install uninstall arm debug test test-c test-python

all: $(TARGET)

# Create output directories
$(OBJDIR):
	mkdir -p $(OBJDIR)

$(BINDIR):
	mkdir -p $(BINDIR)

# Compile object files
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Link executable
$(TARGET): $(OBJECTS) | $(BINDIR)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

# ═══════════════════════════════════════════════════════════════════════════
# Test Targets
# ═══════════════════════════════════════════════════════════════════════════

# Build and run all tests
test: test-c test-python
	@echo ""
	@echo "══════════════════════════════════════════════════════════"
	@echo "                 ALL TESTS COMPLETED"
	@echo "══════════════════════════════════════════════════════════"

# Build and run C tests
test-c: $(TEST_TARGET)
	@echo ""
	@echo "Running C unit tests..."
	@./$(TEST_TARGET)

# Build C test runner
$(TEST_TARGET): $(TEST_MODULES) $(TESTDIR)/test_all.c $(TESTDIR)/test_font.c $(TESTDIR)/test_renderer.c $(TESTDIR)/test_display.c $(TESTDIR)/test_icons.c $(TESTDIR)/test_ipc.c | $(BINDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) -DTEST_ALL $(TEST_MODULES) $(TESTDIR)/test_all.c $(TESTDIR)/test_font.c $(TESTDIR)/test_renderer.c $(TESTDIR)/test_display.c $(TESTDIR)/test_icons.c $(TESTDIR)/test_ipc.c -o $@ $(LDFLAGS)

# Build individual test executables (for development)
test-font: $(SRCDIR)/font.c $(TESTDIR)/test_font.c | $(BINDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) $(SRCDIR)/font.c $(TESTDIR)/test_font.c -o $(BINDIR)/test_font $(LDFLAGS)
	./$(BINDIR)/test_font

test-renderer: $(SRCDIR)/renderer.c $(SRCDIR)/font.c $(SRCDIR)/icons.c $(SRCDIR)/display.c $(TESTDIR)/test_renderer.c | $(BINDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) $(SRCDIR)/renderer.c $(SRCDIR)/font.c $(SRCDIR)/icons.c $(SRCDIR)/display.c $(TESTDIR)/test_renderer.c -o $(BINDIR)/test_renderer $(LDFLAGS)
	./$(BINDIR)/test_renderer

test-display: $(SRCDIR)/display.c $(TESTDIR)/test_display.c | $(BINDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) $(SRCDIR)/display.c $(TESTDIR)/test_display.c -o $(BINDIR)/test_display $(LDFLAGS)
	./$(BINDIR)/test_display

test-icons: $(SRCDIR)/icons.c $(SRCDIR)/renderer.c $(SRCDIR)/font.c $(SRCDIR)/display.c $(TESTDIR)/test_icons.c | $(BINDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) $(SRCDIR)/icons.c $(SRCDIR)/renderer.c $(SRCDIR)/font.c $(SRCDIR)/display.c $(TESTDIR)/test_icons.c -o $(BINDIR)/test_icons $(LDFLAGS)
	./$(BINDIR)/test_icons

test-ipc: $(SRCDIR)/ipc.c $(TESTDIR)/test_ipc.c | $(BINDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) $(SRCDIR)/ipc.c $(TESTDIR)/test_ipc.c -o $(BINDIR)/test_ipc $(LDFLAGS)
	./$(BINDIR)/test_ipc

# Run Python tests
test-python:
	@echo ""
	@echo "Running Python unit tests..."
	@python3 $(TESTDIR)/run_python_tests.py

# ═══════════════════════════════════════════════════════════════════════════

# ARM-optimized build for Raspberry Pi
arm: CFLAGS += $(ARMFLAGS)
arm: clean all

# Debug build
debug: CFLAGS += -g -DDEBUG -O0
debug: clean all

# Clean build artifacts
clean:
	rm -rf $(OBJDIR) $(BINDIR)

# Clean test artifacts
clean-tests:
	rm -f $(BINDIR)/test_*

# Install daemon and Python client
install: $(TARGET)
	@echo "Installing PwnaUI..."
	install -d $(DESTDIR)$(BINPREFIX)
	install -m 755 $(TARGET) $(DESTDIR)$(BINPREFIX)/pwnaui
	install -d $(DESTDIR)$(LIBPREFIX)
	install -m 644 python/pwnaui_client.py $(DESTDIR)$(LIBPREFIX)/pwnaui_client.py
	install -m 644 python/pwnaui_view.py $(DESTDIR)$(LIBPREFIX)/pwnaui_view.py
	install -d $(DESTDIR)$(SYSTEMD_DIR)
	install -m 644 pwnaui.service $(DESTDIR)$(SYSTEMD_DIR)/pwnaui.service
	@echo ""
	@echo "Installation complete!"
	@echo "To enable the service:"
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable pwnaui"
	@echo "  sudo systemctl start pwnaui"

# Uninstall
uninstall:
	@echo "Uninstalling PwnaUI..."
	systemctl stop pwnaui 2>/dev/null || true
	systemctl disable pwnaui 2>/dev/null || true
	rm -f $(DESTDIR)$(BINPREFIX)/pwnaui
	rm -f $(DESTDIR)$(LIBPREFIX)/pwnaui_client.py
	rm -f $(DESTDIR)$(SYSTEMD_DIR)/pwnaui.service
	systemctl daemon-reload
	@echo "Uninstall complete"

# Cross-compile for ARM on x86 host
CROSS_CC = arm-linux-gnueabihf-gcc
cross:
	$(CROSS_CC) $(CFLAGS) $(ARMFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS)

# Generate dependency info (for development)
depend: $(SOURCES)
	$(CC) -MM $(CFLAGS) $(SOURCES) > .depend

-include .depend
