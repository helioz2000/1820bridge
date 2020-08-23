TARGET = 1820bridge
BIN_READ = 1820read
BINDIR = /usr/local/sbin/
CFGDIR = /etc/
CFGEXT = .cfg
CFGFILE = $(TARGET)$(CFGEXT)

SERVICE = $(TARGET).service
SERVICEDIR = /etc/systemd/system

# - Compiler
CC=gcc
CXX=g++
CFLAGS = -Wall -Wno-unused -Wno-unknown-pragmas

# Release:
CFLAGS += -O3
# Debug:
#CFLAGS += -Og

# - Linker
LIBS = -lpthread -lstdc++ -lm -lmosquitto -lconfig++

OBJDIR = ./obj

.PHONY: default all clean bridge read

default:
	@echo
	@echo "Use one of the following:"
	@echo "make read (to compile 1820read)"
	@echo "make bridge (to compile 1820bridge)"
	@echo "sudo make install (to install binaries)"
	@echo "sudo make service (to make 1820bridge a service)"

all: read bridge

#CSRCS += $(wildcard *.c)
#CSRCS += $(wildcard $(HWDIR)*.c)
#CPPSRCS += $(wildcard *.cpp)
#CPPSRCS += $(wildcard $(HWDIR)*.cpp)

#COBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(CSRCS))
#CPPOBJS = $(patsubst %.cpp,$(OBJDIR)/%.o,$(CPPSRCS))

#SRCS = $(CSRCS) $(CPPSRCS)
#OBJS = $(COBJS) $(CPPOBJS)

#SRC = $(wildcard *.cpp) $(wildcard aprs/*.c*)
#HDR = $(wildcard *.h) $(wildcard aprs/*.h)
#OBJ = $(SRC:%.c=%.o)

#OBJECTS = $(patsubst %c, %o, $(wildcard *.c))
#HEADERS = $(wildcard *.h)

#$(OBJDIR)/%.o: %.c
#	@mkdir -p $(OBJDIR)
#	@echo "CC $<"
#	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(OBJDIR)
	@echo "CXX $<"
	@$(CXX) $(CFLAGS) -c $< -o $@

# Dependencies
$(OBJDIR)/1820tag.o: 1820tag.h
$(OBJDIR)/dev1820.o: dev1820.h
$(OBJDIR)/mqtt.o: mqtt.h
$(OBJDIR)/1820bridge.o: 1820bridge.h 1820tag.h dev1820.h mqtt.h

read: $(OBJDIR)/dev1820.o $(OBJDIR)/1820read.o
	$(CXX) -o $(BIN_READ) $(OBJDIR)/dev1820.o $(OBJDIR)/1820read.o $(LDFLAGS)

bridge: $(OBJDIR)/dev1820.o $(OBJDIR)/1820bridge.o $(OBJDIR)/1820tag.o $(OBJDIR)/mqtt.o
	$(CXX) -o $(TARGET) $(LIBS) $(OBJDIR)/1820bridge.o $(OBJDIR)/dev1820.o $(OBJDIR)/1820tag.o $(OBJDIR)/mqtt.o

.PRECIOUS: $(TARGET) $(OBJ)

#
# install target and config file
#
install:
ifneq ($(shell id -u), 0)
	@echo "!!!! install requires root !!!!"
else
	install -o root $(TARGET) $(BINDIR)$(TARGET)
	install -o root $(CFGFILE) $(CFGDIR)$(CFGFILE)
#	install -m 755 -o root $(INITFILE) $(INITDIR)$(INITFILE)
#	mv $(INITDIR)$(INITFILE) $(INITDIR)$(TARGET)
	@echo ++++++++++++++++++++++++++++++++++++++++++++
	@echo ++ Files have been installed
	@echo ++ You will need to restart $(TARGET)
	@echo ++ sudo systemctl restart $(TARGET)
endif

#
# make systemd service
#
service:
ifneq ($(shell id -u), 0)
	@echo "!!!! service requires root !!!!"
else
	install -m 644 -o root $(SERVICE) $(SERVICEDIR)
	@systemctl daemon-reload
	@systemctl enable $(SERVICE)
	@echo $(TARGET) is now available as systemd service
endif

clean:
	-rm -f $(OBJDIR)/*.o
#	-rm -f $(OBJS)
#	-rm -rf $(TARGET)

