#include <windows.h>
#include "jbtest.h"
static __thread int t_value = 5;
static __thread char t_zero[256];
static DWORD WINAPI worker(LPVOID p)
{
    int *ok = p;
    *ok = (t_value == 5 && t_zero[100] == 0);
    t_value = 9;
    return 0;
}
int main(void)
{
    CHECK(t_value == 5);
    t_value = 7;
    int ok[4] = {0};
    HANDLE h[4];
    for (int i = 0; i < 4; i++) h[i] = CreateThread(NULL, 0, worker, &ok[i], 0, NULL);
    WaitForMultipleObjects(4, h, TRUE, INFINITE);
    for (int i = 0; i < 4; i++) CHECK(ok[i]);
    CHECK(t_value == 7);
    DONE();
}
