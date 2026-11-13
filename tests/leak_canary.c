/* Canary: a definite leak, used to prove LeakSanitizer actually fires in this
   environment. If this reports nothing, a clean report from ced means nothing
   either. */
#include <stdlib.h>
#include <string.h>

int main(void)
{
	char *lost = malloc(1234);

	memset(lost, 'x', 1234);
	lost = NULL;		/* drop the only pointer: definitely leaked */
	return lost == NULL ? 0 : 1;
}
