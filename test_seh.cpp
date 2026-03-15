#include <windows.h>
#include <eh.h>
#include <cstdio>
void handler(unsigned int code, _EXCEPTION_POINTERS*) { 
    printf("handled 0x%x\n", code); 
}
int main() { 
    _set_se_translator(handler); 
    printf("_set_se_translator works!\n");
    return 0; 
}
