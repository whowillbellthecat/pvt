
pvt: pvt.c
	$(CC) -Wall -o pvt -lcurses $<

clean:
	rm -f pvt
	@rm -f pvt.core
	@rm -f ktrace.out
