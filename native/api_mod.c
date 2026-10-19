#include <stdio.h>

__declspec(dllexport) int test(int i) {
	return i * 2 + 1;
}
