#include <stdlib.h>
#include <stdio.h>
#include "screen.h"
#include "system.h"

int
main(int argc, char *argv[])
{
	int result;

	if (screen_init())
		return 1;
	atexit(screen_done);

	if (system_init())
		return 1;
	atexit(system_done);

	if (argc == 1) {
		result = system_loadfile("hello.com");
	} else if (argc == 2) {
		result = system_loadfile(argv[1]);
	} else {
		fprintf(stderr, "usage: %s [yourfile.com]\n", argv[0]);
		return -1;
	}

	if (result) {
		return -1;
	}

	result = system_tick(100);
	printf("result=%d\n", result);

	return 0;
}
