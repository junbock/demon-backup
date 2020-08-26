.SUFFIXES : .c .o

CC = gcc
LDFLAGS = -lpthread

OBJS = ssu_backup.o
SRCS = ssu_backup.c

TARGET = ssu_backup

all : $(TARGET)

$(TARGET) : $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

clean :
	rm -rf $(OBJS) $(TARGET)
	rm ./backup/*
	rm backup_log

ssu_backup.o: ssu_backup.c
