
PREFIX=/usr/local/
BIN=$(PREFIX)/bin/
INSTALL=install

pvt: pvt.c
	$(CC) -Wall -o pvt $< -lcurses

clean:
	rm -f pvt
	@rm -f pvt.core
	@rm -f ktrace.out

install: pvt
	$(INSTALL) -csm 0755 pvt $(BIN)
