# Compile options
CFLAGS=-Wall -g

DEP=
OBJ=main.o

%.o: %.c $(DEP)
	gcc -c $(CFLAGS) -o $@ $<

all: $(OBJ)
	gcc $(CFLAGS) -o itsh $^

clean:
	rm -f *.o itsh

dist: clean
	tar -czf s4236151.tar.gz *
